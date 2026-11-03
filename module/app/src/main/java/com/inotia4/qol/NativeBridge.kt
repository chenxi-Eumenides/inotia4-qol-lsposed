package com.inotia4.qol

import com.inotia4.qol.storage.PublicExportBridge
import java.util.concurrent.locks.ReentrantLock

object NativeBridge {

    private val initializationLock = ReentrantLock()

    @Volatile
    var libraryLoaded: Boolean = false
        private set

    @Volatile
    var ready: Boolean = false
        private set

    @Volatile
    var initializationAttempted: Boolean = false
        private set

    @Volatile
    var initializationInProgress: Boolean = false
        private set

    init {
        try {
            System.loadLibrary("gamebridge")
            libraryLoaded = true
        } catch (t: Throwable) {
            libraryLoaded = false
            LogFile.error(LogDomain.PLATFORM, "loadLibrary(gamebridge) failed", t)
        }
    }

    fun init(): Boolean {
        if (ready) return true
        if (!libraryLoaded || !initializationLock.tryLock()) return false
        return try {
            if (ready) return true
            initializationAttempted = true
            initializationInProgress = true
            ready = try {
                nativeRegisterPublicExportBridge(PublicExportBridge::class.java)
                nativeRegisterExtensionBagUiBridge(ExtensionBagUiBridge::class.java)
                nativeInit()
            } catch (t: Throwable) {
                LogFile.error(LogDomain.PLATFORM, "nativeInit failed", t)
                false
            }
            ready
        } finally {
            initializationInProgress = false
            initializationLock.unlock()
        }
    }

    external fun nativeModuleHealthSubscribe(callbacks: Class<*>): Long
    external fun nativeModuleHealthUnsubscribe(subscriptionId: Long): Boolean

    external fun nativeConfigStoreBootstrap(externalFilesDirectory: String): Int
    external fun nativeRuntimeEffectSubscribe(pluginId: String, callbacks: Class<*>): Long
    external fun nativeRuntimeEffectUnsubscribe(subscriptionId: Long): Boolean
    external fun nativePluginConfigRequestIfRevision(
        pluginId: String,
        key: String,
        valueJson: String,
        expectedConfigRevision: Long,
    ): String
    external fun nativePluginConfigSubscribeReceipt(requestId: Long, callbacks: Class<*>): Long
    external fun nativePluginConfigUnsubscribeReceipt(subscriptionId: Long): Boolean

    external fun nativeInit(): Boolean

    // ---- 统一日志（qol_log）----
    external fun nativeQolLogInit()
    external fun nativeQolLogWrite(level: Int, domain: String, src: String, msg: String)

    // ---- 崩溃取证（native crash handler 附加信息下发；文本由 RuntimeEnvironment 构造）----
    external fun nativeQolCrashSetIdentity(text: String)
    external fun nativeQolCrashSetConfig(text: String)

    external fun nativeRegisterExtensionBagUiBridge(bridge: Class<*>)
    external fun nativeRegisterPublicExportBridge(bridge: Class<*>)
    external fun nativeGetInitReport(): String
    external fun nativeHttpDispatch(method: Int, path: String, query: String?, body: String?): String

    // ---- direct-Kotlin 路由 lease（Host route metadata 匹配 + OP/owner 门禁）----
    // 仅 Kotlin 直接处理、无 native handler 的路由使用；native handler 路由已在 dispatch 内持 lease。
    // 返回 0 成功；lease_required=false 时 leaseOut[0]=0（无需 Leave）。错误码见 util/NativeRouteLease。
    external fun nativeHttpRouteEnter(method: Int, path: String, leaseOut: LongArray): Int
    external fun nativeHttpRouteLeave(lease: Long): Int
    external fun nativeCoreDataBegin(fileCount: Int, totalBytes: Int): Boolean
    external fun nativeCoreDataAsset(path: String, bytes: ByteArray): Boolean
    external fun nativeCoreDataFinish(): Boolean
    external fun nativeCoreDataCancel()
    external fun nativeGetMapJson(): String
    external fun nativeSetTilesData(json: String): Boolean
    external fun nativeGetUnitsJson(): String
    external fun nativeGetEnemiesJson(): String
    external fun nativeGetInteractivesJson(): String
    external fun nativeGetGamestateJson(): String
    external fun nativeGetDebugUiJson(): String
    external fun nativeGetSnapshotJson(): String
    external fun nativeGetDropsJson(): String
    external fun nativeDebugPathJson(tx: Int, ty: Int): String
    external fun nativeDebugItemRawJson(bag: Int, slot: Int): String

    // ---- OP 直连：HTTP 写路由只表达绝对值（money/experience）与既有动词，
    // 增量、按类别删除与单技能档位没有等价路由，因此保留 JNI 入口。----
    external fun nativeOpAddMoney(delta: Long): String
    external fun nativeOpMinusMoney(delta: Long): String
    external fun nativeOpAddExperience(role: Int, delta: Long): String
    external fun nativeOpRemoveItem(category: Int): String
    external fun nativeOpSetSkillUsage(role: Int, onoff: Int): String

    // ---- 存档管理器备份（save-backup feature：bundle 导出/导入/列表/删除收口 native）----
    external fun nativeSaveBackupInit(dataDir: String, externalFilesDir: String)
    external fun nativeSaveBackupSetMapNames(json: String)

    external fun nativeSetWorldMapExits(json: String)

    // ---- HTTP owner 静态 enrichment 下发（启动期一次；owner handler 运行期只读）----
    external fun nativeSetItemStaticData(payload: String)
    external fun nativeSetQuestStaticData(payload: String)
    // system_facts 的 info/game 需要模块身份（版本/包名）；另下发宿主游戏 APK versionName 与 Android 版本。
    external fun nativeSetSystemIdentity(
        version: String,
        packageName: String,
        gameVersionName: String,
        androidRelease: String,
        androidSdkInt: Int,
    )
    external fun nativeSaveBackupUiInject(): String
    external fun nativeSaveBackupUiSelfCheck(): String
    // SAF 导入落盘后的面板列表刷新（仅在面板打开时有实际效果）。
    external fun nativeSaveBackupUiRefresh(): String
    external fun nativeSettingsUiStatus(): String
    external fun nativeRecoverAfterHiveBlock(): String
    external fun nativeNpcDialogOptions(): String
    external fun nativeGetEventsJson(): String

    // ---- 自动出售（auto-sell 阶段 B）----
    // 值即开关：0=关闭，正整数为 1-based 档位（rarity 1..5 / enhance 1..32 /
    // socket 1..16 / gemTier 1..5 / gemRange 1..5）。
    // special 为特殊类型名 JSON 数组文本（合法名 backpack / normalSeal / dice；空数组 = 关）。
    external fun nativeSetAutoSellConfig(
        enabled: Boolean,
        rarity: Int,
        enhance: Int,
        socket: Int,
        gemTier: Int,
        special: String,
        gemRange: Int
    ): Boolean
    external fun nativeAutoSellStatusJson(): String

    // ---- module sidecar 容器（infra/store：MSAV 单写者；返回 infra/store::Status 码，0=ok）----
    external fun nativeStoreAvailable(): Boolean
    external fun nativeStoreEnsureSlot(slot: Int): Int
    external fun nativeStoreResetSlot(slot: Int): Int
    external fun nativeStoreReadSection(slot: Int, name: String, versionOut: IntArray): ByteArray?
    external fun nativeStoreWriteSection(slot: Int, name: String, version: Int, payload: ByteArray): Int
    external fun nativeStoreRemoveSection(slot: Int, name: String): Int
    external fun nativeStoreReplaceSections(
        slot: Int,
        names: Array<String>,
        versions: IntArray,
        payloads: Array<ByteArray>,
        removals: Array<String>,
    ): Int
    external fun nativeStoreReadContainer(slot: Int): ByteArray?
    external fun nativeStoreImportContainer(slot: Int, payload: ByteArray): Int

    // ---- 功能诊断（P1 最小诊断接口：原生功能状态汇总 JSON）----
    external fun nativeFeatureStatusJson(): String

    // ---- P3 资源登记表 ----
    // 资源登记表导出 JSON（/api/system/resources）
    external fun nativeSystemResourcesJson(): String
    // 命令票据状态 JSON（/api/system/tickets/{id}）
    external fun nativeTicketStateJson(ticket: Long): String
    external fun nativeHttpRoutesJson(): String

    // 游戏变体诊断（P1）：libgame.so 查表结果 JSON，供 /api/health 加法扩展使用
    external fun nativeGetVariantJson(): String
}
