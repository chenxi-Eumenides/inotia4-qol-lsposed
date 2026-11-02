package com.inotia4.qol.service.info

import com.inotia4.qol.NativeBridge
import com.inotia4.qol.StaticData
import com.inotia4.qol.UiActivityTracker
import com.inotia4.qol.util.ApiException
import com.inotia4.qol.util.JsonUtil
import com.yanzhenjie.andserver.http.StatusCode
import org.json.JSONObject
import com.inotia4.qol.service.contract.InfoApiService

/**
 * 信息查询服务实现（InfoApiService 接口的唯一实现，v0.4.0 迁移自 InfoService）。
 * 字段提取、名称注入、快照组装全部在此，调用层只做路由与参数透传。
 */
class InfoApiServiceCore : InfoApiService {
    private fun notFound(): Nothing = throw ApiException(StatusCode.SC_NOT_FOUND, "not found")

    override fun ready(): Boolean = NativeBridge.ready

    override fun currentMapExits(): String = InfoMapQueries.exits()
    override fun currentMapId(): String = InfoMapQueries.id()
    override fun currentMapUnits(): String = InfoMapQueries.units()
    override fun currentMapEnemies(): String = InfoMapQueries.enemies()
    override fun currentMapInteractives(): String = InfoMapQueries.interactives()
    override fun currentMapDrops(): String = InfoMapQueries.drops()
    override fun currentMapDistance(tx: Int, ty: Int): String = InfoMapQueries.distance(tx, ty)

    // character 域 GET（party 12 + mercenary 3）已迁 owner（core.character）：Kotlin 只做
    // nativeHttpDispatch 透传，成功响应与 404/非 world 语义由 owner 快照与原生拼装生成
    // （见 InfoCharacterQueries）。
    override fun party(): String = InfoCharacterQueries.party()

    override fun partyCount(): String = InfoCharacterQueries.partyCount()

    override fun partyLeader(): String = InfoCharacterQueries.partyLeader()

    override fun partyMember(slot: Int): String = InfoCharacterQueries.partyMember(slot)

    override fun partyMemberId(slot: Int): String = InfoCharacterQueries.partyMemberId(slot)

    override fun partyMemberName(slot: Int): String = InfoCharacterQueries.partyMemberName(slot)

    override fun partyMemberLevel(slot: Int): String = InfoCharacterQueries.partyMemberLevel(slot)

    override fun partyMemberStatus(slot: Int): String = InfoCharacterQueries.partyMemberStatus(slot)

    override fun partyMemberStats(slot: Int): String = InfoCharacterQueries.partyMemberStats(slot)

    override fun partyMemberEquipment(slot: Int): String =
        InfoCharacterQueries.partyMemberEquipment(slot)

    override fun partyMemberEquip(slot: Int, equipSlot: Int): String =
        InfoCharacterQueries.partyMemberEquip(slot, equipSlot)

    override fun partyMemberSkills(slot: Int): String = InfoCharacterQueries.partyMemberSkills(slot)

    override fun mercenary(): String = InfoCharacterQueries.mercenary()
    override fun mercenaryList(): String = InfoCharacterQueries.mercenaryList()
    override fun mercenarySlot(slot: Int): String = InfoCharacterQueries.mercenarySlot(slot)

    // item/quest 查询已迁 owner（item_facts/quest）：Kotlin 只做 nativeHttpDispatch 透传，
    // 成功响应与冻结语义由 owner 快照与启动期静态 enrichment 生成（见 NativeHttpQuery）。
    override fun inventory(space: Int?, cursor: String?, limit: Int?): String =
        InfoItemQueries.inventory(space, cursor, limit)

    override fun inventoryMoney(): String = InfoItemQueries.money()

    override fun inventoryItems(space: Int?, cursor: String?, limit: Int?): String =
        InfoItemQueries.items(space, cursor, limit)

    override fun bagInfo(bag: Int, space: Int?): String = InfoItemQueries.bagInfo(bag, space)

    override fun bagSlot(bag: Int, slot: Int, space: Int?): String =
        InfoItemQueries.bagSlot(bag, slot, space)

    override fun quest(): String = InfoQuestQueries.quest()

    override fun questActive(): String = InfoQuestQueries.active()

    override fun questDetails(): String = InfoQuestQueries.details()

    override fun questListId(id: Int): String = notFound()

    override fun questCompleted(): String = InfoQuestQueries.completed()

    override fun uiState(): String {
        val activityCheck = UiActivityTracker.check()
        if (activityCheck.blockingActivityName != null) {
            return """{"name":"android_agreement","type":4,"source":4,"info":{},"options":[{"id":"ok","label":"同意"}]}"""
        }
        // Activity 检测失败不代表同意页存在，仍查询 native 状态。
        return InfoSystemQueries.uiState()
    }

    override fun uiStateTable(): String = InfoSystemQueries.uiStateTable()

    override fun game(): String = InfoSystemQueries.game()

    // system 快照/info/game_frame 已迁 owner（core.system）：Kotlin 只做 nativeHttpDispatch
    // 透传，成功响应与冻结语义由 owner 主线程快照生成（见 InfoSystemQueries）。
    override fun gameSnapshot(): String = InfoSystemQueries.snapshot()

    override fun gameFrame(): String = InfoSystemQueries.gameFrame()

    override fun gameInfo(): String = InfoSystemQueries.gameInfo()

    override fun events(since: Long?): String = NativeBridge.nativeGetEventsJson()

    override fun npcDialogOptions(): String {
        val json = NativeBridge.nativeNpcDialogOptions()
        if (isNativeError(json)) return json
        return json
    }

    override fun debugUi(): String = InfoSystemQueries.debugUi()
    override fun debugPath(tx: Int, ty: Int): String = InfoSystemQueries.debugPath(tx, ty)
    override fun debugItemRaw(bag: Int, slot: Int): String = InfoSystemQueries.debugItemRaw(bag, slot)
    override fun settingsUiStatus(): String = InfoSystemQueries.settingsUiStatus()

    override fun shopItems(): String = InfoItemQueries.shopItems()

    override fun health(): String = InfoSystemQueries.health()

    // 原生功能状态 JSON 原样透传（不经 StaticData/名称注入）。HTTP 路径由 ControllerGuard 在
    // ready=false 时先抛 503；此处的 ready 判定是给非 HTTP 调用方的兜底——libgamebridge 未加载
    // 时调用 JNI 会抛 UnsatisfiedLinkError。
    override fun featureStatusJson(): String =
        if (NativeBridge.ready) NativeBridge.nativeFeatureStatusJson() else JsonUtil.NOT_READY

    // P3 资源登记表 JSON 原样透传（/api/system/resources），就绪判定同 featureStatusJson。
    override fun systemResourcesJson(): String =
        if (NativeBridge.ready) NativeBridge.nativeSystemResourcesJson() else JsonUtil.NOT_READY

    // P3 命令票据状态 JSON 原样透传（/api/system/tickets/{id}），就绪判定同上。
    override fun ticketStateJson(ticket: Long): String =
        if (NativeBridge.ready) NativeBridge.nativeTicketStateJson(ticket) else JsonUtil.NOT_READY

    /**
     * native 数据函数在非 world 状态（主菜单等）下返回 {"error":"..."}（与写操作 game_in_world() 一致）。
     * Kotlin 侧不自判游戏状态，只诚实转发：顶层含 "error" 字段 → 原样返回。顶层判定避免嵌套字段误报。
     */
    private fun isNativeError(json: String): Boolean = JsonUtil.parseObj(json)?.has("error") == true

    private fun mapJson(): String = NativeBridge.nativeGetMapJson()

    private fun unitsJson(): String = NativeBridge.nativeGetUnitsJson()

    private fun enemiesJson(): String = NativeBridge.nativeGetEnemiesJson()

    private fun interactivesJson(): String = NativeBridge.nativeGetInteractivesJson()

    private fun dropsJson(): String = NativeBridge.nativeGetDropsJson()

    private fun snapshotJson(): String = NativeBridge.nativeGetSnapshotJson()

    private fun attachMapStatic(mapId: Int): JSONObject? {
        if (mapId < 0) return null
        val tables = JsonUtil.parseObj(StaticData.read("tables/MAPINFOBASE.json")) ?: return null
        val records = tables.optJSONArray("records") ?: return null
        if (mapId >= records.length()) return null
        val r = records.optJSONObject(mapId) ?: return null
        val out = JSONObject()
        out.put("text_id", r.optJSONArray("u16")?.optInt(0, -1) ?: -1)
        out.put("name", r.optString("text_0", ""))
        r.optJSONArray("u16")?.let { out.put("u16", it) }
        r.optString("hex", "")?.let { if (it.isNotEmpty()) out.put("hex", it) }
        return out
    }
}
