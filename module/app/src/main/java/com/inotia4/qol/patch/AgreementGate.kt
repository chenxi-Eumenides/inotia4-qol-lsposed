package com.inotia4.qol.patch

import android.content.Intent
import android.os.SystemClock
import com.inotia4.qol.LogFile
import com.inotia4.qol.NativeBridge
import com.inotia4.qol.util.JsonUtil
import io.github.libxposed.api.XposedModuleInterface
import java.lang.reflect.Method

/**
 * 仅允许同意页（AgreementUIActivity）在主菜单弹出。hook Instrumentation.execStartActivity
 * 丢弃其余场景的启动：进档宽限期内（enter_slot/create_slot 后 15s）或 native screen 非
 * main_menu。否则同意页 Activity 启动会打断读档流程导致角色数据丢失（2026-08-28 save1 实测复现）。
 * 首次启动时放行同意页，避免启动阶段的 loading/connecting 被误判为读档；
 * 探针失败时放行（API 层门禁仍 fail-closed 兜底）。
 */
object AgreementGate {
    const val AGREEMENT_ACTIVITY = "com.com2us.module.activeuser.useragree.AgreementUIActivity"
    private const val WORLD_LOAD_GRACE_MS = 15_000L

    @Volatile
    private var worldLoadUntil = 0L

    @Volatile
    private var startupAgreementPending = true

    fun beginWorldLoad() {
        worldLoadUntil = SystemClock.uptimeMillis() + WORLD_LOAD_GRACE_MS
    }

    fun shouldBlock(): Boolean {
        if (SystemClock.uptimeMillis() < worldLoadUntil) return true
        if (startupAgreementPending) {
            startupAgreementPending = false
            return false
        }
        if (!NativeBridge.ready) return false
        val screen = try {
            JsonUtil.parseObj(NativeBridge.nativeGetGamestateJson())?.optString("screen", "")
        } catch (t: Throwable) {
            LogFile.logError("agreement gate screen probe failed", t)
            null
        }
        return screen != null && screen != "main_menu"
    }

    fun install(param: XposedModuleInterface.PackageLoadedParam, hooker: (Method, Int) -> Unit) {
        try {
            val cl = param.getDefaultClassLoader()
            val intentClass = Class.forName("android.content.Intent", false, cl)
            Class.forName("android.app.Instrumentation", false, cl).declaredMethods
                .filter { it.name == "execStartActivity" }
                .mapNotNull { m ->
                    val idx = m.parameterTypes.indexOf(intentClass)
                    if (idx >= 0) m to idx else null
                }
                .forEach { (m, idx) -> hooker(m, idx) }
        } catch (t: Throwable) {
            LogFile.logError("AgreementGate install failed", t)
        }
    }
}
