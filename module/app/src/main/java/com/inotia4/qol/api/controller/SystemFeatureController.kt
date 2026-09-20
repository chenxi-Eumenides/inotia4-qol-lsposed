package com.inotia4.qol.api.controller

import com.inotia4.qol.NativeBridge
import com.inotia4.qol.service.ApiServices
import com.inotia4.qol.util.ControllerGuard
import com.yanzhenjie.andserver.annotation.GetMapping
import com.yanzhenjie.andserver.annotation.PathVariable
import com.yanzhenjie.andserver.annotation.RestController

/**
 * 系统功能诊断：/api/system/features（api-reference §7.7）、/api/system/resources（§7.8）、
 * /api/system/tickets/{id}（§7.9）。路由统一全路径写法（无类级 RequestMapping，见 architecture §3）。
 */
@RestController
class SystemFeatureController {

    @GetMapping("/api/system/features")
    fun features(): String = ControllerGuard.guard(ApiServices.info::featureStatusJson)

    @GetMapping("/api/system/resources")
    fun resources(): String = ControllerGuard.guard(ApiServices.info::systemResourcesJson)

    @GetMapping("/api/system/tickets/{id}")
    fun ticket(@PathVariable("id") id: Long): String =
        ControllerGuard.guard { ApiServices.info.ticketStateJson(id) }

    @GetMapping("/api/system/routes")
    fun routes(): String = ControllerGuard.guard { NativeBridge.nativeHttpRoutesJson() }
}
