package com.inotia4.qol

import android.app.Activity
import io.github.libxposed.api.XposedInterface
import io.github.libxposed.api.XposedModule
import com.inotia4.qol.patch.ImmersiveMode
import com.inotia4.qol.patch.PopupBlockers
import com.inotia4.qol.patch.ResourceNamespaceBridge
import com.inotia4.qol.patch.WatermarkOverlay
import io.github.libxposed.api.XposedModuleInterface

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

        ModuleHealthMonitor.start()
    }

    override fun onPackageLoaded(param: XposedModuleInterface.PackageLoadedParam) {
        if (!TargetPackages.contains(param.packageName)) return
        TargetPackages.activate(param.packageName)
        val moduleApkPath = getModuleApplicationInfo().sourceDir
        MainActivityStartupHook.install(param) { method ->
            hook(method)
                .setExceptionMode(XposedInterface.ExceptionMode.PROTECTIVE)
                .intercept { chain ->
                    val result = chain.proceed()
                    (chain.thisObject as? Activity)?.let {
                        MainActivityStartupHook.onActivityCreated(it, moduleApkPath)
                    }
                    result
                }
        }
        if (BuildConfig.KERNEL_ONLY) {
            installWatermarkHook(param, moduleApkPath)
            LogFile.info(LogDomain.PLATFORM, "kernel-only mode: business hooks skipped")
            return
        }
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

        // 弹窗屏蔽收束入口：更新通知弹窗、Hive 内购弹窗、同意页启动拦截与自动关闭、
        // "Connecting.." 进度框，全部在 patch/PopupBlockers.kt 一个文件内管理。
        PopupBlockers.install(param) { method, mode, handler ->
            hook(method)
                .setExceptionMode(mode)
                .intercept(handler)
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
        installWatermarkHook(param, moduleApkPath)
    }

    private fun installWatermarkHook(param: XposedModuleInterface.PackageLoadedParam, moduleApkPath: String) {
        // Health overlay and the regular watermark share the existing MainActivity focus seam.
        WatermarkOverlay.install(param) { method ->
            hook(method)
                .setExceptionMode(XposedInterface.ExceptionMode.PROTECTIVE)
                .intercept { chain ->
                    val focused = chain.getArg(0) as? Boolean ?: false
                    val result = chain.proceed()
                    ModuleHealthMonitor.onMainActivityVisibility(focused)
                    (chain.thisObject as? Activity)?.let { activity ->
                        if (focused) MainActivityStartupHook.onActivityFocused(activity, moduleApkPath)
                        WatermarkOverlay.attachWatermark(activity)
                    }
                    result
                }
        }
    }
}
