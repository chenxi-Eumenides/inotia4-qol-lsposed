package com.inotia4.qol.service.info

import com.inotia4.qol.NativeBridge
import com.inotia4.qol.StaticData
import com.inotia4.qol.util.JsonUtil
import org.json.JSONArray
import org.json.JSONObject

/** Map query endpoints. Keeps map data lookup separate from the service facade. */
internal object InfoMapQueries {
    fun id(): String {
        val json = NativeBridge.nativeGetMapJson()
        if (JsonUtil.parseObj(json)?.has("error") == true) return json
        val mapId = JsonUtil.parseObj(json)?.optInt("map_id", -1) ?: -1
        val out = JSONObject().put("map_id", mapId)
        static(mapId)?.let { out.put("id_name", it.optString("name", "")) }
        return out.toString()
    }

    fun exits(): String {
        val json = NativeBridge.nativeGetMapJson()
        if (JsonUtil.parseObj(json)?.has("error") == true) return json
        val mapId = JsonUtil.parseObj(json)?.optInt("map_id", -1) ?: -1
        return JsonUtil.wrap("exits", StaticData.mapExits(mapId) ?: JSONArray())
    }

    fun units(): String = pass(NativeBridge.nativeGetUnitsJson())
    fun enemies(): String = pass(NativeBridge.nativeGetEnemiesJson())
    fun interactives(): String = pass(NativeBridge.nativeGetInteractivesJson())
    fun drops(): String = pass(NativeBridge.nativeGetDropsJson())
    fun distance(tx: Int, ty: Int): String = NativeBridge.nativeDistanceJson(tx, ty)

    private fun pass(json: String): String = json

    private fun static(mapId: Int): JSONObject? {
        if (mapId < 0) return null
        val tables = JsonUtil.parseObj(StaticData.read("tables/MAPINFOBASE.json")) ?: return null
        val records = tables.optJSONArray("records") ?: return null
        if (mapId >= records.length()) return null
        val record = records.optJSONObject(mapId) ?: return null
        return JSONObject()
            .put("text_id", record.optJSONArray("u16")?.optInt(0, -1) ?: -1)
            .put("name", record.optString("text_0", ""))
            .also {
                record.optJSONArray("u16")?.let { values -> it.put("u16", values) }
                val hex = record.optString("hex", "")
                if (hex.isNotEmpty()) it.put("hex", hex)
            }
    }
}
