package com.inotia4.qol.api.controller

import com.inotia4.qol.service.ApiServices
import com.inotia4.qol.util.ControllerGuard
import com.yanzhenjie.andserver.annotation.GetMapping
import com.yanzhenjie.andserver.annotation.RestController

/**
 * 系统功能诊断：/api/system/features（api-reference §7.7）。
 * 路由统一全路径写法（无类级 RequestMapping，见 architecture §3）。
 */
@RestController
class SystemFeatureController {

    @GetMapping("/api/system/features")
    fun features(): String = ControllerGuard.guard(ApiServices.info::featureStatusJson)
}
