package com.inotia4.qol.service.action

import com.inotia4.qol.AgreementPopup
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

/**
 * 合法操作服务实现（ActionApiService 接口的唯一实现，v0.4.0 迁移自 PlayerController 操作编排）。
 * 操作调用 + 快照 attach 全部在此，controller 只做路由与参数解析。
 * 每个操作经 LogFile.op 统一记录（端点/参数/结果/耗时），端点路径与 controller @PostMapping 一一对应。
 */
class ActionApiServiceCore : ActionApiService {


    override fun move(x: Int, y: Int): String = MovementActions.move(x, y)

    override fun walk(direction: Int): String = MovementActions.walk(direction)

    override fun walkStop(): String = MovementActions.walkStop()

    override fun interact(): String = MovementActions.interact()

    override fun useItem(bag: Int, slot: Int): String = InventoryActions.useItem(bag, slot)

    override fun diceAccept(): String = InventoryActions.diceAccept()

    override fun diceReject(): String = InventoryActions.diceReject()

    override fun sellItem(bag: Int, slot: Int): String = InventoryActions.sellItem(bag, slot)

    override fun moveItem(bag: Int, slot: Int, count: Int, toBag: Int, toSlot: Int): String = InventoryActions.moveItem(bag, slot, count, toBag, toSlot)

    override fun equip(role: Int, bag: Int, slot: Int): String = InventoryActions.equip(role, bag, slot)

    override fun equipByCategory(role: Int, category: Int): String = InventoryActions.equipByCategory(role, category)

    override fun unequip(role: Int, slot: Int): String = InventoryActions.unequip(role, slot)

    override fun shopBuy(slot: Int): String = InventoryActions.shopBuy(slot)

    override fun jewel(role: Int, bag: Int, slot: Int, equipSlot: Int): String = InventoryActions.jewel(role, bag, slot, equipSlot)

    override fun enchant(role: Int, bag: Int, slot: Int, equipSlot: Int): String = InventoryActions.enchant(role, bag, slot, equipSlot)

    override fun discardItem(bag: Int, slot: Int): String = InventoryActions.discardItem(bag, slot)

    override fun autoAttack(role: Int, on: Boolean): String = CharacterActions.autoAttack(role, on)

    override fun learnSkill(role: Int, actionId: Int, level: Int): String = CharacterActions.learnSkill(role, actionId, level)

    override fun addStat(role: Int, attrs: List<Pair<Int, Int>>): String = CharacterActions.addStat(role, attrs)

    override fun statReset(role: Int): String = CharacterActions.statReset(role)

    override fun skillReset(role: Int): String = CharacterActions.skillReset(role)

    override fun cast(role: Int, actionId: Int): String = CharacterActions.cast(role, actionId)

    override fun switchPlayer(slot: Int): String = CharacterActions.switchPlayer(slot)

    override fun attack(role: Int, targetSlot: Int): String = CharacterActions.attack(role, targetSlot)

    override fun stopCombat(role: Int): String = CharacterActions.stopCombat(role)

    override fun includeParty(mercenarySlot: Int): String = PartyActions.includeParty(mercenarySlot)

    override fun excludeParty(mercenarySlot: Int): String = PartyActions.excludeParty(mercenarySlot)

    override fun discharge(mercenarySlot: Int): String = PartyActions.discharge(mercenarySlot)

    override fun withdraw(mercenarySlot: Int, equipSlot: Int): String = PartyActions.withdraw(mercenarySlot, equipSlot)

    override fun questQuit(questId: Int): String = QuestActions.questQuit(questId)

    override fun save(): String = SaveActions.save()

    override fun mainMenu(): String = SaveActions.mainMenu()

    override fun enterSlot(slot: Int): String = SaveActions.enterSlot(slot)

    override fun createSlot(slot: Int, classIdx: Int): String = SaveActions.createSlot(slot, classIdx)

    override fun panelClose(): String = UiActions.panelClose()

    override fun panelOpen(panel: String): String = UiActions.panelOpen(panel)

    override fun npcInteract(): String = UiActions.npcInteract()

    override fun dialogSelect(action: String, index: Int): String = UiActions.dialogSelect(action, index)

    override fun extensionBagEnterView(bag: Int): String = ExtensionBagActions.extensionBagEnterView(bag)

    override fun extensionBagExitView(): String = ExtensionBagActions.extensionBagExitView()

    override fun extensionBagUnequip(bag: Int): String = ExtensionBagActions.extensionBagUnequip(bag)

    override fun extensionBagSelectBag(bag: Int): String = ExtensionBagActions.extensionBagSelectBag(bag)

    override fun extensionBagClickItem(bag: Int, slot: Int): String = ExtensionBagActions.extensionBagClickItem(bag, slot)

    override fun extensionBagMoveItem(fromBag: Int, fromSlot: Int, toBag: Int, toSlot: Int): String = ExtensionBagActions.extensionBagMoveItem(fromBag, fromSlot, toBag, toSlot)
}
