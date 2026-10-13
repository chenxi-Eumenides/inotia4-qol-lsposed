package com.inotia4.qol.api.controller

import com.inotia4.qol.NativeBridge
import com.inotia4.qol.service.ApiServices
import com.inotia4.qol.util.ControllerGuard
import com.inotia4.qol.util.withNativeRouteLease
import com.yanzhenjie.andserver.annotation.GetMapping
import com.yanzhenjie.andserver.annotation.PathVariable
import com.yanzhenjie.andserver.annotation.RestController
import com.yanzhenjie.andserver.http.HttpRequest

/**
 * 系统功能诊断：/api/system/features（api-reference §7.7）、/api/system/resources（§7.8）、
 * /api/system/tickets/{id}（§7.9）、/api/system/routes。路由统一全路径写法
 * （无类级 RequestMapping，见 architecture §3）。
 *
 * 这些路由由 Kotlin 直接处理（无 native handler），故按 direct 路由获取 native route lease；
 * 响应体不变，门禁由 Host route metadata 决定。
 */
@RestController
class SystemFeatureController {

    @GetMapping("/api/system/features")
    fun features(request: HttpRequest): String =
        withNativeRouteLease(request) { ControllerGuard.guard(ApiServices.info::featureStatusJson) }

    @GetMapping("/api/system/resources")
    fun resources(request: HttpRequest): String =
        withNativeRouteLease(request) { ControllerGuard.guard(ApiServices.info::systemResourcesJson) }

    @GetMapping("/api/system/tickets/{id}")
    fun ticket(@PathVariable("id") id: Long, request: HttpRequest): String =
        withNativeRouteLease(request) { ControllerGuard.guard { ApiServices.info.ticketStateJson(id) } }

    @GetMapping("/api/system/routes")
    fun routes(request: HttpRequest): String =
        withNativeRouteLease(request) { ControllerGuard.guard { NativeBridge.nativeHttpRoutesJson() } }
}
