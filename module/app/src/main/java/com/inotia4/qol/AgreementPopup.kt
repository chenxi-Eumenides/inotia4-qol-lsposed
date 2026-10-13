package com.inotia4.qol

import android.app.Activity
import android.os.Handler
import android.os.Looper
import android.view.animation.Animation
import java.lang.reflect.Field
import java.lang.reflect.Method
import java.util.WeakHashMap

/** Closes the agreement activity after its opening animation without finishing the game Activity. */
object AgreementPopup {
    private const val AGREEMENT_ACTIVITY = "com.com2us.module.activeuser.useragree.AgreementUIActivity"
    private const val OPENING_LISTENER = "com.com2us.module.activeuser.useragree.UserAgreeAnimation\$2"
    private const val FIELD_DESTROY_PARENT = "destroyParentActivity"
    private const val FIELD_ANIMATION = "userAgreeAnimation"
    private const val FIELD_OPENED = "isOpened"
    private const val FIELD_ANIMATING = "isAnimation"
    private const val FIELD_ANIMATION_OWNER = "this\$0"
    private const val FIELD_ACTIVITY_OWNER = "thiz"
    private const val METHOD_CLOSE = "closeAgreementUI"
    private const val METHOD_ANIMATION_END = "onAnimationEnd"
    private const val METHOD_ACTIVITY_DESTROY = "onDestroy"

    /** `closeAgreementUI` parameter: 1000 = SDK's accepted-agreement result. */
    private const val RESULT_PASSED = 1000

    private val mainHandler = Handler(Looper.getMainLooper())
    private val pendingCloseActivities = WeakHashMap<Activity, Unit>()
    @Volatile private var openingAnimationHookInstalled = false
    @Volatile private var activityDestroyHookInstalled = false

    private data class CloseTarget(
        val animation: Any,
        val closeMethod: Method,
        val keepParentField: Field,
    )

    fun installOpeningAnimationHook(classLoader: ClassLoader, hooker: (Method) -> Unit) {
        try {
            val listenerClass = classLoader.loadClass(OPENING_LISTENER)
            val method = listenerClass.getDeclaredMethod(METHOD_ANIMATION_END, Animation::class.java)
            hooker(method)
            openingAnimationHookInstalled = true
            LogFile.info(LogDomain.PLATFORM, "agreement opening-animation readiness hook installed")
        } catch (t: Throwable) {
            openingAnimationHookInstalled = false
            LogFile.error(LogDomain.PLATFORM, "agreement opening-animation readiness hook failed", t)
        }
    }

    fun installActivityDestroyHook(classLoader: ClassLoader, hooker: (Method) -> Unit) {
        try {
            val activityClass = classLoader.loadClass(AGREEMENT_ACTIVITY)
            val method = activityClass.getDeclaredMethod(METHOD_ACTIVITY_DESTROY)
            hooker(method)
            activityDestroyHookInstalled = true
            LogFile.info(LogDomain.PLATFORM, "agreement Activity teardown hook installed")
        } catch (t: Throwable) {
            activityDestroyHookInstalled = false
            LogFile.error(LogDomain.PLATFORM, "agreement Activity teardown hook failed", t)
        }
    }

    /** Called after the decompiled opening listener's `$2.onAnimationEnd` has proceeded. */
    fun onOpeningAnimationEnded(listener: Any) {
        val animation = readOwner(listener, FIELD_ANIMATION_OWNER, "agreement opening listener owner") ?: return
        val activity = readOwner(animation, FIELD_ACTIVITY_OWNER, "agreement animation Activity owner") as? Activity
            ?: return
        val shouldClose = synchronized(pendingCloseActivities) {
            pendingCloseActivities.remove(activity) != null
        }
        if (shouldClose) close(activity)
    }

    fun onActivityDestroyed(activity: Activity) {
        synchronized(pendingCloseActivities) {
            pendingCloseActivities.remove(activity)
        }
    }

    /** Returns true once a close is dispatched or accepted for the opening-animation event. */
    fun dismiss(activity: Activity): Boolean {
        if (activity.isFinishing || activity.isDestroyed) return false
        val target = closeTarget(activity) ?: return false
        if (!animationReady(target.animation) && !canWaitForOpeningEvent()) {
            LogFile.warn(LogDomain.PLATFORM, "agreement opening animation is not ready and readiness hook is unavailable")
            return false
        }
        if (!mainHandler.post {
                if (activity.isFinishing || activity.isDestroyed) {
                    onActivityDestroyed(activity)
                } else {
                    dispatchOrWait(activity)
                }
            }) {
            LogFile.error(LogDomain.PLATFORM, "agreement close dispatch could not reach the main thread")
            return false
        }
        return true
    }

    private fun dispatchOrWait(activity: Activity) {
        val target = closeTarget(activity) ?: return
        if (animationReady(target.animation)) {
            close(activity, target)
            return
        }
        if (!canWaitForOpeningEvent()) {
            LogFile.warn(LogDomain.PLATFORM, "agreement opening animation is not ready; close skipped")
            return
        }
        synchronized(pendingCloseActivities) {
            pendingCloseActivities[activity] = Unit
        }
    }

    private fun canWaitForOpeningEvent(): Boolean = openingAnimationHookInstalled && activityDestroyHookInstalled

    private fun close(activity: Activity) {
        val target = closeTarget(activity) ?: return
        if (!animationReady(target.animation)) {
            LogFile.warn(LogDomain.PLATFORM, "agreement readiness event arrived before animation became idle; close skipped")
            return
        }
        close(activity, target)
    }

    private fun close(activity: Activity, target: CloseTarget) {
        if (activity.isFinishing || activity.isDestroyed) {
            onActivityDestroyed(activity)
            return
        }
        try {
            target.keepParentField.setBoolean(activity, false)
            target.closeMethod.invoke(target.animation, RESULT_PASSED)
            LogFile.info(LogDomain.PLATFORM, "agreement close dispatched, parent kept")
        } catch (t: Throwable) {
            LogFile.error(LogDomain.PLATFORM, "agreement close dispatch failed", t)
        }
    }

    private fun closeTarget(activity: Activity): CloseTarget? {
        val animation = readField(activity, FIELD_ANIMATION, "agreement animation lookup") ?: return null
        val keepParentField = try {
            activity.javaClass.getDeclaredField(FIELD_DESTROY_PARENT).apply { isAccessible = true }
        } catch (t: Throwable) {
            LogFile.error(LogDomain.PLATFORM, "agreement parent-flag field lookup failed", t)
            return null
        }
        val closeMethod = try {
            animation.javaClass.getMethod(METHOD_CLOSE, Int::class.javaPrimitiveType)
        } catch (t: Throwable) {
            LogFile.error(LogDomain.PLATFORM, "agreement close method lookup failed", t)
            return null
        }
        return CloseTarget(animation, closeMethod, keepParentField)
    }

    private fun animationReady(animation: Any): Boolean = try {
        val cls = animation.javaClass
        val opened = cls.getDeclaredField(FIELD_OPENED).apply { isAccessible = true }.getBoolean(animation)
        val animating = cls.getDeclaredField(FIELD_ANIMATING).apply { isAccessible = true }.getBoolean(animation)
        opened && !animating
    } catch (t: Throwable) {
        LogFile.error(LogDomain.PLATFORM, "agreement animation state read failed", t)
        false
    }

    private fun readOwner(owner: Any, fieldName: String, label: String): Any? = try {
        owner.javaClass.getDeclaredField(fieldName).apply { isAccessible = true }.get(owner)
    } catch (t: Throwable) {
        LogFile.error(LogDomain.PLATFORM, "$label lookup failed", t)
        null
    }

    private fun readField(owner: Any, fieldName: String, label: String): Any? = try {
        owner.javaClass.getDeclaredField(fieldName).apply { isAccessible = true }.get(owner)
    } catch (t: Throwable) {
        LogFile.error(LogDomain.PLATFORM, "$label failed", t)
        null
    }
}
