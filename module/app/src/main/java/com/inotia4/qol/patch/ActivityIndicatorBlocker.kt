package com.inotia4.qol.patch

import com.inotia4.qol.LogDomain
import com.inotia4.qol.LogFile
import io.github.libxposed.api.XposedModuleInterface
import java.lang.reflect.Method

/**
 * 禁用 native 的 "Connecting.." 模态进度框。
 *
 * libgame.so 在联网等待点经 JNI 反射调用
 * `com.com2us.wrapper.WrapperUserDefined.ActivityIndicatorOpen()`（静态、无参、无返回值）；
 * 它只做一件事：给 MainActivity 挂一个 `setCancelable(false)` 的 ProgressDialog
 * （WrapperUserDefined.java:166-183），无业务语义，连接逻辑在 CWrapperHttp/native。
 *
 * 该框的消失完全依赖 native 再调 `ActivityIndicatorClose()`（:185-198）：Java 侧既无超时也无
 * Activity 生命周期兜底，`setCancelable(false)` 又让返回键无效。因此在它显示期间切场景/进存档时
 * native 不再回调关闭，弹窗会永久抢占输入并盖住 HUD。
 *
 * 处理：让 `ActivityIndicatorOpen()` 空实现——不弹，不改动任何网络与存档流程。
 * `ActivityIndicatorClose()` 保持原样（它仍负责清理静态 progressDialog 引用）。
 */
object ActivityIndicatorBlocker {
    private const val TARGET_CLASS = "com.com2us.wrapper.WrapperUserDefined"
    private const val TARGET_METHOD = "ActivityIndicatorOpen"

    fun install(param: XposedModuleInterface.PackageLoadedParam, hooker: (Method) -> Unit) {
        try {
            val cl = param.getDefaultClassLoader()
            val method = cl.loadClass(TARGET_CLASS).declaredMethods
                .firstOrNull { it.name == TARGET_METHOD && it.parameterTypes.isEmpty() }
                ?: throw NoSuchMethodException("$TARGET_CLASS.$TARGET_METHOD()")
            hooker(method)
        } catch (t: Throwable) {
            LogFile.error(LogDomain.PLATFORM, "ActivityIndicatorBlocker install failed", t)
        }
    }
}
