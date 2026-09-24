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
class CharacterController {

    @PostMapping("/api/character/grow/add_skill")
    fun skill(@RequestBody body: String): String {
        val o = JsonUtil.parseBody(body) ?: throw ApiException(StatusCode.SC_BAD_REQUEST, "bad request")
        val actionId = o.optInt("action_id", -1)
        if (actionId < 0) throw ApiException(StatusCode.SC_BAD_REQUEST, "action_id required")
        val level = o.optInt("level", 1)
        return NativeHttpCommand.post("/api/character/grow/add_skill", body)
    }

    @PostMapping("/api/character/grow/{role}/add_stat")
    fun stat(@PathVariable("role") role: Int, @RequestBody body: String): String {
        val o = JsonUtil.parseBody(body) ?: throw ApiException(StatusCode.SC_BAD_REQUEST, "bad request")
        // 批量加点：{"attrs":{"strength":1,"agility":2}}，属性名英文或索引 0-4，可只传部分
        val attrs = o.optJSONObject("attrs") ?: throw ApiException(StatusCode.SC_BAD_REQUEST, "attrs required (object: strength/agility/vitality/intelligence/spirit 或 0-4 → 数量)")
        val mainNames = listOf("strength", "agility", "vitality", "intelligence", "spirit")
        val keys = attrs.keys()
        var hasAttributes = false
        while (keys.hasNext()) {
            hasAttributes = true
            val k = keys.next()
            val idx = when (k) {
                "0", "1", "2", "3", "4" -> k.toInt()
                else -> mainNames.indexOf(k)
            }
            if (idx < 0) throw ApiException(StatusCode.SC_BAD_REQUEST, "bad attr: $k (0-4 或 strength/agility/vitality/intelligence/spirit)")
            val v = attrs.optInt(k, -1)
            if (v <= 0) throw ApiException(StatusCode.SC_BAD_REQUEST, "bad value for $k (must be positive)")
        }
        if (!hasAttributes) throw ApiException(StatusCode.SC_BAD_REQUEST, "attrs empty")
        return NativeHttpCommand.post("/api/character/grow/$role/add_stat", body)
    }

    @PostMapping("/api/character/grow/reset_stat")
    fun statReset(): String = NativeHttpCommand.post("/api/character/grow/reset_stat", "{}")

    @PostMapping("/api/character/grow/reset_skill")
    fun skillReset(): String = NativeHttpCommand.post("/api/character/grow/reset_skill", "{}")
}
