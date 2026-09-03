package com.inotia4.qol.api.controller

import com.inotia4.qol.LogFile
import com.inotia4.qol.service.ApiServices
import com.inotia4.qol.util.ApiException
import com.inotia4.qol.util.ControllerGuard
import com.inotia4.qol.util.JsonUtil
import com.yanzhenjie.andserver.annotation.PathVariable
import com.yanzhenjie.andserver.annotation.PostMapping
import com.yanzhenjie.andserver.annotation.RequestBody
import com.yanzhenjie.andserver.annotation.RestController
import com.yanzhenjie.andserver.http.StatusCode

// controller: 路由层，业务走 ApiServices。路径首段必须静态（AndServer 处理器约束，architecture §3）
@RestController
class CombatController {

    @PostMapping("/api/character/combat/{role}/set_auto_attack")
    fun autoAttack(@PathVariable("role") role: Int, @RequestBody body: String): String {
        val o = JsonUtil.parseBody(body) ?: throw ApiException(StatusCode.SC_BAD_REQUEST, "bad request")
        if (!o.has("on")) throw ApiException(StatusCode.SC_BAD_REQUEST, "on required")
        return ControllerGuard.guard { ApiServices.action.autoAttack(role, o.optBoolean("on")) }
    }

    // ⏳ 占位：文档请求 {action_id, mode:off/normal/high}（单技能 AI 档位），native 仅支持全局布尔开关，
    // 单技能档位编码（技能链表节点 +0x07）缺失前置探索，暂返回 not implemented
    @PostMapping("/api/character/combat/{role}/set_skill_usage")
    fun skillUsage(@PathVariable("role") role: Int, @RequestBody body: String): String =
        LogFile.op("POST /api/character/combat/{role}/set_skill_usage", "role=$role") {
            throw ApiException(StatusCode.SC_NOT_IMPLEMENTED, "not implemented")
        }

    @PostMapping("/api/character/combat/switch_player")
    fun switchPlayer(@RequestBody body: String): String {
        val o = JsonUtil.parseBody(body) ?: throw ApiException(StatusCode.SC_BAD_REQUEST, "bad request")
        val slot = o.optInt("slot", -1)
        if (slot < 0 || slot > 2) throw ApiException(StatusCode.SC_BAD_REQUEST, "slot required (0-2)")
        return ControllerGuard.guard { ApiServices.action.switchPlayer(slot) }
    }

    @PostMapping("/api/character/combat/{role}/cast_skill")
    fun cast(@PathVariable("role") role: Int, @RequestBody body: String): String {
        val o = JsonUtil.parseBody(body) ?: throw ApiException(StatusCode.SC_BAD_REQUEST, "bad request")
        val actionId = o.optInt("action_id", -1)
        if (actionId < 0) throw ApiException(StatusCode.SC_BAD_REQUEST, "action_id required")
        return ControllerGuard.guard { ApiServices.action.cast(role, actionId) }
    }

    @PostMapping("/api/character/combat/{role}/attack_target")
    fun attack(@PathVariable("role") role: Int, @RequestBody body: String): String {
        val o = JsonUtil.parseBody(body) ?: throw ApiException(StatusCode.SC_BAD_REQUEST, "bad request")
        val targetSlot = o.optInt("target_slot", -1)
        if (targetSlot < 0) throw ApiException(StatusCode.SC_BAD_REQUEST, "target_slot required")
        return ControllerGuard.guard { ApiServices.action.attack(role, targetSlot) }
    }

    @PostMapping("/api/character/combat/{role}/stop_combat")
    fun stop(@PathVariable("role") role: Int): String =
        ControllerGuard.guard { ApiServices.action.stopCombat(role) }
}
