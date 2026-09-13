package com.inotia4.qol

import android.content.Context
import org.json.JSONObject
import java.io.File

/**
 * 模块配置文件组件（v0.5.17）。
 *
 * 配置文件（v0.5.21 起）：**外部存储 `getExternalFilesDir(null)/config.json` 为唯一配置来源**
 * （与 LogFile 同目录，`/sdcard/Android/data/<游戏包>/files/config.json`，用户可见可编辑）。
 * 启动加载时：
 * - 外部文件存在 → 读取生效
 * - 外部文件不存在/损坏 → 使用默认值（DEFAULT_*），并立即写入外部 config.json
 * 每次运行时修改（POST /api/config/set）同样立即持久化到该文件。
 * 删除外部文件即恢复出厂默认。
 *
 * 当前配置项：
 * - listenAddress：HTTP 服务监听地址，默认 0.0.0.0
 * - listenPort：HTTP 服务监听端口，默认 8088
 * - stackLimitIncrease：是否启用游戏物品堆叠上限增加（99→999）。
 *   默认 false；只控制新操作的 99/999 上限。
 * - moveMergeEnabled：是否启用背包内拖拽同类物品合并。
 *   默认 false；启动期和配置变更时均下发 native GOT hook。
 * - opEnabled：OP 能力全局开关（/api/op 门禁，architecture §9.1-2）。
 *   默认 false（安全基线：OP 默认关闭）；开启后 OpApiService 各方法才放行。
 * - extensionBagEnabled：是否启用扩展背包，默认 false。
 * - gemCraftOptimize：是否启用合成器宝石合成操作优化，默认 false。
 * - apiEnabled：API 全局开关，默认 true；关闭时不启动 HTTP 服务与 native 缓存预取线程，
 *   唯一恢复通道为游戏内设置页（ModuleConfigUiBridge JNI，不依赖 HTTP）。
 *
 * 线程安全：配置可能被 API 请求线程/启动线程并发读写，字段用 @Volatile 保护。
 */
object ModuleConfig {

    private const val CONFIG_FILE = "config.json"

    const val DEFAULT_LISTEN_ADDRESS = "0.0.0.0"
    const val DEFAULT_LISTEN_PORT = 8088
    const val DEFAULT_STACK_LIMIT_INCREASE = false
    const val DEFAULT_MOVE_MERGE_ENABLED = false
    const val DEFAULT_OP_ENABLED = false
    const val DEFAULT_EXTENSION_BAG_ENABLED = false
    const val DEFAULT_GEM_CRAFT_OPTIMIZE = false
    const val DEFAULT_API_ENABLED = true

    @Volatile
    private var loaded = false

    /** 持久化与加载用的应用 context（load() 时缓存） */
    @Volatile
    private var appContext: Context? = null

    /** HTTP 监听地址（默认 0.0.0.0） */
    @Volatile
    var listenAddress: String = DEFAULT_LISTEN_ADDRESS
        private set

    /** HTTP 监听端口（默认 8088，合法范围 1-65535） */
    @Volatile
    var listenPort: Int = DEFAULT_LISTEN_PORT
        private set

    /** 是否启用游戏物品堆叠上限增加（99→999，默认 false） */
    @Volatile
    var stackLimitIncrease: Boolean = DEFAULT_STACK_LIMIT_INCREASE
        private set

    /** 是否启用背包内拖拽同类物品合并（默认 false） */
    @Volatile
    var moveMergeEnabled: Boolean = DEFAULT_MOVE_MERGE_ENABLED
        private set

    /** OP 能力全局开关（默认 false，安全基线）；OpApiService 门禁读取 */
    @Volatile
    var opEnabled: Boolean = DEFAULT_OP_ENABLED
        private set

    @Volatile
    var extensionBagEnabled: Boolean = DEFAULT_EXTENSION_BAG_ENABLED
        private set

    /** 是否启用合成器宝石合成操作优化（默认 false） */
    @Volatile
    var gemCraftOptimize: Boolean = DEFAULT_GEM_CRAFT_OPTIMIZE
        private set

    /** API 全局开关（默认 true）：关闭时不启动 HTTP 服务与 native 缓存预取线程 */
    @Volatile
    var apiEnabled: Boolean = DEFAULT_API_ENABLED
        private set

    /** 加载配置（幂等）：外部 config.json 为唯一来源；不存在/损坏时用默认值并立即写入 */
    @Synchronized
    fun load(context: Context) {
        appContext = context
        if (loaded) return
        val content = readPersisted(context)
        if (content == null) {
            LogFile.log("$CONFIG_FILE missing, using defaults and persisting")
            persist(toJson())
            loaded = true
            return
        }
        try {
            val json = JSONObject(content)
            json.optString("listenAddress", DEFAULT_LISTEN_ADDRESS).let {
                if (it.isNotBlank()) listenAddress = it
            }
            json.optInt("listenPort", DEFAULT_LISTEN_PORT).let {
                if (it in 1..65535) listenPort = it
            }
            stackLimitIncrease = json.optBoolean("stackLimitIncrease", DEFAULT_STACK_LIMIT_INCREASE)
            moveMergeEnabled = json.optBoolean("moveMergeEnabled", DEFAULT_MOVE_MERGE_ENABLED)
            opEnabled = json.optBoolean("opEnabled", DEFAULT_OP_ENABLED)
            extensionBagEnabled = json.optBoolean("extensionBagEnabled", DEFAULT_EXTENSION_BAG_ENABLED)
            gemCraftOptimize = json.optBoolean("gemCraftOptimize", DEFAULT_GEM_CRAFT_OPTIMIZE)
            apiEnabled = json.optBoolean("apiEnabled", DEFAULT_API_ENABLED)
            if (!json.has("moveMergeEnabled") || !json.has("extensionBagEnabled") ||
                !json.has("gemCraftOptimize") ||
                !json.has("apiEnabled") ||
                json.has("jewelBatchMix")
            ) {
                LogFile.log("updating $CONFIG_FILE with current configuration fields")
                persist(toJson())
            }
            LogFile.log(
                "config loaded: listenAddress=$listenAddress listenPort=$listenPort " +
                    "stackLimitIncrease=$stackLimitIncrease moveMergeEnabled=$moveMergeEnabled opEnabled=$opEnabled " +
                    "gemCraftOptimize=$gemCraftOptimize apiEnabled=$apiEnabled"
            )
        } catch (t: Throwable) {
            LogFile.logError("config parse failed, using defaults and persisting", t)
            persist(toJson())
        }
        loaded = true
    }

    // ---- 运行时修改（每次修改立即持久化到 config.json） ----

    /**
     * 应用配置（v0.5.19，配置端点调用）：仅更新 JSON 中出现的字段。
     * 先持久化合并后的完整配置，成功后才提交到内存（原子性），
     * 返回 null=成功、否则错误消息。
     */
    @Synchronized
    fun apply(json: JSONObject): String? {
        var newAddress = listenAddress
        var newPort = listenPort
        var newStack = stackLimitIncrease
        var newMoveMerge = moveMergeEnabled
        var newOp = opEnabled
        var newExtensionBag = extensionBagEnabled
        var newGemCraft = gemCraftOptimize
        var newApiEnabled = apiEnabled
        if (json.has("listenAddress")) {
            val a = json.optString("listenAddress")
            if (a.isBlank()) return "listenAddress required"
            newAddress = a
        }
        if (json.has("listenPort")) {
            val p = json.optInt("listenPort", -1)
            if (p !in 1..65535) return "listenPort must be 1-65535"
            newPort = p
        }
        if (json.has("stackLimitIncrease")) newStack = json.optBoolean("stackLimitIncrease", newStack)
        if (json.has("moveMergeEnabled")) newMoveMerge = json.optBoolean("moveMergeEnabled", newMoveMerge)
        if (json.has("opEnabled")) newOp = json.optBoolean("opEnabled", newOp)
        if (json.has("extensionBagEnabled")) newExtensionBag = json.optBoolean("extensionBagEnabled", newExtensionBag)
        if (json.has("gemCraftOptimize")) newGemCraft = json.optBoolean("gemCraftOptimize", newGemCraft)
        if (json.has("apiEnabled")) newApiEnabled = json.optBoolean("apiEnabled", newApiEnabled)
        val merged = JSONObject()
            .put("listenAddress", newAddress)
            .put("listenPort", newPort)
            .put("stackLimitIncrease", newStack)
            .put("moveMergeEnabled", newMoveMerge)
            .put("opEnabled", newOp)
            .put("extensionBagEnabled", newExtensionBag)
            .put("gemCraftOptimize", newGemCraft)
            .put("apiEnabled", newApiEnabled)
        if (!persist(merged)) return "config save failed"
        listenAddress = newAddress
        listenPort = newPort
        stackLimitIncrease = newStack
        moveMergeEnabled = newMoveMerge
        opEnabled = newOp
        extensionBagEnabled = newExtensionBag
        gemCraftOptimize = newGemCraft
        apiEnabled = newApiEnabled
        return null
    }

    /** 当前生效配置序列化（配置端点 GET 返回用） */
    fun toJson(): JSONObject = JSONObject().apply {
        put("listenAddress", listenAddress)
        put("listenPort", listenPort)
        put("stackLimitIncrease", stackLimitIncrease)
        put("moveMergeEnabled", moveMergeEnabled)
        put("opEnabled", opEnabled)
        put("extensionBagEnabled", extensionBagEnabled)
        put("gemCraftOptimize", gemCraftOptimize)
        put("apiEnabled", apiEnabled)
    }

    /**
     * 监听端口回退到默认值（v0.5.22）：端口被占用导致启动失败时调用。
     * 先持久化再提交内存（原子性），返回是否成功。
     */
    @Synchronized
    fun fallbackListenPortToDefault(): Boolean {
        if (listenPort == DEFAULT_LISTEN_PORT) return true
        val merged = toJson().put("listenPort", DEFAULT_LISTEN_PORT)
        if (!persist(merged)) return false
        listenPort = DEFAULT_LISTEN_PORT
        LogFile.log("listenPort fallback to default $DEFAULT_LISTEN_PORT")
        return true
    }

    private fun readPersisted(context: Context): String? {
        return try {
            val dir = context.getExternalFilesDir(null) ?: return null
            val f = File(dir, CONFIG_FILE)
            if (f.exists()) f.readText() else null
        } catch (t: Throwable) {
            LogFile.logError("read persisted config failed", t)
            null
        }
    }

    private fun persist(content: JSONObject): Boolean {
        val ctx = appContext ?: return false
        return try {
            val dir = ctx.getExternalFilesDir(null)
                ?: return false
            if (!dir.exists() && !dir.mkdirs()) return false
            val f = File(dir, CONFIG_FILE)
            // 原子写：临时文件 + rename，避免崩溃留下半截 JSON
            val tmp = File(dir, "$CONFIG_FILE.tmp")
            tmp.writeText(content.toString())
            if (!tmp.renameTo(f)) {
                f.writeText(content.toString())
                tmp.delete()
            }
            LogFile.log("config persisted: ${f.absolutePath}")
            true
        } catch (t: Throwable) {
            LogFile.logError("config persist failed", t)
            false
        }
    }
}
