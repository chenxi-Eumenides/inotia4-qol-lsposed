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
// OP 定稿端点（api-reference §8）：10 个已实现端点（6 个 v0.5.46 迁移自 CharacterController + 4 个 v0.5.47 接线）
// + 11 个占位端点（结构已定稿，待权限机制与底层实现，占位返回 not implemented）
@RestController
class OpController {

    private fun notImpl(): Nothing = throw ApiException(StatusCode.SC_NOT_IMPLEMENTED, "not implemented")

    @PostMapping("/api/op/quest/accept")
    fun questAccept(): String = LogFile.op("POST /api/op/quest/accept", "") { notImpl() }

    @PostMapping("/api/op/quest/complete")
    fun questComplete(): String = LogFile.op("POST /api/op/quest/complete", "") { notImpl() }

    @PostMapping("/api/op/character/{role}/status-point")
    fun statusPoint(@PathVariable("role") role: Int, @RequestBody body: String): String {
        val o = JsonUtil.parseBody(body) ?: throw ApiException(StatusCode.SC_BAD_REQUEST, "bad request")
        val points = o.optInt("points", -1)
        if (points < 0) throw ApiException(StatusCode.SC_BAD_REQUEST, "points required")
        return ControllerGuard.guard { ApiServices.op.setStatusPoint(role, points) }
    }

    @PostMapping("/api/op/character/{role}/skill-point")
    fun skillPoint(@PathVariable("role") role: Int): String =
        LogFile.op("POST /api/op/character/{role}/skill-point", "role=$role") { notImpl() }

    @PostMapping("/api/op/character/{role}/skill-level")
    fun skillLevel(@PathVariable("role") role: Int): String =
        LogFile.op("POST /api/op/character/{role}/skill-level", "role=$role") { notImpl() }

    @PostMapping("/api/op/party/swap")
    fun partySwap(@RequestBody body: String): String {
        val o = JsonUtil.parseBody(body) ?: throw ApiException(StatusCode.SC_BAD_REQUEST, "bad request")
        if (!o.has("a") || !o.has("b")) throw ApiException(StatusCode.SC_BAD_REQUEST, "a and b required")
        val a = o.optInt("a", -1)
        val b = o.optInt("b", -1)
        if (a < 0 || b < 0) throw ApiException(StatusCode.SC_BAD_REQUEST, "a and b required")
        return ControllerGuard.guard { ApiServices.op.partySwap(a, b) }
    }

    @PostMapping("/api/op/inventory/set-slot")
    fun setSlot(): String = LogFile.op("POST /api/op/inventory/set-slot", "") { notImpl() }

    @PostMapping("/api/op/inventory/set-equip")
    fun setEquip(): String = LogFile.op("POST /api/op/inventory/set-equip", "") { notImpl() }

    @PostMapping("/api/op/inventory/money")
    fun money(@RequestBody body: String): String {
        val o = JsonUtil.parseBody(body) ?: throw ApiException(StatusCode.SC_BAD_REQUEST, "bad request")
        val m = o.optLong("money", -1)
        if (m < 0) throw ApiException(StatusCode.SC_BAD_REQUEST, "money required")
        return ControllerGuard.guard { ApiServices.op.setMoney(m) }
    }

    @PostMapping("/api/op/craft/mix-direct")
    fun mixDirect(): String = LogFile.op("POST /api/op/craft/mix-direct", "") { notImpl() }

    @PostMapping("/api/op/combat/{role}/heal")
    fun heal(@PathVariable("role") role: Int): String =
        LogFile.op("POST /api/op/combat/{role}/heal", "role=$role") { notImpl() }

    @PostMapping("/api/op/combat/{role}/rest")
    fun rest(@PathVariable("role") role: Int): String =
        LogFile.op("POST /api/op/combat/{role}/rest", "role=$role") { notImpl() }

    @PostMapping("/api/op/combat/{role}/revive")
    fun revive(@PathVariable("role") role: Int): String =
        LogFile.op("POST /api/op/combat/{role}/revive", "role=$role") { notImpl() }

    @PostMapping("/api/op/combat/{role}/hate")
    fun hate(@PathVariable("role") role: Int): String =
        LogFile.op("POST /api/op/combat/{role}/hate", "role=$role") { notImpl() }

    @PostMapping("/api/op/movement/teleport")
    fun teleport(@RequestBody body: String): String {
        val o = JsonUtil.parseBody(body) ?: throw ApiException(StatusCode.SC_BAD_REQUEST, "bad request")
        if (!o.has("map_id") || !o.has("x") || !o.has("y")) {
            throw ApiException(StatusCode.SC_BAD_REQUEST, "map_id/x/y required")
        }
        val mapId = o.optInt("map_id", -1)
        val x = o.optInt("x", -1)
        val y = o.optInt("y", -1)
        if (mapId < 0 || x < 0 || y < 0) throw ApiException(StatusCode.SC_BAD_REQUEST, "map_id/x/y required")
        return ControllerGuard.guard { ApiServices.op.teleport(mapId, x, y) }
    }

    // ---- 已实现 OP 端点（v0.5.46 迁移自 CharacterController，路由全路径注解逐字保留） ----

    @PostMapping("/api/op/character/{role}/hp")
    fun opSetHp(@PathVariable("role") role: Int, @RequestBody body: String): String {
        val o = JsonUtil.parseBody(body) ?: throw ApiException(StatusCode.SC_BAD_REQUEST, "bad request")
        val hp = o.optInt("hp", -1)
        if (hp < 0) throw ApiException(StatusCode.SC_BAD_REQUEST, "hp required")
        return ControllerGuard.guard { ApiServices.op.setHp(role, hp) }
    }

    @PostMapping("/api/op/character/{role}/mp")
    fun opSetMp(@PathVariable("role") role: Int, @RequestBody body: String): String {
        val o = JsonUtil.parseBody(body) ?: throw ApiException(StatusCode.SC_BAD_REQUEST, "bad request")
        val mp = o.optInt("mp", -1)
        if (mp < 0) throw ApiException(StatusCode.SC_BAD_REQUEST, "mp required")
        return ControllerGuard.guard { ApiServices.op.setMp(role, mp) }
    }

    @PostMapping("/api/op/character/{role}/experience")
    fun opSetExp(@PathVariable("role") role: Int, @RequestBody body: String): String {
        val o = JsonUtil.parseBody(body) ?: throw ApiException(StatusCode.SC_BAD_REQUEST, "bad request")
        val exp = o.optLong("exp", -1)
        if (exp < 0) throw ApiException(StatusCode.SC_BAD_REQUEST, "exp required")
        return ControllerGuard.guard { ApiServices.op.setExperience(role, exp) }
    }

    @PostMapping("/api/op/character/{role}/level")
    fun opSetLevel(@PathVariable("role") role: Int, @RequestBody body: String): String {
        val o = JsonUtil.parseBody(body) ?: throw ApiException(StatusCode.SC_BAD_REQUEST, "bad request")
        if (!o.has("level")) throw ApiException(StatusCode.SC_BAD_REQUEST, "level required")
        val level = o.optInt("level", 0)
        val force = o.optBoolean("force", false)
        if (!force && (level < 1 || level > 105)) throw ApiException(StatusCode.SC_BAD_REQUEST, "level 1-105 (game max); force=true 跳过限制")
        return ControllerGuard.guard { ApiServices.op.setLevel(role, level, force) }
    }

    @PostMapping("/api/op/character/{role}/set_attr")
    fun opSetAttr(@PathVariable("role") role: Int, @RequestBody body: String): String {
        val o = JsonUtil.parseBody(body) ?: throw ApiException(StatusCode.SC_BAD_REQUEST, "bad request")
        // 批量设置基础属性（骰子 SetStatBase 路径）：{"stats": {"strength":10,"agility":7}} 或 {"stats":{"0":10,"3":7}}，可只传部分
        val stats = o.optJSONObject("stats") ?: throw ApiException(StatusCode.SC_BAD_REQUEST, "stats required (object: 属性名/索引 → 值)")
        val mainNames = listOf("strength", "agility", "vitality", "intelligence", "spirit")
        val pairs = mutableListOf<Pair<Int, Int>>()
        val keys = stats.keys()
        while (keys.hasNext()) {
            val k = keys.next()
            val idx = when (k) {
                "0", "1", "2", "3", "4" -> k.toInt()
                else -> mainNames.indexOf(k)
            }
            if (idx < 0) throw ApiException(StatusCode.SC_BAD_REQUEST, "bad attr: $k (0-4 或 strength/agility/vitality/intelligence/spirit)")
            val v = stats.optInt(k, -1)
            if (v < 0 || v > 255) throw ApiException(StatusCode.SC_BAD_REQUEST, "bad value for $k (0-255)")
            pairs.add(idx to v)
        }
        if (pairs.isEmpty()) throw ApiException(StatusCode.SC_BAD_REQUEST, "stats empty")
        return ControllerGuard.guard { ApiServices.op.setAttr(role, pairs) }
    }

    @PostMapping("/api/op/inventory/add")
    fun opAddItem(@RequestBody body: String): String {
        val o = JsonUtil.parseBody(body) ?: throw ApiException(StatusCode.SC_BAD_REQUEST, "bad request")
        val category = o.optInt("category", -1)
        val count = o.optInt("count", 1)
        if (category < 0) throw ApiException(StatusCode.SC_BAD_REQUEST, "category required")
        return ControllerGuard.guard { ApiServices.op.addItem(category, count) }
    }
}
