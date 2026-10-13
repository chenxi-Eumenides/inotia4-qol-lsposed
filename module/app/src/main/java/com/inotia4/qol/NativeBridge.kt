package com.inotia4.qol

import java.util.concurrent.locks.ReentrantLock

object NativeBridge {

    private val initializationLock = ReentrantLock()

    @Volatile
    var libraryLoaded: Boolean = false
        private set

    @Volatile
    var ready: Boolean = false
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
            ready = try {
                nativeRegisterExtensionBagUiBridge(ExtensionBagUiBridge::class.java)
                nativeInit()
            } catch (t: Throwable) {
                LogFile.error(LogDomain.PLATFORM, "nativeInit failed", t)
                false
            }
            ready
        } finally {
            initializationLock.unlock()
        }
    }

    external fun nativeConfigStoreBootstrap(externalFilesDirectory: String): Int
    external fun nativeConfigPlatformSubscribe(callbacks: Class<*>): Boolean
    external fun nativeConfigPlatformReportBindFailure(revision: Long, failedPort: Int): Boolean
    external fun nativeRuntimeEffectSubscribe(pluginId: String, callbacks: Class<*>): Long
    external fun nativeRuntimeEffectUnsubscribe(subscriptionId: Long): Boolean
    external fun nativePluginConfigRequestIfRevision(
        pluginId: String,
        key: String,
        valueJson: String,
        expectedConfigRevision: Long,
    ): String
    external fun nativePluginConfigReceipt(requestId: Long): String
    external fun nativePluginConfigSubscribeReceipt(requestId: Long, callbacks: Class<*>): Long
    external fun nativePluginConfigUnsubscribeReceipt(subscriptionId: Long): Boolean

    external fun nativeInit(): Boolean

    // ---- 统一日志（qol_log）----
    external fun nativeQolLogInit()
    external fun nativeQolLogWrite(level: Int, domain: String, src: String, msg: String)
    external fun nativeQolLogSetDebugEnabled(enabled: Boolean)
    external fun nativeSetSimpleModeEnabled(enabled: Boolean): Boolean
    external fun nativeSetPushAllEnabled(enabled: Boolean): Boolean

    external fun nativeRegisterConfigBridge(bridge: Class<*>)
    external fun nativeRegisterExtensionBagUiBridge(bridge: Class<*>)
    external fun nativeGetInitReport(): String
    external fun nativeGetBaseAddr(): Long
    external fun nativeGetFrameCount(): Long
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
    external fun nativeSetOpEnabled(enabled: Boolean): Boolean
    external fun nativeConfigSetString(key: String, value: String): Boolean
    external fun nativeConfigSetInt(key: String, value: Int): Boolean
    external fun nativeGetActiveQuest(): Int
    external fun nativeQuestList(): String
    external fun nativeQuestCompleted(): String
    external fun nativeQuestActive(): String
    external fun nativeCurrentSaveSlot(): String
    external fun nativeGetPlayerJson(): String
    external fun nativeGetPartyJson(): String
    external fun nativeGetInventoryJson(): String
    external fun nativeGetMapJson(): String
    external fun nativeGetTilesJson(): String
    external fun nativeSetTilesData(json: String): Boolean
    external fun nativeGetUnitsJson(): String
    external fun nativeGetEnemiesJson(): String
    external fun nativeGetInteractivesJson(): String
    external fun nativeGetGamestateJson(): String
    external fun nativeGetDebugUiJson(): String
    external fun nativeGetSnapshotJson(): String
    external fun nativeGetSkillsJson(): String
    external fun nativeGetMercenariesJson(): String
    external fun nativeGetDropsJson(): String
    external fun nativeGetPathJson(tx: Int, ty: Int): String
    external fun nativeDistanceJson(tx: Int, ty: Int): String
    external fun nativeDebugPathJson(tx: Int, ty: Int): String
    external fun nativeDebugItemRawJson(bag: Int, slot: Int): String

    external fun nativeOpSetMoney(money: Long): String
    external fun nativeOpAddMoney(delta: Long): String
    external fun nativeOpMinusMoney(delta: Long): String
    external fun nativeOpSetExperience(role: Int, exp: Long): String
    external fun nativeOpSetLevel(role: Int, level: Int, force: Boolean): String
    external fun nativeOpAddExperience(role: Int, delta: Long): String
    external fun nativeOpSetStatusPoint(role: Int, points: Int): String
    external fun nativeOpSetHp(role: Int, hp: Int): String
    external fun nativeOpSetMp(role: Int, mp: Int): String
    external fun nativeOpSetAttr(role: Int, attrIndex: Int, value: Int): String
    external fun nativeOpAddItem(category: Int, count: Int, socket: Int, enhanceRemaining: Int, rarity: Int): String
    external fun nativeOpAddStat(role: Int, attr: Int): String
    external fun nativeOpStatReset(role: Int): String
    external fun nativeOpSkillReset(role: Int): String
    external fun nativeOpCast(role: Int, actionId: Int): String
    external fun nativeOpQuestQuit(questId: Int): String
    external fun nativeOpSave(): String
    // ---- 存档管理器备份（save-backup feature：bundle 导出/导入/列表/删除收口 native）----
    external fun nativeSaveBackupInit(dataDir: String, externalFilesDir: String)
    external fun nativeSaveBackupSetMapNames(json: String)

    external fun nativeSetWorldMapExits(json: String)

    // ---- HTTP owner 静态 enrichment 下发（启动期一次；owner handler 运行期只读）----
    external fun nativeSetItemStaticData(payload: String)
    external fun nativeSetQuestStaticData(payload: String)
    // system_facts 的 info/game 需要模块身份（版本/包名），非游戏静态表，单独下发。
    external fun nativeSetSystemIdentity(version: String, packageName: String)
    external fun nativeBackupList(): String
    external fun nativeBackupExport(slot: Int): String
    external fun nativeBackupImport(checksum: String, slot: Int): String
    external fun nativeBackupDelete(checksum: String): String
    external fun nativeSaveBackupUiInject(): String
    external fun nativeSaveBackupUiStatus(): String
    external fun nativeSaveBackupUiRestore(): String
    external fun nativeSaveBackupUiOpenPanel(): String
    external fun nativeSaveBackupUiSelfCheck(): String
    external fun nativeOpMainMenu(): String
    external fun nativeOpEnterSlot(slot: Int): String
    external fun nativeOpCreateSlot(slot: Int, classIdx: Int): String
    external fun nativeOpPanelClose(): String
    external fun nativeOpPanelOpen(panel: String): String
    external fun nativeRecoverAfterHiveBlock(): String
    external fun nativeSaveSlotsJson(): String
    external fun nativeOpNpcInteract(): String
    external fun nativeNpcDialogOptions(): String
    external fun nativeShopItems(): String
    external fun nativeOpShopBuy(slot: Int): String
    external fun nativeDialogContent(): String
    external fun nativeOpDialogSelect(action: String, index: Int): String
    external fun nativeOpJewel(role: Int, bag: Int, slot: Int, equipSlot: Int): String
    external fun nativeOpEnchant(role: Int, bag: Int, slot: Int, equipSlot: Int): String
    external fun nativeOpSetAutoAttack(role: Int, onoff: Int): String
    external fun nativeOpSetSkillUsage(role: Int, onoff: Int): String
    external fun nativeOpEquip(role: Int, bag: Int, slot: Int): String
    external fun nativeOpUnequip(role: Int, slot: Int): String
    external fun nativeOpSwitchPlayer(slot: Int): String
    external fun nativeOpTeleport(mapId: Int, x: Int, y: Int): String
    external fun nativeOpTeleportEx(mapId: Int, x: Int, y: Int, force: Boolean): String
    external fun nativeOpRemoveItem(category: Int): String
    external fun nativeOpLearnAction(role: Int, actionId: Int, level: Int): String
    external fun nativeGetEventsJson(): String

    external fun nativeOpMove(x: Int, y: Int): String
    external fun nativeOpWalk(direction: Int): String
    external fun nativeOpWalkStop(): String
    external fun nativeOpInteract(): String
    external fun nativeOpAttack(role: Int, targetSlot: Int): String
    external fun nativeOpStopCombat(role: Int): String
    external fun nativeOpUseItem(bag: Int, slot: Int): String
    external fun nativeOpDiceAccept(): String
    external fun nativeOpDiceReject(): String
    external fun nativeOpDiscardItem(bag: Int, slot: Int): String
    external fun nativeOpSellItem(bag: Int, slot: Int): String
    external fun nativeOpMoveItem(bag: Int, slot: Int, count: Int, toBag: Int, toSlot: Int): String
    external fun nativeOpIncludeParty(mercSlot: Int): String
    external fun nativeOpExcludeParty(mercSlot: Int): String
    external fun nativeOpDischarge(mercSlot: Int): String
    external fun nativeOpWithdraw(mercSlot: Int, equipSlot: Int): String
    external fun nativeOpPartySwap(a: Int, b: Int): String
    external fun nativeSetStackLimitEnabled(enabled: Boolean): Boolean
    external fun nativeSetMoveMergeEnabled(enabled: Boolean): Boolean
    external fun nativeSetExtensionBagEnabled(enabled: Boolean): Boolean
    external fun nativeSetCustomRecipeEnabled(enabled: Boolean): Boolean
    external fun nativeSetAutoSellEnabled(enabled: Boolean): Boolean
    external fun nativeSetApiEnabled(enabled: Boolean): Boolean
    external fun nativeSettingsUiInject(): String
    external fun nativeSettingsUiStatus(): String
    external fun nativeSettingsUiRestore(): String
    external fun nativeSettingsUiOpenOption(): String
    external fun nativeSettingsUiOpenPanel(): String
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
    external fun nativeAutoSellRunNow(): Boolean
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

    // ---- P3 资源登记表与生命周期控制 ----
    // 资源登记表导出 JSON（/api/system/resources）
    external fun nativeSystemResourcesJson(): String
    // 生命周期动作下发：返回命令票据（0 = 投递失败）；action 取 native 侧 kFeatureLifecycle*
    external fun nativeFeatureLifecycle(pluginId: String, action: Int): Long
    // 命令票据状态 JSON（/api/system/tickets/{id}）
    external fun nativeTicketStateJson(ticket: Long): String
    external fun nativeHttpRoutesJson(): String

    // 游戏变体诊断（P1）：libgame.so 查表结果 JSON，供 /api/health 加法扩展使用
    external fun nativeGetVariantJson(): String

    // ---- 持久配置桥（settings_config v1.0：Kotlin 镜像 + 请求回执）----
    // 初始化：注册 enqueueSettingsConfigRequest 反调类并发布首次镜像（native ready 后调用）。
    external fun nativeSettingsConfigInit(bridge: Class<*>, values: IntArray, revision: Long): Boolean
    // 配置落盘后重发镜像；revision 必须严格递增，native 侧拒绝回退（Conflict）。
    external fun nativeSettingsConfigPublish(values: IntArray, revision: Long): Boolean
    // 后台 writer 完成一次 set 后回传回执；persisted/configured/apply 语义见
    // cpp/contracts/settings_config.h（succeeded 只代表持久化 + setter 接受，不含监听就绪）。
    external fun nativeSettingsConfigComplete(
        id: Long,
        persisted: Int,
        configured: Int,
        applyState: Int,
        errorCode: Int,
        error: String,
        applyMessage: String,
    ): Boolean
}
