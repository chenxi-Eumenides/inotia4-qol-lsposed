package com.inotia4.qol.controller

import com.inotia4.qol.service.ApiServices
import com.inotia4.qol.util.ControllerGuard
import com.yanzhenjie.andserver.annotation.GetMapping
import com.yanzhenjie.andserver.annotation.RestController

/**
 * 服务健康：/api/health（顶层，api-reference §7.0）。返回服务运行状态。
 * 路由统一全路径写法（无类级 RequestMapping，见 architecture §3）。
 */
@RestController
class HealthController {

    @GetMapping("/api/health")
    fun health(): String = ControllerGuard.guard(ApiServices.info::health)
}
