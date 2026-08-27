package com.inotia4.export.service

import com.inotia4.export.BuildConfig
import com.inotia4.export.LogFile
import com.inotia4.export.NativeBridge
import com.inotia4.export.StaticData
import com.inotia4.export.UiActivityTracker
import com.inotia4.export.util.ApiException
import com.inotia4.export.util.JsonUtil
import com.yanzhenjie.andserver.http.StatusCode
import org.json.JSONArray
import org.json.JSONObject

/**
 * 信息查询服务实现（InfoApiService 接口的唯一实现，v0.4.0 迁移自 InfoService）。
 * 字段提取、名称注入、快照组装全部在此，调用层只做路由与参数透传。
 */
class InfoApiServiceImpl : InfoApiService {

    private fun notFound(): Nothing = throw ApiException(StatusCode.SC_NOT_FOUND, "not found")

    override fun ready(): Boolean = NativeBridge.ready

    override fun currentMapExits(): String {
        val mj = mapJson()
        if (isNativeError(mj)) return mj
        val mapId = JsonUtil.parseObj(mj)?.optInt("map_id", -1) ?: -1
        val exits = StaticData.mapExits(mapId) ?: return "{\"exits\":[]}"
        return JsonUtil.wrap("exits", exits)
    }

    override fun currentMapId(): String {
        val mj = mapJson()
        if (isNativeError(mj)) return mj
        val mapId = JsonUtil.parseObj(mj)?.optInt("map_id", -1) ?: -1
        // W3 (v0.5.3)：单值端点注入 id_name（MAPINFOBASE 联查，与复合端点 map_data.name 一致）
        val out = JSONObject().put("map_id", mapId)
        attachMapStatic(mapId)?.let { out.put("id_name", it.optString("name", "")) }
        return out.toString()
    }

    override fun currentMapUnits(): String {
        val uj = unitsJson()
        if (isNativeError(uj)) return uj
        return uj
    }

    override fun currentMapEnemies(): String {
        val ej = enemiesJson()
        if (isNativeError(ej)) return ej
        return ej
    }

    override fun currentMapInteractives(): String {
        val ij = interactivesJson()
        if (isNativeError(ij)) return ij
        return ij
    }

    override fun currentMapDrops(): String {
        val dj = dropsJson()
        if (isNativeError(dj)) return dj
        return dj
    }

    override fun currentMapDistance(tx: Int, ty: Int): String = NativeBridge.nativeDistanceJson(tx, ty)

    override fun party(): String {
        val pj = partyJson()
        if (isNativeError(pj)) return pj
        return NameInjector.withItemNames(pj)
    }

    override fun partyCount(): String {
        val pj = playerJson()
        if (isNativeError(pj)) return pj
        return JsonUtil.wrap("count", JsonUtil.parseObj(pj)?.optInt("party_count", -1) ?: -1)
    }

    override fun partyLeader(): String {
        val pj = playerJson()
        if (isNativeError(pj)) return pj
        val pj2 = partyJson()
        if (isNativeError(pj2)) return pj2
        val p = partyArr() ?: notFound()
        val leaderSlot = JsonUtil.parseObj(pj)?.optInt("leader_slot", 0) ?: 0
        val m = if (leaderSlot in 0 until p.length()) p.optJSONObject(leaderSlot) else null
        return m?.let { NameInjector.withItemNames(it.toString()) } ?: notFound()
    }

    override fun partyMember(slot: Int): String {
        val pj = partyJson()
        if (isNativeError(pj)) return pj
        val p = partyArr() ?: notFound()
        val m = p.optJSONObject(slot) ?: notFound()
        return NameInjector.withItemNames(m.toString())
    }

    override fun partyMemberId(slot: Int): String {
        val pj = partyJson()
        if (isNativeError(pj)) return pj
        val m = memberObj(slot) ?: notFound()
        val classIdx = m.optInt("class_idx", -1)
        if (classIdx < 0) return notFound()
        val out = JSONObject().put("id", classIdx)
        StaticData.className(classIdx)?.let { out.put("id_name", it) }
        return out.toString()
    }

    override fun partyMemberName(slot: Int): String {
        val pj = partyJson()
        if (isNativeError(pj)) return pj
        return memberString(slot, "name")
    }

    override fun partyMemberLevel(slot: Int): String {
        val pj = partyJson()
        if (isNativeError(pj)) return pj
        return memberInt(slot, "level")
    }

    // v0.5.13：状态聚合端点（api-reference §2.1 party/{slot}/status），数据源 member json + skills json
    override fun partyMemberStatus(slot: Int): String {
        val pj = partyJson()
        if (isNativeError(pj)) return pj
        val m = memberObj(slot) ?: notFound()
        val out = JSONObject()
        m.optInt("hp", -1).takeIf { it >= 0 }?.let { out.put("hp", it) }
        m.optInt("max_hp", -1).takeIf { it >= 0 }?.let { out.put("max_hp", it) }
        m.optInt("mp", -1).takeIf { it >= 0 }?.let { out.put("mp", it) }
        m.optInt("max_mp", -1).takeIf { it >= 0 }?.let { out.put("max_mp", it) }
        m.optInt("exp", -1).takeIf { it >= 0 }?.let { out.put("exp", it) }
        m.optInt("exp_next", -1).takeIf { it >= 0 }?.let { out.put("exp_next", it) }
        m.optInt("status_point", -1).takeIf { it >= 0 }?.let { out.put("attribute_points", it) }
        skillsArr()?.optJSONObject(slot)?.optInt("skill_points", -1)?.takeIf { it >= 0 }?.let { out.put("skill_points", it) }
        return JsonUtil.wrap("status", out)
    }

    override fun partyMemberStats(slot: Int): String {
        val pj = partyJson()
        if (isNativeError(pj)) return pj
        return JsonUtil.wrap("stats", memberObj(slot)?.optJSONObject("stats"))
    }

    override fun partyMemberEquipment(slot: Int): String {
        val pj = partyJson()
        if (isNativeError(pj)) return pj
        val eq = memberObj(slot)?.optJSONArray("equipment") ?: notFound()
        val arr = JSONArray()
        for (i in 0 until eq.length()) {
            eq.optJSONObject(i)?.let { NameInjector.injectItemName(it, true); arr.put(it) }
        }
        return JsonUtil.wrap("equipment", arr)
    }

    override fun partyMemberEquip(slot: Int, equipSlot: Int): String {
        val pj = partyJson()
        if (isNativeError(pj)) return pj
        val eq = memberObj(slot)?.optJSONArray("equipment") ?: notFound()
        val it = eq.optJSONObject(equipSlot) ?: notFound()
        NameInjector.injectItemName(it, true)
        return it.toString()
    }

    override fun partyMemberSkills(slot: Int): String {
        val sj = skillsJson()
        if (isNativeError(sj)) return sj
        val s = JsonUtil.parseArr(sj) ?: notFound()
        val obj = s.optJSONObject(slot) ?: notFound()
        NameInjector.injectSkillNames(obj)
        return obj.toString()
    }

    override fun mercenary(): String {
        val mj = mercenariesJson()
        if (isNativeError(mj)) return mj
        return mj
    }

    override fun mercenaryList(): String {
        val mj = mercenariesJson()
        if (isNativeError(mj)) return mj
        val arr = JsonUtil.parseArr(mj) ?: return JsonUtil.wrap("slots", JSONArray())
        val slots = JSONArray()
        for (i in 0 until arr.length()) {
            arr.optJSONObject(i)?.optInt("slot", -1)?.takeIf { it >= 0 }?.let { slots.put(it) }
        }
        return JsonUtil.wrap("slots", slots)
    }

    override fun mercenarySlot(slot: Int): String {
        val mj = mercenariesJson()
        if (isNativeError(mj)) return mj
        val arr = JsonUtil.parseArr(mj) ?: notFound()
        for (i in 0 until arr.length()) {
            val m = arr.optJSONObject(i) ?: continue
            if (m.optInt("slot", -1) == slot) return m.toString()
        }
        return notFound()
    }

    override fun inventory(): String {
        val ij = inventoryJson()
        if (isNativeError(ij)) return ij
        return NameInjector.withItemNames(ij)
    }

    override fun inventoryMoney(): String {
        val pj = playerJson()
        if (isNativeError(pj)) return pj
        return JsonUtil.wrap("money", JsonUtil.parseObj(pj)?.optLong("money", -1) ?: -1L)
    }

    override fun inventoryItems(): String {
        val ij = inventoryJson()
        if (isNativeError(ij)) return ij
        val bags = JsonUtil.parseObj(ij)?.optJSONArray("bags") ?: return JsonUtil.wrap("items", JSONArray())
        val items = JSONArray()
        for (b in 0 until bags.length()) {
            val bag = bags.optJSONObject(b) ?: continue
            val bagItems = bag.optJSONArray("items") ?: continue
            for (i in 0 until bagItems.length()) {
                val it = bagItems.optJSONObject(i) ?: continue
                it.put("bag", bag.optInt("bag", -1))
                NameInjector.injectItemName(it)
                items.put(it)
            }
        }
        return JsonUtil.wrap("items", items)
    }

    override fun bagInfo(bag: Int): String {
        val ij = inventoryJson()
        if (isNativeError(ij)) return ij
        val bags = JsonUtil.parseObj(ij)?.optJSONArray("bags") ?: notFound()
        for (b in 0 until bags.length()) {
            val o = bags.optJSONObject(b) ?: continue
            if (o.optInt("bag", -1) == bag) {
                return JsonUtil.wrap("bag" to o.optInt("bag", -1),
                    "capacity" to o.optInt("capacity", -1),
                    "slot_count" to o.optInt("slot_count", -1))
            }
        }
        return notFound()
    }

    override fun bagSlot(bag: Int, slot: Int): String {
        val ij = inventoryJson()
        if (isNativeError(ij)) return ij
        val bags = JsonUtil.parseObj(ij)?.optJSONArray("bags") ?: notFound()
        for (b in 0 until bags.length()) {
            val o = bags.optJSONObject(b) ?: continue
            if (o.optInt("bag", -1) != bag) continue
            val items = o.optJSONArray("items") ?: notFound()
            for (i in 0 until items.length()) {
                val it = items.optJSONObject(i) ?: continue
                if (it.optInt("slot", -1) == slot) {
                    NameInjector.injectItemName(it)
                    return it.toString()
                }
            }
            return "null"
        }
        return notFound()
    }

    override fun quest(): String {
        // v0.5.13：与细分端点保持一致——active/details/completed 分别取 questActive/questDetails/questCompleted 的 quests 数组
        // 非 world 状态下 native 返回 {"error":...}——复合端点诚实转发首个错误，不伪造空数组
        val a = questActive()
        if (isNativeError(a)) return a
        val l = questDetails()
        if (isNativeError(l)) return l
        val c = questCompleted()
        if (isNativeError(c)) return c
        val active = JsonUtil.parseObj(a)?.optJSONArray("quests")
        val list = JsonUtil.parseObj(l)?.optJSONArray("quests")
        val completed = JsonUtil.parseObj(c)?.optJSONArray("quests")
        return JsonUtil.wrap(
            "active" to (active ?: JSONArray()),
            "details" to (list ?: JSONArray()),
            "completed" to (completed ?: JSONArray())
        )
    }

    override fun questActive(): String {
        val json = NativeBridge.nativeQuestActive()
        if (isNativeError(json)) return json
        return try {
            val root = JSONObject(json)
            val arr = root.optJSONArray("quests") ?: return json
            // 排除游戏面板不显示的任务（QUESTS.json hidden=true，战斗/教学类，v0.5.38）
            val visible = JSONArray()
            for (i in 0 until arr.length()) {
                val q = arr.optJSONObject(i) ?: continue
                val qid = q.optInt("quest_id", -1)
                if (qid < 0) continue
                val data = StaticData.questData(qid) ?: continue
                if (data.optBoolean("hidden", false)) continue
                val name = data.optString("name").takeIf { it.isNotEmpty() }
                if (name != null) q.put("id_name", name)
                NameInjector.injectQuestFields(q, data, listOf("group_id", "name", "detail", "is_side", "is_mainline"))
                visible.put(q)
            }
            root.put("quests", visible)
            root.toString()
        } catch (e: Exception) {
            json
        }
    }

    override fun questDetails(): String {
        val json = NativeBridge.nativeQuestList()
        if (isNativeError(json)) return json
        return try {
            val root = JSONObject(json)
            val arr = root.optJSONArray("quests") ?: return json
            for (i in 0 until arr.length()) {
                val q = arr.optJSONObject(i) ?: continue
                val qid = q.optInt("quest_id", -1)
                if (qid < 0) continue
                val data = StaticData.questData(qid) ?: continue
                NameInjector.injectQuestFields(
                    q, data,
                    listOf(
                        "group_id", "group_name", "name", "detail", "accepted_dialog",
                        "delivered_dialog", "class_req", "reward_hint",
                        "is_side", "is_mainline", "hidden"
                    )
                )
                val rewards = data.optJSONArray("rewards")
                if (rewards != null) q.put("rewards", rewards)
            }
            root.toString()
        } catch (e: Exception) {
            json
        }
    }

    override fun questListId(id: Int): String = notFound()

    override fun questCompleted(): String {
        val json = NativeBridge.nativeQuestCompleted()
        if (isNativeError(json)) return json
        return try {
            val root = JSONObject(json)
            val arr = root.optJSONArray("quests") ?: return json
            for (i in 0 until arr.length()) {
                val q = arr.optJSONObject(i) ?: continue
                val qid = q.optInt("quest_id", -1)
                if (qid < 0) continue
                val data = StaticData.questData(qid) ?: continue
                val name = data.optString("name").takeIf { it.isNotEmpty() }
                if (name != null) q.put("name", name)
                NameInjector.injectQuestFields(q, data, listOf("group_id", "detail", "is_side", "is_mainline"))
            }
            root.toString()
        } catch (e: Exception) {
            json
        }
    }

    override fun ui(): String = gamestateJson()

    override fun uiScreen(): String = JsonUtil.wrap("screen", screenName())

    override fun uiPanel(): String {
        val s = screenName()
        val panel = if (s in PANELS) s else null
        return JsonUtil.wrap("panel", panel)
    }

    override fun uiDialog(): String {
        // v0.5.42：native 完整检测（popup/story/npc/wipeout/npc_quest/面板态），与 gamestate 的
        // dialog 同源（data_dialog_content_json）；active 改为基于 screen 判定（screen 以 dialog_
        // 开头即 true）——替代旧 type!=none（数据残留误报：关闭 NPC 对话框后 type 残留 npc 但
        // UI 栈已空，旧逻辑 active 仍为 true，与已删除的 dialog_active 同源问题）
        val activityCheck = UiActivityTracker.check()
        if (activityCheck.blockingActivityName != null) {
            return JSONObject()
                .put("type", "popup")
                .put("active", true)
                .put("activity", activityCheck.blockingActivityName)
                .put("options", JSONArray())
                .toString()
        }
        val json = NativeBridge.nativeDialogContent()
        if (isNativeError(json)) return json
        return try {
            val obj = JSONObject(json)
            if (!obj.has("active")) {
                val active = screenName().startsWith("dialog_")
                obj.put("active", active)
            }
            obj.toString()
        } catch (e: Exception) {
            json
        }
    }

    override fun game(): String {
        return JsonUtil.wrap("snapshot" to JsonUtil.parseObj(snapshotJson()), "info" to JsonUtil.parseObj(gameInfo()))
    }

    override fun gameSnapshot(): String = NameInjector.withItemNames(snapshotJson())

    override fun gameFrame(): String = JsonUtil.wrap("frame", NativeBridge.nativeGetFrameCount())

    override fun gameInfo(): String {
        val slots: Any? = JsonUtil.parseObj(NativeBridge.nativeSaveSlotsJson())?.opt("slots")
        val currentSlot = JsonUtil.parseObj(NativeBridge.nativeCurrentSaveSlot())?.optInt("current_save_slot", -1) ?: -1
        return JsonUtil.wrap(
            "version" to BuildConfig.VERSION_NAME,
            "save_slots" to slots,
            "current_save_slot" to currentSlot,
            "package_name" to PKG_NAME,
            "base" to NativeBridge.nativeGetBaseAddr()
        )
    }

    override fun events(since: Long?): String = NativeBridge.nativeGetEventsJson()

    override fun npcDialogOptions(): String {
        val json = NativeBridge.nativeNpcDialogOptions()
        if (isNativeError(json)) return json
        return json
    }

    override fun debugUi(): String = NativeBridge.nativeGetDebugUiJson()

    override fun debugPath(tx: Int, ty: Int): String = NativeBridge.nativeDebugPathJson(tx, ty)

    override fun exp1BtnBehavior(): String = NativeBridge.nativeExp1BtnBehavior()
    override fun exp2AddControl(): String = NativeBridge.nativeExp2AddControl()
    override fun exp3CustomDialog(text: String): String = NativeBridge.nativeExp3CustomDialog(text)
    override fun exp4TextAppearance(): String = NativeBridge.nativeExp4TextAppearance()
    override fun exp5NewPanel(): String = NativeBridge.nativeExp5NewPanel()
    override fun expRestoreAll(): String = NativeBridge.nativeExpRestoreAll()
    override fun expStatus(): String = NativeBridge.nativeExpStatus()

    override fun settingsUiInject(): String = NativeBridge.nativeSettingsUiInject()
    override fun settingsUiStatus(): String = NativeBridge.nativeSettingsUiStatus()
    override fun settingsUiRestore(): String = NativeBridge.nativeSettingsUiRestore()
    override fun settingsUiOpenOption(): String = NativeBridge.nativeSettingsUiOpenOption()
    override fun settingsUiOpenPanel(): String = NativeBridge.nativeSettingsUiOpenPanel()
    override fun extensionBagStatus(): String = NativeBridge.nativeExtensionBagUiStatus()
    override fun extensionBagTestEquip(index: Int, bagType: Int): String =
        NativeBridge.nativeExtensionBagTestEquip(index, bagType)
    override fun extensionBagTestItem(index: Int, slot: Int, category: Int, count: Int): String =
        NativeBridge.nativeExtensionBagTestItem(index, slot, category, count)

    override fun shopItems(): String {
        val json = NativeBridge.nativeShopItems()
        if (isNativeError(json)) return json
        return try {
            val root = JSONObject(json)
            val arr = root.optJSONArray("items") ?: return json
            for (i in 0 until arr.length()) {
                val it = arr.optJSONObject(i) ?: continue
                val category = it.optInt("category", -1)
                if (category >= 0) StaticData.itemName(category)?.let { n -> it.put("name", n) }
            }
            root.toString()
        } catch (e: Exception) {
            json
        }
    }

    override fun health(): String = JsonUtil.wrap("ok" to true)

    /**
     * native 数据函数在非 world 状态（主菜单等）下返回 {"error":"..."}（与写操作 game_in_world() 一致）。
     * Kotlin 侧不自判游戏状态，只诚实转发：顶层含 "error" 字段 → 原样返回。顶层判定避免嵌套字段误报。
     */
    private fun isNativeError(json: String): Boolean = JsonUtil.parseObj(json)?.has("error") == true

    private fun playerJson(): String = NativeBridge.nativeGetPlayerJson()

    private fun partyJson(): String = NativeBridge.nativeGetPartyJson()

    private fun partyArr(): JSONArray? = JsonUtil.parseArr(partyJson())

    private fun inventoryJson(): String = NativeBridge.nativeGetInventoryJson()

    private fun mapJson(): String = NativeBridge.nativeGetMapJson()

    private fun unitsJson(): String = NativeBridge.nativeGetUnitsJson()

    private fun enemiesJson(): String = NativeBridge.nativeGetEnemiesJson()

    private fun interactivesJson(): String = NativeBridge.nativeGetInteractivesJson()

    private fun dropsJson(): String = NativeBridge.nativeGetDropsJson()

    private fun gamestateJson(): String {
        val json = NativeBridge.nativeGetGamestateJson()
        val activityCheck = UiActivityTracker.check()
        val blockingActivity = activityCheck.blockingActivityName ?: return json
        return try {
            val root = JSONObject(json)
            root.put("screen", "dialog_popup")
            root.put(
                "dialog",
                JSONObject()
                    .put("type", "popup")
                    .put("active", true)
                    .put("activity", blockingActivity)
                    .put("options", JSONArray()),
            )
            root.toString()
        } catch (e: Exception) {
            LogFile.logError("attach foreground activity to gamestate failed", e)
            json
        }
    }

    private fun snapshotJson(): String = NativeBridge.nativeGetSnapshotJson()

    private fun skillsJson(): String = NativeBridge.nativeGetSkillsJson()

    private fun skillsArr(): JSONArray? = JsonUtil.parseArr(skillsJson())

    private fun mercenariesJson(): String = NativeBridge.nativeGetMercenariesJson()

    private fun memberObj(slot: Int): JSONObject? = partyArr()?.optJSONObject(slot)

    private fun memberInt(slot: Int, key: String): String {
        val m = memberObj(slot) ?: notFound()
        if (!m.has(key)) return notFound()
        return JsonUtil.wrap(key, m.optInt(key, -1))
    }

    private fun memberString(slot: Int, key: String): String {
        val m = memberObj(slot) ?: notFound()
        if (!m.has(key)) return notFound()
        return JsonUtil.wrap(key, m.optString(key, ""))
    }

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

    private fun screenName(): String =
        JsonUtil.parseObj(gamestateJson())?.optString("screen", "loading") ?: "loading"

    companion object {
        private const val PKG_NAME =
            "com.com2us.inotia4.normal.freefull.google.global.android.common"

        // v0.5.42：screen 枚举体系（与 data_ui_screen 完全对齐）——面板类 panel_* 前缀、
        // 主菜单面板类 main_menu_* 前缀；对话框类（dialog_*）不属于面板
        private val PANELS = setOf(
            "panel_character_info", "panel_inventory", "panel_skills", "panel_mercenary",
            "panel_quests", "panel_settings", "panel_shop", "panel_craft",
            "panel_npc_rest", "panel_npc_revive", "panel_save_slot", "panel_character_select",
            "panel_options", "panel_shortcut", "panel_world_map",
            "panel_daily_reward", "panel_in_app", "panel_ui_panel",
            "main_menu_save_slot", "main_menu_character_select", "main_menu_daily_reward",
            "main_menu_options", "main_menu_settings"
        )
    }
}
