package com.inotia4.qol

import android.app.Activity
import android.content.Context
import android.os.Bundle
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
    private const val TARGET_ACTIVITY =
        "com.com2us.inotia4.normal.freefull.google.global.android.common.MainActivity"

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
            val activityClass = param.getDefaultClassLoader().loadClass(TARGET_ACTIVITY)
            val onCreate = activityClass.getDeclaredMethod("onCreate", Bundle::class.java)
            hooker(onCreate)
            LogFile.info(LogDomain.PLATFORM, "MainActivity startup hook installed on onCreate")
        } catch (t: Throwable) {
            LogFile.error(LogDomain.PLATFORM, "MainActivity onCreate startup hook failed", t)
        }
    }

    fun onActivityCreated(activity: Activity, moduleApkPath: String) {
        requestStartup(activity.applicationContext, moduleApkPath)
    }

    fun onActivityFocused(activity: Activity, moduleApkPath: String) {
        requestStartup(activity.applicationContext, moduleApkPath)
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
                    context.getExternalFilesDir(null)?.absolutePath.orEmpty(),
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
            LogFile.info(LogDomain.PLATFORM, "NativeBridge init OK: ${nativeInitReport()}")
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

    private fun nativeInitReport(): String = try {
        NativeBridge.nativeGetInitReport()
    } catch (t: Throwable) {
        LogFile.error(LogDomain.PLATFORM, "native init report unavailable", t)
        "native init report unavailable"
    }
}
