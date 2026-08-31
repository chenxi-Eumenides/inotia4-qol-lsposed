package com.inotia4.qol.util

import com.yanzhenjie.andserver.error.HttpException

/**
 * 业务异常：携带 HTTP 状态码（400/404/500/503...），由 [GlobalExceptionResolver]
 * 统一转为 `{"ok":false,"error":"..."}` JSON 信封 + 对应状态码（v0.5.45，P1）。
 * controller 错误路径 throw 本异常替代返回手写错误串。
 */
class ApiException(code: Int, msg: String, cause: Throwable? = null) :
    HttpException(code, msg, cause)
