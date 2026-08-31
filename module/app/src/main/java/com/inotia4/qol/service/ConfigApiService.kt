package com.inotia4.qol.service

import com.inotia4.qol.LogFile
import com.inotia4.qol.ModuleConfig
import com.inotia4.qol.NativeBridge
import com.inotia4.qol.StaticData

/**
 * 模块配置→native 生效服务层接口（v0.5.46 P1 收边）。
 * 收口 nativeSetStackLimitEnabled/nativeSetMoveMergeEnabled/nativeSetTilesData 直调：
 * 启动期走 [applyToNative]，配置端点变更走 [applyOnChange]，瓦片矩阵加载走 [applyTiles]。
 */
interface ConfigApiService {
    /** 启动期全量应用：堆叠上限、拖拽合并与静态瓦片矩阵（内部自行判断 NativeBridge.ready） */
    fun applyToNative()

    /** 配置变更增量应用：仅当对应值变化时通知 native（POST /api/config/set 用） */
    fun applyOnChange(oldStack: Boolean, oldMoveMerge: Boolean, oldExtensionBag: Boolean)

    /** 加载静态瓦片矩阵入 native（替代运行时读内存，P0#瓦片矩阵 2026-08-12） */
    fun applyTiles()
}

/**
 * 模块配置→native 生效服务实现（ConfigApiService 唯一实现，v0.5.46 迁移自 ConfigController/ApiServer 直调）。
 */
class ConfigApiServiceImpl : ConfigApiService {

    override fun applyToNative() {
        if (!NativeBridge.ready) return
        val applied = NativeBridge.nativeSetStackLimitEnabled(ModuleConfig.stackLimitIncrease)
        LogFile.log("stackLimitIncrease=${ModuleConfig.stackLimitIncrease} applied=$applied")
        val moveMergeApplied = NativeBridge.nativeSetMoveMergeEnabled(ModuleConfig.moveMergeEnabled)
        LogFile.log("moveMergeEnabled=${ModuleConfig.moveMergeEnabled} applied=$moveMergeApplied")
        val extensionApplied = NativeBridge.nativeSetExtensionBagEnabled(ModuleConfig.extensionBagEnabled)
        LogFile.log("extensionBagEnabled=${ModuleConfig.extensionBagEnabled} applied=$extensionApplied")
        applyTiles()
    }

    override fun applyOnChange(oldStack: Boolean, oldMoveMerge: Boolean, oldExtensionBag: Boolean) {
        if (!NativeBridge.ready) return
        if (ModuleConfig.stackLimitIncrease != oldStack) {
            NativeBridge.nativeSetStackLimitEnabled(ModuleConfig.stackLimitIncrease)
        }
        if (ModuleConfig.moveMergeEnabled != oldMoveMerge) {
            NativeBridge.nativeSetMoveMergeEnabled(ModuleConfig.moveMergeEnabled)
        }
        if (ModuleConfig.extensionBagEnabled != oldExtensionBag) {
            NativeBridge.nativeSetExtensionBagEnabled(ModuleConfig.extensionBagEnabled)
        }
    }

    override fun applyTiles() {
        try {
            val tilesJson = StaticData.read("maps/tiles.json")
            if (tilesJson != null && NativeBridge.ready) {
                val ok = NativeBridge.nativeSetTilesData(tilesJson)
                LogFile.log("static tiles loaded: $ok")
            } else if (tilesJson == null) {
                LogFile.log("static tiles read failed: maps/tiles.json missing")
            }
        } catch (t: Throwable) {
            LogFile.logError("load static tiles failed", t)
        }
    }
}
