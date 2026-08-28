package com.inotia4.export

import android.app.Activity
import android.content.Context
import android.content.Intent
import android.os.Handler
import android.os.HandlerThread
import io.github.libxposed.api.XposedInterface
import io.github.libxposed.api.XposedModule
import com.inotia4.export.patch.AgreementGate
import com.inotia4.export.patch.IapBlocker
import com.inotia4.export.patch.ImmersiveMode
import io.github.libxposed.api.XposedModuleInterface

class HookMain : XposedModule() {

    override fun onModuleLoaded(param: XposedModuleInterface.ModuleLoadedParam) {
        super.onModuleLoaded(param)
        if (param.isSystemServer) return
        if (param.processName != TARGET_PROCESS) return

        // 无 context 提前初始化日志（进程 uid 与游戏一致，可写游戏私有目录）
        LogFile.initEarly()
        LogFile.log("module loaded in process: ${param.processName}")

        // 捕获未处理异常写日志（防闪退信息丢失）
        Thread.setDefaultUncaughtExceptionHandler { thread, throwable ->
            LogFile.logError("uncaught exception on ${thread.name}", throwable)
        }

        startBridgeLoop()
    }

    override fun onPackageLoaded(param: XposedModuleInterface.PackageLoadedParam) {
        IapBlocker.install(param) { method ->
            hook(method)
                .setExceptionMode(XposedInterface.ExceptionMode.PROTECTIVE)
                .intercept { chain ->
                    LogFile.log("blocked Hive SelectTarget.iapSelectTarget (payment dialog)")
                    IapBlocker.recover()
                    null
                }
        }

        AgreementGate.install(param) { method, intentIdx ->
            hook(method)
                .setExceptionMode(XposedInterface.ExceptionMode.PROTECTIVE)
                .intercept { chain ->
                    val intent = chain.getArg(intentIdx) as? Intent
                    if (intent?.component?.className == AgreementGate.AGREEMENT_ACTIVITY && AgreementGate.shouldBlock()) {
                        LogFile.log("blocked AgreementUIActivity launch (outside main menu or world load in progress)")
                        null
                    } else {
                        chain.proceed()
                    }
                }
        }

        // 沉浸模式：hook 获焦回调隐藏系统栏（导航栏/手势条）。先 proceed 走原逻辑再做副作用。
        ImmersiveMode.install(param) { method ->
            hook(method)
                .setExceptionMode(XposedInterface.ExceptionMode.PROTECTIVE)
                .intercept { chain ->
                    val result = chain.proceed()
                    (chain.thisObject as? Activity)?.let { ImmersiveMode.applyImmersive(it) }
                    result
                }
        }
    }

    // 零 hook 方案：轮询 dlopen libgame.so（游戏加载后 dlsym 即成功），
    // 规避 hook 框架方法（System.loadLibrary 等）导致宿主崩溃的风险。
    private fun startBridgeLoop() {
        val thread = HandlerThread("bridge-init").also { it.start() }
        val handler = Handler(thread.looper)
        handler.post(object : Runnable {
            override fun run() {
                if (NativeBridge.init()) {
                    LogFile.log("NativeBridge init OK: ${NativeBridge.nativeGetInitReport()}")
                    val ctx = currentApplication()
                    if (ctx != null) {
                        val moduleApk = getModuleApplicationInfo().sourceDir
                        LogFile.logModuleIdentity(moduleApk)
                        ApiServer.start(ctx, moduleApk)
                        LogFile.log("ApiServer start requested, moduleApk=$moduleApk")
                    } else {
                        LogFile.log("context not ready, retrying in 500ms")
                        handler.postDelayed(this, 500)
                    }
                } else {
                    LogFile.log("NativeBridge init failed: ${NativeBridge.nativeGetInitReport()}, retrying in 1s")
                    handler.postDelayed(this, 1000)
                }
            }
        })
    }

    private fun currentApplication(): Context? = try {
        val cl = Class.forName("android.app.ActivityThread")
        cl.getMethod("currentApplication").invoke(null) as? Context
    } catch (t: Throwable) {
        LogFile.logError("currentApplication failed", t)
        null
    }

    companion object {
        private const val TARGET_PROCESS =
            "com.com2us.inotia4.normal.freefull.google.global.android.common"
    }
}
