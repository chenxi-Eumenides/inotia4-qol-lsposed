package com.inotia4.qol.service.contract

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
    fun debugItemRaw(bag: Int, slot: Int): String
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
