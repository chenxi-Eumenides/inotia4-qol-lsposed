package com.inotia4.qol.service.info

import com.inotia4.qol.NativeBridge
import com.inotia4.qol.util.ApiException
import com.inotia4.qol.util.JsonUtil
import com.yanzhenjie.andserver.http.StatusCode

/**
 * HTTP owner 查询透传：Kotlin 只做传输适配，不拼装业务响应。
 *
 * nativeHttpDispatch 返回 `{"status":N,"body":"<转义后的响应体>"}` 信封；本对象解出 body，
 * 并把 owner 的 4xx/5xx 转成 [ApiException]，由全局 Resolver 还原同一状态码与错误信封。
 * 这样 Controller/Service 不需要知道任何游戏语义，成功响应逐字节来自 owner。
 */
internal object NativeHttpQuery {

    fun get(path: String, query: String? = null): String {
        val envelope = JsonUtil.parseObj(NativeBridge.nativeHttpDispatch(0, path, query, null))
            ?: throw ApiException(StatusCode.SC_SERVICE_UNAVAILABLE, "not ready")
        val status = envelope.optInt("status", 200)
        val body = envelope.optString("body", "")
        if (status >= 400) {
            val message = JsonUtil.parseObj(body)?.optString("error")
                ?.takeIf { it.isNotEmpty() } ?: "request failed"
            throw ApiException(status, message)
        }
        return body
    }
}
