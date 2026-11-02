package com.inotia4.qol

import android.content.Context
import android.os.Process
import java.io.BufferedWriter
import java.io.File
import java.io.FileOutputStream
import java.io.OutputStreamWriter
import java.io.PrintWriter
import java.io.StringWriter
import java.io.Writer
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import kotlin.system.exitProcess

/**
 * Java 层未捕获异常取证：崩溃现场写入独立文件 java-crash.log（而非 qol.log），
 * 因为 qol.log 的 error 行会压缩栈，且全线程转储体量不适合进统一日志轮转。
 *
 * handler 始终把异常转发给安装时的前序 handler，保持宿主既有崩溃处理语义；
 * 前序 handler 缺失时按 ART 默认行为自行终止进程（退出码 2）。
 */
object JavaCrashReporter {

    private const val FILE_NAME = "java-crash.log"
    private const val PREV_NAME = "java-crash.log.prev"
    private const val MAX_THREADS = 128
    private const val MAX_FRAMES_PER_THREAD = 64
    private const val BYTE_LIMIT = 256 * 1024

    /** 内容写入预算为硬上限减去截断标记长度，保证标记总能落在上限内。 */
    private const val TRUNCATION_MARKER_RESERVE = 64

    @Volatile
    private var installed = false

    @Volatile
    private var rootDir: File? = null

    /** onModuleLoaded 阶段安装；幂等，重复调用不叠加 handler。 */
    @Synchronized
    fun install() {
        if (installed) return
        installed = true
        val previous = Thread.getDefaultUncaughtExceptionHandler()
        Thread.setDefaultUncaughtExceptionHandler { thread, throwable ->
            try {
                dump(thread, throwable)
            } catch (_: Throwable) {
                // 取证失败不得阻断转发：崩溃处理链优先保证既有终止行为。
            }
            if (previous != null) {
                previous.uncaughtException(thread, throwable)
            } else {
                // 无前序 handler（一般为 ART 默认链）时按惯例自行终止进程。
                Process.killProcess(Process.myPid())
                exitProcess(2)
            }
        }
    }

    /** 由 ModuleRuntimeBootstrap 在取得 context 后调用，缓存崩溃文件落盘目录。 */
    fun attachRoot(context: Context) {
        rootDir = QolPaths.root(context)
    }

    fun dump(thread: Thread, throwable: Throwable) {
        // 单行摘要先行：LogFile 内部已做栈压缩，native 未就绪时走 logcat 兜底。
        LogFile.error(LogDomain.PLATFORM, "uncaught exception on ${thread.name}", throwable)
        val root = rootDir ?: return
        val target = File(root, FILE_NAME)
        // 保留上一次崩溃现场：写入前把旧文件改名 .prev（失败忽略，本次直接覆盖）。
        try {
            if (target.exists()) target.renameTo(File(root, PREV_NAME))
        } catch (_: Throwable) {
        }
        try {
            BufferedWriter(OutputStreamWriter(FileOutputStream(target), Charsets.UTF_8)).use { writer ->
                val bounded = BoundedWriter(writer)
                bounded.writeLine(headerLine(thread))
                bounded.write(RuntimeEnvironment.identityBlock())
                bounded.writeLine("--- throwable ---")
                bounded.writeLine(stackTraceText(throwable))
                writeThreadDump(bounded)
            }
        } catch (t: Throwable) {
            LogFile.error(LogDomain.PLATFORM, "java crash file write failed: ${target.path}", t)
        }
    }

    /** 完整栈含 cause 链。走 Writer 是日志门面的强制要求：Kotlin 侧不得直接使用系统 Log。 */
    private fun stackTraceText(throwable: Throwable): String {
        val buffer = StringWriter()
        PrintWriter(buffer).use { throwable.printStackTrace(it) }
        return buffer.toString()
    }

    private fun headerLine(thread: Thread): String {
        val ts = SimpleDateFormat("yyyy-MM-dd HH:mm:ss.SSS", Locale.US).format(Date())
        return "ts=$ts pid=${Process.myPid()} proc=${RuntimeEnvironment.processName()} " +
            "thread=${thread.name} " +
            "module=${BuildConfig.APPLICATION_ID} ${BuildConfig.VERSION_NAME}(${BuildConfig.VERSION_CODE}) " +
            "config=${RuntimeEnvironment.configLine()}"
    }

    /** 全线程转储：线程数与单线程帧数封顶，任何一段写不下时追加截断标记并停止。 */
    private fun writeThreadDump(bounded: BoundedWriter) {
        bounded.writeLine("--- threads ---")
        var written = 0
        for ((t, frames) in Thread.getAllStackTraces()) {
            if (written >= MAX_THREADS) break
            val header = "=== ${t.name} id=${t.id} state=${t.state} daemon=${t.isDaemon} ==="
            if (!bounded.writeLine(header)) {
                bounded.writeTruncationMarker(written)
                return
            }
            for (frame in frames.take(MAX_FRAMES_PER_THREAD)) {
                if (!bounded.writeLine(frame.toString())) {
                    bounded.writeTruncationMarker(written)
                    return
                }
            }
            written++
        }
    }

    /** 手写字节计数的有界写入：总写入量硬上限 BYTE_LIMIT，超预算整体拒绝（无部分写入）。 */
    private class BoundedWriter(private val out: Writer) {
        var bytes = 0
            private set

        fun write(text: String): Boolean {
            val size = text.toByteArray(Charsets.UTF_8).size
            if (bytes + size > BYTE_LIMIT - TRUNCATION_MARKER_RESERVE) return false
            out.write(text)
            bytes += size
            return true
        }

        fun writeLine(text: String): Boolean = write("$text\n")

        /** 截断标记按剩余预算裁剪写入（标记为纯 ASCII，字符数即字节数）。 */
        fun writeTruncationMarker(threadsWritten: Int) {
            val remaining = BYTE_LIMIT - bytes
            if (remaining <= 0) return
            val marker = "...(truncated threads=$threadsWritten)\n"
            val clipped = if (marker.length <= remaining) marker else marker.take(remaining)
            out.write(clipped)
            bytes += clipped.length
        }
    }
}
