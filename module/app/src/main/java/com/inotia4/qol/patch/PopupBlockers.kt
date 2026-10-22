package com.inotia4.qol.patch

import android.app.Activity
import android.content.Intent
import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import android.view.animation.Animation
import com.inotia4.qol.LogDomain
import com.inotia4.qol.LogFile
import com.inotia4.qol.NativeBridge
import com.inotia4.qol.util.JsonUtil
import io.github.libxposed.api.XposedInterface
import io.github.libxposed.api.XposedModuleInterface
import java.lang.reflect.Field
import java.lang.reflect.Method
import java.util.WeakHashMap

/** 单条 hook 的安装方式：HookMain 注入 `hook(method).setExceptionMode(mode).intercept(handler)`。 */
typealias PopupHookInstaller = (
    method: Method,
    mode: XposedInterface.ExceptionMode,
    handler: (XposedInterface.Chain) -> Any?,
) -> Unit

/**
 * 弹窗压制收束入口：Java 层全部弹窗压制 hook 集中在本文件。
 * 原生引擎侧唯一一处弹窗压制是 core 常驻指令 patch `P_HUB_LOGIN_SKIP`
 * （game/access/patches.cpp：主菜单 Hub 登录询问确认框，分支改写令调用不再执行），
 * 按 ARCHITECTURE §4.3 分层不并入本文件；完整清单见 docs/reference/game/ui.md §2.2。
 *
 * - [NoticePopupBlocker]：「更新通知」弹窗（ActiveUserUpdateDialog，文案服务器下发）
 * - [IapBlocker]：Hive 内购选择弹窗（SelectTarget.iapSelectTarget）
 * - [AgreementGate]：同意页 AgreementUIActivity 启动拦截（仅主菜单放行）
 * - [AgreementPopup]：同意页放行后的自动关闭（反射 closeAgreementUI(1000)，只关页不退出游戏）
 * - [ActivityIndicatorBlocker]：「Connecting..」模态进度框
 */
object PopupBlockers {
    fun install(param: XposedModuleInterface.PackageLoadedParam, hook: PopupHookInstaller) {
        NoticePopupBlocker.install(param, hook)
        IapBlocker.install(param, hook)
        AgreementGate.install(param, hook)
        AgreementPopup.installHooks(param, hook)
        ActivityIndicatorBlocker.install(param, hook)
    }
}

/**
 * 「更新通知」弹窗（用户所见：标题「更新通知」、按钮「确认」=finish 退出 /「移动到应用商店」=VIEW url）。
 *
 * 弹窗本体是 `com.com2us.module.activeuser.ActiveUserUpdateDialog`：标题/正文/按钮文案
 * （title/message/btn_N/numButtons/url_N）全部来自网关 `get_notice` 响应字段（notice_show /
 * notice_title / notice_message / notice_button / notice_action / notice_url），APK 内不存在任何
 * 一份静态文案。触发链（唯一活路径）：`ActiveUser.start()` → `startConfigurationThread()`（get_agreement
 * → 同意页处置）→ `get_did` → `runGetDIDCallback`（isShowNoticePopup && errno==0）→ `showNoticePopup()`
 * → `processNetworkTask("get_notice")` → `notice_show==1` → `ActiveUserUpdateDialog.show()`。
 *
 * native 侧 `CS_UpdateNoticeShowPopup/CS_UpdateNoticeGetPopupInfo → jniShowNoticePopup /
 * jniGetNoticePopupInfo` 汇入同两个 Java 方法，但该导出在本包 libgame.so 中无调用者（死代码）；
 * `getNoticePopupInfo` 的 hook 为防御性收口。`WrapperUserDefined.GetNoticeHttp` 是老 wrapper 的
 * 另一条公告 HTTP 通道（native `jRequestNotice` 经 GetStaticMethodID 调用），与本弹窗无关。
 *
 * 处理：替换 `showNoticePopup` 为直接以 `POPUP_STATE.UNSHOWN` 回调。不能吞掉不回调——回调承载
 * 启动完成通知（activeUserNotifier / jniActiveUserNotifier），只有 `POPUP_STATE_ERROR(-1)` 才判
 * FAIL，UNSHOWN(1) 走 SUCCESS，启动序列得以继续；不回调会让游戏卡在启动。
 */
object NoticePopupBlocker {
    private const val AU_CLASS = "com.com2us.module.activeuser.ActiveUser"

    fun install(param: XposedModuleInterface.PackageLoadedParam, hook: PopupHookInstaller) {
        try {
            val cl = param.getDefaultClassLoader()
            val au = cl.loadClass(AU_CLASS)
            val popupStateClass = cl.loadClass("$AU_CLASS\$POPUP_STATE")
            val showPopupClass = cl.loadClass("$AU_CLASS\$SHOW_POPUP")
            val unshown = enumConst(popupStateClass, "UNSHOWN")
            val notShow = enumConst(showPopupClass, "NOT_SHOW")

            val showMethod = au.declaredMethods.firstOrNull {
                it.name == "showNoticePopup" && it.parameterTypes.size == 1 &&
                    it.parameterTypes[0].name == "$AU_CLASS\$NoticeShowPopupCBWithData"
            } ?: throw NoSuchMethodException("$AU_CLASS.showNoticePopup(NoticeShowPopupCBWithData)")
            val showCb = cl.loadClass("$AU_CLASS\$NoticeShowPopupCBWithData")
                .getMethod("noticeShowPopupCBWithData", popupStateClass)
            hook(showMethod, XposedInterface.ExceptionMode.PROTECTIVE) { chain ->
                LogFile.info(LogDomain.PLATFORM, "blocked ActiveUser.showNoticePopup (update notice dialog)")
                val cb = chain.getArg(0)
                if (cb != null) invokeQuietly(showCb, cb, unshown)
                null
            }

            val infoMethod = au.declaredMethods.firstOrNull {
                it.name == "getNoticePopupInfo" && it.parameterTypes.size == 1 &&
                    it.parameterTypes[0].name == "$AU_CLASS\$NoticePopupInfoCBWithData"
            } ?: throw NoSuchMethodException("$AU_CLASS.getNoticePopupInfo(NoticePopupInfoCBWithData)")
            val infoCb = cl.loadClass("$AU_CLASS\$NoticePopupInfoCBWithData")
                .getMethod("noticePopupInfoCBWithData", showPopupClass, String::class.java)
            hook(infoMethod, XposedInterface.ExceptionMode.PROTECTIVE) { chain ->
                LogFile.info(LogDomain.PLATFORM, "blocked ActiveUser.getNoticePopupInfo (update notice dialog)")
                val cb = chain.getArg(0)
                if (cb != null) invokeQuietly(infoCb, cb, notShow, null)
                null
            }
        } catch (t: Throwable) {
            LogFile.error(LogDomain.PLATFORM, "NoticePopupBlocker install failed", t)
        }
    }

    private fun enumConst(enumClass: Class<*>, constant: String): Any =
        enumClass.enumConstants.firstOrNull { (it as Enum<*>).name == constant }
            ?: throw NoSuchElementException("${enumClass.name}.$constant")

    private fun invokeQuietly(method: Method, target: Any, vararg args: Any?) {
        try {
            method.invoke(target, *args)
        } catch (t: Throwable) {
            LogFile.error(LogDomain.PLATFORM, "notice popup callback ${method.name} dispatch failed", t)
        }
    }
}

/**
 * 屏蔽 Hive 内购选择弹窗：`com.com2us.module.inapp.SelectTarget.iapSelectTarget`。
 * 拦截后立即触发 native 恢复（`nativeRecoverAfterHiveBlock`），支付语义完全不进入。
 */
object IapBlocker {
    private const val TARGET_CLASS = "com.com2us.module.inapp.SelectTarget"

    fun install(param: XposedModuleInterface.PackageLoadedParam, hook: PopupHookInstaller) {
        try {
            val cl = param.getDefaultClassLoader()
            val method = cl.loadClass(TARGET_CLASS).getDeclaredMethod(
                "iapSelectTarget",
                Class.forName("android.app.Activity", false, cl),
                Class.forName("com.com2us.module.view.SurfaceViewWrapper", false, cl),
                Class.forName("com.com2us.module.inapp.SelectTargetCallback", false, cl),
                Long::class.javaPrimitiveType,
            )
            hook(method, XposedInterface.ExceptionMode.PROTECTIVE) { _ ->
                LogFile.info(LogDomain.PLATFORM, "blocked Hive SelectTarget.iapSelectTarget (payment dialog)")
                recover()
                null
            }
        } catch (t: Throwable) {
            LogFile.error(LogDomain.PLATFORM, "IapBlocker install failed", t)
        }
    }

    private fun recover() {
        if (!NativeBridge.ready) return
        try {
            LogFile.info(LogDomain.PLATFORM, "hive recovery: ${NativeBridge.nativeRecoverAfterHiveBlock()}")
        } catch (t: Throwable) {
            LogFile.error(LogDomain.PLATFORM, "hive recovery failed", t)
        }
    }
}

/**
 * 仅允许同意页（AgreementUIActivity）在主菜单弹出。hook Instrumentation.execStartActivity
 * 丢弃其余场景的启动：进档宽限期内（enter_slot/create_slot 后 15s）或 native screen 非
 * main_menu。否则同意页 Activity 启动会打断读档流程导致角色数据丢失（save1 实测复现）。
 * 首次启动时放行同意页，避免启动阶段的 loading/connecting 被误判为读档；
 * 探针失败时放行（API 层门禁仍 fail-closed 兜底）。
 */
object AgreementGate {
    const val AGREEMENT_ACTIVITY = "com.com2us.module.activeuser.useragree.AgreementUIActivity"
    private const val WORLD_LOAD_GRACE_MS = 15_000L

    @Volatile
    private var worldLoadUntil = 0L

    @Volatile
    private var startupAgreementPending = true

    fun beginWorldLoad() {
        worldLoadUntil = SystemClock.uptimeMillis() + WORLD_LOAD_GRACE_MS
    }

    fun shouldBlock(): Boolean {
        if (SystemClock.uptimeMillis() < worldLoadUntil) return true
        if (startupAgreementPending) {
            startupAgreementPending = false
            return false
        }
        if (!NativeBridge.ready) return false
        val screen = try {
            JsonUtil.parseObj(NativeBridge.nativeGetGamestateJson())?.optString("screen", "")
        } catch (t: Throwable) {
            LogFile.error(LogDomain.PLATFORM, "agreement gate screen probe failed", t)
            null
        }
        return screen != null && screen != "main_menu"
    }

    fun install(param: XposedModuleInterface.PackageLoadedParam, hook: PopupHookInstaller) {
        try {
            val cl = param.getDefaultClassLoader()
            val intentClass = Class.forName("android.content.Intent", false, cl)
            Class.forName("android.app.Instrumentation", false, cl).declaredMethods
                .filter { it.name == "execStartActivity" }
                .mapNotNull { m ->
                    val idx = m.parameterTypes.indexOf(intentClass)
                    if (idx >= 0) m to idx else null
                }
                .forEach { (m, idx) ->
                    hook(m, XposedInterface.ExceptionMode.PROTECTIVE) { chain ->
                        val intent = chain.getArg(idx) as? Intent
                        if (intent?.component?.className == AGREEMENT_ACTIVITY && shouldBlock()) {
                            LogFile.info(
                                LogDomain.PLATFORM,
                                "blocked AgreementUIActivity launch (outside main menu or world load in progress)",
                            )
                            null
                        } else {
                            chain.proceed()
                        }
                    }
                }
        } catch (t: Throwable) {
            LogFile.error(LogDomain.PLATFORM, "AgreementGate install failed", t)
        }
    }
}

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

    fun installHooks(param: XposedModuleInterface.PackageLoadedParam, hook: PopupHookInstaller) {
        try {
            val cl = param.getDefaultClassLoader()
            val listenerClass = cl.loadClass(OPENING_LISTENER)
            val animationEnd = listenerClass.getDeclaredMethod(METHOD_ANIMATION_END, Animation::class.java)
            hook(animationEnd, XposedInterface.ExceptionMode.PROTECTIVE) { chain ->
                val result = chain.proceed()
                onOpeningAnimationEnded(chain.thisObject)
                result
            }
            openingAnimationHookInstalled = true
            LogFile.info(LogDomain.PLATFORM, "agreement opening-animation readiness hook installed")
        } catch (t: Throwable) {
            openingAnimationHookInstalled = false
            LogFile.error(LogDomain.PLATFORM, "agreement opening-animation readiness hook failed", t)
        }
        try {
            val cl = param.getDefaultClassLoader()
            val activityClass = cl.loadClass(AGREEMENT_ACTIVITY)
            val destroy = activityClass.getDeclaredMethod(METHOD_ACTIVITY_DESTROY)
            hook(destroy, XposedInterface.ExceptionMode.PROTECTIVE) { chain ->
                val activity = chain.thisObject as? Activity
                try {
                    chain.proceed()
                } finally {
                    activity?.let { onActivityDestroyed(it) }
                }
            }
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

/**
 * 禁用 native 的 "Connecting.." 模态进度框。
 *
 * libgame.so 在联网等待点经 JNI 反射调用
 * `com.com2us.wrapper.WrapperUserDefined.ActivityIndicatorOpen()`（静态、无参、无返回值）；
 * 它只做一件事：给 MainActivity 挂一个 `setCancelable(false)` 的 ProgressDialog
 * （WrapperUserDefined.java:166-198），无业务语义，连接逻辑在 CWrapperHttp/native。
 *
 * 该框的消失完全依赖 native 再调 `ActivityIndicatorClose()`：Java 侧既无超时也无
 * Activity 生命周期兜底，`setCancelable(false)` 又让返回键无效。因此在它显示期间切场景/进存档时
 * native 不再回调关闭，弹窗会永久抢占输入并盖住 HUD（机制记载见 docs/reference/game/ui.md §2.2）。
 *
 * 处理：让 `ActivityIndicatorOpen()` 空实现——不弹，不改动任何网络与存档流程。
 * `ActivityIndicatorClose()` 保持原样（它仍负责清理静态 progressDialog 引用）。
 */
object ActivityIndicatorBlocker {
    private const val TARGET_CLASS = "com.com2us.wrapper.WrapperUserDefined"
    private const val TARGET_METHOD = "ActivityIndicatorOpen"

    fun install(param: XposedModuleInterface.PackageLoadedParam, hook: PopupHookInstaller) {
        try {
            val cl = param.getDefaultClassLoader()
            val method = cl.loadClass(TARGET_CLASS).declaredMethods
                .firstOrNull { it.name == TARGET_METHOD && it.parameterTypes.isEmpty() }
                ?: throw NoSuchMethodException("$TARGET_CLASS.$TARGET_METHOD()")
            hook(method, XposedInterface.ExceptionMode.PROTECTIVE) { _ ->
                LogFile.info(LogDomain.PLATFORM, "blocked ActivityIndicatorOpen (Connecting.. progress dialog)")
                null
            }
        } catch (t: Throwable) {
            LogFile.error(LogDomain.PLATFORM, "ActivityIndicatorBlocker install failed", t)
        }
    }
}
