package com.inotia4.qol

import com.inotia4.qol.service.ApiServices
import org.json.JSONObject
import java.util.concurrent.Executors
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicLong

/**
 * 模块设置 UI 的 native→Kotlin 配置桥接（ui-settings v0.6.9）。
 * 游戏主循环线程（native 侧经 JNI 反调）读取/翻转配置：
 * - [getConfigJson]：面板打开时下拉当前配置快照（布尔项 + 监听地址/端口 + moduleVersion）
 * - [toggleConfig]：点击开关翻转对应布尔配置（持久化 + 增量下发 native）
 * 只允许翻转布尔项；listenAddress/listenPort/moduleVersion 为只读。
 * moduleVersion 只用于面板顶部版本标题，不落盘 config.json（故不在 ModuleConfig.toJson 中）。
 *
 * 持久配置桥（settings_config v1.0）Kotlin 侧：
 * - [initSettingsConfigBridge]：native ready 后注册 enqueue 反调类并发布首次镜像
 *   （read_bool 依赖 revision>0；调用点 ApiServer.bootstrap 的 ModuleConfig.load 之后）。
 * - [enqueueSettingsConfigRequest]：native 桥工作线程的排队回调，仅做入队准许，
 *   实际执行在单线程 writer（ModuleConfig.apply → applyOnChange → 镜像重发 → 回执）。
 * - 回执语义：succeeded 只确认持久化 + setter 接受；apiEnabled 的监听启停是异步调度，
 *   回 apply_state=kPlatformPending，不确认监听就绪。
 */
object ModuleConfigUiBridge {

    // 镜像键序必须与 native 侧 kKeys（host/settings_config_bridge.cpp）逐位一致。
    private val MIRROR_KEYS = listOf(
        "stackLimitIncrease", "moveMergeEnabled", "opEnabled", "extensionBagEnabled",
        "customRecipeEnabled",
        "autoSellEnabled", "apiEnabled", "debugLogEnabled",
        "simpleModeEnabled", "pushAllEnabled"
    )

    private val BOOL_KEYS = MIRROR_KEYS.toSet()

    // apply_state 取值与 contracts/settings_config.h 的 SettingsConfigApplyState 枚举逐位一致。
    private const val APPLY_NOT_SUBMITTED = 0
    private const val APPLY_APPLIED = 2
    private const val APPLY_REJECTED = 3
    private const val APPLY_PLATFORM_PENDING = 4
    private const val APPLY_UNCHANGED = 5

    private val bridgeInitialized = AtomicBoolean(false)
    private val revision = AtomicLong(0)
    private val writer = Executors.newSingleThreadExecutor { runnable ->
        Thread(runnable, "settings-config-writer").apply { isDaemon = true }
    }

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
        val oldPushAll = ModuleConfig.pushAllEnabled
        val current = when (key) {
            "stackLimitIncrease" -> ModuleConfig.stackLimitIncrease
            "moveMergeEnabled" -> ModuleConfig.moveMergeEnabled
            "opEnabled" -> ModuleConfig.opEnabled
            "extensionBagEnabled" -> ModuleConfig.extensionBagEnabled
            "customRecipeEnabled" -> ModuleConfig.customRecipeEnabled
            "autoSellEnabled" -> ModuleConfig.autoSellEnabled
            "apiEnabled" -> ModuleConfig.apiEnabled
            "debugLogEnabled" -> ModuleConfig.debugLogEnabled
            "simpleModeEnabled" -> ModuleConfig.simpleModeEnabled
            "pushAllEnabled" -> ModuleConfig.pushAllEnabled
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
            oldAddress, oldPort, oldStack, oldMoveMerge, oldExtensionBag, oldCustomRecipe,
            oldAutoSell, oldApiEnabled, oldDebugLog, oldSimpleMode, oldPushAll
        )
        publishSettingsConfigMirror()
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

    /** native ready 后调用一次：注册 enqueue 反调类并发布初始镜像（revision=1 起）。 */
    @JvmStatic
    fun initSettingsConfigBridge(): Boolean {
        if (!NativeBridge.ready) return false
        if (!bridgeInitialized.compareAndSet(false, true)) return true
        return try {
            NativeBridge.nativeSettingsConfigInit(
                ModuleConfigUiBridge::class.java, mirrorValues(), revision.incrementAndGet()
            )
        } catch (t: Throwable) {
            LogFile.error(LogDomain.CONFIG, "settings config bridge init failed", t)
            false
        }
    }

    /** 配置变更落盘后重发镜像；未初始化或 native 未就绪时静默跳过（镜像属尽力同步面）。 */
    @JvmStatic
    fun publishSettingsConfigMirror() {
        if (!NativeBridge.ready || !bridgeInitialized.get()) return
        try {
            NativeBridge.nativeSettingsConfigPublish(mirrorValues(), revision.incrementAndGet())
        } catch (t: Throwable) {
            LogFile.error(LogDomain.CONFIG, "settings config mirror publish failed", t)
        }
    }

    /**
     * native 桥工作线程的排队回调（签名必须匹配 (JLjava/lang/String;Z)Z）。
     * 只做入队准许：返回 true 表示受理，回执经 [nativeSettingsConfigComplete] 异步回传。
     */
    @JvmStatic
    fun enqueueSettingsConfigRequest(id: Long, key: String, value: Boolean): Boolean {
        if (id <= 0L || key !in BOOL_KEYS) return false
        return try {
            writer.execute { processSettingsConfigRequest(id, key, value) }
            true
        } catch (t: Throwable) {
            LogFile.error(LogDomain.CONFIG, "settings config enqueue rejected id=$id", t)
            false
        }
    }

    /** 单线程 writer 内执行：持久化 → 增量下发 → 镜像重发 → 回执；顺序与 toggleConfig 一致。 */
    private fun processSettingsConfigRequest(id: Long, key: String, value: Boolean) {
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
        val oldPushAll = ModuleConfig.pushAllEnabled
        val current = when (key) {
            "stackLimitIncrease" -> ModuleConfig.stackLimitIncrease
            "moveMergeEnabled" -> ModuleConfig.moveMergeEnabled
            "opEnabled" -> ModuleConfig.opEnabled
            "extensionBagEnabled" -> ModuleConfig.extensionBagEnabled
            "customRecipeEnabled" -> ModuleConfig.customRecipeEnabled
            "autoSellEnabled" -> ModuleConfig.autoSellEnabled
            "apiEnabled" -> ModuleConfig.apiEnabled
            "debugLogEnabled" -> ModuleConfig.debugLogEnabled
            "simpleModeEnabled" -> ModuleConfig.simpleModeEnabled
            "pushAllEnabled" -> ModuleConfig.pushAllEnabled
            else -> {
                completeReceipt(
                    id, 0, 0, APPLY_NOT_SUBMITTED, 1, "unsupported key", "not applied"
                )
                return
            }
        }
        if (current == value) {
            completeReceipt(
                id, 1, if (value) 1 else 0, APPLY_UNCHANGED, 0, "", "unchanged"
            )
            return
        }
        val err = ModuleConfig.apply(JSONObject().put(key, value))
        if (err != null) {
            completeReceipt(
                id, 0, if (current) 1 else 0, APPLY_REJECTED, 1, "persist failed: $err", "not applied"
            )
            return
        }
        try {
            ApiServices.config.applyOnChange(
                oldAddress, oldPort, oldStack, oldMoveMerge, oldExtensionBag, oldCustomRecipe,
                oldAutoSell, oldApiEnabled, oldDebugLog, oldSimpleMode, oldPushAll
            )
            publishSettingsConfigMirror()
        } catch (t: Throwable) {
            // 持久化已成功、setter 失败：persisted=1 且回执失败（契约允许该组合）。
            completeReceipt(
                id, 1, if (value) 1 else 0, APPLY_REJECTED, 2, "setter failed: ${t.message}", "not applied"
            )
            return
        }
        if (key == "apiEnabled") {
            // 监听启停异步调度、就绪不确认，与 toggleConfig 的后台线程路径一致。
            if (value) {
                Thread { ApiServer.startFromConfig() }.start()
            } else {
                ApiServer.stopDelayed()
            }
            completeReceipt(
                id, 1, 1, APPLY_PLATFORM_PENDING, 0, "", "listener start/stop scheduled"
            )
            return
        }
        completeReceipt(id, 1, 1, APPLY_APPLIED, 0, "", "applied")
    }

    private fun completeReceipt(
        id: Long,
        persisted: Int,
        configured: Int,
        applyState: Int,
        errorCode: Int,
        error: String,
        applyMessage: String,
    ) {
        try {
            val accepted = NativeBridge.nativeSettingsConfigComplete(
                id, persisted, configured, applyState, errorCode, error, applyMessage
            )
            if (!accepted) {
                LogFile.warn(LogDomain.CONFIG, "settings config receipt rejected id=$id")
            }
        } catch (t: Throwable) {
            LogFile.error(LogDomain.CONFIG, "settings config receipt failed id=$id", t)
        }
    }

    /** 镜像值序与 [MIRROR_KEYS] 一致；只允许 0/1（native 侧校验）。 */
    private fun mirrorValues(): IntArray = intArrayOf(
        if (ModuleConfig.stackLimitIncrease) 1 else 0,
        if (ModuleConfig.moveMergeEnabled) 1 else 0,
        if (ModuleConfig.opEnabled) 1 else 0,
        if (ModuleConfig.extensionBagEnabled) 1 else 0,
        if (ModuleConfig.customRecipeEnabled) 1 else 0,
        if (ModuleConfig.autoSellEnabled) 1 else 0,
        if (ModuleConfig.apiEnabled) 1 else 0,
        if (ModuleConfig.debugLogEnabled) 1 else 0,
        if (ModuleConfig.simpleModeEnabled) 1 else 0,
        if (ModuleConfig.pushAllEnabled) 1 else 0,
    )
}
