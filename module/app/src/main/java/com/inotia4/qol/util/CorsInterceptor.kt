package com.inotia4.qol.util

import com.yanzhenjie.andserver.annotation.Interceptor
import com.yanzhenjie.andserver.framework.HandlerInterceptor
import com.yanzhenjie.andserver.framework.handler.RequestHandler
import com.yanzhenjie.andserver.http.HttpHeaders
import com.yanzhenjie.andserver.http.HttpMethod
import com.yanzhenjie.andserver.http.HttpRequest
import com.yanzhenjie.andserver.http.HttpResponse

/**
 * 全局 CORS 拦截器：浏览器跨域访问 API 所需（backlog P0 #7）。
 *
 * - 普通请求：回显请求 Origin 设置 `Access-Control-Allow-Origin`（无 Origin 时用 `*`），
 *   并带 `Vary: Origin` 保证代理缓存正确。
 * - OPTIONS 预检：返回 200 + 允许方法/请求头/预检缓存时间，短路不再走后续 handler。
 *
 * 经 `@Interceptor` 注解由 AndServer annotation processor 编译期自动注册，无需改动 ApiServer。
 * 仅影响 HTTP 响应头，不涉及 native 层与业务逻辑。
 */
@Interceptor
class CorsInterceptor : HandlerInterceptor {

    override fun onIntercept(request: HttpRequest, response: HttpResponse, handler: RequestHandler): Boolean {
        val origin = request.getHeader(HttpHeaders.ORIGIN)
        response.setHeader(
            HttpHeaders.Access_Control_Allow_Origin,
            if (origin.isNullOrEmpty()) "*" else origin
        )
        if (!origin.isNullOrEmpty()) {
            response.setHeader(HttpHeaders.VARY, HttpHeaders.ORIGIN)
        }

        if (request.method == HttpMethod.OPTIONS) {
            response.setHeader(HttpHeaders.Access_Control_Allow_Methods, "GET, POST, PUT, DELETE, OPTIONS")
            response.setHeader(HttpHeaders.Access_Control_Allow_Headers, "*")
            response.setHeader(HttpHeaders.Access_Control_Max_Age, "3600")
            response.setStatus(200)
            return true
        }
        return false
    }
}
