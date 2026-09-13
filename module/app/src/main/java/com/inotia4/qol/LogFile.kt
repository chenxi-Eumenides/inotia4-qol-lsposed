package com.inotia4.qol

import android.util.Log
import com.inotia4.qol.util.JsonUtil
import org.json.JSONObject
import java.io.File
import java.io.FileInputStream
import java.security.MessageDigest
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import java.util.concurrent.ArrayBlockingQueue

/**
 * 统一日志门面（P1 重写）。
 *
 * - **native 单写者**：native 就绪后 `info/warn/error/op/debug` 一律经
 *   [NativeBridge.nativeQolLogWrite] 转发 native；native 补时间戳与帧号，并同时写
 *   logcat(tag=`Inotia4Qol`) 与文件 `/sdcard/Android/data/<pkg>/files/inotia4-export.log`。
 * - **早期 backlog**：[onNativeReady] 之前日志进入有界队列（上限 512，溢出丢最旧），
 *   同时用 logcat 兜底；[onNativeReady] 调 `nativeQolLogInit()` 后按序重放 backlog，再置就绪直写。
 *   native 永不就绪则只走 logcat。
 * - **debug 门控**：`debug()` 在构造消息前检查 [ModuleConfig.debugLogEnabled]，关闭时直接返回。
 * - **error 单行化**：message 与压缩栈合并为单行，非 debug 不产生多行。
 */
object LogFile {

    private const val LOGCAT_TAG = "Inotia4Qol"
    private const val FILE_NAME = "inotia4-export.log"
    private const val BACKLOG_LIMIT = 512
    private const val STACK_FRAME_LIMIT = 8

    private const val LEVEL_DEBUG = 0
    private const val LEVEL_INFO = 1
    private const val LEVEL_WARN = 2
    private const val LEVEL_ERROR = 3

    private val lock = Any()

    /** 统一行格式时间戳（与 native `qol_log_format_ts` 同形，本地时区，仅兜底使用）。 */
    private val tsFormat = SimpleDateFormat("yyyy-MM-dd HH:mm:ss.SSS", Locale.US)

    @Volatile
    private var nativeReady = false

    /** 有界 backlog：native 就绪前的日志；溢出时丢最旧（见 [emit]）。 */
    private val backlog = ArrayBlockingQueue<Pending>(BACKLOG_LIMIT)

    private class Pending(
        val level: Int,
        val domain: LogDomain,
        val src: String,
        val msg: String
    )

    /** 早期初始化：标记 native 未就绪；此阶段日志走 backlog + logcat 兜底。 */
    fun initEarly() {
        synchronized(lock) { nativeReady = false }
        Log.i(
            LOGCAT_TAG,
            fallbackLine(
                LEVEL_INFO, LogDomain.PLATFORM, callerSrc(),
                "log early init: native pending, file=${path()}"
            )
        )
    }

    /** NativeBridge 初始化成功后调用：初始化 native sink 并重放 backlog，之后直写 native。 */
    fun onNativeReady() {
        synchronized(lock) {
            if (nativeReady) return
            NativeBridge.nativeQolLogInit()
            NativeBridge.nativeQolLogSetDebugEnabled(ModuleConfig.debugLogEnabled)
            while (true) {
                val pending = backlog.poll() ?: break
                NativeBridge.nativeQolLogWrite(
                    pending.level, pending.domain.token, pending.src, pending.msg
                )
            }
            nativeReady = true
        }
    }

    /** debug 日志是否开启（由 [ModuleConfig.debugLogEnabled] 决定）。 */
    fun isDebugEnabled(): Boolean = ModuleConfig.debugLogEnabled

    /** 统一日志文件路径（native 实际写入路径）。 */
    fun path(): String = TargetPackages.externalFilesPath(FILE_NAME)

    /** debug 日志：门控开启时才在调用线程构造消息（惰性 lambda）。 */
    fun debug(domain: LogDomain, message: () -> String) {
        if (!ModuleConfig.debugLogEnabled) return
        emit(LEVEL_DEBUG, domain, message())
    }

    fun info(domain: LogDomain, message: String) {
        emit(LEVEL_INFO, domain, message)
    }

    fun warn(domain: LogDomain, message: String) {
        emit(LEVEL_WARN, domain, message)
    }

    /** 错误日志：message 与压缩栈合并为单行（无栈时省略 ` stack=`）。 */
    fun error(domain: LogDomain, message: String, t: Throwable? = null) {
        emit(LEVEL_ERROR, domain, if (t == null) message else "$message stack=${stackSummary(t)}")
    }

    /**
     * 计时并记录一次操作日志，返回 block 结果；异常时记录 error 后原样重抛。
     * 文案格式：`endpoint=<endpoint> params={<k=v,...>} result=<summary> dur=<ms>ms`。
     */
    fun <T> op(domain: LogDomain, endpoint: String, params: Map<String, String>, block: () -> T): T {
        val t0 = System.nanoTime()
        val paramsText = params.entries.joinToString(",") { "${it.key}=${it.value}" }
        return try {
            val result = block()
            info(
                domain,
                "endpoint=$endpoint params={$paramsText} result=${resultSummary(result)} " +
                    "dur=${(System.nanoTime() - t0) / 1_000_000}ms"
            )
            result
        } catch (t: Throwable) {
            error(
                domain,
                "endpoint=$endpoint params={$paramsText} result=" +
                    "${resultSummary(JsonUtil.err("exception:${t.javaClass.simpleName}", 500))} " +
                    "dur=${(System.nanoTime() - t0) / 1_000_000}ms",
                t
            )
            throw t
        }
    }

    /** 模块身份日志：包名/versionCode/versionName/apk 路径/SHA-256（信息逐字保留）。 */
    fun logModuleIdentity(moduleApk: File?) {
        if (moduleApk == null) {
            error(LogDomain.PLATFORM, "module identity failed: apk path unavailable")
            return
        }
        val apkPath = moduleApk.absolutePath
        try {
            val digest = MessageDigest.getInstance("SHA-256")
            FileInputStream(moduleApk).use { input ->
                val buffer = ByteArray(64 * 1024)
                while (true) {
                    val count = input.read(buffer)
                    if (count < 0) break
                    digest.update(buffer, 0, count)
                }
            }
            info(
                LogDomain.PLATFORM,
                "module identity: package=${BuildConfig.APPLICATION_ID} " +
                    "versionCode=${BuildConfig.VERSION_CODE} " +
                    "versionName=${BuildConfig.VERSION_NAME} " +
                    "apk=$apkPath sha256=${digest.digest().toHex()}"
            )
        } catch (t: Throwable) {
            error(LogDomain.PLATFORM, "module identity failed: apk=$apkPath", t)
        }
    }

    // ---- 内部发射 ----

    private fun emit(level: Int, domain: LogDomain, msg: String) {
        val src = callerSrc()
        val direct = synchronized(lock) {
            if (nativeReady) {
                true
            } else {
                if (!backlog.offer(Pending(level, domain, src, msg))) {
                    backlog.poll()
                    backlog.offer(Pending(level, domain, src, msg))
                }
                false
            }
        }
        if (direct) {
            try {
                NativeBridge.nativeQolLogWrite(level, domain.token, src, msg)
            } catch (t: Throwable) {
                // native 写入失败：退回 logcat，保持同形单行；异常栈按既有单行压缩逻辑处理。
                Log.e(
                    LOGCAT_TAG,
                    fallbackLine(
                        level, domain, src,
                        "$msg native_write_failed stack=${stackSummary(t)}"
                    )
                )
            }
        } else {
            Log.i(LOGCAT_TAG, fallbackLine(level, domain, src, msg))
        }
    }

    /**
     * logcat 兜底行：与 native 统一格式同形的单行
     * `<ts> f=-1 <L> <domain> <src> <msg>`（无帧号来源时固定 `f=-1`）。
     * 非 debug 级别套用与 native `qol_log_format` 相同的单行转义（`\n`→字面 `\n`、丢弃 `\r`）。
     */
    private fun fallbackLine(level: Int, domain: LogDomain, src: String, msg: String): String {
        val ch = when (level) {
            LEVEL_DEBUG -> 'D'
            LEVEL_WARN -> 'W'
            LEVEL_ERROR -> 'E'
            else -> 'I'
        }
        val body = if (level == LEVEL_DEBUG) {
            msg
        } else {
            msg.replace("\r", "").replace("\n", "\\n")
        }
        val ts = synchronized(tsFormat) { tsFormat.format(Date()) }
        return "$ts f=-1 $ch ${domain.token} $src $body"
    }

    /** 调用点定位：第一个 `com.inotia4.qol` 且非 LogFile 自身的栈帧 → `File.kt:line`。 */
    private fun callerSrc(): String {
        for (e in Throwable().stackTrace) {
            val className = e.className
            if (className.startsWith("com.inotia4.qol") && !className.endsWith("LogFile")) {
                return "${e.fileName ?: "-"}:${e.lineNumber}"
            }
        }
        return "-"
    }

    /** 压缩栈：`<Type>: <msg> @ SimpleClass.method(F.kt:1)<-...`（前 8 帧）。 */
    private fun stackSummary(t: Throwable): String {
        val type = "${t.javaClass.name}: ${t.message}"
        val frames = t.stackTrace
        if (frames.isEmpty()) return type
        val chain = frames.take(STACK_FRAME_LIMIT).joinToString("<-") { frame ->
            val simpleClass = frame.className.substringAfterLast('.')
            "$simpleClass.${frame.methodName}(${frame.fileName ?: "-"}:${frame.lineNumber})"
        }
        return "$type @ $chain"
    }

    private fun resultSummary(result: Any?): String {
        val text = result?.toString() ?: "null"
        return try {
            val obj = JSONObject(text)
            if (obj.optBoolean("ok", false)) {
                "{ok:true}"
            } else {
                "{ok:false,error=${obj.optString("error", "unknown").take(120)}}"
            }
        } catch (e: Exception) {
            // 非 JSON 结果（如 "not ready" 语义串），截断兜底
            "{ok:false,error=${text.take(120)}}"
        }
    }

    private fun ByteArray.toHex(): String =
        joinToString(separator = "") { byte -> "%02x".format(Locale.US, byte.toInt() and 0xff) }
}
