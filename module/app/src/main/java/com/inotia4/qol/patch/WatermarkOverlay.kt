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
import com.inotia4.qol.ModuleConfig
import io.github.libxposed.api.XposedModuleInterface
import java.lang.ref.WeakReference
import java.lang.reflect.Method

/**
 * 常驻水印（qol 三字母）：hook MainActivity.onWindowFocusChanged，在获焦回调后
 * 向 android.R.id.content 末尾注入一个半透明 TextView。
 * 游戏经默认 z 序的 SurfaceView（CCustomGLSurfaceView）渲染，Activity 合成时窗口层
 * view 恒在游戏画面之上，故所有场景（主菜单/大世界/面板）均可见；纯 UI 装饰，
 * 不消费触摸，任何失败只记日志、绝不影响游戏进程。
 *
 * 颜色策略：由静态配色表 [COLOR_RULES] 决定 —— 按序取第一条命中的规则，全不命中用默认色。
 * 表中每一行是完整的 ARGB（色相与不透明度一起定义），所以不同情况可以有不同的显眼程度：
 * 当前一行「简单模式」开启 → 亮绿 + 80% 不透明度（作为开关状态指示，需明显可见），
 * 默认态仍是 15% 的半透明白（仅作痕迹）。
 * 新增其它情况只需往表里加一行；开关变更由 ConfigApiService.applyOnChange 调
 * refreshColor() 实时刷新（UI 开关与 HTTP API 都经该点）。
 */
object WatermarkOverlay {

    private const val TARGET_ACTIVITY =
        "com.com2us.inotia4.normal.freefull.google.global.android.common.MainActivity"

    /** 注入视图的 tag，用于幂等判定（重复获焦时不叠加第二个水印）。 */
    private const val WATERMARK_TAG = "inotia4_qol_watermark"

    /** 默认色：半透明白色，无任何装饰。alpha 只调这一处：0x26=15%、0x1A=10%、0x33=20%。 */
    private const val WATERMARK_COLOR = 0x26FFFFFF.toInt()

    /**
     * 简单模式开启时的水印色：亮绿色 + 明显提高不透明度。
     * 这里 alpha 与默认色**不同是有意的**——它是「开关状态」的可见指示，不能是背景里几乎看不见的
     * 半透明痕迹；0xCC = 80%。要调就改这一个常量（ARGB 一次写全）。
     */
    private const val WATERMARK_COLOR_SIMPLE_MODE = 0xCC00FF00.toInt()

    /**
     * 配色表一行的形状：一条配置条件 + 条件成立时水印用的颜色。
     * [enabled] 只读 ModuleConfig（@Volatile），可被任意线程安全调用。
     */
    private class WatermarkColorRule(
        /** 规则名，仅用于日志定位命中项。 */
        val name: String,
        /** 条件成立时的水印 ARGB 色。 */
        val color: Int,
        /** 条件取值（布尔配置项）。 */
        val enabled: () -> Boolean,
    )

    /**
     * 水印配色表（静态表；按顺序取第一条命中的规则）。
     *
     * 顺序即优先级：越靠前的行越优先，多条同时成立时取第一条。
     * 全部不成立（含表为空）时回退 [WATERMARK_COLOR]。
     *
     * 新增情况（其它配置开关 → 其它颜色）：只需在表里加一行，
     * 不必改注入、刷新、取色或日志逻辑。
     */
    private val COLOR_RULES: List<WatermarkColorRule> = listOf(
        WatermarkColorRule("简单模式", WATERMARK_COLOR_SIMPLE_MODE) { ModuleConfig.simpleModeEnabled },
    )

    private const val MARGIN_DP = 12f

    /** 已注入的水印视图。弱引用：水印随 Activity 生命周期存在，模块不得持有 Activity。 */
    private var watermarkView: WeakReference<TextView>? = null

    /** 配色表中第一条命中的规则；无命中返回 null（调用方回退默认色）。 */
    private fun currentRule(): WatermarkColorRule? = COLOR_RULES.firstOrNull { it.enabled() }

    /** 当前水印色：命中规则取规则色，否则默认半透明白。 */
    private fun currentColor(): Int = currentRule()?.color ?: WATERMARK_COLOR

    /** 当前命中的规则名（仅日志用）。 */
    private fun currentRuleName(): String = currentRule()?.name ?: "默认"

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
                // 顺带同步颜色：获焦回调是「配置可能已变」的自然刷新点（与 refreshColor 双保险）。
                (existing as? TextView)?.setTextColor(currentColor())
                return
            }

            val label = TextView(activity).apply {
                text = "qol"
                setTextColor(currentColor())
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
            watermarkView = WeakReference(label)
            LogFile.info(LogDomain.PLATFORM, "watermark overlay attached color=${Integer.toHexString(currentColor())} rule=${currentRuleName()}")
        } catch (t: Throwable) {
            LogFile.error(LogDomain.PLATFORM, "watermark attach failed", t)
        }
    }

    /**
     * 配置变更后实时刷新水印颜色（简单模式开关 → 亮绿色）。
     * 可从任意线程调用：视图更新经 post 回到 UI 线程；视图尚未注入或已随 Activity
     * 销毁时静默跳过（下一次 onWindowFocusChanged 注入时会用新色）。
     */
    fun refreshColor() {
        val view = watermarkView?.get() ?: return
        view.post {
            try {
                view.setTextColor(currentColor())
                LogFile.info(LogDomain.PLATFORM, "watermark color refreshed color=${Integer.toHexString(currentColor())} rule=${currentRuleName()}")
            } catch (t: Throwable) {
                LogFile.error(LogDomain.PLATFORM, "watermark color refresh failed", t)
            }
        }
    }
}
