package com.inotia4.qol.api.controller

import com.inotia4.qol.service.info.NativeHttpCommand
import com.inotia4.qol.util.ApiException
import com.inotia4.qol.util.JsonUtil
import com.yanzhenjie.andserver.annotation.PathVariable
import com.yanzhenjie.andserver.annotation.PostMapping
import com.yanzhenjie.andserver.annotation.RequestBody
import com.yanzhenjie.andserver.annotation.RestController
import com.yanzhenjie.andserver.http.StatusCode

// 写请求先保留兼容校验，再由 NativeHttpCommand 透传给领域 owner。
@RestController
class CombatController {

    @PostMapping("/api/character/combat/{role}/set_auto_attack")
    fun autoAttack(@PathVariable("role") role: Int, @RequestBody body: String): String {
        val o = JsonUtil.parseBody(body) ?: throw ApiException(StatusCode.SC_BAD_REQUEST, "bad request")
        if (!o.has("on")) throw ApiException(StatusCode.SC_BAD_REQUEST, "on required")
        return NativeHttpCommand.post("/api/character/combat/$role/set_auto_attack", body)
    }

    // 单技能档位尚未实现；由临时 core.feature owner 返回兼容的 501 占位响应。
    @PostMapping("/api/character/combat/{role}/set_skill_usage")
    fun skillUsage(@PathVariable("role") role: Int, @RequestBody body: String): String =
        NativeHttpCommand.post("/api/character/combat/$role/set_skill_usage", body)

    @PostMapping("/api/character/combat/switch_player")
    fun switchPlayer(@RequestBody body: String): String {
        val o = JsonUtil.parseBody(body) ?: throw ApiException(StatusCode.SC_BAD_REQUEST, "bad request")
        val slot = o.optInt("slot", -1)
        if (slot < 0 || slot > 2) throw ApiException(StatusCode.SC_BAD_REQUEST, "slot required (0-2)")
        return NativeHttpCommand.post("/api/character/combat/switch_player", body)
    }

    @PostMapping("/api/character/combat/{role}/cast_skill")
    fun cast(@PathVariable("role") role: Int, @RequestBody body: String): String {
        val o = JsonUtil.parseBody(body) ?: throw ApiException(StatusCode.SC_BAD_REQUEST, "bad request")
        val actionId = o.optInt("action_id", -1)
        if (actionId < 0) throw ApiException(StatusCode.SC_BAD_REQUEST, "action_id required")
        return NativeHttpCommand.post("/api/character/combat/$role/cast_skill", body)
    }

    @PostMapping("/api/character/combat/{role}/attack_target")
    fun attack(@PathVariable("role") role: Int, @RequestBody body: String): String {
        val o = JsonUtil.parseBody(body) ?: throw ApiException(StatusCode.SC_BAD_REQUEST, "bad request")
        val targetSlot = o.optInt("target_slot", -1)
        if (targetSlot < 0) throw ApiException(StatusCode.SC_BAD_REQUEST, "target_slot required")
        return NativeHttpCommand.post("/api/character/combat/$role/attack_target", body)
    }

    @PostMapping("/api/character/combat/{role}/stop_combat")
    fun stop(@PathVariable("role") role: Int): String =
        NativeHttpCommand.post("/api/character/combat/$role/stop_combat", "{}")
}
