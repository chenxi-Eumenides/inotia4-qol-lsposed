package com.inotia4.qol

import android.app.Activity
import android.content.Context
import android.content.Intent
import android.os.Handler
import android.os.HandlerThread
import io.github.libxposed.api.XposedInterface
import io.github.libxposed.api.XposedModule
import com.inotia4.qol.patch.AgreementGate
import com.inotia4.qol.patch.IapBlocker
import com.inotia4.qol.patch.ImmersiveMode
import com.inotia4.qol.patch.ResourceNamespaceBridge
import io.github.libxposed.api.XposedModuleInterface
import java.io.File

class HookMain : XposedModule() {

    override fun onModuleLoaded(param: XposedModuleInterface.ModuleLoadedParam) {
        super.onModuleLoaded(param)
        if (param.isSystemServer) return
        val processPackage = param.processName.substringBefore(':')
        if (!TargetPackages.contains(processPackage)) return
        TargetPackages.activate(processPackage)

        // 无 context 提前初始化日志（进程 uid 与游戏一致，可写游戏私有目录）
        LogFile.initEarly()
        LogFile.info(LogDomain.PLATFORM, "module loaded in process: ${param.processName}")

        // 捕获未处理异常写日志（防闪退信息丢失）
        Thread.setDefaultUncaughtExceptionHandler { thread, throwable ->
            LogFile.error(LogDomain.PLATFORM, "uncaught exception on ${thread.name}", throwable)
        }

        startBridgeLoop()
    }

    override fun onPackageLoaded(param: XposedModuleInterface.PackageLoadedParam) {
        if (!TargetPackages.contains(param.packageName)) return
        TargetPackages.activate(param.packageName)
        ResourceNamespaceBridge.install(param) { method ->
            hook(method)
                .setExceptionMode(XposedInterface.ExceptionMode.PROTECTIVE)
                .intercept { chain ->
                    if (chain.getExecutable().declaringClass.name == "android.content.res.Resources") {
                        val args = chain.getArgs().toMutableList()
                        ResourceNamespaceBridge.rewriteIdentifierArgs(args)
                        chain.proceed(args.toTypedArray())
                    } else {
                        ResourceNamespaceBridge.resolve(chain.getArg(0) as? String ?: "")
                    }
                }
        }
        IapBlocker.install(param) { method ->
            hook(method)
                .setExceptionMode(XposedInterface.ExceptionMode.PROTECTIVE)
                .intercept { chain ->
                    LogFile.info(LogDomain.PLATFORM, "blocked Hive SelectTarget.iapSelectTarget (payment dialog)")
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
                        LogFile.info(LogDomain.PLATFORM, "blocked AgreementUIActivity launch (outside main menu or world load in progress)")
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
                val ctx = currentApplication()
                if (ctx == null) {
                    LogFile.info(LogDomain.PLATFORM, "context not ready, retrying in 500ms")
                    handler.postDelayed(this, 500)
                    return
                }
                if (!NativeBridge.init()) {
                    LogFile.info(LogDomain.PLATFORM, "NativeBridge init failed: ${NativeBridge.nativeGetInitReport()}, retrying in 1s")
                    handler.postDelayed(this, 1000)
                    return
                }
                LogFile.onNativeReady()
                LogFile.info(LogDomain.PLATFORM, "NativeBridge init OK: ${NativeBridge.nativeGetInitReport()}")
                val moduleApk = getModuleApplicationInfo().sourceDir
                LogFile.logModuleIdentity(File(moduleApk))
                // feature 初始化无条件执行；HTTP 服务器按 apiEnabled 决定是否启动，
                // native 预取线程由 feature 初始化内的 applyToNative（nativeSetApiEnabled）控制。
                ApiServer.bootstrap(ctx, moduleApk)
            }
        })
    }

    private fun currentApplication(): Context? = try {
        val cl = Class.forName("android.app.ActivityThread")
        cl.getMethod("currentApplication").invoke(null) as? Context
    } catch (t: Throwable) {
        LogFile.error(LogDomain.PLATFORM, "currentApplication failed", t)
        null
    }
}
