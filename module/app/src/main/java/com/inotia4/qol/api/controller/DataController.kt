package com.inotia4.qol.api.controller

import android.util.Base64
import com.inotia4.qol.StaticData
import com.inotia4.qol.util.ApiException
import com.inotia4.qol.util.JsonUtil
import com.yanzhenjie.andserver.annotation.GetMapping
import com.yanzhenjie.andserver.annotation.PathVariable
import com.yanzhenjie.andserver.annotation.RequestParam
import com.yanzhenjie.andserver.annotation.RestController
import com.yanzhenjie.andserver.http.StatusCode
import org.json.JSONArray
import org.json.JSONObject

@RestController
class DataController {

    private fun notFound(): Nothing = throw ApiException(StatusCode.SC_NOT_FOUND, "not found")

    private fun notImpl(): Nothing = throw ApiException(StatusCode.SC_NOT_IMPLEMENTED, "not implemented")

    @GetMapping("/api/world/maps/list")
    fun mapList(): String {
        val tables = JsonUtil.parseObj(StaticData.read("tables/MAPINFOBASE.json")) ?: notFound()
        val records = tables.optJSONArray("records") ?: notFound()
        val list = JSONArray()
        for (i in 0 until records.length()) {
            val r = records.optJSONObject(i) ?: continue
            val u16 = r.optJSONArray("u16")
            val mapId = u16?.optInt(0, -1) ?: -1
            if (mapId < 0) continue
            list.put(JSONObject().put("map_id", mapId).put("name", r.optString("text_0", "")))
        }
        return JsonUtil.wrap("maps", list)
    }

    @GetMapping("/api/world/maps/{map_id}")
    fun mapDetail(@PathVariable("map_id") mapId: Int): String {
        val tables = JsonUtil.parseObj(StaticData.read("tables/MAPINFOBASE.json")) ?: notFound()
        val records = tables.optJSONArray("records") ?: notFound()
        // v0.4.28：真 mapId = MAPINFOBASE 记录下标（运行时 current_map_id 验证：30=影子丛林1/31=影子丛林2）
        if (mapId in 0 until records.length()) {
            val r = records.optJSONObject(mapId) ?: notFound()
            val textId = r.optJSONArray("u16")?.optInt(0, -1) ?: -1
            return JsonUtil.wrap("map_id" to mapId, "text_id" to textId,
                "name" to r.optString("text_0", ""), "raw" to r)
        }
        // 兼容旧语义：按 text_id 匹配
        for (i in 0 until records.length()) {
            val r = records.optJSONObject(i) ?: continue
            val u16 = r.optJSONArray("u16")
            if (u16?.optInt(0, -1) == mapId) {
                return JsonUtil.wrap("map_id" to mapId, "index" to i, "text_id" to mapId,
                    "name" to r.optString("text_0", ""), "raw" to r)
            }
        }
        return notFound()
    }

    @GetMapping("/api/world/maps/{map_id}/tiles")
    fun mapTiles(@PathVariable("map_id") mapId: Int): String {
        val tiles = JsonUtil.parseObj(StaticData.read("maps/tiles.json")) ?: notFound()
        val entry = tiles.optJSONObject("m$mapId") ?: notFound()
        val raw = entry.optString("tiles", "")
        if (raw.isEmpty()) notFound()
        return try {
            val bytes = Base64.decode(raw, Base64.DEFAULT)
            val rows = JSONArray()
            for (y in 0 until 64) {
                val row = JSONArray()
                for (x in 0 until 64) row.put(bytes[y * 64 + x].toInt() and 0xFF)
                rows.put(row)
            }
            JsonUtil.wrap(
                "map_id" to mapId,
                "src" to "static",
                "width" to entry.optInt("width", 64),
                "height" to entry.optInt("height", 64),
                "size" to 64,
                "encoding" to "array",
                "tiles" to rows
            )
        } catch (t: Throwable) {
            notFound()
        }
    }

    @GetMapping("/api/world/maps/{map_id}/exits")
    fun mapExits(@PathVariable("map_id") mapId: Int): String {
        val exits = StaticData.mapExits(mapId) ?: notFound()
        return JsonUtil.wrap(
            "map_id" to mapId,
            "src" to "static",
            "exits" to exits
        )
    }

    @GetMapping("/api/system/tables")
    fun list(): String {
        val manifest = JsonUtil.parseObj(StaticData.read("manifest.json")) ?: notFound()
        return JsonUtil.wrap("tables", manifest.optJSONArray("tables") ?: JSONArray())
    }

    @GetMapping("/api/system/tables/{table}")
    fun table(@PathVariable("table") table: String): String =
        readTable(table.uppercase())

    @GetMapping("/api/system/tables/{table}/search")
    fun search(@PathVariable("table") table: String, @RequestParam("q") q: String): String =
        searchTable(table.uppercase(), q)

    // ⏳ 占位：tables/{table}/download（api-reference §7.4，暂不实现，后续文件流输出）
    @GetMapping("/api/system/tables/{table}/download")
    fun download(@PathVariable("table") table: String): String = notImpl()

    @GetMapping("/api/system/tables/story-events")
    fun events(): String = StaticData.read("reverse/events.json") ?: notFound()

    @GetMapping("/api/system/tables/text")
    fun text(@RequestParam("lang") lang: String): String =
        StaticData.read("text/${lang}.json") ?: notFound()

    // ⏳ 占位：/api/system/help（api-reference §7.5，帮助文档内容待提供）
    @GetMapping("/api/system/help")
    fun help(): String = notImpl()

    // ⏳ 占位：/api/system/download（api-reference §7.5，文件格式待定）
    @GetMapping("/api/system/download")
    fun downloadAll(): String = notImpl()

    private fun readTable(name: String): String =
        StaticData.read("tables/${name}.json") ?: notFound()

    private fun searchTable(name: String, q: String): String {
        val json = StaticData.read("tables/${name}.json") ?: notFound()
        val tables = JsonUtil.parseObj(json) ?: notFound()
        val records = tables.optJSONArray("records") ?: notFound()
        val keyword = q.trim().lowercase()
        if (keyword.isEmpty()) return JsonUtil.wrap("items", records)
        val out = JSONArray()
        for (i in 0 until records.length()) {
            val r = records.optJSONObject(i) ?: continue
            val name = r.optString("text_0", "")
            if (name.lowercase().contains(keyword)) {
                out.put(JSONObject().put("index", i).put("name", name).put("raw", r))
            }
        }
        return JsonUtil.wrap("items", out)
    }
}
