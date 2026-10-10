package com.inotia4.qol.api.controller

import com.inotia4.qol.service.info.NativeHttpCommand
import com.inotia4.qol.service.info.NativeHttpQuery
import com.yanzhenjie.andserver.annotation.GetMapping
import com.yanzhenjie.andserver.annotation.PathVariable
import com.yanzhenjie.andserver.annotation.PostMapping
import com.yanzhenjie.andserver.annotation.RequestBody
import com.yanzhenjie.andserver.annotation.RestController

@RestController
class ConfigController {
    @GetMapping("/api/config/list")
    fun list(): String = NativeHttpQuery.get("/api/config/list")

    @PostMapping("/api/config/set")
    fun set(@RequestBody body: String): String = NativeHttpCommand.post("/api/config/set", body)

    @GetMapping("/api/config/receipt/{id}")
    fun receipt(@PathVariable("id") id: Long): String =
        NativeHttpQuery.get("/api/config/receipt/$id")
}
