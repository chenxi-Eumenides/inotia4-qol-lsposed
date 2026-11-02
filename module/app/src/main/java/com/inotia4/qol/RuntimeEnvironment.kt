package com.inotia4.qol

import android.content.Context
import android.os.Build
import android.os.Process
import org.json.JSONObject
import java.io.File
import java.security.MessageDigest
import java.util.Locale

/**
 * 运行时身份信息单一来源：单行 INFO 日志与 crash 文件 identity 段共用同一批字段，
 * 避免两处各自读取 Build 值产生漂移。
 */
object RuntimeEnvironment {

    private const val CONFIG_FP_CHARS = 12
    private const val CONFIG_LIST_LIMIT = 32

    private const val CONFIG_LINE_UNAVAILABLE = "fp=none enabled=0 list="

    @Volatile
    private var cachedIdentityBlock: String = ""

    @Volatile
    private var cachedConfigLine: String = ""

    /** Crash 文件 identity 段原文（每行 `ident.*` 前缀，行尾 \n）；initialize 之前为空串。 */
    fun identityBlock(): String = cachedIdentityBlock

    /** 单行配置摘要 `fp=... enabled=N list=a,b`；initialize 之前为空串。 */
    fun configLine(): String = cachedConfigLine

    /**
     * 构造并缓存身份信息，下发 native crash handler，并写单行 INFO 汇总。
     * 重复调用只刷新缓存并重新下发；用 @Synchronized 防并发构造。
     */
    @Synchronized
    fun initialize(context: Context, gameVersionName: String) {
        val configLine = computeConfigLine(context)
        val metrics = context.resources.displayMetrics
        val variant = try {
            NativeBridge.nativeGetVariantJson()
        } catch (t: Throwable) {
            "unavailable(${t.javaClass.simpleName})"
        }
        val device = "${Build.MANUFACTURER}/${Build.BRAND}/${Build.MODEL.replace(' ', '_')}"
        val android = "${Build.VERSION.RELEASE}(API ${Build.VERSION.SDK_INT})"
        val abis = Build.SUPPORTED_ABIS.joinToString(",")
        val bits = if (Process.is64Bit()) 64 else 32
        val screen = "${metrics.widthPixels}x${metrics.heightPixels}@${metrics.densityDpi}dpi"
        val module = "${BuildConfig.APPLICATION_ID} ${BuildConfig.VERSION_NAME}(${BuildConfig.VERSION_CODE})"
        val game = "${TargetPackages.current()} $gameVersionName"

        val identityBlock = buildString {
            append("ident.device=").append(device).append('\n')
            append("ident.fingerprint=").append(Build.FINGERPRINT).append('\n')
            append("ident.android=").append(android)
                .append(" abis=").append(abis)
                .append(" bits=").append(bits).append('\n')
            append("ident.screen=").append(screen).append('\n')
            append("ident.proc=").append(processName())
                .append(" pid=").append(Process.myPid()).append('\n')
            append("ident.module=").append(module).append('\n')
            append("ident.game=").append(game)
                .append(" variant=").append(variant).append('\n')
            // config 行由 native 捕获时从独立缓冲追加，此处不重复写，避免同键两行。
        }

        cachedIdentityBlock = identityBlock
        cachedConfigLine = configLine

        try {
            NativeBridge.nativeQolCrashSetIdentity(identityBlock)
        } catch (t: Throwable) {
            LogFile.error(LogDomain.PLATFORM, "crash identity set failed", t)
        }
        try {
            NativeBridge.nativeQolCrashSetConfig(configLine)
        } catch (t: Throwable) {
            LogFile.error(LogDomain.PLATFORM, "crash config set failed", t)
        }

        LogFile.info(
            LogDomain.PLATFORM,
            "runtime env: device=$device fingerprint=${Build.FINGERPRINT} " +
                "android=$android abis=$abis bits=$bits screen=$screen " +
                "module=$module game=$game variant=$variant",
        )
    }

    /** 进程名：/proc/self/cmdline 截断到首个 \0；读取失败或为空回退目标包名。 */
    internal fun processName(): String = try {
        val bytes = File("/proc/self/cmdline").readBytes()
        val end = bytes.indexOf(0.toByte())
        val text = String(bytes, 0, if (end >= 0) end else bytes.size, Charsets.UTF_8).trim()
        if (text.isEmpty()) TargetPackages.current() else text
    } catch (_: Throwable) {
        TargetPackages.current()
    }

    /**
     * config.json 指纹与启用插件摘要：fp 取 SHA-256 前 12 个 hex 字符；
     * enabled 为启用 section 真实总数；list 为排序后前 32 个 section key（`,` 替换为 `;`）。
     * 文件缺失或读取/解析失败退化为 [CONFIG_LINE_UNAVAILABLE]。
     */
    private fun computeConfigLine(context: Context): String {
        val file = QolPaths.root(context)?.let { File(it, "config.json") }
            ?: return CONFIG_LINE_UNAVAILABLE
        return try {
            val bytes = file.readBytes()
            val fp = MessageDigest.getInstance("SHA-256").digest(bytes)
                .joinToString(separator = "") { byte -> "%02x".format(Locale.US, byte.toInt() and 0xff) }
                .take(CONFIG_FP_CHARS)
            val json = JSONObject(String(bytes, Charsets.UTF_8))
            val enabled = ArrayList<String>()
            for (key in json.keys()) {
                val section = json.optJSONObject(key) ?: continue
                if (!section.optBoolean("enable", true)) continue
                enabled.add(key)
            }
            enabled.sort()
            val list = enabled.take(CONFIG_LIST_LIMIT)
                .joinToString(",") { id -> id.replace(',', ';') }
            "fp=$fp enabled=${enabled.size} list=$list"
        } catch (_: Throwable) {
            CONFIG_LINE_UNAVAILABLE
        }
    }
}
