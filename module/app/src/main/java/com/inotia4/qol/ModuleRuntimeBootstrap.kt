package com.inotia4.qol

import android.content.Context
import com.inotia4.qol.store.ModuleSaveStore

/** Module-owned startup work that remains active independently of the API listener. */
object ModuleRuntimeBootstrap {
    @Volatile
    private var initialized = false

    @Volatile
    private var assetsInjected = false

    @Synchronized
    fun initialize(context: Context, moduleApkPath: String?) {
        if (initialized) return
        initialized = true

        StaticData.attach(context)
        injectModuleAssets(context, moduleApkPath)
        try {
            CoreDataAssets.load(context)
        } catch (t: Throwable) {
            LogFile.error(LogDomain.HTTP, "core.data static asset ingress failed", t)
        }
        ModuleSaveStore.initialize(context)
        NativeBridge.nativeSaveBackupInit(
            context.applicationInfo.dataDir,
            QolPaths.root(context)?.absolutePath ?: "",
        )
        try {
            NativeBridge.nativeSaveBackupSetMapNames(StaticData.buildMapNamesJson())
        } catch (t: Throwable) {
            LogFile.error(LogDomain.HTTP, "savebackup set map names failed", t)
        }
        try {
            NativeBridge.nativeSetWorldMapExits(StaticData.buildMapExitsJson())
        } catch (t: Throwable) {
            LogFile.error(LogDomain.HTTP, "world facts set map exits failed", t)
        }
        try {
            NativeBridge.nativeSetItemStaticData(StaticData.buildItemStaticData())
        } catch (t: Throwable) {
            LogFile.error(LogDomain.HTTP, "item facts set static data failed", t)
        }
        try {
            NativeBridge.nativeSetQuestStaticData(StaticData.buildQuestStaticData())
        } catch (t: Throwable) {
            LogFile.error(LogDomain.HTTP, "quest set static data failed", t)
        }
        try {
            val gameVersionName = try {
                context.packageManager.getPackageInfo(TargetPackages.current(), 0).versionName ?: ""
            } catch (t: Throwable) {
                LogFile.error(LogDomain.HTTP, "game version read failed", t)
                ""
            }
            NativeBridge.nativeSetSystemIdentity(
                BuildConfig.VERSION_NAME,
                TargetPackages.current(),
                gameVersionName,
            )
        } catch (t: Throwable) {
            LogFile.error(LogDomain.HTTP, "system_facts set identity failed", t)
        }
        try {
            val result = NativeBridge.nativeSaveBackupUiInject()
            LogFile.info(LogDomain.HTTP, "savebackup ui inject: $result")
            val selfCheck = NativeBridge.nativeSaveBackupUiSelfCheck()
            LogFile.info(LogDomain.HTTP, "savebackup self check: $selfCheck")
        } catch (t: Throwable) {
            LogFile.error(LogDomain.HTTP, "savebackup ui init failed", t)
        }
        ExtensionBagUiBridge.initialize(context)
        com.inotia4.qol.service.ApiServices.config.applyTiles()
    }

    /** AndServer scans module APK assets even when its listener is enabled later. */
    @Synchronized
    fun injectModuleAssets(context: Context, moduleApkPath: String?) {
        if (assetsInjected) return
        if (moduleApkPath == null) {
            assetsInjected = true
            return
        }
        try {
            val addAssetPath = android.content.res.AssetManager::class.java
                .getMethod("addAssetPath", String::class.java)
            addAssetPath.invoke(context.assets, moduleApkPath)
            assetsInjected = true
            LogFile.info(LogDomain.HTTP, "module assets added: $moduleApkPath")
        } catch (t: Throwable) {
            LogFile.error(LogDomain.HTTP, "addAssetPath failed", t)
        }
    }
}
