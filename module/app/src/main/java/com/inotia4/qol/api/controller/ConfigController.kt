package com.inotia4.qol.api.controller

import com.inotia4.qol.ApiServer
import com.inotia4.qol.ModuleConfig
import com.inotia4.qol.service.ApiServices
import com.inotia4.qol.util.ApiException
import com.inotia4.qol.util.JsonUtil
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
        val oldCustomRecipe = ModuleConfig.customRecipeEnabled
        val oldAutoSell = ModuleConfig.autoSellEnabled
        val oldApiEnabled = ModuleConfig.apiEnabled
        val oldDebugLog = ModuleConfig.debugLogEnabled
        val oldSimpleMode = ModuleConfig.simpleModeEnabled
        val err = ModuleConfig.apply(json)
        if (err != null) throw ApiException(StatusCode.SC_BAD_REQUEST, err)
        // v0.5.46 收边：native 直调收口到 ConfigApiService（内部判断 ready + 增量生效）
        ApiServices.config.applyOnChange(
            oldStack, oldMoveMerge, oldExtensionBag, oldCustomRecipe, oldAutoSell, oldApiEnabled,
            oldDebugLog, oldSimpleMode
        )
        val restartNeeded = ModuleConfig.listenAddress != oldAddress || ModuleConfig.listenPort != oldPort
        val apiEnabledChanged = ModuleConfig.apiEnabled != oldApiEnabled
        // apiEnabled 变更优先：关闭走延迟停止（先送回响应），开启走启动入口（HTTP 不可达时不会走到这）
        when {
            apiEnabledChanged && !ModuleConfig.apiEnabled -> ApiServer.stopDelayed()
            apiEnabledChanged && ModuleConfig.apiEnabled -> ApiServer.startFromConfig()
            restartNeeded -> ApiServer.restartDelayed()
        }
        return ModuleConfig.toJson()
            .put("ok", true)
            .put("restart", restartNeeded)
            .toString()
    }
}
