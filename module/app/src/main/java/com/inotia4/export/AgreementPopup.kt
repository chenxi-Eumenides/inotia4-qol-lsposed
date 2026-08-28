package com.inotia4.export

import android.app.Activity
import android.os.SystemClock
import android.view.MotionEvent

/**
 * 关闭 Java 层同意页（AgreementUIActivity）：在该 Activity 窗口上重放登记的启动触摸
 * （docs/environment.md §3.1：touch_automation --inject input click 420,280），与手动点击
 * 走完全相同的 touch dispatch 管线，不依赖其内部按钮实现。
 */
object AgreementPopup {
    const val TAP_X = 420
    const val TAP_Y = 280
    private const val TAP_DURATION_MS = 80L

    /** 在 UI 线程重放 down + 延迟 up；窗口已销毁时返回 false。点击异步生效，调用方随后轮询
     *  /api/ui/screen 直到 main_menu。 */
    fun dismiss(activity: Activity): Boolean {
        val decor = activity.window?.decorView ?: return false
        val downTime = SystemClock.uptimeMillis()
        val down = MotionEvent.obtain(
            downTime, downTime, MotionEvent.ACTION_DOWN, TAP_X.toFloat(), TAP_Y.toFloat(), 0,
        )
        val up = MotionEvent.obtain(
            downTime, downTime + TAP_DURATION_MS, MotionEvent.ACTION_UP, TAP_X.toFloat(), TAP_Y.toFloat(), 0,
        )
        activity.runOnUiThread {
            try {
                decor.dispatchTouchEvent(down)
                decor.postDelayed({
                    try {
                        decor.dispatchTouchEvent(up)
                    } finally {
                        up.recycle()
                    }
                }, TAP_DURATION_MS)
            } finally {
                down.recycle()
            }
        }
        return true
    }
}
