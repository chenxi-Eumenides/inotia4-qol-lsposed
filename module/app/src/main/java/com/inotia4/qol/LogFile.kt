package com.inotia4.qol

import android.os.Environment
import android.util.Log
import com.inotia4.qol.util.JsonUtil
import org.json.JSONObject
import java.io.BufferedWriter
import java.io.File
import java.io.FileInputStream
import java.io.FileWriter
import java.io.PrintWriter
import java.io.StringWriter
import java.security.MessageDigest
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

object LogFile {

    private const val TAG = "Inotia4Export"
    private const val FILE_NAME = "inotia4-export.log"
    private const val GAME_PKG =
        "com.com2us.inotia4.normal.freefull.google.global.android.common"

    private val lock = Any()

    @Volatile
    private var file: File? = null

    private var writer: BufferedWriter? = null

    fun initEarly() {
        synchronized(lock) {
            if (file != null) return
            try {
                val base = Environment.getExternalStorageDirectory()
                val dir = File(base, "Android/data/$GAME_PKG/files")
                if (dir.exists() || dir.mkdirs()) {
                    val f = File(dir, FILE_NAME)
                    // 覆盖模式：每次进程启动清空旧日志再记录
                    val w = BufferedWriter(FileWriter(f, false))
                    w.write("=== Inotia4Export log start ${timestamp()} ===\n")
                    w.flush()
                    writer = w
                    file = f
                    Log.i(TAG, "log file: ${f.absolutePath}")
                } else {
                    Log.e(TAG, "log dir mkdir failed: $dir")
                }
            } catch (t: Throwable) {
                Log.e(TAG, "LogFile initEarly failed", t)
            }
        }
    }

    fun log(msg: String) {
        Log.i(TAG, msg)
        writeLine("${timestamp()} [I] $msg")
    }

    fun logError(msg: String, t: Throwable? = null) {
        Log.e(TAG, msg, t)
        val sw = StringWriter()
        t?.printStackTrace(PrintWriter(sw))
        writeLine("${timestamp()} [E] $msg${if (t != null) "\n$sw" else ""}")
    }

    /**
     * 统一操作日志：每个 POST 操作端点一条，文件 + logcat 双写。
     * 格式：[OP] ts=... endpoint=POST /api/... params={...} result={ok:true} dur=12ms
     * 内部异常安全：日志失败绝不影响操作返回。
     */
    fun logOp(endpoint: String, params: String, result: String, durationMs: Long) {
        val line = "[OP] ts=${timestamp()} endpoint=$endpoint params={$params} result=${resultSummary(result)} dur=${durationMs}ms"
        Log.i(TAG, line)
        writeLine(line)
    }

    /**
     * 计时并记录一次操作日志，返回 block 结果。
     * block 抛异常时记录 error 后原样重抛（由上层 ControllerGuard 兜底为 503），日志本身不吞异常、不打断操作。
     */
    fun op(endpoint: String, params: String, block: () -> String): String {
        val t0 = System.nanoTime()
        return try {
            val r = block()
            logOp(endpoint, params, r, (System.nanoTime() - t0) / 1_000_000)
            r
        } catch (t: Throwable) {
            logOp(
                endpoint,
                params,
                JsonUtil.err("exception:${t.javaClass.simpleName}", 500),
                (System.nanoTime() - t0) / 1_000_000
            )
            throw t
        }
    }

    fun path(): String? = file?.absolutePath

    fun logModuleIdentity(moduleApk: String) {
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
            log(
                "module identity: package=${BuildConfig.APPLICATION_ID} " +
                    "versionCode=${BuildConfig.VERSION_CODE} " +
                    "versionName=${BuildConfig.VERSION_NAME} " +
                    "apk=$moduleApk sha256=${digest.digest().toHex()}"
            )
        } catch (t: Throwable) {
            logError("module identity failed: apk=$moduleApk", t)
        }
    }

    private fun resultSummary(result: String): String = try {
        val obj = JSONObject(result)
        if (obj.optBoolean("ok", false)) {
            "{ok:true}"
        } else {
            "{ok:false,error=${obj.optString("error", "unknown").take(120)}}"
        }
    } catch (e: Exception) {
        // 非 JSON 结果（如 "not ready" 语义串），截断兜底
        "{ok:false,error=${result.take(120)}}"
    }

    private fun writeLine(line: String) {
        synchronized(lock) {
            try {
                writer?.let { w ->
                    w.write(line)
                    w.write("\n")
                    w.flush()
                }
            } catch (t: Throwable) {
                Log.e(TAG, "log write failed", t)
            }
        }
    }

    private fun timestamp(): String =
        SimpleDateFormat("yyyy-MM-dd HH:mm:ss.SSS", Locale.US).format(Date())

    private fun ByteArray.toHex(): String =
        joinToString(separator = "") { byte -> "%02x".format(Locale.US, byte.toInt() and 0xff) }
}
