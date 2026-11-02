package com.inotia4.qol.service.info

/**
 * character 域 GET 查询端点（party 12 + mercenary 3）：owner = character_facts。
 * Kotlin 只构造路径，响应体（含成员名称/职业注入、restructureRole 重排、装备 restructureItem、
 * 技能名注入与多源组合）完全由 native owner 生成。
 */
internal object InfoCharacterQueries {
    fun mercenary(): String = NativeHttpQuery.get("/api/character/mercenary")
    fun mercenaryList(): String = NativeHttpQuery.get("/api/character/mercenary/list")
    fun mercenarySlot(slot: Int): String = NativeHttpQuery.get("/api/character/mercenary/$slot")

    fun party(): String = NativeHttpQuery.get("/api/character/party")
    fun partyCount(): String = NativeHttpQuery.get("/api/character/party/count")
    fun partyLeader(): String = NativeHttpQuery.get("/api/character/leader")
    fun partyMember(slot: Int): String = NativeHttpQuery.get("/api/character/party/$slot")
    fun partyMemberId(slot: Int): String = NativeHttpQuery.get("/api/character/party/$slot/id")
    fun partyMemberName(slot: Int): String = NativeHttpQuery.get("/api/character/party/$slot/name")
    fun partyMemberLevel(slot: Int): String = NativeHttpQuery.get("/api/character/party/$slot/level")
    fun partyMemberStatus(slot: Int): String = NativeHttpQuery.get("/api/character/party/$slot/status")
    fun partyMemberStats(slot: Int): String = NativeHttpQuery.get("/api/character/party/$slot/stats")
    fun partyMemberEquipment(slot: Int): String =
        NativeHttpQuery.get("/api/character/party/$slot/equipment")

    fun partyMemberEquip(slot: Int, equipSlot: Int): String =
        NativeHttpQuery.get("/api/character/party/$slot/equipment/$equipSlot")

    fun partyMemberSkills(slot: Int): String = NativeHttpQuery.get("/api/character/party/$slot/skills")
}
