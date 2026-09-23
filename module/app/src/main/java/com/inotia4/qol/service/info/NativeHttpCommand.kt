package com.inotia4.qol.service.info

import com.inotia4.qol.NativeBridge
import com.inotia4.qol.util.ApiException
import com.inotia4.qol.util.JsonUtil
import com.yanzhenjie.andserver.http.StatusCode

/**
 * HTTP owner 命令透传：Kotlin 只做传输适配，不解析业务 body、不拼装响应。
 *
 * nativeHttpDispatch 返回 `{"status":N,"body":"<转义后的响应体>"}` 信封；本对象解出 body，
 * 并把 owner 的 4xx/5xx 转成 [ApiException]，由全局 Resolver 还原同一状态码与错误信封
 * （含 op 门禁的 403 `{"ok":false,"error":"op disabled"}`）。成功响应逐字节来自 owner。
 */
internal object NativeHttpCommand {

    fun post(path: String, body: String?): String {
        val envelope = JsonUtil.parseObj(NativeBridge.nativeHttpDispatch(1, path, null, body))
            ?: throw ApiException(StatusCode.SC_SERVICE_UNAVAILABLE, "not ready")
        val status = envelope.optInt("status", 200)
        val responseBody = envelope.optString("body", "")
        if (status >= 400) {
            val message = JsonUtil.parseObj(responseBody)?.optString("error")
                ?.takeIf { it.isNotEmpty() } ?: "request failed"
            throw ApiException(status, message)
        }
        return responseBody
    }
}
