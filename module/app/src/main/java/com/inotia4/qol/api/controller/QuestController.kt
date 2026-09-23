package com.inotia4.qol.api.controller

import com.inotia4.qol.service.ApiServices
import com.inotia4.qol.service.info.NativeHttpCommand
import com.inotia4.qol.util.ApiException
import com.inotia4.qol.util.ControllerGuard
import com.inotia4.qol.util.JsonUtil
import com.yanzhenjie.andserver.annotation.GetMapping
import com.yanzhenjie.andserver.annotation.PathVariable
import com.yanzhenjie.andserver.annotation.PostMapping
import com.yanzhenjie.andserver.annotation.RequestBody
import com.yanzhenjie.andserver.annotation.RestController
import com.yanzhenjie.andserver.http.StatusCode
import org.json.JSONObject

/**
 * 任务：/api/quest（api-reference §0.2）。复合 + active/list/{id}/completed。
 * 方法级路径首段必须静态（AndServer 处理器约束，architecture §3）。
 */
@RestController
class QuestController {

    @GetMapping("/api/quest")
        fun composite(): String = ControllerGuard.guard(ApiServices.info::quest)

    @GetMapping("/api/quest/active")
    fun active(): String = ControllerGuard.guard(ApiServices.info::questActive)

    @GetMapping("/api/quest/details")
    fun details(): String = ControllerGuard.guard(ApiServices.info::questDetails)

    // 写路由（阶段 5 批 5-3）：与同层的 {id} 通配放在同一控制器并声明在通配之前，
    // 避免跨 adapter 的通配遮蔽（quit_quest 曾被 {id} 吞掉返回 405）。
    @PostMapping("/api/quest/quit_quest")
    fun quit(@RequestBody body: String): String {
        val o = JsonUtil.parseBody(body) ?: throw ApiException(StatusCode.SC_BAD_REQUEST, "bad request")
        val questId = o.optInt("quest_id", -1)
        if (questId < 0) throw ApiException(StatusCode.SC_BAD_REQUEST, "questId required")
        return ControllerGuard.guard {
            NativeHttpCommand.post("/api/quest/quit_quest", JSONObject().put("quest_id", questId).toString())
        }
    }

    @GetMapping("/api/quest/{id}")
    fun questId(@PathVariable("id") id: Int): String =
        ControllerGuard.guard { ApiServices.info.questListId(id) }

    @GetMapping("/api/quest/completed")
    fun completed(): String = ControllerGuard.guard(ApiServices.info::questCompleted)
}
