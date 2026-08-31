package com.inotia4.qol.controller

import com.inotia4.qol.service.ApiServices
import com.inotia4.qol.util.ApiException
import com.inotia4.qol.util.ControllerGuard
import com.inotia4.qol.util.JsonUtil
import com.yanzhenjie.andserver.annotation.PostMapping
import com.yanzhenjie.andserver.annotation.RequestBody
import com.yanzhenjie.andserver.annotation.RestController
import com.yanzhenjie.andserver.http.StatusCode

// controller: 路由层，业务走 ApiServices。路径首段必须静态（AndServer 处理器约束，architecture §3）
@RestController
class MovementController {

    @PostMapping("/api/world/movement/move_to")
    fun move(@RequestBody body: String): String {
        val o = JsonUtil.parseBody(body) ?: throw ApiException(StatusCode.SC_BAD_REQUEST, "bad request")
        val x = o.optInt("x", -1)
        val y = o.optInt("y", -1)
        if (x < 0 || y < 0) throw ApiException(StatusCode.SC_BAD_REQUEST, "x/y required")
        return ControllerGuard.guard { ApiServices.action.move(x, y) }
    }

    @PostMapping("/api/world/movement/walk_dir")
    fun walk(@RequestBody body: String): String {
        val o = JsonUtil.parseBody(body) ?: throw ApiException(StatusCode.SC_BAD_REQUEST, "bad request")
        val direction = o.optInt("direction", -1)
        if (direction !in 0..3) throw ApiException(StatusCode.SC_BAD_REQUEST, "direction 0-3 required")
        return ControllerGuard.guard { ApiServices.action.walk(direction) }
    }

    @PostMapping("/api/world/movement/stop_move")
    fun stop(): String = ControllerGuard.guard { ApiServices.action.walkStop() }

    // 交互/攻击键（v0.4.41）：复现官方攻击键链（EVTSYSTEM_DoCheckAllEvent(2) 遍历事件条件触发）
    @PostMapping("/api/world/movement/interact_with")
    fun interact(): String = ControllerGuard.guard { ApiServices.action.interact() }
}
