package com.inotia4.qol

import android.app.Activity
import android.content.Context
import android.content.Intent
import android.os.Bundle
import com.inotia4.qol.storage.HostActivityTracker
import io.github.libxposed.api.XposedModuleInterface
import java.io.File
import java.lang.reflect.Method
import java.util.concurrent.Executors

internal class ActivityStartupProgress {
    private var attemptInProgress = false
    private var pendingActivityEvent = false
    private var completed = false
    private var moduleRuntimeInitialized = false

    val isCompleted: Boolean
        get() = completed

    val needsModuleRuntimeInitialization: Boolean
        get() = !moduleRuntimeInitialized

    fun requestActivityEvent(): Boolean {
        if (completed) return false
        if (attemptInProgress) {
            pendingActivityEvent = true
            return false
        }
        attemptInProgress = true
        return true
    }

    fun beginAttempt() {
        pendingActivityEvent = false
    }

    fun markModuleRuntimeInitialized() {
        moduleRuntimeInitialized = true
    }

    /** Returns true only when a real Activity event arrived during the failed attempt. */
    fun finishAttempt(runtimeReady: Boolean): Boolean {
        if (runtimeReady) completed = true
        if (!completed && pendingActivityEvent) {
            pendingActivityEvent = false
            return true
        }
        attemptInProgress = false
        return false
    }

    fun cancelQueuedAttempt() {
        attemptInProgress = false
        pendingActivityEvent = false
    }
}

/** Starts native/runtime work from real MainActivity lifecycle events after libgame is loaded. */
internal object MainActivityStartupHook {
    private val lock = Any()
    private val executor = Executors.newSingleThreadExecutor { runnable ->
        Thread(runnable, "bridge-init").apply { isDaemon = true }
    }
    private val startupProgress = ActivityStartupProgress()
    private var pendingContext: Context? = null
    private var pendingModuleApkPath: String? = null
    private var kernelServerBootstrapped = false

    fun install(param: XposedModuleInterface.PackageLoadedParam, hooker: (Method) -> Unit) {
        try {
            val activityClass = HostIdentity.resolveMainActivity(
                param.getDefaultClassLoader(),
                param.packageName,
            ) ?: error("MainActivity class not found for ${param.packageName}")
            val onCreate = activityClass.getDeclaredMethod("onCreate", Bundle::class.java)
            hooker(onCreate)
            LogFile.info(LogDomain.PLATFORM, "MainActivity startup hook installed on onCreate")
        } catch (t: Throwable) {
            LogFile.error(LogDomain.PLATFORM, "MainActivity onCreate startup hook failed", t)
        }
    }

    fun onActivityCreated(activity: Activity, moduleApkPath: String) {
        ModuleErrorDialog.onActivityAvailable(activity)
        HostActivityTracker.onActivityAvailable(activity)
        requestStartup(activity.applicationContext, moduleApkPath)
    }

    fun onActivityFocused(activity: Activity, moduleApkPath: String) {
        ModuleErrorDialog.onActivityAvailable(activity)
        HostActivityTracker.onActivityAvailable(activity)
        requestStartup(activity.applicationContext, moduleApkPath)
    }

    /**
     * 安装宿主 MainActivity.onActivityResult hook：SAF 文件选择器的结果回调入口。
     * 与 [install] 用 [HostIdentity.resolveMainActivity] 解析同一个类，失败按相同风格落日志不影响其他 hook。
     */
    fun installActivityResult(param: XposedModuleInterface.PackageLoadedParam, hooker: (Method) -> Unit) {
        try {
            val activityClass = HostIdentity.resolveMainActivity(
                param.getDefaultClassLoader(),
                param.packageName,
            ) ?: error("MainActivity class not found for ${param.packageName}")
            val onActivityResult = activityClass.getDeclaredMethod(
                "onActivityResult",
                Int::class.javaPrimitiveType,
                Int::class.javaPrimitiveType,
                Intent::class.java,
            )
            hooker(onActivityResult)
            LogFile.info(LogDomain.PLATFORM, "MainActivity onActivityResult hook installed")
        } catch (t: Throwable) {
            LogFile.error(LogDomain.PLATFORM, "MainActivity onActivityResult hook failed", t)
        }
    }

    private fun requestStartup(context: Context, moduleApkPath: String) {
        val shouldSchedule = synchronized(lock) {
            if (startupProgress.isCompleted) return
            pendingContext = context.applicationContext
            pendingModuleApkPath = moduleApkPath
            startupProgress.requestActivityEvent()
        }
        if (!shouldSchedule) return
        scheduleAttempt("MainActivity event")
    }

    private fun scheduleAttempt(reason: String) {
        try {
            executor.execute(::runPendingAttempt)
        } catch (t: Throwable) {
            synchronized(lock) { startupProgress.cancelQueuedAttempt() }
            LogFile.error(LogDomain.PLATFORM, "bridge-init worker rejected $reason", t)
        }
    }

    private fun runPendingAttempt() {
        val (context, moduleApkPath) = synchronized(lock) {
            startupProgress.beginAttempt()
            val pending = pendingContext to pendingModuleApkPath
            pendingContext = null
            pendingModuleApkPath = null
            pending
        }
        if (context == null || moduleApkPath == null) {
            finishAttempt(runtimeReady = false)
            return
        }

        val runtimeReady = try {
            initializeFromActivity(context, moduleApkPath)
        } catch (t: Throwable) {
            LogFile.error(LogDomain.PLATFORM, "MainActivity startup initialization failed", t)
            false
        }
        finishAttempt(runtimeReady)
    }

    private fun finishAttempt(runtimeReady: Boolean) {
        val retryPendingEvent = synchronized(lock) {
            val retry = startupProgress.finishAttempt(runtimeReady)
            if (!retry) {
                pendingContext = null
                pendingModuleApkPath = null
            }
            retry
        }
        if (retryPendingEvent) scheduleAttempt("coalesced MainActivity event")
    }

    private fun initializeFromActivity(context: Context, moduleApkPath: String): Boolean {
        QolPaths.ensureLayout(context)
        QolPaths.migrateLegacy(context)
        if (BuildConfig.KERNEL_ONLY) {
            if (!kernelServerBootstrapped) {
                ApiServer.bootstrap(context, moduleApkPath)
                kernelServerBootstrapped = true
            }
            return true
        }

        if (!NativeBridge.ready) {
            try {
                when (NativeBridge.nativeConfigStoreBootstrap(
                    QolPaths.root(context)?.absolutePath.orEmpty(),
                )) {
                    0 -> Unit
                    1 -> LogFile.warn(LogDomain.CONFIG, "native config store ready without persistence")
                    2 -> LogFile.warn(LogDomain.CONFIG, "native config store degraded read-only defaults")
                    else -> LogFile.warn(LogDomain.CONFIG, "native config store returned unknown status")
                }
            } catch (t: Throwable) {
                LogFile.error(LogDomain.CONFIG, "native config store bootstrap failed", t)
            }

            val nativeReady = NativeBridge.init()
            ModuleHealthMonitor.onNativeInitializationAttemptCompleted()
            if (!nativeReady) {
                val report = if (!NativeBridge.libraryLoaded) {
                    "native library unavailable"
                } else {
                    try {
                        NativeBridge.nativeGetInitReport()
                    } catch (t: Throwable) {
                        LogFile.error(LogDomain.PLATFORM, "native init report unavailable", t)
                        "native init report unavailable"
                    }
                }
                LogFile.info(LogDomain.PLATFORM, "NativeBridge init failed: $report; waiting for next Activity event")
                return false
            }
            LogFile.onNativeReady()
            LogFile.info(LogDomain.PLATFORM, "NativeBridge init OK: ${nativeInitReportSummary()}")
            LogFile.debug(LogDomain.PLATFORM) { "NativeBridge init symbol report: ${nativeInitReport()}" }
            LogFile.logModuleIdentity(File(moduleApkPath))
        }

        if (synchronized(lock) { startupProgress.needsModuleRuntimeInitialization }) {
            ModuleRuntimeBootstrap.initialize(context, moduleApkPath)
            synchronized(lock) { startupProgress.markModuleRuntimeInitialized() }
        }
        when (PluginRuntimeEffects.subscribeAndAwaitInitialSnapshot()) {
            RuntimeEffectBootstrapState.SUBSCRIPTION_NOT_ESTABLISHED -> {
                LogFile.warn(
                    LogDomain.CONFIG,
                    "core.api runtime-effect subscription was not established; waiting for next Activity event",
                )
                return false
            }
            RuntimeEffectBootstrapState.INITIAL_SNAPSHOT_PENDING -> {
                LogFile.warn(
                    LogDomain.CONFIG,
                    "initial core.api runtime effect is pending; waiting for a later Activity event",
                )
                return false
            }
            RuntimeEffectBootstrapState.READY -> Unit
        }
        ApiServer.bootstrap(context, moduleApkPath)
        return true
    }

    private val trueFieldPattern = Regex("\":true")
    private val falseFieldPattern = Regex("\":false")

    /**
     * 符号就绪摘要：INFO 只留 `symbols=<ok>/<total>`。
     * 完整 90 项 VMA 报告单行约 5 KB，超过 logcat 单条上限，且逐项明细只在排查
     * 符号缺失时有用，因此明细降为 DEBUG，由 debug.logging 开关控制。
     */
    private fun nativeInitReportSummary(): String {
        val report = nativeInitReport()
        val ok = trueFieldPattern.findAll(report).count()
        val missing = falseFieldPattern.findAll(report).count()
        val total = ok + missing
        return if (total == 0) report else "symbols=$ok/$total"
    }

    private fun nativeInitReport(): String = try {
        NativeBridge.nativeGetInitReport()
    } catch (t: Throwable) {
        LogFile.error(LogDomain.PLATFORM, "native init report unavailable", t)
        "native init report unavailable"
    }
}
