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
    fun partyMemberAttributes(slot: Int): String
    fun partyMemberEquipment(slot: Int): String
    fun partyMemberEquip(slot: Int, equipSlot: Int): String
    fun partyMemberSkills(slot: Int): String
    fun mercenary(): String
    fun mercenaryList(): String
    fun mercenarySlot(slot: Int): String
    fun inventory(space: Int? = null, cursor: String? = null, limit: Int? = null): String
    fun inventoryMoney(): String
    fun inventoryItems(space: Int? = null, cursor: String? = null, limit: Int? = null): String
    fun bagInfo(bag: Int, space: Int? = null): String
    fun bagSlot(bag: Int, slot: Int, space: Int? = null): String
    fun quest(): String
    fun questActive(): String
    fun questDetails(): String
    fun questListId(id: Int): String
    fun questCompleted(): String
    fun uiState(): String
    fun uiStateTable(): String
    fun game(): String
    fun gameSnapshot(): String
    fun gameInfo(): String
    fun gameFrame(): String
    fun events(since: Long?): String
    fun health(): String
    fun featureStatusJson(): String
    fun systemResourcesJson(): String
    fun ticketStateJson(ticket: Long): String
    fun npcDialogOptions(): String
    fun shopItems(): String
    fun debugUi(): String
    fun debugPath(tx: Int, ty: Int): String
    fun debugItemRaw(bag: Int, slot: Int): String
    fun settingsUiStatus(): String
}
