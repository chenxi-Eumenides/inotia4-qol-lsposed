package com.inotia4.qol.controller

import com.inotia4.qol.service.ApiServices
import com.inotia4.qol.util.ControllerGuard
import com.yanzhenjie.andserver.annotation.GetMapping
import com.yanzhenjie.andserver.annotation.PathVariable
import com.yanzhenjie.andserver.annotation.RequestParam
import com.yanzhenjie.andserver.annotation.RestController

/**
 * 当前地图信息：/api/world/map 子端点（api-reference §3.1）。简单子端点。
 * 路由统一全路径写法（无类级 RequestMapping，见 architecture §3）。
 */
@RestController
class CurrentMapController {

    @GetMapping("/api/world/map/id")
    fun id(): String = ControllerGuard.guard(ApiServices.info::currentMapId)

    @GetMapping("/api/world/map/exits")
    fun exits(): String = ControllerGuard.guard(ApiServices.info::currentMapExits)

    @GetMapping("/api/world/map/units")
    fun units(): String = ControllerGuard.guard(ApiServices.info::currentMapUnits)

    @GetMapping("/api/world/map/enemies")
    fun enemies(): String = ControllerGuard.guard(ApiServices.info::currentMapEnemies)

    @GetMapping("/api/world/map/interactives")
    fun interactives(): String = ControllerGuard.guard(ApiServices.info::currentMapInteractives)

    @GetMapping("/api/world/map/drops")
    fun drops(): String = ControllerGuard.guard(ApiServices.info::currentMapDrops)

    @GetMapping("/api/world/map/distance")
    fun distance(@RequestParam("tx") tx: Int, @RequestParam("ty") ty: Int): String =
        ControllerGuard.guard { ApiServices.info.currentMapDistance(tx, ty) }
}
