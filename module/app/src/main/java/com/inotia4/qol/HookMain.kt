package com.inotia4.qol

import android.app.Activity
import android.content.Intent
import io.github.libxposed.api.XposedInterface
import io.github.libxposed.api.XposedModule
import com.inotia4.qol.patch.ImmersiveMode
import com.inotia4.qol.patch.PopupBlockers
import com.inotia4.qol.patch.ResourceNamespaceBridge
import com.inotia4.qol.patch.WatermarkOverlay
import com.inotia4.qol.storage.PickedFileIngest
import io.github.libxposed.api.XposedModuleInterface

class HookMain : XposedModule() {

    override fun onModuleLoaded(param: XposedModuleInterface.ModuleLoadedParam) {
        super.onModuleLoaded(param)
        if (param.isSystemServer) return
        val processPackage = param.processName.substringBefore(':')
        if (processPackage.isEmpty()) return
        // 改包名的变体 applicationId 不再是构建期列表中的值；先采用框架上报的进程包名，
        // 宿主身份在 onPackageLoaded 用类探测确认。
        TargetPackages.activate(processPackage)

        // 无 context 提前初始化日志（进程 uid 与游戏一致，可写游戏私有目录）
        LogFile.initEarly()
        LogFile.info(LogDomain.PLATFORM, "module loaded in process: ${param.processName}")

        // 捕获未处理异常：写独立崩溃取证文件并转发前序 handler（防闪退信息丢失）
        JavaCrashReporter.install()
    }

    override fun onPackageLoaded(param: XposedModuleInterface.PackageLoadedParam) {
        if (!HostIdentity.isInotia4(param)) {
            LogFile.warn(
                LogDomain.PLATFORM,
                "package ${param.packageName} is not an Inotia4 host; hooks skipped",
            )
            return
        }
        TargetPackages.activate(param.packageName)
        ModuleHealthMonitor.start()
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
        // SAF 自选备份导入：拦截宿主 MainActivity.onActivityResult。先 proceed 放行游戏自身
        // 逻辑（它内部调 super 并转发 Peppermint），再把结果转发给摄入器；kernel-only 分支
        // 不安装，PickedFileIngest 在该模式下不产生行为副作用。
        MainActivityStartupHook.installActivityResult(param) { method ->
            hook(method)
                .setExceptionMode(XposedInterface.ExceptionMode.PROTECTIVE)
                .intercept { chain ->
                    val result = chain.proceed()
                    (chain.thisObject as? Activity)?.let { activity ->
                        PickedFileIngest.onActivityResult(
                            activity,
                            chain.getArg(0) as Int,
                            chain.getArg(1) as Int,
                            chain.getArg(2) as Intent?,
                        )
                    }
                    result
                }
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
