package com.inotia4.export.service

import com.inotia4.export.AgreementPopup
import com.inotia4.export.LogFile
import com.inotia4.export.NativeBridge
import com.inotia4.export.UiActivityTracker
import com.inotia4.export.store.ModuleSaveStore
import com.inotia4.export.util.ApiException
import com.inotia4.export.util.JsonUtil
import com.yanzhenjie.andserver.http.StatusCode
import org.json.JSONObject

/**
 * 合法操作服务实现（ActionApiService 接口的唯一实现，v0.4.0 迁移自 PlayerController 操作编排）。
 * 操作调用 + 快照 attach 全部在此，controller 只做路由与参数解析。
 * 每个操作经 LogFile.op 统一记录（端点/参数/结果/耗时），端点路径与 controller @PostMapping 一一对应。
 */
class ActionApiServiceImpl : ActionApiService {

    override fun move(x: Int, y: Int): String =
        LogFile.op("POST /api/world/movement/move_to", "x=$x,y=$y") { attachPlayer(NativeBridge.nativeOpMove(x, y)) }

    override fun walk(direction: Int): String =
        LogFile.op("POST /api/world/movement/walk_dir", "dir=$direction") { attachPlayer(NativeBridge.nativeOpWalk(direction)) }

    override fun walkStop(): String =
        LogFile.op("POST /api/world/movement/stop_move", "") { NativeBridge.nativeOpWalkStop() }

    override fun interact(): String =
        LogFile.op("POST /api/world/movement/interact_with", "") { attachPlayer(NativeBridge.nativeOpInteract()) }

    override fun useItem(bag: Int, slot: Int): String =
        LogFile.op("POST /api/item/inventory/use_item", "bag=$bag,slot=$slot") { attachInventory(NativeBridge.nativeOpUseItem(bag, slot)) }

    override fun diceAccept(): String =
        LogFile.op("POST /api/item/inventory/accept_dice", "") { attachPlayer(NativeBridge.nativeOpDiceAccept()) }

    override fun diceReject(): String =
        LogFile.op("POST /api/item/inventory/reject_dice", "") { NativeBridge.nativeOpDiceReject() }

    override fun sellItem(bag: Int, slot: Int): String =
        LogFile.op("POST /api/item/inventory/sell_item", "bag=$bag,slot=$slot") { attachInventory(NativeBridge.nativeOpSellItem(bag, slot)) }

    override fun moveItem(bag: Int, slot: Int, count: Int, toBag: Int, toSlot: Int): String =
        LogFile.op("POST /api/item/inventory/move_item", "bag=$bag,slot=$slot,count=$count,toBag=$toBag,toSlot=$toSlot") {
            attachInventory(NativeBridge.nativeOpMoveItem(bag, slot, count, toBag, toSlot))
        }

    override fun equip(role: Int, bag: Int, slot: Int): String =
        LogFile.op("POST /api/item/inventory/{role}/equip_item", "role=$role,bag=$bag,slot=$slot") { attachParty(NativeBridge.nativeOpEquip(role, bag, slot)) }

    override fun equipByCategory(role: Int, category: Int): String =
        LogFile.op("POST /api/item/inventory/{role}/equip_item", "role=$role,category=$category") {
            val pos = findItemSlot(category) ?: throw ApiException(StatusCode.SC_NOT_FOUND, "item not found")
            attachParty(NativeBridge.nativeOpEquip(role, pos.first, pos.second))
        }

    override fun unequip(role: Int, slot: Int): String =
        LogFile.op("POST /api/item/inventory/{role}/unequip_item", "role=$role,slot=$slot") { attachParty(NativeBridge.nativeOpUnequip(role, slot)) }

    override fun autoAttack(role: Int, on: Boolean): String =
        LogFile.op("POST /api/character/combat/{role}/set_auto_attack", "role=$role,on=$on") {
            attachParty(NativeBridge.nativeOpSetAutoAttack(role, if (on) 1 else 0))
        }

    override fun learnSkill(role: Int, actionId: Int, level: Int): String =
        LogFile.op("POST /api/character/grow/add_skill", "role=$role,actionId=$actionId,level=$level") {
            attachSkills(NativeBridge.nativeOpLearnAction(role, actionId, level))
        }

    override fun addStat(role: Int, attrs: List<Pair<Int, Int>>): String =
        LogFile.op("POST /api/character/grow/{role}/add_stat", "role=$role,attrs=$attrs") {
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

    override fun statReset(role: Int): String =
        LogFile.op("POST /api/character/grow/reset_stat", "role=$role") { attachPlayer(NativeBridge.nativeOpStatReset(role)) }

    override fun skillReset(role: Int): String =
        LogFile.op("POST /api/character/grow/reset_skill", "role=$role") { attachPlayer(NativeBridge.nativeOpSkillReset(role)) }

    override fun cast(role: Int, actionId: Int): String =
        LogFile.op("POST /api/character/combat/{role}/cast_skill", "role=$role,actionId=$actionId") { attachParty(NativeBridge.nativeOpCast(role, actionId)) }

    override fun questQuit(questId: Int): String =
        LogFile.op("POST /api/quest/quit_quest", "questId=$questId") { attachPlayer(NativeBridge.nativeOpQuestQuit(questId)) }

    override fun save(): String =
        LogFile.op("POST /api/system/save", "") {
            attachPlayer(afterSave(NativeBridge.nativeOpSave()))
        }

    override fun mainMenu(): String =
        LogFile.op("POST /api/ui/go_main_menu", "") { attachPlayer(NativeBridge.nativeOpMainMenu()) }

    override fun enterSlot(slot: Int): String =
        LogFile.op("POST /api/system/enter_slot", "slot=$slot") {
            val activityCheck = UiActivityTracker.check()
            if (activityCheck.failed) {
                JsonUtil.err("ui state unavailable")
            } else if (activityCheck.blockingActivityName != null) {
                JsonUtil.err("ui occupied: agreement")
            } else {
                attachPlayer(afterNativeSuccess(NativeBridge.nativeOpEnterSlot(slot)) { ModuleSaveStore.ensureSlot(slot) })
            }
        }

    override fun createSlot(slot: Int, classIdx: Int): String =
        LogFile.op("POST /api/system/create_slot", "slot=$slot,classIdx=$classIdx") {
            val activityCheck = UiActivityTracker.check()
            if (activityCheck.failed) {
                JsonUtil.err("ui state unavailable")
            } else if (activityCheck.blockingActivityName != null) {
                JsonUtil.err("ui occupied: agreement")
            } else {
                attachPlayer(afterNativeSuccess(NativeBridge.nativeOpCreateSlot(slot, classIdx)) { ModuleSaveStore.resetSlot(slot) })
            }
        }

    override fun panelClose(): String =
        LogFile.op("POST /api/ui/close_panel", "") { attachUi(NativeBridge.nativeOpPanelClose()) }

    override fun panelOpen(panel: String): String =
        LogFile.op("POST /api/ui/open_panel", "panel=$panel") { attachUi(NativeBridge.nativeOpPanelOpen(panel)) }

    override fun npcInteract(): String =
        LogFile.op("POST /api/ui/start_interact", "") { NativeBridge.nativeOpNpcInteract() }

    override fun dialogSelect(action: String, index: Int): String =
        LogFile.op("POST /api/ui/dialog/select", "action=$action,index=$index") {
            val activityCheck = UiActivityTracker.check()
            if (activityCheck.blockingActivityName != null) {
                // agreement 域（Java 同意页，仅主菜单）：select 不 fail-closed——检测失败时落回
                // native（其自带 in_world 守卫），避免 Java 反射故障拖垮 world 内全部 select。
                if (action == "ok") {
                    val activity = UiActivityTracker.agreementActivity()
                    if (activity == null || !AgreementPopup.dismiss(activity)) {
                        JsonUtil.err("agreement window unavailable")
                    } else {
                        JSONObject().put("ok", true).put("result", "tap_dispatched").toString()
                    }
                } else {
                    JsonUtil.err("no such option in agreement")
                }
            } else {
                NativeBridge.nativeOpDialogSelect(action, index)
            }
        }

    override fun shopBuy(slot: Int): String =
        LogFile.op("POST /api/item/shop/buy_item", "slot=$slot") { attachInventory(NativeBridge.nativeOpShopBuy(slot)) }

    override fun jewel(role: Int, bag: Int, slot: Int, equipSlot: Int): String =
        LogFile.op("POST /api/item/inventory/{role}/put_jewel", "role=$role,bag=$bag,slot=$slot,equipSlot=$equipSlot") {
            attachParty(NativeBridge.nativeOpJewel(role, bag, slot, equipSlot))
        }

    override fun enchant(role: Int, bag: Int, slot: Int, equipSlot: Int): String =
        LogFile.op("POST /api/item/inventory/{role}/enchant", "role=$role,bag=$bag,slot=$slot,equipSlot=$equipSlot") {
            attachParty(NativeBridge.nativeOpEnchant(role, bag, slot, equipSlot))
        }

    override fun switchPlayer(slot: Int): String =
        LogFile.op("POST /api/character/combat/switch_player", "slot=$slot") { attachPlayer(NativeBridge.nativeOpSwitchPlayer(slot)) }

    override fun discardItem(bag: Int, slot: Int): String =
        LogFile.op("POST /api/item/inventory/discard_item", "bag=$bag,slot=$slot") { attachInventory(NativeBridge.nativeOpDiscardItem(bag, slot)) }

    override fun includeParty(mercenarySlot: Int): String =
        LogFile.op("POST /api/character/party/include", "mercSlot=$mercenarySlot") { attachParty(NativeBridge.nativeOpIncludeParty(mercenarySlot)) }

    override fun excludeParty(mercenarySlot: Int): String =
        LogFile.op("POST /api/character/party/exclude", "mercSlot=$mercenarySlot") { attachParty(NativeBridge.nativeOpExcludeParty(mercenarySlot)) }

    override fun discharge(mercenarySlot: Int): String =
        LogFile.op("POST /api/character/party/discharge", "mercSlot=$mercenarySlot") { attachParty(NativeBridge.nativeOpDischarge(mercenarySlot)) }

    override fun withdraw(mercenarySlot: Int, equipSlot: Int): String =
        LogFile.op("POST /api/character/party/withdraw", "mercSlot=$mercenarySlot,equipSlot=$equipSlot") {
            attachParty(NativeBridge.nativeOpWithdraw(mercenarySlot, equipSlot))
        }

    override fun attack(role: Int, targetSlot: Int): String =
        LogFile.op("POST /api/character/combat/{role}/attack_target", "role=$role,targetSlot=$targetSlot") {
            attachParty(NativeBridge.nativeOpAttack(role, targetSlot))
        }

    override fun stopCombat(role: Int): String =
        LogFile.op("POST /api/character/combat/{role}/stop_combat", "role=$role") {
            attach(NativeBridge.nativeOpStopCombat(role)) { NativeBridge.nativeGetPlayerJson() }
        }

    private fun attachPlayer(op: String): String =
        attach(op) { NativeBridge.nativeGetPlayerJson() }

    private fun attachParty(op: String): String =
        attach(op) { NativeBridge.nativeGetPartyJson() }

    private fun attachInventory(op: String): String =
        attach(op) { NativeBridge.nativeGetInventoryJson() }

    private fun attachSkills(op: String): String =
        attach(op) { NativeBridge.nativeGetSkillsJson() }

    private fun attachUi(op: String): String =
        attach(op) { NativeBridge.nativeGetGamestateJson() }

    private fun currentSaveSlot(): Int = try {
        JSONObject(NativeBridge.nativeCurrentSaveSlot()).optInt("current_save_slot", -1)
    } catch (e: Exception) {
        -1
    }

    private fun afterSave(op: String): String = afterNativeSuccess(op) {
        val current = currentSaveSlot()
        if (current in 0..2) ModuleSaveStore.ensureSlot(current)
    }

    private fun afterNativeSuccess(op: String, action: () -> Unit): String {
        val succeeded = try {
            JSONObject(op).optBoolean("ok", false)
        } catch (e: Exception) {
            false
        }
        if (succeeded) action()
        return op
    }

    private fun findItemSlot(category: Int): Pair<Int, Int>? {
        val inv = try {
            JSONObject(NativeBridge.nativeGetInventoryJson())
        } catch (e: Exception) {
            return null
        }
        val bags = inv.optJSONArray("bags") ?: return null
        for (b in 0 until bags.length()) {
            val bag = bags.getJSONObject(b)
            val items = bag.optJSONArray("items") ?: continue
            for (i in 0 until items.length()) {
                val item = items.getJSONObject(i)
                if (item.optInt("category", -1) == category) return b to item.optInt("slot", -1)
            }
        }
        return null
    }

    private fun attach(op: String, latest: () -> String): String {
        return try {
            val obj = JSONObject(op)
            if (obj.optBoolean("ok", false)) obj.put("state", JSONObject(latest()))
            obj.toString()
        } catch (e: Exception) {
            op
        }
    }
}
