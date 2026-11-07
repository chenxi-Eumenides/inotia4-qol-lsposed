package com.inotia4.qol.patch

import android.app.Activity
import android.content.Context
import com.inotia4.qol.LogDomain
import com.inotia4.qol.LogFile
import io.github.libxposed.api.XposedModuleInterface
import java.lang.reflect.Field
import java.lang.reflect.Method

/**
 * 改包名工具对资源表的处理分两种：
 * - NPatch --newpackage 只改 manifest package，资源表 namespace 仍是原版包名；
 *   游戏用新包名查询资源得到 0，需要桥接回原版 namespace。
 * - 共存版重打包同时改写了资源表 namespace（资源表包名与 manifest 一致）；
 *   游戏自己的查询本来可用，不能再改写，否则同样得到 0。
 *
 * 因此桥接前先探测目标资源在宿主包名下是否可解析，只有不可解析时才回退原版 namespace。
 */
object ResourceNamespaceBridge {

    const val ORIGINAL_RESOURCE_PACKAGE =
        "com.com2us.inotia4.normal.freefull.google.global.android.common"
    private const val RESOURCE_CLASS = "com.com2us.wrapper.function.CResource"
    private var resourceClass: Class<*>? = null

    @Volatile
    private var renamedPackageName: String? = null

    /** 首次探测确认宿主资源表使用 manifest 包名后置位，后续查询不再重复探测。 */
    @Volatile
    private var resourcesUseHostPackage: Boolean = false

    /**
     * getIdentifier 探测会再次进入本 hook；用线程本地标记让探测直接走原方法，
     * 避免改写逻辑递归调用自身。
     */
    private val probing = ThreadLocal<Boolean>()

    fun install(param: XposedModuleInterface.PackageLoadedParam, hooker: (Method) -> Unit) {
        if (param.packageName == ORIGINAL_RESOURCE_PACKAGE) return
        try {
            renamedPackageName = param.packageName
            val resourceClass = param.getDefaultClassLoader().loadClass(RESOURCE_CLASS)
            this.resourceClass = resourceClass
            val method = resourceClass.getDeclaredMethod("R", String::class.java)
            hooker(method)
            val resourcesClass = param.getDefaultClassLoader().loadClass("android.content.res.Resources")
            val identifierMethod = resourcesClass.getDeclaredMethod(
                "getIdentifier",
                String::class.java,
                String::class.java,
                String::class.java
            )
            hooker(identifierMethod)
            LogFile.info(LogDomain.PLATFORM, "Resource namespace bridge installed for ${param.packageName}")
        } catch (t: Throwable) {
            LogFile.error(LogDomain.PLATFORM, "Resource namespace bridge failed", t)
        }
    }

    /**
     * Rewrite Resources.getIdentifier args for a renamed-package process.
     * Covers two game call shapes:
     * 1. CResource.R(): getIdentifier(name, type, renamedPackage)
     * 2. WrapperUserDefined.GetStringFromXml(Ex): getIdentifier("$renamedPackage:xml/$file", null, null)
     */
    fun rewriteIdentifierArgs(args: MutableList<Any?>) {
        if (probing.get() == true) return
        val renamed = renamedPackageName ?: return
        if (args.getOrNull(2) == renamed) {
            if (resourcesUseHostPackage) return
            val name = args.getOrNull(0) as? String ?: return
            val type = args.getOrNull(1) as? String
            // null 表示上下文尚未就绪、无法判断；此时保持原参数，避免把共存版资源误改写。
            if (hostResourceVisible(name, type) != false) return
            args[2] = ORIGINAL_RESOURCE_PACKAGE
            return
        }
        if (args.getOrNull(1) == null && args.getOrNull(2) == null) {
            val name = args.getOrNull(0) as? String ?: return
            if (!name.startsWith("$renamed:")) return
            if (resourcesUseHostPackage) return
            if (hostResourceVisible(name, null) != false) return
            args[0] = ORIGINAL_RESOURCE_PACKAGE + name.substring(renamed.length)
        }
    }

    fun resolve(resourceKey: String): Int {
        val parts = resourceKey.split('.', limit = 3)
        if (parts.size != 3 || parts[0] != "R") return 0
        val context = currentContext() ?: return 0
        val type = parts[1]
        val name = parts[2]
        val renamed = renamedPackageName
        if (renamed != null && (resourcesUseHostPackage || hostResourceVisible(name, type) == true)) {
            return identifier(context, name, type, renamed)
        }
        return identifier(context, name, type, ORIGINAL_RESOURCE_PACKAGE)
    }

    /** null 表示上下文尚未就绪，无法判断宿主资源表是否使用 manifest 包名。 */
    private fun hostResourceVisible(name: String, type: String?): Boolean? {
        val renamed = renamedPackageName ?: return null
        val context = currentContext() ?: return null
        if (identifier(context, name, type, renamed) == 0) return false
        resourcesUseHostPackage = true
        return true
    }

    private fun identifier(context: Context, name: String, type: String?, packageName: String?): Int {
        val wasProbing = probing.get() == true
        probing.set(true)
        return try {
            context.resources.getIdentifier(name, type, packageName)
        } finally {
            probing.set(wasProbing)
        }
    }

    private fun currentContext(): Context? {
        try {
            val field: Field = (resourceClass ?: return null).getDeclaredField("mActivity")
            field.isAccessible = true
            (field.get(null) as? Activity)?.let { return it }
        } catch (t: Throwable) {
            LogFile.error(LogDomain.PLATFORM, "Resource namespace bridge activity lookup failed", t)
        }
        return try {
            val activityThread = Class.forName("android.app.ActivityThread")
            activityThread.getMethod("currentApplication").invoke(null) as? Context
        } catch (t: Throwable) {
            LogFile.error(LogDomain.PLATFORM, "Resource namespace bridge application lookup failed", t)
            null
        }
    }
}
