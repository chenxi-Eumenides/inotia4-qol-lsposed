package com.inotia4.qol.storage

import android.content.Context
import android.os.Handler
import android.os.Looper
import android.widget.Toast
import com.inotia4.qol.LogDomain
import com.inotia4.qol.LogFile
import com.inotia4.qol.QolPaths
import com.inotia4.qol.StaticData
import java.io.File
import java.util.concurrent.Executors

/**
 * 公共 Download 导出桥（native public_export contract 的 Kotlin 侧）。
 *
 * native 经 JNI 静态方法把任务交给本桥；方法必须立即返回（文件复制在
 * qol-public-export 单线程 executor 内执行），结果经 toast 与 LogFile 反馈。
 */
object PublicExportBridge {

    private const val LOG_DIR_RELATIVE = "Download/qol/"
    private const val BACKUP_DIR_RELATIVE = "Download/qol/backups/"

    // 单线程串行执行：导出是 IO 任务且带 toast 反馈，串行化避免结果提示互相覆盖。
    private val executor = Executors.newSingleThreadExecutor { runnable ->
        Thread(runnable, "qol-public-export").apply { isDaemon = true }
    }

    private val mainHandler = Handler(Looper.getMainLooper())

    @JvmStatic
    fun exportCrashLogs() {
        submit("export_crash_logs") { context ->
            val root = QolPaths.root(context)
            if (root == null) {
                LogFile.error(LogDomain.PLATFORM, "public export logs failed: qol root unavailable")
                toast(context, "导出失败：模块目录不可用")
                return@submit
            }
            val sources = listOf("qol-crash.log.prev", "java-crash.log.prev")
                .map { File(root, it) }
                .filter { it.isFile }
            if (sources.isEmpty()) {
                LogFile.info(LogDomain.PLATFORM, "public export logs skipped: no .prev crash logs under ${root.path}")
                toast(context, "没有可导出的日志（.prev 需日志轮转后生成）")
                return@submit
            }
            var exported = 0
            val failures = ArrayList<String>()
            for (file in sources) {
                val result = PublicDownloads.copy(
                    context, file, LOG_DIR_RELATIVE, file.name, mimeFor(file.name)
                )
                if (result.isSuccess) {
                    exported++
                } else {
                    failures.add("${file.name}: ${result.exceptionOrNull()?.message ?: "未知原因"}")
                }
            }
            if (failures.isEmpty()) {
                LogFile.info(LogDomain.PLATFORM, "public export logs ok: count=$exported target=$LOG_DIR_RELATIVE")
                toast(context, "已导出 $exported 个文件到 Download/qol/")
            } else {
                LogFile.error(
                    LogDomain.PLATFORM,
                    "public export logs partial: exported=$exported failed=${failures.joinToString("; ")}"
                )
                toast(context, "已导出 $exported 个文件，失败：${failures.joinToString("; ")}")
            }
        }
    }

    @JvmStatic
    fun exportBackup(sourcePath: String, displayName: String) {
        submit("export_backup") { context ->
            val source = File(sourcePath)
            if (!source.isFile) {
                LogFile.error(LogDomain.PLATFORM, "public export backup failed: source missing path=$sourcePath")
                toast(context, "导出失败：源文件不存在（$sourcePath）")
                return@submit
            }
            val result = PublicDownloads.copy(
                context, source, BACKUP_DIR_RELATIVE, displayName, mimeFor(displayName)
            )
            if (result.isSuccess) {
                LogFile.info(LogDomain.PLATFORM, "public export backup ok: target=${BACKUP_DIR_RELATIVE}$displayName")
                toast(context, "已导出到 Download/qol/backups/$displayName")
            } else {
                val reason = result.exceptionOrNull()?.message ?: "未知原因"
                LogFile.error(
                    LogDomain.PLATFORM,
                    "public export backup failed: target=${BACKUP_DIR_RELATIVE}$displayName reason=$reason"
                )
                toast(context, "导出失败：$reason")
            }
        }
    }

    @JvmStatic
    fun pickImportFile() {
        LogFile.info(LogDomain.PLATFORM, "public export: pick requested")
        // native 可能在任意线程调用；SAF 选择器必须在主线程发起，这里统一 post。
        mainHandler.post { PickedFileIngest.pick() }
    }

    /** .log 是文本，其余按二进制处理。 */
    private fun mimeFor(name: String): String =
        if (name.endsWith(".log", ignoreCase = true)) "text/plain" else "application/octet-stream"

    /** 统一入口：立即返回，任务在 executor 内执行；模块未初始化时只写日志。 */
    private fun submit(op: String, body: (Context) -> Unit) {
        executor.execute {
            val context = StaticData.context()
            if (context == null) {
                // context 为空时没有 Toast 可挂靠的 application，只能落日志。
                LogFile.error(LogDomain.PLATFORM, "public export $op skipped: module not initialized")
                return@execute
            }
            try {
                body(context)
            } catch (t: Throwable) {
                LogFile.error(LogDomain.PLATFORM, "public export $op failed", t)
                toast(context, "导出失败：${t.message ?: t.javaClass.simpleName}")
            }
        }
    }

    private fun toast(context: Context, message: String) {
        mainHandler.post {
            try {
                Toast.makeText(context, message, Toast.LENGTH_SHORT).show()
            } catch (t: Throwable) {
                // 宿主窗口不可用时 toast 抛异常；提示失败不改变导出结果，只落日志。
                LogFile.warn(LogDomain.PLATFORM, "public export toast failed: $message")
            }
        }
    }
}
