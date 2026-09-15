package com.inotia4.qol

import com.inotia4.qol.service.ApiServices
import org.json.JSONObject

/**
 * 模块设置 UI 的 native→Kotlin 配置桥接（ui-settings v0.6.9）。
 * 游戏主循环线程（native 侧经 JNI 反调）读取/翻转配置：
 * - [getConfigJson]：面板打开时下拉当前配置快照（布尔项 + 监听地址/端口 + moduleVersion）
 * - [toggleConfig]：点击开关翻转对应布尔配置（持久化 + 增量下发 native）
 * 只允许翻转布尔项；listenAddress/listenPort/moduleVersion 为只读。
 * moduleVersion 只用于面板顶部版本标题，不落盘 config.json（故不在 ModuleConfig.toJson 中）。
 */
object ModuleConfigUiBridge {

    private val BOOL_KEYS = setOf(
        "stackLimitIncrease", "moveMergeEnabled", "opEnabled", "extensionBagEnabled", "gemCraftOptimize",
        "customRecipeEnabled",
        "autoSellEnabled", "apiEnabled", "debugLogEnabled",
        "simpleModeEnabled"
    )

    @JvmStatic
    fun getConfigJson(): String {
        val json = ModuleConfig.toJson()
        // moduleVersion 仅注入这份 UI 快照供面板顶部标题使用，不进入持久化的 ModuleConfig.toJson()。
        json.put("moduleVersion", BuildConfig.VERSION_NAME)
        return json.toString()
    }

    @JvmStatic
    fun toggleConfig(key: String): String {
        if (key !in BOOL_KEYS) return "error:not_boolean"
        val oldStack = ModuleConfig.stackLimitIncrease
        val oldMoveMerge = ModuleConfig.moveMergeEnabled
        val oldExtensionBag = ModuleConfig.extensionBagEnabled
        val oldGemCraft = ModuleConfig.gemCraftOptimize
        val oldCustomRecipe = ModuleConfig.customRecipeEnabled
        val oldAutoSell = ModuleConfig.autoSellEnabled
        val oldApiEnabled = ModuleConfig.apiEnabled
        val oldDebugLog = ModuleConfig.debugLogEnabled
        val oldSimpleMode = ModuleConfig.simpleModeEnabled
        val current = when (key) {
            "stackLimitIncrease" -> ModuleConfig.stackLimitIncrease
            "moveMergeEnabled" -> ModuleConfig.moveMergeEnabled
            "opEnabled" -> ModuleConfig.opEnabled
            "extensionBagEnabled" -> ModuleConfig.extensionBagEnabled
            "gemCraftOptimize" -> ModuleConfig.gemCraftOptimize
            "customRecipeEnabled" -> ModuleConfig.customRecipeEnabled
            "autoSellEnabled" -> ModuleConfig.autoSellEnabled
            "apiEnabled" -> ModuleConfig.apiEnabled
            "debugLogEnabled" -> ModuleConfig.debugLogEnabled
            "simpleModeEnabled" -> ModuleConfig.simpleModeEnabled
            else -> {
                // BOOL_KEYS 与分支必须逐项同步：未来新增 key 未接线时显式失败，
                // 杜绝静默落到某个既有开关（历史上 opEnabled 曾靠 else 兜底）。
                LogFile.warn(LogDomain.CONFIG, "toggleConfig unsupported key=$key")
                return "error:unsupported_key"
            }
        }
        val json = JSONObject()
        json.put(key, !current)
        val err = ModuleConfig.apply(json)
        if (err != null) return "error:$err"
        ApiServices.config.applyOnChange(
            oldStack, oldMoveMerge, oldExtensionBag, oldGemCraft, oldCustomRecipe, oldAutoSell, oldApiEnabled,
            oldDebugLog, oldSimpleMode
        )
        // apiEnabled 变更时启停 HTTP 服务；本方法经 JNI 在游戏主线程调用，
        // 启动路径较重（静态数据/服务构建），放后台线程避免卡顿。
        if (ModuleConfig.apiEnabled != oldApiEnabled) {
            if (ModuleConfig.apiEnabled) {
                Thread { ApiServer.startFromConfig() }.start()
            } else {
                ApiServer.stopDelayed()
            }
        }
        return "ok"
    }
}
