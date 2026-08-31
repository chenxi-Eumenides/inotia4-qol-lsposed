package com.inotia4.qol

import android.content.Context
import org.json.JSONArray
import org.json.JSONObject

object StaticData {

    @Volatile
    private var appContext: Context? = null

    private val cache = HashMap<String, String>()

    @Volatile
    private var itemNames: Map<Int, String>? = null

    @Volatile
    private var optionNames: Map<Int, String>? = null

    fun attach(context: Context) {
        appContext = context
    }

    /** 游戏数据目录（/data/data/<包名>/，v0.5.12 P0-2 存档导出定位用） */
    fun dataDir(): String? = appContext?.applicationInfo?.dataDir

    fun read(path: String): String? {
        cache[path]?.let { return it }
        val ctx = appContext ?: return null
        val content = try {
            ctx.assets.open("static-data/$path").bufferedReader().use { it.readText() }
        } catch (e: Exception) {
            return null
        }
        if (cache.size < 64) cache[path] = content
        return content
    }

    fun itemName(itemId: Int): String? {
        val m = itemNames ?: synchronized(this) {
            itemNames ?: buildItemNames().also { itemNames = it }
        }
        return m[itemId]
    }

    /** 品质前缀（v0.4.62 frida 真机 rarity 0-15 实测）：rarity 位 → 品级前缀 */
    fun rarityPrefix(rarity: Int): String = when (rarity) {
        0 -> "生锈的 "
        1 -> "陈旧的 "
        3 -> "太古的 "
        4 -> "锐利的 "
        5 -> "打磨的 "
        6 -> "工匠的 "
        7 -> "钢铁 "
        8 -> "钛金 "
        9 -> "秘银 "
        else -> ""
    }

    /** 稀有度档位名（v0.5.12 ⑥，用户确认游戏仅 5 档）：ITEMSYSTEM_GetRarity 返回 0-4 → 白/绿/蓝/黄/紫 */
    fun rarityTierName(tier: Int): String = when (tier) {
        0 -> "白"
        1 -> "绿"
        2 -> "蓝"
        3 -> "黄"
        4 -> "紫"
        else -> ""
    }

    /** 词缀名称（v0.4.62 静态表解析）：词缀 id → text 表名称（力/敏/体/智...） */
    fun optionName(optionId: Int): String? {
        val m = optionNames ?: synchronized(this) {
            optionNames ?: buildOptionNames().also { optionNames = it }
        }
        return m[optionId]
    }

    private fun buildItemNames(): Map<Int, String> {
        val json = read("tables/ITEMDATABASE.json") ?: return emptyMap()
        return try {
            val records = JSONObject(json).getJSONArray("records")
            val map = HashMap<Int, String>(records.length())
            for (i in 0 until records.length()) {
                map[i] = records.getJSONObject(i).optString("text_0", "")
            }
            map
        } catch (e: Exception) {
            emptyMap()
        }
    }

    private fun buildOptionNames(): Map<Int, String> {
        val json = read("tables/ITEMOPTINFOBASE.json") ?: return emptyMap()
        return try {
            val records = JSONObject(json).getJSONArray("records")
            val text = read("text/zh-Hans.json") ?: return emptyMap()
            val textArr = JSONArray(text)
            val map = HashMap<Int, String>(records.length())
            for (i in 0 until records.length()) {
                val u16 = records.getJSONObject(i).optJSONArray("u16") ?: continue
                val textId = u16.optInt(0, -1)
                if (textId in 0 until textArr.length()) {
                    val name = textArr.optString(textId)
                    if (name.isNotEmpty()) map[i] = name
                }
            }
            map
        } catch (e: Exception) {
            emptyMap()
        }
    }

    /**
     * 附魔/强化名称（2026-08-12）：enchantId → ITEMENCHANTBASE 记录(enchantId-1) +0x00 卷轴类别 →
     * ITEMDATABASE 物品名。enchantId=0（无附魔）或无记录时返回 null。
     */
    fun enchantName(enchantId: Int): String? {
        if (enchantId <= 0) return null
        val m = enchantNames ?: synchronized(this) {
            enchantNames ?: buildEnchantNames().also { enchantNames = it }
        }
        return m[enchantId]
    }

    @Volatile
    private var enchantNames: Map<Int, String>? = null

    private fun buildEnchantNames(): Map<Int, String> {
        val ench = read("tables/ITEMENCHANTBASE.json") ?: return emptyMap()
        return try {
            val records = JSONObject(ench).getJSONArray("records")
            val map = HashMap<Int, String>(records.length())
            for (i in 0 until records.length()) {
                val u16 = records.getJSONObject(i).optJSONArray("u16") ?: continue
                val scrollCat = u16.optInt(0, -1)
                if (scrollCat >= 0) {
                    val name = itemName(scrollCat) ?: continue
                    map[i + 1] = name
                }
            }
            map
        } catch (e: Exception) {
            emptyMap()
        }
    }

    @Volatile
    private var staticOptions: Map<Int, List<String>>? = null

    /**
     * 静态词条名（v0.5.12 ⑦，v0.5.34 修复 key 映射）：ITEMSTATICOPTBASE 记录 +2 u16 低字节 = 词缀索引（0-35，与 ITEMOPTINFOBASE 对齐），
     * 按 item_id 聚合。item_id = ITEMDATABASE u16[0]（物品 id，全 1018 条恒 = 记录下标+30），而查询键 itemId = category = ITEMDATABASE
     * 记录下标——两套键体系差 30，v0.5.34 起 buildStaticOptions 用 id→下标映射转回记录下标作 key（此前直接用 item_id 作 key，
     * 短剑等 category<75 物品查不到词条、category≥75 物品查到错位物品的词条）。无记录返回空表。
     */
    fun staticOptionNames(itemId: Int): List<String> {
        val m = staticOptions ?: synchronized(this) {
            staticOptions ?: buildStaticOptions().also { staticOptions = it }
        }
        return m[itemId] ?: emptyList()
    }

    private fun buildStaticOptions(): Map<Int, List<String>> {
        val json = read("tables/ITEMSTATICOPTBASE.json") ?: return emptyMap()
        return try {
            // item_id（ITEMDATABASE u16[0] 物品 id）→ 记录下标（category）映射，桥接两套键体系（v0.5.34 修复）
            val idToIndex = buildItemIdToIndex()
            val records = JSONObject(json).getJSONArray("records")
            val map = HashMap<Int, List<String>>(records.length())
            for (i in 0 until records.length()) {
                val u16 = records.getJSONObject(i).optJSONArray("u16") ?: continue
                val itemId = u16.optInt(0, -1)
                val code = u16.optInt(1, -1)
                if (itemId < 0 || code < 0) continue
                val index = idToIndex[itemId] ?: continue
                val optIdx = code and 0xFF
                val name = optionName(optIdx) ?: continue
                val list = map[index] ?: ArrayList<String>()
                if (name !in list) (list as ArrayList<String>).add(name)
                map[index] = list
            }
            map
        } catch (e: Exception) {
            emptyMap()
        }
    }

    @Volatile
    private var itemUseTypes: Map<Int, Int>? = null

    /** 物品类型五类（ITEMDATABASE 记录 +2 字节 = 用途类型，2026-08-17 逆向全量核实）：
     *  equipment(0-21 装备) / potion(22-23 药水) / scroll(24 卷轴) / gem(25 宝石) / consumable(其余消耗/材料/任务/货币)。 */
    fun itemType(category: Int): String = when (itemUseType(category)) {
        in 0..0x15 -> "equipment"
        0x16, 0x17 -> "potion"
        0x18 -> "scroll"
        0x19 -> "gem"
        else -> "consumable"
    }

    private fun itemUseType(category: Int): Int {
        val m = itemUseTypes ?: synchronized(this) {
            itemUseTypes ?: buildItemUseTypes().also { itemUseTypes = it }
        }
        return m[category] ?: -1
    }

    private fun buildItemUseTypes(): Map<Int, Int> {
        val json = read("tables/ITEMDATABASE.json") ?: return emptyMap()
        return try {
            val records = JSONObject(json).getJSONArray("records")
            val map = HashMap<Int, Int>(records.length())
            for (i in 0 until records.length()) {
                val u16 = records.getJSONObject(i).optJSONArray("u16") ?: continue
                if (u16.length() > 1) map[i] = u16.optInt(1, 0) and 0xFF
            }
            map
        } catch (e: Exception) {
            emptyMap()
        }
    }

    /** ITEMDATABASE u16[0]（物品 id，= 记录下标+30，2026-08-16 全 1018 条核实恒等）→ 记录下标（category）映射 */
    private fun buildItemIdToIndex(): Map<Int, Int> {
        val json = read("tables/ITEMDATABASE.json") ?: return emptyMap()
        return try {
            val records = JSONObject(json).getJSONArray("records")
            val map = HashMap<Int, Int>(records.length())
            for (i in 0 until records.length()) {
                val u16 = records.getJSONObject(i).optJSONArray("u16") ?: continue
                val id = u16.optInt(0, -1)
                if (id >= 0) map[id] = i
            }
            map
        } catch (e: Exception) {
            emptyMap()
        }
    }

    // ---- v0.5.1 联查函数（N5，研究依据见 docs/research/character-data-gaps.md）----

    @Volatile
    private var classNames: Map<Int, String>? = null

    /** 职业名称（v0.5.1 实机验证）：CHARCLASSBASE u16[0] = class_idx×2 → text 表（黑魔导=4→黑魔导） */
    fun className(classIdx: Int): String? {
        val m = classNames ?: synchronized(this) {
            classNames ?: buildClassNames().also { classNames = it }
        }
        return m[classIdx]
    }

    private fun buildClassNames(): Map<Int, String> {
        val json = read("tables/CHARCLASSBASE.json") ?: return emptyMap()
        return try {
            val records = JSONObject(json).getJSONArray("records")
            val text = read("text/zh-Hans.json") ?: return emptyMap()
            val textArr = JSONArray(text)
            val map = HashMap<Int, String>(records.length())
            for (i in 0 until records.length()) {
                val u16 = records.getJSONObject(i).optJSONArray("u16") ?: continue
                val textId = u16.optInt(0, -1)
                if (textId in 0 until textArr.length()) {
                    val name = textArr.optString(textId)
                    if (name.isNotEmpty()) map[i] = name
                }
            }
            map
        } catch (e: Exception) {
            emptyMap()
        }
    }

    /** 技能名称（v0.5.1 实机验证）：技能信息表 recN↔action N，rec+0 = text_id = 1220+action；普攻 action<20 段为空 */
    fun skillName(actionId: Int): String? {
        if (actionId < 0) return null
        val text = read("text/zh-Hans.json") ?: return null
        return try {
            val textArr = JSONArray(text)
            val textId = 1220 + actionId
            if (textId in 0 until textArr.length()) {
                textArr.optString(textId).takeIf { it.isNotEmpty() }
            } else null
        } catch (e: Exception) {
            null
        }
    }

    @Volatile
    private var mercNames: Map<Int, String>? = null

    /** 佣兵名称（v0.5.1 实机验证）：MERCENARYINFOBASE u16[2] = 35752+idx → text 表（47 名全量验证） */
    fun mercName(mercIdx: Int): String? {
        val m = mercNames ?: synchronized(this) {
            mercNames ?: buildMercNames().also { mercNames = it }
        }
        return m[mercIdx]
    }

    private fun buildMercNames(): Map<Int, String> {
        val json = read("tables/MERCENARYINFOBASE.json") ?: return emptyMap()
        return try {
            val records = JSONObject(json).getJSONArray("records")
            val text = read("text/zh-Hans.json") ?: return emptyMap()
            val textArr = JSONArray(text)
            val map = HashMap<Int, String>(records.length())
            for (i in 0 until records.length()) {
                val u16 = records.getJSONObject(i).optJSONArray("u16") ?: continue
                val textId = u16.optInt(2, -1)
                if (textId in 0 until textArr.length()) {
                    val name = textArr.optString(textId)
                    if (name.isNotEmpty()) map[i] = name
                }
            }
            map
        } catch (e: Exception) {
            emptyMap()
        }
    }

    @Volatile
    private var quests: Map<Int, JSONObject>? = null

    /**
     * 任务全量语义数据（v0.5.37）：QUESTS.json 解析产物按 quest_id 索引。字段含
     * group_id/group_name/name/detail/accepted_dialog(接取后对话)/delivered_dialog(交付对话)/
     * class_req/reward_hint/rewards/is_mainline(主线=组bit0)/is_side/hidden/side_flag/raw_u16
     * （文本已去 $S/$B/$R 颜色标签）。
     */
    fun questData(questId: Int): JSONObject? {
        val m = quests ?: synchronized(this) {
            quests ?: buildQuests().also { quests = it }
        }
        return m[questId]
    }

    /** 任务名称（v0.5.37 改从 QUESTS.json 解析产物取，文本已去色） */
    fun questName(questId: Int): String? =
        questData(questId)?.optString("name")?.takeIf { it.isNotEmpty() }

    private fun buildQuests(): Map<Int, JSONObject> {
        val json = read("tables/QUESTS.json") ?: return emptyMap()
        return try {
            val records = JSONObject(json).getJSONArray("records")
            val map = HashMap<Int, JSONObject>(records.length())
            for (i in 0 until records.length()) {
                val q = records.getJSONObject(i)
                val qid = q.optInt("quest_id", -1)
                if (qid >= 0) map[qid] = q
            }
            map
        } catch (e: Exception) {
            emptyMap()
        }
    }

    @Volatile
    private var mapExitsRoot: JSONObject? = null

    /** 地图出口（静态数据 maps/exits.json）：按 mapId 取 exits 数组，每项 {x, y 瓦片坐标, targetMapId, targetX, targetY} */
    fun mapExits(mapId: Int): JSONArray? {
        val root = mapExitsRoot ?: synchronized(this) {
            if (mapExitsRoot == null) {
                mapExitsRoot = try {
                    val s = read("maps/exits.json")
                    if (s != null) JSONObject(s) else null
                } catch (e: Exception) {
                    null
                }
            }
            mapExitsRoot
        }
        return root?.optJSONObject("m$mapId")?.optJSONArray("exits")
    }
}
