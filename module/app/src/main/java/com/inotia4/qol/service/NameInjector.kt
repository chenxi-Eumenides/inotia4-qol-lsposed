package com.inotia4.qol.service

import com.inotia4.qol.LogFile
import com.inotia4.qol.StaticData
import org.json.JSONArray
import org.json.JSONObject

/**
 * 名称/字段注入助手（v0.5.46 P1 收边，从 InfoApiServiceImpl 抽出）：
 * 7 个 inject* 纯函数式注入 + withItemNames 组装，无状态，仅依赖 StaticData 查询与 LogFile。
 */
object NameInjector {

    // v0.5.1：技能名注入（StaticData.skillName = 技能信息表 rec+0 text_id = 1220+action）
    fun injectSkillNames(role: JSONObject) {
        val skills = role.optJSONArray("skills") ?: return
        for (i in 0 until skills.length()) {
            val sk = skills.optJSONObject(i) ?: continue
            val actionId = sk.optInt("action_id", -1)
            if (actionId >= 0) {
                val name = StaticData.skillName(actionId)
                if (name != null) sk.put("skill_name", name)
            }
        }
    }

    /** 从 QUESTS.json 解析产物注入字段（v0.5.37） */
    fun injectQuestFields(target: JSONObject, data: JSONObject, keys: List<String>) {
        for (key in keys) {
            if (data.has(key)) target.put(key, data.get(key))
        }
    }

    fun injectEquipmentNames(role: JSONObject) {
        val eq = role.optJSONArray("equipment") ?: return
        for (e in 0 until eq.length()) {
            eq.optJSONObject(e)?.let { injectItemName(it, true) }
        }
    }

    /**
     * 装备字段重排：slot/name/category/count/item_type/need_level/rarity/rarity_tier →
     * base{damage,defense,magic_rate}、bonus[{id,name,value}]（词缀 type==0）、
     * gem{total_slots,slots[{id,name,value}]}（宝石 type==1）、chaos{is_chaos,level,rate}、
     * enchant{id,level,effect}。缺失标量置 null、列表置 []；magic_rate 除以 100 显示。
     * 丢弃 type_flags/raw_rarity/位域拆解与旧组装字段
     */
    fun restructureItem(item: JSONObject) {
        val slot = item.opt("slot")
        val name = item.opt("name")
        val category = item.opt("category")
        val count = item.opt("count")
        val cat = (category as? Number)?.toInt() ?: -1
        val needLevel = item.opt("need_level")
        val rarity = item.opt("rarity")
        val rarityTier = item.opt("rarity_tier")
        val damage = item.opt("damage")
        val defense = item.opt("defense")
        val magicRate = item.opt("magic_rate")
        val optionIds = item.optJSONArray("option_ids")
        val options = item.optJSONArray("options")
        val optionTypes = item.optJSONArray("option_types")
        val socketTotal = item.opt("socket_total")
        val chaosFlag = item.opt("chaos")
        val chaosLevel = item.opt("chaos_level")
        val chaosRate = item.opt("chaos_rate")
        val enchantId = item.opt("enchant_id")
        val enchantLevel = item.opt("enchant_level")

        val keys = item.keys().asSequence().toList()
        for (k in keys) item.remove(k)

        item.put("slot", slot ?: JSONObject.NULL)
        item.put("name", name ?: JSONObject.NULL)
        item.put("category", category ?: JSONObject.NULL)
        item.put("count", count ?: JSONObject.NULL)
        // item_type 五类：ITEMDATABASE +2 字节用途类型（装备/药水/卷轴/宝石/消耗品）
        item.put("item_type", StaticData.itemType(cat))
        // need_level 负值（消耗品无等级概念，ITEMCLASSBASE +3=0xFF）归 0
        item.put("need_level", ((needLevel as? Number)?.toInt() ?: 0).coerceAtLeast(0))
        item.put("rarity", rarity ?: JSONObject.NULL)
        item.put("rarity_tier", rarityTier ?: JSONObject.NULL)

        val base = JSONObject()
        base.put("damage", (damage as? Number)?.toInt() ?: JSONObject.NULL)
        base.put("defense", (defense as? Number)?.toInt() ?: JSONObject.NULL)
        // 魔法伤害倍率显示：原始值/100（110 → 1.1）
        base.put("magic_rate", (magicRate as? Number)?.toInt()?.div(100.0) ?: JSONObject.NULL)
        item.put("base", base)

        item.put("bonus", buildOptionList(optionIds, options, optionTypes, type = 0))

        val gem = JSONObject()
        gem.put("total_slots", (socketTotal as? Number)?.toInt() ?: JSONObject.NULL)
        gem.put("slots", buildOptionList(optionIds, options, optionTypes, type = 1))
        item.put("gem", gem)

        val chaos = JSONObject()
        chaos.put("is_chaos", chaosFlag as? Boolean ?: false)
        chaos.put("level", (chaosLevel as? Number)?.toInt() ?: JSONObject.NULL)
        chaos.put("rate", (chaosRate as? Number)?.toInt() ?: JSONObject.NULL)
        item.put("chaos", chaos)

        val eid = (enchantId as? Number)?.toInt()
        val enchant = JSONObject()
        enchant.put("id", eid ?: JSONObject.NULL)
        enchant.put("level", (enchantLevel as? Number)?.toInt() ?: JSONObject.NULL)
        enchant.put("effect", if (eid != null && eid > 0) StaticData.enchantName(eid) ?: JSONObject.NULL else JSONObject.NULL)
        item.put("enchant", enchant)
    }

    private fun buildOptionList(optionIds: JSONArray?, options: JSONArray?, optionTypes: JSONArray?, type: Int): JSONArray {
        val arr = JSONArray()
        val n = optionIds?.length() ?: 0
        for (i in 0 until n) {
            if ((optionTypes?.optInt(i, 0) ?: 0) != type) continue
            val id = optionIds?.optInt(i, -1) ?: -1
            if (id < 0) continue
            val value = options?.optInt(i, 0) ?: 0
            arr.put(JSONObject().put("id", id).put("name", StaticData.optionName(id) ?: "").put("value", value))
        }
        return arr
    }

    fun injectItemName(item: JSONObject, equipOverride: Boolean? = null) {
        val category = item.optInt("category", -1)
        if (category >= 0) {
            val base = StaticData.itemName(category)
            if (base != null) {
                // v0.5.12：装备判定优先显式标记（party 装备路径无 count 字段），否则用 native equip 标志
                //（⑤ count 语义修正后装备 count=1，不能再以 count==100 判定）
                val isEquip = equipOverride ?: item.optBoolean("equip", false)
                // v0.5.12 ③修复：品级前缀用 raw_rarity（原始位 0-15，ITEMGRADEBASE 表），rarity=GetRarity 档位仅用于 tier
                val prefix = if (isEquip) StaticData.rarityPrefix(item.optInt("raw_rarity", -1)) else ""
                item.put("name", prefix + base)
                // v0.5.12 ⑥ 稀有度档位名（GetRarity 0-4 → 白绿蓝黄紫）
                if (item.has("rarity")) {
                    StaticData.rarityTierName(item.optInt("rarity", -1)).takeIf { it.isNotEmpty() }
                        ?.let { item.put("rarity_tier", it) }
                }
                // v0.5.12 ⑦ 静态词条名（ITEMSTATICOPTBASE，item_id=category 聚合）
                val staticOpts = StaticData.staticOptionNames(category)
                if (staticOpts.isNotEmpty()) item.put("static_options", JSONArray(staticOpts))
            }
        }
        injectItemOptions(item)
        injectSocketEnchant(item)
        restructureItem(item)
    }

    // v0.4.64：词缀名称/明细（optionIds 索引数组 + options 值数组，一一对应）
    fun injectItemOptions(item: JSONObject) {
        val optionIds = item.optJSONArray("option_ids")
        if (optionIds == null || optionIds.length() == 0) return
        val options = item.optJSONArray("options")
        val optNames = JSONArray()
        val optDetails = JSONArray()
        for (o in 0 until optionIds.length()) {
            val id = optionIds.optInt(o, -1)
            if (id < 0) continue
            val name = StaticData.optionName(id) ?: ""
            optNames.put(name)
            val value = if (options != null && o < options.length()) options.optInt(o) else 0
            optDetails.put(JSONObject().put("id", id).put("name", name).put("value", value))
        }
        item.put("option_names", optNames)
        item.put("options_detailed", optDetails)
    }

    // v0.4.64：宝石孔/附魔/混沌 拆解信息（native 已输出位域拆解字段，此处组装可读对象）
    fun injectSocketEnchant(item: JSONObject) {
        val hasSocket = item.has("socket_filled") || item.has("socket_total")
        if (hasSocket) {
            val info = JSONObject()
            info.put("filled", item.optInt("socket_filled", 0))
            info.put("total", item.optInt("socket_total", 0))
            item.put("socket_info", info)
        }
        val hasEnchant = item.has("enchant_id") || item.has("enchant_level") || item.has("chaos")
        if (hasEnchant) {
            val info = JSONObject()
            val eid = item.optInt("enchant_id", 0)
            info.put("id", eid)
            info.put("level", item.optInt("enchant_level", 0))
            info.put("chaos", item.optBoolean("chaos", false))
            StaticData.enchantName(eid)?.let { info.put("name", it) }
            item.put("enchant_info", info)
        }
        if (item.has("chaos_level") || item.has("chaos_rate")) {
            val info = JSONObject()
            info.put("level", item.optInt("chaos_level", 0))
            info.put("rate", item.optInt("chaos_rate", 0))
            item.put("chaos_info", info)
        }
    }

    /** 角色类型名注入：type 0=主角 1=佣兵（C_TYPE 偏移；装饰物 type==2 不注入） */
    fun injectTypeName(role: JSONObject) {
        when (role.optInt("type", -1)) {
            0 -> role.put("type_name", "主角")
            1 -> role.put("type_name", "佣兵")
        }
    }

    /** 职业名称注入：class_idx 0-5 → CHARCLASSBASE 联查（StaticData.className，v0.5.1 实机验证） */
    fun injectClassName(role: JSONObject) {
        val classIdx = role.optInt("class_idx", -1)
        // 限定 0-5：type==2 装饰物该字段存 type 值（C_CLASS 注释），非真实职业索引，防误注入
        if (classIdx in 0..5) {
            StaticData.className(classIdx)?.let { role.put("class_name", it) }
        }
    }

    /** Role 字段重排 + main_stats 结构化（v0.6.3）：呈现顺序 name→…→stats；main_stats 为
     *  [{stat_name,base_stat,additional_stat}]；移除 attrs/base_stats/bonus_stats（原始字段归尾部） */
    fun restructureRole(role: JSONObject) {
        val mainStats = role.optJSONArray("main_stats")
        val baseStats = role.optJSONArray("base_stats")
        val orderedKeys = listOf(
            "name", "type_name", "class_name", "level", "hp", "max_hp", "mp", "max_mp",
            "exp", "exp_next", "status_point", "equipment", "type", "class_idx", "name_id", "stats"
        )
        val keep = mutableListOf<Pair<String, Any?>>()
        for (k in orderedKeys) {
            val v = role.opt(k)
            if (v != null && v != JSONObject.NULL) keep.add(k to v)
        }
        val keys = role.keys().asSequence().toList()
        for (k in keys) role.remove(k)
        for ((k, v) in keep) {
            role.put(k, v)
            // main_stats 紧跟 exp_next 之后（F_GET_STAT=总属性=Base+Main+Bonus+Sub，additional=总-基础）
            if (k == "exp_next") role.put("main_stats", buildMainStats(mainStats, baseStats))
        }
        if (keep.none { it.first == "exp_next" }) role.put("main_stats", buildMainStats(mainStats, baseStats))
    }

    private fun buildMainStats(mainStats: JSONArray?, baseStats: JSONArray?): JSONArray {
        val names = listOf("力量", "敏捷", "体力", "智力", "精力")
        val arr = JSONArray()
        for (i in 0 until 5) {
            val total = mainStats?.optInt(i, 0) ?: 0
            val base = baseStats?.optInt(i, 0) ?: 0
            arr.put(JSONObject()
                .put("stat_name", names[i])
                .put("base_stat", base)
                .put("additional_stat", total - base))
        }
        return arr
    }

    fun withItemNames(json: String): String {
        return try {
            val trimmed = json.trimStart()
            if (trimmed.startsWith("[")) {
                val arr = JSONArray(json)
                for (i in 0 until arr.length()) {
                    val role = arr.optJSONObject(i) ?: continue
                    injectTypeName(role)
                    injectClassName(role)
                    injectEquipmentNames(role)
                    restructureRole(role)
                }
                arr.toString()
            } else {
                val root = JSONObject(json)
                if (root.has("class_idx")) {
                    // 单角色对象（party/{slot} 等）：与数组分支同构注入
                    injectTypeName(root)
                    injectClassName(root)
                    injectEquipmentNames(root)
                    restructureRole(root)
                } else if (root.has("bags")) {
                    val bags = root.getJSONArray("bags")
                    for (b in 0 until bags.length()) {
                        val items = bags.getJSONObject(b).optJSONArray("items") ?: continue
                        for (i in 0 until items.length()) {
                            injectItemName(items.getJSONObject(i))
                        }
                    }
                } else if (root.has("party")) {
                    val party = root.optJSONArray("party")
                    if (party != null) {
                        for (p in 0 until party.length()) {
                            val member = party.optJSONObject(p) ?: continue
                            injectTypeName(member)
                            injectClassName(member)
                            injectEquipmentNames(member)
                            restructureRole(member)
                        }
                    }
                }
                root.toString()
            }
        } catch (e: Exception) {
            LogFile.logError("withItemNames failed", e)
            json
        }
    }
}
