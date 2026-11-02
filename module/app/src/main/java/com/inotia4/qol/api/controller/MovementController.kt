package com.inotia4.qol.api.controller

import com.inotia4.qol.service.info.NativeHttpCommand
import com.inotia4.qol.util.ApiException
import com.inotia4.qol.util.JsonUtil
import com.yanzhenjie.andserver.annotation.PostMapping
import com.yanzhenjie.andserver.annotation.RequestBody
import com.yanzhenjie.andserver.annotation.RestController
import com.yanzhenjie.andserver.http.StatusCode

// controller: 路由层。世界写路由由 world_teleport owner 承接（票据契约：POST 返回票据，原结果经
// GET /api/system/tickets/{id} 回取）。Kotlin 只做 body 语法校验与透传（字段范围校验在 owner），
// API 坐标为瓦片索引、native 收像素（tile*16+8）的换算也在 owner 内完成。
@RestController
class MovementController {

    @PostMapping("/api/world/movement/move_to")
    fun move(@RequestBody body: String): String {
        val o = JsonUtil.parseBody(body) ?: throw ApiException(StatusCode.SC_BAD_REQUEST, "bad request")
        return NativeHttpCommand.post("/api/world/movement/move_to", o.toString())
    }

    @PostMapping("/api/world/movement/walk_dir")
    fun walk(@RequestBody body: String): String {
        val o = JsonUtil.parseBody(body) ?: throw ApiException(StatusCode.SC_BAD_REQUEST, "bad request")
        return NativeHttpCommand.post("/api/world/movement/walk_dir", o.toString())
    }

    @PostMapping("/api/world/movement/stop_move")
    fun stop(): String = NativeHttpCommand.post("/api/world/movement/stop_move", "{}")

    @PostMapping("/api/world/movement/interact_with")
    fun interact(): String = NativeHttpCommand.post("/api/world/movement/interact_with", "{}")
}
