package com.inotia4.qol

import android.app.Activity
import android.os.SystemClock
import android.view.MotionEvent
import android.view.View
import android.view.ViewGroup
import android.webkit.WebView
import android.widget.TextView
import java.util.ArrayDeque

/**
 * 关闭 Java 层同意页（AgreementUIActivity）。
 *
 * 该页内容为 WebView（HTML），View 树中通常没有原生按钮，因此优先在 View 树中找原生
 * 「同意」文本按钮（其他布局兜底），否则找到 WebView 并注入 JS 定位/点击文本为「同意」
 * 的元素；两条路径都与手动点击走相同的点击管线，不依赖固定坐标或按钮内部实现。
 */
object AgreementPopup {
    private const val TAP_DURATION_MS = 80L
    private const val AGREE_TEXT = "同意"

    private const val JS_CLICK_AGREE = """
(function(){
  var els = document.querySelectorAll('*');
  var best = null;
  for (var i = 0; i < els.length; i++) {
    var e = els[i];
    if (e.offsetParent === null) continue;
    if ((e.textContent || '').trim() !== '同意') continue;
    if (!best || best.contains(e)) best = e;
  }
  if (!best) return 'notfound';
  var c = best;
  for (var j = 0; j < 6 && c; j++) {
    if (c.onclick || c.tagName === 'A' || c.tagName === 'BUTTON' ||
        c.getAttribute('role') === 'button') {
      c.click();
      return 'clicked:' + c.tagName;
    }
    c = c.parentElement;
  }
  best.click();
  return 'clicked-self:' + best.tagName;
})()
"""

    /** 关闭同意页；未找到可点击目标或窗口已销毁时返回 false。点击异步生效，调用方随后
     *  轮询 /api/ui/screen 直到 main_menu。 */
    fun dismiss(activity: Activity): Boolean {
        val decor = activity.window?.decorView ?: return false

        findAgreeTextView(decor)?.let { return dispatchTap(activity, decor, it) }

        val webView = findWebView(decor) ?: return false
        activity.runOnUiThread {
            try {
                webView.evaluateJavascript(JS_CLICK_AGREE) { result ->
                    LogFile.info(LogDomain.PLATFORM, "agreement js click: $result")
                }
            } catch (t: Throwable) {
                LogFile.error(LogDomain.PLATFORM, "agreement js click failed", t)
            }
        }
        return true
    }

    private fun dispatchTap(activity: Activity, decor: View, target: View): Boolean {
        val btnLoc = IntArray(2).also { target.getLocationInWindow(it) }
        val rootLoc = IntArray(2).also { decor.getLocationInWindow(it) }
        val x = (btnLoc[0] - rootLoc[0]) + target.width / 2f
        val y = (btnLoc[1] - rootLoc[1]) + target.height / 2f

        val downTime = SystemClock.uptimeMillis()
        val down = MotionEvent.obtain(downTime, downTime, MotionEvent.ACTION_DOWN, x, y, 0)
        val up = MotionEvent.obtain(downTime, downTime + TAP_DURATION_MS, MotionEvent.ACTION_UP, x, y, 0)
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

    private fun findAgreeTextView(root: View): TextView? {
        var found: TextView? = null
        walk(root) { v ->
            if (found == null && v is TextView && v.isShown && v.text?.contains(AGREE_TEXT) == true) {
                found = v
            }
        }
        return found
    }

    private fun findWebView(root: View): WebView? {
        var found: WebView? = null
        walk(root) { v ->
            if (found == null && v is WebView && v.isShown) found = v
        }
        return found
    }

    private inline fun walk(root: View, visit: (View) -> Unit) {
        val queue = ArrayDeque<View>()
        queue.add(root)
        while (queue.isNotEmpty()) {
            val v = queue.removeFirst()
            if (v.visibility != View.VISIBLE) continue
            visit(v)
            if (v is ViewGroup) {
                for (i in 0 until v.childCount) queue.add(v.getChildAt(i))
            }
        }
    }
}
