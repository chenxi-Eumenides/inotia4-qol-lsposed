package com.inotia4.qol.service.action

import com.inotia4.qol.AgreementPopup
import com.inotia4.qol.LogDomain
import com.inotia4.qol.LogFile
import com.inotia4.qol.NativeBridge
import com.inotia4.qol.UiActivityTracker
import com.inotia4.qol.patch.AgreementGate
import com.inotia4.qol.store.ModuleSaveStore
import com.inotia4.qol.util.ApiException
import com.inotia4.qol.util.JsonUtil
import com.yanzhenjie.andserver.http.StatusCode
import org.json.JSONObject
import com.inotia4.qol.service.contract.ActionApiService

internal object CharacterActions {
    fun autoAttack(role: Int, on: Boolean): String =
        LogFile.op(LogDomain.API, "POST /api/character/combat/{role}/set_auto_attack", mapOf("role" to "$role", "on" to "$on")) {
            ActionSupport.attachParty(NativeBridge.nativeOpSetAutoAttack(role, if (on) 1 else 0))
        }
    fun learnSkill(role: Int, actionId: Int, level: Int): String =
        LogFile.op(LogDomain.API, "POST /api/character/grow/add_skill", mapOf("role" to "$role", "actionId" to "$actionId", "level" to "$level")) {
            ActionSupport.attachSkills(NativeBridge.nativeOpLearnAction(role, actionId, level))
        }
    fun addStat(role: Int, attrs: List<Pair<Int, Int>>): String =
        LogFile.op(LogDomain.API, "POST /api/character/grow/{role}/add_stat", mapOf("role" to "$role", "attrs" to "$attrs")) {
            // native 单点 +1（data_op_add_stat 检查能力点），批量按数量循环调用，任一点失败即中断返回
            val applied = mutableListOf<String>()
            for ((idx, count) in attrs) {
                for (i in 0 until count) {
                    val r = NativeBridge.nativeOpAddStat(role, idx)
                    if (r.contains("\"ok\":false")) return@op r
                    applied.add("{\"attr\":$idx}")
                }
            }
            "{\"ok\":true,\"applied\":${applied.joinToString(",")}}"
        }
    fun statReset(role: Int): String =
        LogFile.op(LogDomain.API, "POST /api/character/grow/reset_stat", mapOf("role" to "$role")) { ActionSupport.attachPlayer(NativeBridge.nativeOpStatReset(role)) }
    fun skillReset(role: Int): String =
        LogFile.op(LogDomain.API, "POST /api/character/grow/reset_skill", mapOf("role" to "$role")) { ActionSupport.attachPlayer(NativeBridge.nativeOpSkillReset(role)) }
    fun cast(role: Int, actionId: Int): String =
        LogFile.op(LogDomain.API, "POST /api/character/combat/{role}/cast_skill", mapOf("role" to "$role", "actionId" to "$actionId")) { ActionSupport.attachParty(NativeBridge.nativeOpCast(role, actionId)) }
    fun switchPlayer(slot: Int): String =
        LogFile.op(LogDomain.API, "POST /api/character/combat/switch_player", mapOf("slot" to "$slot")) { ActionSupport.attachPlayer(NativeBridge.nativeOpSwitchPlayer(slot)) }
    fun attack(role: Int, targetSlot: Int): String =
        LogFile.op(LogDomain.API, "POST /api/character/combat/{role}/attack_target", mapOf("role" to "$role", "targetSlot" to "$targetSlot")) {
            ActionSupport.attachParty(NativeBridge.nativeOpAttack(role, targetSlot))
        }
    fun stopCombat(role: Int): String =
        LogFile.op(LogDomain.API, "POST /api/character/combat/{role}/stop_combat", mapOf("role" to "$role")) {
            ActionSupport.attach(NativeBridge.nativeOpStopCombat(role)) { NativeBridge.nativeGetPlayerJson() }
        }
}
