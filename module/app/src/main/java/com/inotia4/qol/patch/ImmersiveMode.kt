package com.inotia4.qol.patch

import android.app.Activity
import android.os.Build
import android.view.View
import android.view.WindowInsets
import android.view.WindowInsetsController
import com.inotia4.qol.LogDomain
import com.inotia4.qol.LogFile
import io.github.libxposed.api.XposedModuleInterface
import java.lang.reflect.Method

/**
 * 沉浸模式（v0.4.36）：hook MainActivity.onWindowFocusChanged，窗口获焦时隐藏系统栏。
 * 游戏主题虽是 NoTitleBar.Fullscreen（状态栏已隐藏），但导航栏/手势条始终显示；
 * 只能通过 hook 在获焦回调中反复隐藏，覆盖弹窗/对话框关闭后系统栏重新出现的情况。
 */
object ImmersiveMode {

    private const val TARGET_ACTIVITY =
        "com.com2us.inotia4.normal.freefull.google.global.android.common.MainActivity"

    fun install(param: XposedModuleInterface.PackageLoadedParam, hooker: (Method) -> Unit) {
        try {
            val cl = param.getDefaultClassLoader()
            val activityCls = cl.loadClass(TARGET_ACTIVITY)
            val method = activityCls.getDeclaredMethod("onWindowFocusChanged", Boolean::class.javaPrimitiveType)
            hooker(method)
            LogFile.info(LogDomain.PLATFORM, "ImmersiveMode hook installed on MainActivity.onWindowFocusChanged")
        } catch (t: Throwable) {
            LogFile.error(LogDomain.PLATFORM, "ImmersiveMode hook failed", t)
        }
    }

    fun applyImmersive(activity: Activity) {
        try {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
                activity.window.insetsController?.let { c: WindowInsetsController ->
                    c.hide(WindowInsets.Type.systemBars())
                    c.systemBarsBehavior =
                        WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
                }
            } else {
                activity.window.decorView.systemUiVisibility =
                    View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY or
                    View.SYSTEM_UI_FLAG_FULLSCREEN or
                    View.SYSTEM_UI_FLAG_HIDE_NAVIGATION or
                    View.SYSTEM_UI_FLAG_LAYOUT_STABLE or
                    View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN or
                    View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
            }
        } catch (t: Throwable) {
            LogFile.error(LogDomain.PLATFORM, "ImmersiveMode apply failed", t)
        }
    }
}
