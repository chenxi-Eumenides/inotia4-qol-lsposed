package com.inotia4.export.service

// API 服务层接口（v0.4.0 P0-3 重构）

// API 服务层接口（v0.4.0 P0-3 重构）
interface InfoApiService {
    fun ready(): Boolean

    fun currentMapId(): String
    fun currentMapExits(): String
    fun currentMapUnits(): String
    fun currentMapEnemies(): String
    fun currentMapInteractives(): String
    fun currentMapDrops(): String
    fun currentMapDistance(tx: Int, ty: Int): String

    fun party(): String
    fun partyCount(): String
    fun partyLeader(): String
    fun partyMember(slot: Int): String
    fun partyMemberId(slot: Int): String
    fun partyMemberName(slot: Int): String
    fun partyMemberLevel(slot: Int): String
    fun partyMemberStatus(slot: Int): String
    fun partyMemberStats(slot: Int): String
    fun partyMemberEquipment(slot: Int): String
    fun partyMemberEquip(slot: Int, equipSlot: Int): String
    fun partyMemberSkills(slot: Int): String

    fun mercenary(): String
    fun mercenaryList(): String
    fun mercenarySlot(slot: Int): String

    fun inventory(): String
    fun inventoryMoney(): String
    fun inventoryItems(): String
    fun bagInfo(bag: Int): String
    fun bagSlot(bag: Int, slot: Int): String

    fun quest(): String
    fun questActive(): String
    fun questDetails(): String
    fun questListId(id: Int): String
    fun questCompleted(): String

    fun ui(): String
    fun uiScreen(): String
    fun uiPanel(): String
    fun uiDialog(): String

    fun game(): String
    fun gameSnapshot(): String
    fun gameInfo(): String
    fun gameFrame(): String

    fun events(since: Long?): String
    fun health(): String
    fun npcDialogOptions(): String
    fun shopItems(): String
    fun debugUi(): String
    fun debugPath(tx: Int, ty: Int): String
    fun exp1BtnBehavior(): String
    fun exp2AddControl(): String
    fun exp3CustomDialog(text: String): String
    fun exp4TextAppearance(): String
    fun exp5NewPanel(): String
    fun expRestoreAll(): String
    fun expStatus(): String
    fun settingsUiInject(): String
    fun settingsUiStatus(): String
    fun settingsUiRestore(): String
    fun settingsUiOpenOption(): String
    fun settingsUiOpenPanel(): String
    fun extensionBagStatusJson(): String
    fun extensionBagTestEquip(index: Int, bagType: Int): String
    fun extensionBagTestItem(index: Int, slot: Int, category: Int, count: Int): String
}

// API 服务层接口（v0.4.0 P0-3 重构）
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
