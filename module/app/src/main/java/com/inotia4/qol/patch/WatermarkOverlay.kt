package com.inotia4.qol.patch

import android.app.Activity
import android.util.TypedValue
import android.view.Gravity
import android.view.View
import android.view.ViewGroup
import android.widget.FrameLayout
import android.widget.TextView
import com.inotia4.qol.LogDomain
import com.inotia4.qol.LogFile
import io.github.libxposed.api.XposedModuleInterface
import java.lang.reflect.Method

/**
 * 常驻水印（qol 三字母）：hook MainActivity.onWindowFocusChanged，在获焦回调后
 * 向 android.R.id.content 末尾注入一个半透明白色 TextView。
 * 游戏经默认 z 序的 SurfaceView（CCustomGLSurfaceView）渲染，Activity 合成时窗口层
 * view 恒在游戏画面之上，故所有场景（主菜单/大世界/面板）均可见；纯 UI 装饰，
 * 不消费触摸，任何失败只记日志、绝不影响游戏进程。
 */
object WatermarkOverlay {

    private const val TARGET_ACTIVITY =
        "com.com2us.inotia4.normal.freefull.google.global.android.common.MainActivity"

    /** 注入视图的 tag，用于幂等判定（重复获焦时不叠加第二个水印）。 */
    private const val WATERMARK_TAG = "inotia4_qol_watermark"

    /** 半透明白色（15% alpha），无任何装饰。透明度只调这一处：0x26=15%、0x1A=10%、0x33=20%。 */
    private const val WATERMARK_COLOR = 0x26FFFFFF.toInt()

    private const val MARGIN_DP = 12f

    fun install(param: XposedModuleInterface.PackageLoadedParam, hooker: (Method) -> Unit) {
        try {
            val cl = param.getDefaultClassLoader()
            val activityCls = cl.loadClass(TARGET_ACTIVITY)
            val method = activityCls.getDeclaredMethod("onWindowFocusChanged", Boolean::class.javaPrimitiveType)
            hooker(method)
            LogFile.info(LogDomain.PLATFORM, "WatermarkOverlay hook installed on MainActivity.onWindowFocusChanged")
        } catch (t: Throwable) {
            LogFile.error(LogDomain.PLATFORM, "WatermarkOverlay hook failed", t)
        }
    }

    /** 由 onWindowFocusChanged 钩子调用（已在 UI 线程）；幂等注入右下角 "qol" 水印。 */
    fun attachWatermark(activity: Activity) {
        try {
            val parent = activity.findViewById<ViewGroup>(android.R.id.content)
            if (parent == null) {
                LogFile.debug(LogDomain.PLATFORM) { "watermark attach skipped: content view is null" }
                return
            }
            val existing = parent.findViewWithTag<View>(WATERMARK_TAG)
            if (existing != null) {
                // 已存在：置顶保持最上层即可，不重复打日志（获焦回调会多次到达）。
                existing.bringToFront()
                return
            }

            val label = TextView(activity).apply {
                text = "qol"
                setTextColor(WATERMARK_COLOR)
                setTextSize(TypedValue.COMPLEX_UNIT_SP, 12f)
                background = null
                tag = WATERMARK_TAG
                isClickable = false
                isLongClickable = false
                isFocusable = false
                isFocusableInTouchMode = false
                importantForAccessibility = View.IMPORTANT_FOR_ACCESSIBILITY_NO
            }

            val marginPx = (MARGIN_DP * activity.resources.displayMetrics.density).toInt()
            val lp = FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT,
                ViewGroup.LayoutParams.WRAP_CONTENT
            ).apply {
                gravity = Gravity.END or Gravity.BOTTOM
                rightMargin = marginPx
                bottomMargin = marginPx
            }

            // addView 追加为最后一个子 view，保证不被窗口内其它视图盖住。
            parent.addView(label, lp)
            LogFile.info(LogDomain.PLATFORM, "watermark overlay attached")
        } catch (t: Throwable) {
            LogFile.error(LogDomain.PLATFORM, "watermark attach failed", t)
        }
    }
}
