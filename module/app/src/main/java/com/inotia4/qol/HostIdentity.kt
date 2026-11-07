package com.inotia4.qol

import io.github.libxposed.api.XposedModuleInterface

/**
 * 宿主识别不依赖 applicationId：改包名工具通常只改 manifest 包名，Java 类和资源表
 * namespace 保持原样，因此用游戏框架类与 MainActivity 类是否存在判断宿主。
 */
internal object HostIdentity {

    private const val ORIGINAL_MAIN_ACTIVITY =
        "com.com2us.inotia4.normal.freefull.google.global.android.common.MainActivity"

    /** 游戏核心框架类；改 applicationId 不会重命名这些 Java 类。 */
    private val CORE_WRAPPER_CLASSES = listOf(
        "com.com2us.wrapper.function.CResource",
        "com.com2us.wrapper.WrapperUserDefined",
    )

    fun isInotia4(param: XposedModuleInterface.PackageLoadedParam): Boolean {
        if (TargetPackages.isConfigured(param.packageName)) return true
        val loader = param.getDefaultClassLoader()
        return CORE_WRAPPER_CLASSES.all { hasClass(loader, it) } &&
            resolveMainActivity(loader, param.packageName) != null
    }

    /**
     * 优先使用原版 MainActivity 类名；若改包名工具同时重写了 Java 包名，
     * 再尝试与新 applicationId 同包的 `MainActivity`。
     */
    fun resolveMainActivity(loader: ClassLoader, packageName: String): Class<*>? {
        for (name in linkedSetOf(ORIGINAL_MAIN_ACTIVITY, "$packageName.MainActivity")) {
            try {
                return Class.forName(name, false, loader)
            } catch (_: Throwable) {
                // 继续尝试下一个候选类名。
            }
        }
        return null
    }

    private fun hasClass(loader: ClassLoader, name: String): Boolean = try {
        Class.forName(name, false, loader)
        true
    } catch (_: Throwable) {
        false
    }
}
