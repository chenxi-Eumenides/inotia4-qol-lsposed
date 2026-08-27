package com.inotia4.export.controller

import com.inotia4.export.ApiServer
import com.inotia4.export.ModuleConfig
import com.inotia4.export.service.ApiServices
import com.inotia4.export.util.ApiException
import com.inotia4.export.util.JsonUtil
import com.yanzhenjie.andserver.annotation.GetMapping
import com.yanzhenjie.andserver.annotation.PostMapping
import com.yanzhenjie.andserver.annotation.RequestBody
import com.yanzhenjie.andserver.annotation.RestController
import com.yanzhenjie.andserver.http.StatusCode

/**
 * 模块配置：GET /api/config/list + POST /api/config/set（api-reference §7.6）。
 * listenAddress/listenPort 为纯 Kotlin 层能力；stackLimitIncrease 与 moveMergeEnabled
 * 变化时通知 native 生效。
 */
@RestController
class ConfigController {

    @GetMapping("/api/config/list")
    fun list(): String = ModuleConfig.toJson().toString()

    @PostMapping("/api/config/set")
    fun set(@RequestBody body: String): String {
        val json = JsonUtil.parseObj(body) ?: throw ApiException(StatusCode.SC_BAD_REQUEST, "bad request")
        val oldAddress = ModuleConfig.listenAddress
        val oldPort = ModuleConfig.listenPort
        val oldStack = ModuleConfig.stackLimitIncrease
        val oldMoveMerge = ModuleConfig.moveMergeEnabled
        val oldExtensionBag = ModuleConfig.extensionBagEnabled
        val err = ModuleConfig.apply(json)
        if (err != null) throw ApiException(StatusCode.SC_BAD_REQUEST, err)
        val restartNeeded = ModuleConfig.listenAddress != oldAddress || ModuleConfig.listenPort != oldPort
        if (restartNeeded) ApiServer.restartDelayed()
        // v0.5.46 收边：native 直调收口到 ConfigApiService（内部判断 ready + 增量生效）
        ApiServices.config.applyOnChange(oldStack, oldMoveMerge, oldExtensionBag)
        return ModuleConfig.toJson()
            .put("ok", true)
            .put("restart", restartNeeded)
            .toString()
    }
}
