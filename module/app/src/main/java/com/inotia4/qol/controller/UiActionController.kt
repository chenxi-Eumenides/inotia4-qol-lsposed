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
class UiActionController {

    @PostMapping("/api/ui/go_main_menu")
    fun mainMenu(): String = ControllerGuard.guard { ApiServices.action.mainMenu() }

    @PostMapping("/api/ui/close_panel")
    fun panelClose(): String = ControllerGuard.guard { ApiServices.action.panelClose() }

    @PostMapping("/api/ui/open_panel")
    fun panelOpen(@RequestBody body: String): String {
        val panel = JsonUtil.parseBody(body)?.optString("panel", "") ?: ""
        if (panel.isBlank()) throw ApiException(StatusCode.SC_BAD_REQUEST, "panel required")
        return ControllerGuard.guard { ApiServices.action.panelOpen(panel) }
    }

    // v0.5.46：NpcController 并入（统一对话端点，v0.4.27 重构自 v0.4.13 npc 三件套）：
    //   interact（开始交互）→ content（获取对话内容+选项）→ select（选择选项）
    // 一套 API 覆盖：剧情对话（AVG）/ NPC 对话 / 任务框 / 弹窗；路由全路径注解逐字保留

    @PostMapping("/api/ui/start_interact")
    fun interact(): String = ControllerGuard.guard { ApiServices.action.npcInteract() }

    @PostMapping("/api/ui/dialog/select")
    fun select(@RequestBody body: String): String {
        val o = JsonUtil.parseBody(body) ?: throw ApiException(StatusCode.SC_BAD_REQUEST, "bad request")
        val action = o.optString("action", "")
        val index = o.optInt("index", -1)
        if (action.isEmpty() && index < 0) throw ApiException(StatusCode.SC_BAD_REQUEST, "action or index required")
        return ControllerGuard.guard { ApiServices.action.dialogSelect(action, index) }
    }
}
