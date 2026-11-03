package com.inotia4.qol.storage

import android.app.Activity
import java.lang.ref.WeakReference

/**
 * 宿主 MainActivity 弱引用登记：SAF 文件选择器需要 Activity 实例发起
 * startActivityForResult，但模块不能持有强引用（泄漏游戏窗口）。
 *
 * 由 MainActivityStartupHook 的 onCreate/onWindowFocusChanged seam 更新；
 * 不注册 Application.registerActivityLifecycleCallbacks，避免与现有 seam
 * 形成两套生命周期状态。
 */
object HostActivityTracker {

    private val lock = Any()

    @Volatile
    private var lastActivity: WeakReference<Activity> = WeakReference(null)

    /** 任意线程可调用；加锁替换弱引用，防止并发写覆盖交错。 */
    fun onActivityAvailable(activity: Activity) {
        synchronized(lock) { lastActivity = WeakReference(activity) }
    }

    /** 未 finishing/未 destroyed 的实例才可用；被回收或已结束时返回 null。 */
    fun current(): Activity? {
        val candidate = synchronized(lock) { lastActivity.get() } ?: return null
        if (candidate.isFinishing || candidate.isDestroyed) return null
        return candidate
    }
}
