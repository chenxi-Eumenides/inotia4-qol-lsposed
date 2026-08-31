package com.inotia4.qol.util

import com.inotia4.qol.NativeBridge
import com.yanzhenjie.andserver.error.HttpException
import com.yanzhenjie.andserver.http.StatusCode

/**
 * controller 公共守卫（architecture §9.3-9，v0.5.45 语义修复）：
 * - native 未就绪 → throw [ApiException](503)，Resolver 转 `{"ok":false,"error":"not ready"}`
 * - 业务异常（[HttpException] 家族）原样 rethrow，交由全局 Resolver 统一转状态码 + JSON 信封
 * - 其他异常 → throw [ApiException](500)（不再吞成 NOT_READY）
 * 88 处调用形态（方法引用/lambda 两式）保持不变。
 */
object ControllerGuard {

    fun ready(): Boolean = NativeBridge.ready

    fun guard(f: () -> String): String {
        if (!NativeBridge.ready) throw ApiException(StatusCode.SC_SERVICE_UNAVAILABLE, "not ready")
        return try {
            f()
        } catch (t: Throwable) {
            if (t is HttpException) throw t
            throw ApiException(StatusCode.SC_INTERNAL_SERVER_ERROR, "internal error", t)
        }
    }
}
