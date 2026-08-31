package com.inotia4.qol.util

import com.yanzhenjie.andserver.annotation.Resolver
import com.yanzhenjie.andserver.error.HttpException
import com.yanzhenjie.andserver.error.ParamValidateException
import com.yanzhenjie.andserver.framework.ExceptionResolver
import com.yanzhenjie.andserver.framework.body.JsonBody
import com.yanzhenjie.andserver.http.HttpRequest
import com.yanzhenjie.andserver.http.HttpResponse
import com.yanzhenjie.andserver.http.StatusCode

/**
 * AndServer 全局异常处理（@Resolver 注册，kapt 处理器自动装配，v0.5.45）。
 * 统一语义：
 * - [ParamValidateException]（PathVariable/RequestParam 解析失败，框架默认 403）→ 400
 * - [HttpException]（含 [ApiException]）→ 其携带状态码
 * - 其他异常 → 500
 * 响应体统一格式 A JSON 信封，不泄漏原始异常串。
 */
@Resolver
class GlobalExceptionResolver : ExceptionResolver {

    override fun onResolve(request: HttpRequest, response: HttpResponse, e: Throwable) {
        val (code, msg) = when (e) {
            is ParamValidateException -> StatusCode.SC_BAD_REQUEST to "invalid parameter"
            is HttpException -> e.statusCode to (e.message ?: "error")
            else -> StatusCode.SC_INTERNAL_SERVER_ERROR to "internal error"
        }
        response.setStatus(code)
        response.setBody(JsonBody(JsonUtil.err(msg, code)))
    }
}
