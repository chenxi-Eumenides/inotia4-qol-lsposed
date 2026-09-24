package com.inotia4.qol.service

import com.inotia4.qol.LogDomain
import com.inotia4.qol.LogFile
import com.inotia4.qol.ModuleConfig
import com.inotia4.qol.NativeBridge
import com.inotia4.qol.StaticData
import com.inotia4.qol.patch.WatermarkOverlay

/**
 * 模块配置→native 生效服务层接口（v0.5.46 P1 收边）。
 * 收口 nativeSetStackLimitEnabled/nativeSetMoveMergeEnabled/nativeSetTilesData 直调：
 * 启动期走 [applyToNative]，配置端点变更走 [applyOnChange]，瓦片矩阵加载走 [applyTiles]。
 */
interface ConfigApiService {
    /** 启动期全量应用：堆叠上限、拖拽合并与静态瓦片矩阵（内部自行判断 NativeBridge.ready） */
    fun applyToNative()

    /** 配置变更增量应用：仅当对应值变化时通知 native（POST /api/config/set 用） */
    fun applyOnChange(
        oldAddress: String,
        oldPort: Int,
        oldStack: Boolean,
        oldMoveMerge: Boolean,
        oldExtensionBag: Boolean,
        oldCustomRecipe: Boolean,
        oldAutoSell: Boolean,
        oldApiEnabled: Boolean,
        oldDebugLog: Boolean,
        oldSimpleMode: Boolean,
        oldPushAll: Boolean
    )

    /** 加载静态瓦片矩阵入 native（替代运行时读内存，P0#瓦片矩阵） */
    fun applyTiles()
}

/**
 * 模块配置→native 生效服务实现（ConfigApiService 唯一实现，v0.5.46 迁移自 ConfigController/ApiServer 直调）。
 */
class ConfigApiServiceImpl : ConfigApiService {

    override fun applyToNative() {
        if (!NativeBridge.ready) return
        NativeBridge.nativeConfigSetString("listenAddress", ModuleConfig.listenAddress)
        NativeBridge.nativeConfigSetInt("listenPort", ModuleConfig.listenPort)
        val applied = NativeBridge.nativeSetStackLimitEnabled(ModuleConfig.stackLimitIncrease)
        LogFile.info(LogDomain.CONFIG, "stackLimitIncrease=${ModuleConfig.stackLimitIncrease} applied=$applied")
        val moveMergeApplied = NativeBridge.nativeSetMoveMergeEnabled(ModuleConfig.moveMergeEnabled)
        LogFile.info(LogDomain.CONFIG, "moveMergeEnabled=${ModuleConfig.moveMergeEnabled} applied=$moveMergeApplied")
        val extensionApplied = NativeBridge.nativeSetExtensionBagEnabled(ModuleConfig.extensionBagEnabled)
        LogFile.info(LogDomain.CONFIG, "extensionBagEnabled=${ModuleConfig.extensionBagEnabled} applied=$extensionApplied")
        val customRecipeApplied = NativeBridge.nativeSetCustomRecipeEnabled(ModuleConfig.customRecipeEnabled)
        LogFile.info(LogDomain.CONFIG, "customRecipeEnabled=${ModuleConfig.customRecipeEnabled} applied=$customRecipeApplied")
        val simpleModeApplied = NativeBridge.nativeSetSimpleModeEnabled(ModuleConfig.simpleModeEnabled)
        LogFile.info(LogDomain.CONFIG, "simpleModeEnabled=${ModuleConfig.simpleModeEnabled} applied=$simpleModeApplied")
        val pushAllApplied = NativeBridge.nativeSetPushAllEnabled(ModuleConfig.pushAllEnabled)
        LogFile.info(LogDomain.CONFIG, "pushAllEnabled=${ModuleConfig.pushAllEnabled} applied=$pushAllApplied")
        val autoSellApplied = NativeBridge.nativeSetAutoSellEnabled(ModuleConfig.autoSellEnabled)
        LogFile.info(LogDomain.CONFIG, "autoSellEnabled=${ModuleConfig.autoSellEnabled} applied=$autoSellApplied")
        val apiApplied = NativeBridge.nativeSetApiEnabled(ModuleConfig.apiEnabled)
        LogFile.info(LogDomain.CONFIG, "apiEnabled=${ModuleConfig.apiEnabled} applied=$apiApplied")
        // opEnabled 镜像到 native：/api/op/* 的 op_gated 路由由 dispatch 层门禁读取，
        // 必须在 HTTP 服务启动（本函数先于 ApiServer.start）前下发。
        NativeBridge.nativeSetOpEnabled(ModuleConfig.opEnabled)
        LogFile.info(LogDomain.CONFIG, "opEnabled=${ModuleConfig.opEnabled} applied to native")
        NativeBridge.nativeQolLogSetDebugEnabled(ModuleConfig.debugLogEnabled)
        LogFile.info(LogDomain.CONFIG, "debugLogEnabled=${ModuleConfig.debugLogEnabled} applied ok")
        applyTiles()
    }

    override fun applyOnChange(
        oldAddress: String,
        oldPort: Int,
        oldStack: Boolean,
        oldMoveMerge: Boolean,
        oldExtensionBag: Boolean,
        oldCustomRecipe: Boolean,
        oldAutoSell: Boolean,
        oldApiEnabled: Boolean,
        oldDebugLog: Boolean,
        oldSimpleMode: Boolean,
        oldPushAll: Boolean
    ) {
        // 水印颜色跟随简单模式开关，属纯 Kotlin 视图层装饰，与 native 就绪无关，
        // 故放在 ready 检查之前（native 未就绪时也要让水印颜色正确）。
        if (ModuleConfig.simpleModeEnabled != oldSimpleMode) {
            WatermarkOverlay.refreshColor()
        }
        if (!NativeBridge.ready) return
        if (ModuleConfig.listenAddress != oldAddress) {
            NativeBridge.nativeConfigSetString("listenAddress", ModuleConfig.listenAddress)
        }
        if (ModuleConfig.listenPort != oldPort) {
            NativeBridge.nativeConfigSetInt("listenPort", ModuleConfig.listenPort)
        }
        if (ModuleConfig.stackLimitIncrease != oldStack) {
            NativeBridge.nativeSetStackLimitEnabled(ModuleConfig.stackLimitIncrease)
        }
        if (ModuleConfig.moveMergeEnabled != oldMoveMerge) {
            NativeBridge.nativeSetMoveMergeEnabled(ModuleConfig.moveMergeEnabled)
        }
        if (ModuleConfig.extensionBagEnabled != oldExtensionBag) {
            NativeBridge.nativeSetExtensionBagEnabled(ModuleConfig.extensionBagEnabled)
        }
        if (ModuleConfig.customRecipeEnabled != oldCustomRecipe) {
            NativeBridge.nativeSetCustomRecipeEnabled(ModuleConfig.customRecipeEnabled)
        }
        if (ModuleConfig.simpleModeEnabled != oldSimpleMode) {
            NativeBridge.nativeSetSimpleModeEnabled(ModuleConfig.simpleModeEnabled)
        }
        if (ModuleConfig.pushAllEnabled != oldPushAll) {
            NativeBridge.nativeSetPushAllEnabled(ModuleConfig.pushAllEnabled)
        }
        if (ModuleConfig.autoSellEnabled != oldAutoSell) {
            NativeBridge.nativeSetAutoSellEnabled(ModuleConfig.autoSellEnabled)
        }
        if (ModuleConfig.apiEnabled != oldApiEnabled) {
            NativeBridge.nativeSetApiEnabled(ModuleConfig.apiEnabled)
        }
        if (ModuleConfig.debugLogEnabled != oldDebugLog) {
            NativeBridge.nativeQolLogSetDebugEnabled(ModuleConfig.debugLogEnabled)
        }
        // opEnabled 无 old 参数（不改 applyOnChange 既有签名）：幂等下发当前值；
        // host 侧仅覆盖一个 bool，代价为一次函数调用。
        NativeBridge.nativeSetOpEnabled(ModuleConfig.opEnabled)
    }

    override fun applyTiles() {
        try {
            val tilesJson = StaticData.read("maps/tiles.json")
            if (tilesJson != null && NativeBridge.ready) {
                val ok = NativeBridge.nativeSetTilesData(tilesJson)
                LogFile.info(LogDomain.CONFIG, "static tiles loaded: $ok")
            } else if (tilesJson == null) {
                LogFile.info(LogDomain.CONFIG, "static tiles read failed: maps/tiles.json missing")
            }
        } catch (t: Throwable) {
            LogFile.error(LogDomain.CONFIG, "load static tiles failed", t)
        }
    }
}
