package com.inotia4.qol.controller

import com.inotia4.qol.service.ApiServices
import com.inotia4.qol.util.ControllerGuard
import com.yanzhenjie.andserver.annotation.GetMapping
import com.yanzhenjie.andserver.annotation.RestController

/**
 * 游戏整体：/api/system/game + /api/system/snapshot（api-reference §7.1）。
 * 路由统一全路径写法（无类级 RequestMapping，见 architecture §3）。
 */
@RestController
class GameController {

    @GetMapping("/api/system/game")
    fun composite(): String = ControllerGuard.guard(ApiServices.info::game)

    @GetMapping("/api/system/snapshot")
    fun snapshot(): String = ControllerGuard.guard(ApiServices.info::gameSnapshot)

    @GetMapping("/api/system/info")
    fun info(): String = ControllerGuard.guard(ApiServices.info::gameInfo)

    @GetMapping("/api/system/game_frame")
    fun frame(): String = ControllerGuard.guard(ApiServices.info::gameFrame)
}
