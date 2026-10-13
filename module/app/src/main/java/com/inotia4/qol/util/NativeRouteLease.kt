package com.inotia4.qol.util

import com.inotia4.qol.LogDomain
import com.inotia4.qol.LogFile
import com.inotia4.qol.NativeBridge
import com.yanzhenjie.andserver.http.HttpMethod
import com.yanzhenjie.andserver.http.HttpRequest
import com.yanzhenjie.andserver.http.StatusCode

/**
 * Direct-Kotlin HTTP 路由的 native route lease 接线。
 *
 * 只有由 Kotlin 直接处理、没有 native handler 的路由才在 controller 层获取 lease；经
 * `NativeHttpCommand`/`NativeHttpQuery`（`nativeHttpDispatch`）处理的路由已在 Host
 * dispatch 内完成 op/lease，不得在此包第二层。Host 按插件声明的 route metadata
 * 匹配并检查 OP，Kotlin 不以 path 前缀判 OP（见
 * `docs/refactor/api-optional-op-design.md` §8）。
 */
internal object NativeRouteLease {

    // nativeHttpRouteEnter 返回码，与 cpp contracts/result.h 的 QolResult.code 对齐。
    // 0 为成功；lease_required=false 时 leaseOut token 为 0，无需 Leave。
    private const val CODE_OK = 0                 // kQolOk
    private const val CODE_BAD_REQUEST = 1        // kQolErrInvalidArgument -> 400（method 不在声明集合）
    private const val CODE_NOT_FOUND = 2          // kQolErrNotFound      -> 404（未声明路由）
    private const val CODE_NOT_READY = 3          // kQolErrNotReady      -> 503（owner 停用/registry 未就绪）
    private const val CODE_BLOCKED = 9            // kQolErrBlocked       -> 403（OP 未开启）
    private const val CODE_BUSY = 10              // kQolErrBusy          -> 503（provider draining）

    /** native route-lease 失败映射；[code] 为 0 时返回 null。 */
    data class Failure(val status: Int, val message: String)

    fun failure(code: Int): Failure? = when (code) {
        CODE_OK -> null
        CODE_BAD_REQUEST -> Failure(StatusCode.SC_BAD_REQUEST, "bad method")
        CODE_NOT_FOUND -> Failure(StatusCode.SC_NOT_FOUND, "handler not found")
        CODE_BLOCKED -> Failure(StatusCode.SC_FORBIDDEN, "op disabled")
        CODE_NOT_READY -> Failure(StatusCode.SC_SERVICE_UNAVAILABLE, "http owner inactive")
        CODE_BUSY -> Failure(StatusCode.SC_SERVICE_UNAVAILABLE, "provider draining")
        else -> Failure(StatusCode.SC_INTERNAL_SERVER_ERROR, "internal error")
    }
}

/**
 * direct 路由通用 lease 流程：enter -> 执行 handler -> finally leave。
 *
 * enter 返回非 0 时按 [NativeRouteLease.failure] 抛错，不执行 handler、不 Leave；
 * 成功时无论 handler 正常返回还是抛异常都在 finally 释放 lease。lease token 为 0
 * （lease_required=false）时跳过 Leave。Leave 失败只经 [onLeaveError] 记录，不覆盖
 * handler 的结果或异常。
 */
internal inline fun <T> withRouteLease(
    enter: (Int, String, LongArray) -> Int,
    leave: (Long) -> Int,
    method: Int,
    path: String,
    onLeaveError: (Long, Int) -> Unit,
    onFailure: (Int, String) -> Nothing,
    handler: () -> T,
): T {
    val leaseOut = LongArray(1)
    val failure = NativeRouteLease.failure(enter(method, path, leaseOut))
    if (failure != null) onFailure(failure.status, failure.message)
    try {
        return handler()
    } finally {
        val lease = leaseOut[0]
        if (lease != 0L) {
            val leaveCode = leave(lease)
            if (leaveCode != 0) onLeaveError(lease, leaveCode)
        }
    }
}

/**
 * AndServer 注入的 [HttpRequest] 作为 handler 参数；取其 method 与 path 交给 Host
 * route metadata 匹配。GET -> 0、POST -> 1；其他方法不属于 direct 路由声明集合。
 * native 库未加载时抛 503（不触发 UnsatisfiedLinkError → 500）。
 */
internal inline fun <T> withNativeRouteLease(request: HttpRequest, handler: () -> T): T {
    if (!NativeBridge.libraryLoaded) {
        throw ApiException(StatusCode.SC_SERVICE_UNAVAILABLE, "not ready")
    }
    val method = when (request.method) {
        HttpMethod.GET -> 0
        HttpMethod.POST -> 1
        else -> throw ApiException(StatusCode.SC_BAD_REQUEST, "bad method")
    }
    return withRouteLease(
        enter = { m, p, out -> NativeBridge.nativeHttpRouteEnter(m, p, out) },
        leave = { lease -> NativeBridge.nativeHttpRouteLeave(lease) },
        method = method,
        path = request.path,
        onLeaveError = { lease, code ->
            LogFile.error(LogDomain.HTTP, "nativeHttpRouteLeave failed: lease=$lease code=$code")
        },
        onFailure = { status, message -> throw ApiException(status, message) },
        handler = handler,
    )
}
