package com.inotia4.qol.api.controller

import com.inotia4.qol.service.info.NativeHttpQuery
import com.yanzhenjie.andserver.annotation.GetMapping
import com.yanzhenjie.andserver.annotation.PathVariable
import com.yanzhenjie.andserver.annotation.RequestParam
import com.yanzhenjie.andserver.annotation.RestController
import java.net.URLEncoder

@RestController
class DataController {

    @GetMapping("/api/world/maps/list")
    fun mapList(): String = NativeHttpQuery.get("/api/world/maps/list")

    @GetMapping("/api/world/maps/{map_id}")
    fun mapDetail(@PathVariable("map_id") mapId: Int): String =
        NativeHttpQuery.get("/api/world/maps/$mapId")

    @GetMapping("/api/world/maps/{map_id}/tiles")
    fun mapTiles(@PathVariable("map_id") mapId: Int): String =
        NativeHttpQuery.get("/api/world/maps/$mapId/tiles")

    @GetMapping("/api/world/maps/{map_id}/exits")
    fun mapExits(@PathVariable("map_id") mapId: Int): String =
        NativeHttpQuery.get("/api/world/maps/$mapId/exits")

    @GetMapping("/api/system/tables")
    fun list(): String = NativeHttpQuery.get("/api/system/tables")

    @GetMapping("/api/system/tables/{table}")
    fun table(@PathVariable("table") table: String): String =
        NativeHttpQuery.get("/api/system/tables/${table.uppercase()}")

    @GetMapping("/api/system/tables/{table}/search")
    fun search(@PathVariable("table") table: String, @RequestParam("q") q: String): String =
        NativeHttpQuery.get(
            "/api/system/tables/${table.uppercase()}/search",
            "q=${URLEncoder.encode(q, "UTF-8")}",
        )

    // ⏳ 占位：tables/{table}/download（api-reference §7.4，暂不实现，后续文件流输出）
    @GetMapping("/api/system/tables/{table}/download")
    fun download(@PathVariable("table") table: String): String =
        NativeHttpQuery.get("/api/system/tables/${table.uppercase()}/download")

    @GetMapping("/api/system/tables/story-events")
    fun events(): String = NativeHttpQuery.get("/api/system/tables/story-events")

    @GetMapping("/api/system/tables/text")
    fun text(@RequestParam("lang") lang: String): String =
        NativeHttpQuery.get("/api/system/tables/text", "lang=${URLEncoder.encode(lang, "UTF-8")}")

    // ⏳ 占位：/api/system/help（api-reference §7.5，帮助文档内容待提供）
    @GetMapping("/api/system/help")
    fun help(): String = NativeHttpQuery.get("/api/system/help")

    // ⏳ 占位：/api/system/download（api-reference §7.5，文件格式待定）
    @GetMapping("/api/system/download")
    fun downloadAll(): String = NativeHttpQuery.get("/api/system/download")
}
