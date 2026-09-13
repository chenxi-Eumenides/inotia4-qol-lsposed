package com.inotia4.qol.patch

import com.inotia4.qol.LogDomain
import com.inotia4.qol.LogFile
import com.inotia4.qol.NativeBridge
import io.github.libxposed.api.XposedModuleInterface
import java.lang.reflect.Method

object IapBlocker {

    fun install(param: XposedModuleInterface.PackageLoadedParam, hooker: (Method) -> Unit) {
        if (skipHiveBlock(param.packageName)) {
            LogFile.info(LogDomain.PLATFORM, "hive payment block skipped (flag file present, observation mode)")
            return
        }
        try {
            val cl = param.getDefaultClassLoader()
            val target = cl.loadClass("com.com2us.module.inapp.SelectTarget")
            val method = target.getDeclaredMethod(
                "iapSelectTarget",
                Class.forName("android.app.Activity", false, cl),
                Class.forName("com.com2us.module.view.SurfaceViewWrapper", false, cl),
                Class.forName("com.com2us.module.inapp.SelectTargetCallback", false, cl),
                Long::class.javaPrimitiveType
            )
            hooker(method)
        } catch (t: Throwable) {
            LogFile.error(LogDomain.PLATFORM, "SelectTarget hook failed", t)
        }
    }

    fun recover() {
        if (!NativeBridge.ready) return
        try {
            LogFile.info(LogDomain.PLATFORM, "hive recovery: ${NativeBridge.nativeRecoverAfterHiveBlock()}")
        } catch (t: Throwable) {
            LogFile.error(LogDomain.PLATFORM, "hive recovery failed", t)
        }
    }

    private fun skipHiveBlock(packageName: String): Boolean = try {
        java.io.File("/sdcard/Android/data/$packageName/files/skip_hive_block.flag").exists()
    } catch (t: Throwable) {
        LogFile.error(LogDomain.PLATFORM, "skipHiveBlock check failed", t)
        false
    }
}
