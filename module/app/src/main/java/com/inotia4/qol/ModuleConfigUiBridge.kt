package com.inotia4.qol

import com.inotia4.qol.service.ApiServices
import org.json.JSONObject

/**
 * 模块设置 UI 的 native→Kotlin 配置桥接（ui-settings v0.6.9）。
 * 游戏主循环线程（native 侧经 JNI 反调）读取/翻转配置：
 * - [getConfigJson]：面板打开时下拉当前配置快照（3 布尔 + 监听地址/端口）
 * - [toggleConfig]：点击开关翻转对应布尔配置（持久化 + 增量下发 native）
 * 只允许翻转布尔项；listenAddress/listenPort 为只读。
 */
object ModuleConfigUiBridge {

    private val BOOL_KEYS = setOf("stackLimitIncrease", "moveMergeEnabled", "opEnabled", "extensionBagEnabled")

    @JvmStatic
    fun getConfigJson(): String = ModuleConfig.toJson().toString()

    @JvmStatic
    fun toggleConfig(key: String): String {
        if (key !in BOOL_KEYS) return "error:not_boolean"
        val oldStack = ModuleConfig.stackLimitIncrease
        val oldMoveMerge = ModuleConfig.moveMergeEnabled
        val oldExtensionBag = ModuleConfig.extensionBagEnabled
        val current = when (key) {
            "stackLimitIncrease" -> ModuleConfig.stackLimitIncrease
            "moveMergeEnabled" -> ModuleConfig.moveMergeEnabled
            "extensionBagEnabled" -> ModuleConfig.extensionBagEnabled
            else -> ModuleConfig.opEnabled
        }
        val json = JSONObject()
        json.put(key, !current)
        val err = ModuleConfig.apply(json)
        if (err != null) return "error:$err"
        ApiServices.config.applyOnChange(oldStack, oldMoveMerge, oldExtensionBag)
        return "ok"
    }
}
