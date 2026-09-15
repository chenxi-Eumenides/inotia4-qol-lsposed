package com.inotia4.qol.service

import com.inotia4.qol.LogDomain
import com.inotia4.qol.LogFile
import com.inotia4.qol.ModuleConfig
import com.inotia4.qol.NativeBridge
import com.inotia4.qol.util.ApiException
import com.inotia4.qol.util.JsonUtil
import com.yanzhenjie.andserver.http.StatusCode

/**
 * OP 直写服务层接口（v0.5.46 P1 收边）：OP 端点统一入口。
 * 10 个方法对应 OpController 10 个已实现端点（6 个路由迁移自 CharacterController + 4 个 v0.5.47 接线），
 * 参数解析留在 controller，native 调用 + LogFile.op 记录在此。
 * 与 ActionApiService 同一形态：controller 只做路由与参数解析，业务走 service。
 */
interface OpApiService {
    fun setHp(role: Int, hp: Int): String
    fun setMp(role: Int, mp: Int): String
    fun setExperience(role: Int, exp: Long): String
    fun setLevel(role: Int, level: Int, force: Boolean): String
    fun setAttr(role: Int, stats: List<Pair<Int, Int>>): String
    fun addItem(category: Int, count: Int, socket: Int, enhanceRemaining: Int, rarity: Int): String
    fun setMoney(money: Long): String
    fun setStatusPoint(role: Int, points: Int): String
    fun partySwap(a: Int, b: Int): String
    fun teleport(mapId: Int, x: Int, y: Int): String
}

/**
 * OP 直写服务实现（OpApiService 唯一实现）。
 * 每个操作经 LogFile.op 统一记录（端点/参数/结果/耗时），端点路径与 controller @PostMapping 一一对应。
 * 所有方法入口统一门禁（architecture §9.1-2，v0.5.47）：[ModuleConfig.opEnabled] 未开启 → 403 op disabled。
 * 门禁在 service 层（controller 不持有 NativeBridge 引用，无法绕过）。
 */
class OpApiServiceImpl : OpApiService {

    /** OP 全局门禁：未开启抛 403（由 GlobalExceptionResolver 转 {"ok":false,"error":"op disabled"}） */
    private fun checkOpEnabled() {
        if (!ModuleConfig.opEnabled) throw ApiException(StatusCode.SC_FORBIDDEN, "op disabled")
    }

    override fun setHp(role: Int, hp: Int): String {
        checkOpEnabled()
        return LogFile.op(LogDomain.OP, "POST /api/op/character/{role}/hp", mapOf("role" to "$role", "hp" to "$hp")) {
            NativeBridge.nativeOpSetHp(role, hp)
        }
    }

    override fun setMp(role: Int, mp: Int): String {
        checkOpEnabled()
        return LogFile.op(LogDomain.OP, "POST /api/op/character/{role}/mp", mapOf("role" to "$role", "mp" to "$mp")) {
            NativeBridge.nativeOpSetMp(role, mp)
        }
    }

    override fun setExperience(role: Int, exp: Long): String {
        checkOpEnabled()
        return LogFile.op(LogDomain.OP, "POST /api/op/character/{role}/experience", mapOf("role" to "$role", "exp" to "$exp")) {
            NativeBridge.nativeOpSetExperience(role, exp)
        }
    }

    override fun setLevel(role: Int, level: Int, force: Boolean): String {
        checkOpEnabled()
        return LogFile.op(LogDomain.OP, "POST /api/op/character/{role}/level", mapOf("role" to "$role", "level" to "$level", "force" to "$force")) {
            NativeBridge.nativeOpSetLevel(role, level, force)
        }
    }

    // 批量设置基础属性（骰子 SetStatBase 路径）：stats 为属性索引/名字→值列表，循环调用 native，
    // 任一点失败即中断返回错误（批量循环逻辑从 controller 移入 impl，v0.5.46）
    override fun setAttr(role: Int, stats: List<Pair<Int, Int>>): String {
        checkOpEnabled()
        return LogFile.op(LogDomain.OP, "POST /api/op/character/{role}/set_attr", mapOf("role" to "$role", "stats" to "$stats")) {
            val sb = StringBuilder("[")
            for ((idx, v) in stats) {
                val r = NativeBridge.nativeOpSetAttr(role, idx, v)
                if (r.contains("\"ok\":false")) return@op JsonUtil.err("set attr $idx failed", 500)
                if (sb.length > 1) sb.append(',')
                sb.append("{\"attr\":$idx,\"value\":$v}")
            }
            sb.append(']')
            "{\"ok\":true,\"set\":$sb}"
        }
    }

    // socket：总孔数（I_SOCKET bits4-7）；enhanceRemaining：剩余强化次数（I_ENCHANT bits2-5）；
    // 缺省 0（CreateItem 产物恒 0，写 0 等价不写），钳制在 native 侧；
    // 已镶嵌数/已强化次数/强化 ID 由游戏维护，不经本接口设置（用户裁决 2026-09-15）；
    // rarity：品质档位 0..4 白绿蓝黄紫（缺省 -1 保留随机），同样钳制在 native 侧
    override fun addItem(category: Int, count: Int, socket: Int, enhanceRemaining: Int, rarity: Int): String {
        checkOpEnabled()
        return LogFile.op(LogDomain.OP, "POST /api/op/inventory/add", mapOf("category" to "$category", "count" to "$count", "socket" to "$socket", "enhanceRemaining" to "$enhanceRemaining", "rarity" to "$rarity")) {
            NativeBridge.nativeOpAddItem(category, count, socket, enhanceRemaining, rarity)
        }
    }

    // ---- v0.5.47 接线 4 端点（D4 用户裁决，native 已备） ----

    override fun setMoney(money: Long): String {
        checkOpEnabled()
        return LogFile.op(LogDomain.OP, "POST /api/op/inventory/money", mapOf("money" to "$money")) {
            NativeBridge.nativeOpSetMoney(money)
        }
    }

    override fun setStatusPoint(role: Int, points: Int): String {
        checkOpEnabled()
        return LogFile.op(LogDomain.OP, "POST /api/op/character/{role}/status-point", mapOf("role" to "$role", "points" to "$points")) {
            NativeBridge.nativeOpSetStatusPoint(role, points)
        }
    }

    override fun partySwap(a: Int, b: Int): String {
        checkOpEnabled()
        return LogFile.op(LogDomain.OP, "POST /api/op/party/swap", mapOf("a" to "$a", "b" to "$b")) {
            NativeBridge.nativeOpPartySwap(a, b)
        }
    }

    override fun teleport(mapId: Int, x: Int, y: Int): String {
        checkOpEnabled()
        return LogFile.op(LogDomain.OP, "POST /api/op/movement/teleport", mapOf("mapId" to "$mapId", "x" to "$x", "y" to "$y")) {
            NativeBridge.nativeOpTeleport(mapId, x, y)
        }
    }
}
