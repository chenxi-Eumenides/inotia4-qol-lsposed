package com.inotia4.qol.service.contract

interface ActionApiService {
    fun move(x: Int, y: Int): String
    fun walk(direction: Int): String
    fun walkStop(): String
    fun interact(): String
    fun useItem(bag: Int, slot: Int): String
    fun diceAccept(): String
    fun diceReject(): String
    fun sellItem(bag: Int, slot: Int): String
    fun moveItem(bag: Int, slot: Int, count: Int, toBag: Int, toSlot: Int): String
    fun equip(role: Int, bag: Int, slot: Int): String
    fun equipByCategory(role: Int, category: Int): String
    fun unequip(role: Int, slot: Int): String
    fun autoAttack(role: Int, on: Boolean): String
    fun learnSkill(role: Int, actionId: Int, level: Int): String
    fun addStat(role: Int, attrs: List<Pair<Int, Int>>): String
    fun statReset(role: Int): String
    fun skillReset(role: Int): String
    fun cast(role: Int, actionId: Int): String
    fun questQuit(questId: Int): String
    fun save(): String
    fun mainMenu(): String
    fun enterSlot(slot: Int): String
    fun createSlot(slot: Int, classIdx: Int): String
    fun backupExport(slot: Int): String
    fun backupImport(checksum: String, slot: Int): String
    fun backupList(): String
    fun backupDelete(checksum: String): String
    fun panelClose(): String
    fun panelOpen(panel: String): String
    fun npcInteract(): String
    fun dialogSelect(action: String, index: Int): String
    fun shopBuy(slot: Int): String
    fun jewel(role: Int, bag: Int, slot: Int, equipSlot: Int): String
    fun enchant(role: Int, bag: Int, slot: Int, equipSlot: Int): String
    fun switchPlayer(slot: Int): String
    fun discardItem(bag: Int, slot: Int): String
    fun includeParty(mercenarySlot: Int): String
    fun excludeParty(mercenarySlot: Int): String
    fun discharge(mercenarySlot: Int): String
    fun withdraw(mercenarySlot: Int, equipSlot: Int): String
    fun attack(role: Int, targetSlot: Int): String
    fun stopCombat(role: Int): String
    fun extensionBagEnterView(bag: Int): String
    fun extensionBagExitView(): String
    fun extensionBagUnequip(bag: Int): String
    fun extensionBagSelectBag(bag: Int): String
    fun extensionBagClickItem(bag: Int, slot: Int): String
    fun extensionBagMoveItem(fromBag: Int, fromSlot: Int, toBag: Int, toSlot: Int): String
}
