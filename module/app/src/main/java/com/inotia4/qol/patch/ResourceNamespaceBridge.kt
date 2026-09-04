package com.inotia4.qol.patch

import android.app.Activity
import android.content.Context
import com.inotia4.qol.LogFile
import io.github.libxposed.api.XposedModuleInterface
import java.lang.reflect.Field
import java.lang.reflect.Method

/**
 * NPatch --newpackage changes the manifest package but keeps the original resource-table
 * namespace. The game's CResource.R() queries resources with the manifest package and gets 0.
 */
object ResourceNamespaceBridge {

    const val ORIGINAL_RESOURCE_PACKAGE =
        "com.com2us.inotia4.normal.freefull.google.global.android.common"
    private const val RESOURCE_CLASS = "com.com2us.wrapper.function.CResource"
    private var resourceClass: Class<*>? = null
    private var renamedPackageName: String? = null

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
            LogFile.log("Resource namespace bridge installed for ${param.packageName}")
        } catch (t: Throwable) {
            LogFile.logError("Resource namespace bridge failed", t)
        }
    }

    /**
     * Rewrite Resources.getIdentifier args for the renamed-package process.
     * Covers two game call shapes:
     * 1. CResource.R(): getIdentifier(name, type, renamedPackage)
     * 2. WrapperUserDefined.GetStringFromXml(Ex): getIdentifier("$renamedPackage:xml/$file", null, null)
     */
    fun rewriteIdentifierArgs(args: MutableList<Any?>) {
        val renamed = renamedPackageName ?: return
        if (args.getOrNull(2) == renamed) {
            args[2] = ORIGINAL_RESOURCE_PACKAGE
            return
        }
        if (args.getOrNull(1) == null && args.getOrNull(2) == null) {
            val name = args.getOrNull(0) as? String ?: return
            if (name.startsWith("$renamed:")) {
                args[0] = ORIGINAL_RESOURCE_PACKAGE + name.substring(renamed.length)
            }
        }
    }

    fun resolve(resourceKey: String): Int {
        val parts = resourceKey.split('.', limit = 3)
        if (parts.size != 3 || parts[0] != "R") return 0
        val context = currentContext() ?: return 0
        return context.resources.getIdentifier(parts[2], parts[1], ORIGINAL_RESOURCE_PACKAGE)
    }

    private fun currentContext(): Context? {
        try {
            val field: Field = (resourceClass ?: return null).getDeclaredField("mActivity")
            field.isAccessible = true
            (field.get(null) as? Activity)?.let { return it }
        } catch (t: Throwable) {
            LogFile.logError("Resource namespace bridge activity lookup failed", t)
        }
        return try {
            val activityThread = Class.forName("android.app.ActivityThread")
            activityThread.getMethod("currentApplication").invoke(null) as? Context
        } catch (t: Throwable) {
            LogFile.logError("Resource namespace bridge application lookup failed", t)
            null
        }
    }
}
