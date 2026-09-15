package com.inotia4.qol

import android.app.Activity
import android.os.Handler
import android.os.Looper
import java.lang.reflect.Field
import java.lang.reflect.Method

/**
 * 关闭 Java 层同意页（AgreementUIActivity），且不结束游戏。
 *
 * SDK 把「关页」与「退出游戏」解耦在 `AgreementUIActivity.destroyParentActivity`（默认 true）上：
 * 该字段为 true 时同意页 `onDestroy` 会 `finish()` 掉游戏主 Activity。系统返回键路径从不把它置
 * false，所以按返回必然带走游戏本体（真机实测：连按两次返回 → 同意页正常关闭 → 游戏退回桌面，
 * 并在退出收尾时触发游戏侧 `CWrapperData_nativeFinalize` 的 Scudo abort）。
 *
 * 这里复刻 SDK 自身的「pass Agreement UI」分支（本地协议数据缺失且版本属性已存在时的自动放行，
 * 见 AgreementUIActivity.java:344-345）：反射置 `destroyParentActivity = false`，再调
 * `UserAgreeAnimation.closeAgreementUI(1000)` → 播关闭动画 → `UserAgreeManager.onUserAgreeResult(1000)`
 * → `ActiveUser.executeModules()` → 同意页 finish，游戏继续停在主菜单。不注入触摸、不依赖 DOM 与 payload。
 *
 * `closeAgreementUI` 自带 `!isOpened || isAnimation` 守卫，同意页开场动画期间调用会被静默吞掉，
 * 而调用方是在页面刚可见时立刻发请求，因此这里在主线程上自己等就绪（最多约 3s）后再关，
 * 不把时序问题丢给调用方。
 *
 * 已知代价：该分支不写 `AGREEMENT_VERSION_PROPERTY`（那是 H5 回调
 * `c2s://activeuser?agreement=…` 分支写的），所以下次冷启动同意页仍会照常弹出，由本接口再关一次。
 */
object AgreementPopup {
    private const val FIELD_DESTROY_PARENT = "destroyParentActivity"
    private const val FIELD_ANIMATION = "userAgreeAnimation"
    private const val FIELD_OPENED = "isOpened"
    private const val FIELD_ANIMATING = "isAnimation"
    private const val METHOD_CLOSE = "closeAgreementUI"

    /** `closeAgreementUI` 参数：-1 = 拒绝并退出、0 = 跳过放行、1000 = SDK 自身的「已放行」值。 */
    private const val RESULT_PASSED = 1000

    private const val READY_RETRY_INTERVAL_MS = 150L
    private const val READY_RETRY_MAX = 20

    /** 关闭同意页且保留游戏主 Activity；同意页结构不可用时返回 false。关闭异步生效，
     *  调用方随后轮询 `/api/ui/screen` 直到 `main_menu`。 */
    fun dismiss(activity: Activity): Boolean {
        if (activity.isFinishing || activity.isDestroyed) return false
        val animation = userAgreeAnimation(activity) ?: return false
        val closeMethod = closeMethod(animation) ?: return false
        val keepParent = parentField(activity) ?: return false

        val handler = Handler(Looper.getMainLooper())
        handler.post(object : Runnable {
            private var attempts = 0

            override fun run() {
                if (activity.isFinishing || activity.isDestroyed) return
                if (!animationReady(animation)) {
                    if (attempts < READY_RETRY_MAX) {
                        attempts++
                        handler.postDelayed(this, READY_RETRY_INTERVAL_MS)
                    } else {
                        LogFile.warn(LogDomain.PLATFORM, "agreement never finished opening, close skipped")
                    }
                    return
                }
                try {
                    keepParent.setBoolean(activity, false)
                    closeMethod.invoke(animation, RESULT_PASSED)
                    LogFile.info(LogDomain.PLATFORM, "agreement close dispatched, parent kept")
                } catch (t: Throwable) {
                    LogFile.error(LogDomain.PLATFORM, "agreement close dispatch failed", t)
                }
            }
        })
        return true
    }

    private fun userAgreeAnimation(activity: Activity): Any? {
        return try {
            val field = activity.javaClass.getDeclaredField(FIELD_ANIMATION)
            field.isAccessible = true
            field.get(activity)
        } catch (t: Throwable) {
            LogFile.error(LogDomain.PLATFORM, "agreement animation lookup failed", t)
            null
        }
    }

    private fun closeMethod(animation: Any): Method? {
        return try {
            animation.javaClass.getMethod(METHOD_CLOSE, Int::class.javaPrimitiveType)
        } catch (t: Throwable) {
            LogFile.error(LogDomain.PLATFORM, "agreement close method lookup failed", t)
            null
        }
    }

    private fun parentField(activity: Activity): Field? {
        return try {
            activity.javaClass.getDeclaredField(FIELD_DESTROY_PARENT).apply { isAccessible = true }
        } catch (t: Throwable) {
            LogFile.error(LogDomain.PLATFORM, "agreement parent-flag field lookup failed", t)
            null
        }
    }

    /** `closeAgreementUI` 的守卫条件：未开场完成或动画进行中调它都是空操作。 */
    private fun animationReady(animation: Any): Boolean {
        return try {
            val cls = animation.javaClass
            val opened = cls.getDeclaredField(FIELD_OPENED).apply { isAccessible = true }.getBoolean(animation)
            val animating = cls.getDeclaredField(FIELD_ANIMATING).apply { isAccessible = true }.getBoolean(animation)
            opened && !animating
        } catch (t: Throwable) {
            LogFile.error(LogDomain.PLATFORM, "agreement animation state read failed", t)
            false
        }
    }
}
