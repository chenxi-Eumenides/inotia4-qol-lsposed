package com.inotia4.qol.service.info

import com.inotia4.qol.NativeBridge
import com.inotia4.qol.StaticData
import com.inotia4.qol.util.JsonUtil
import org.json.JSONObject

/** Shop query and static item-name enrichment. */
internal object InfoShopQueries {
    fun items(): String {
        val json = NativeBridge.nativeShopItems()
        if (JsonUtil.parseObj(json)?.has("error") == true) return json
        return try {
            val root = JSONObject(json)
            val items = root.optJSONArray("items") ?: return json
            for (i in 0 until items.length()) {
                val item = items.optJSONObject(i) ?: continue
                val category = item.optInt("category", -1)
                if (category >= 0) StaticData.itemName(category)?.let { name -> item.put("name", name) }
            }
            root.toString()
        } catch (e: Exception) {
            json
        }
    }
}
