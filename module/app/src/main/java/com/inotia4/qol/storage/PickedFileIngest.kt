package com.inotia4.qol.storage

import android.app.Activity
import android.content.ActivityNotFoundException
import android.content.Context
import android.content.Intent
import android.net.Uri
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.provider.DocumentsContract
import android.provider.OpenableColumns
import android.widget.Toast
import com.inotia4.qol.LogDomain
import com.inotia4.qol.LogFile
import com.inotia4.qol.NativeBridge
import com.inotia4.qol.QolPaths
import com.inotia4.qol.StaticData
import java.io.File
import java.util.UUID
import java.util.concurrent.Executors
import java.util.zip.CRC32

/**
 * 系统文件选择器自选备份文件（SAF）摄入。
 *
 * 流程：面板侧栏「从文件导入」→ native public_export → PublicExportBridge.pickImportFile
 * → 主线程 [pick] 拉起 ACTION_OPEN_DOCUMENT；宿主 MainActivity.onActivityResult 被 hook
 * 转发回 [onActivityResult]，URI 交单线程 executor 做流式拷贝 + 头部/尾部校验，
 * 通过后原子移入 qol/backups/。只落盘，不自动导入存档；用户仍需在面板选中并点导入。
 */
object PickedFileIngest {

    /** 0x4B51（"KQ"）：模块内唯一，不与游戏自身 startActivityForResult 冲突的取值。 */
    private const val REQUEST_CODE_PICK_BACKUP = 0x4B51

    /** bundle 布局约束（与 native save_backup::kMinBundleBytes / kBundleMagic 一致）。 */
    private const val MIN_BYTES = 32L
    private const val MAX_BYTES = 64L * 1024 * 1024

    private val KNOWN_SUFFIXES = listOf(".qol.bak", ".qol_save")

    // 单线程串行执行：摄入是带 toast 反馈的 IO 任务，串行化避免多个结果提示互相覆盖。
    private val executor = Executors.newSingleThreadExecutor { runnable ->
        Thread(runnable, "qol-saf-import").apply { isDaemon = true }
    }

    private val mainHandler = Handler(Looper.getMainLooper())

    /** 拉起系统文件选择器；只能在主线程调用，非主线程调用只写日志。 */
    fun pick() {
        if (Looper.myLooper() != Looper.getMainLooper()) {
            // startActivityForResult 必须在主线程发起；native 调用线程不可控，静默失败并留痕。
            LogFile.error(LogDomain.PLATFORM, "saf pick skipped: not on main thread")
            return
        }
        val activity = HostActivityTracker.current()
        if (activity == null) {
            LogFile.warn(LogDomain.PLATFORM, "saf pick skipped: no available activity")
            toast(StaticData.context(), "当前没有可用界面，稍后再试")
            return
        }
        val intent = Intent(Intent.ACTION_OPEN_DOCUMENT).apply {
            addCategory(Intent.CATEGORY_OPENABLE)
            type = "*/*"
            putExtra(Intent.EXTRA_MIME_TYPES, arrayOf("application/octet-stream", "*/*"))
            addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
        }
        // 备份文件通常就在 Download；从该目录起浏览，避免用户在根目录逐层翻找。
        // EXTRA_INITIAL_URI 自 API 26 起生效；不支持的 provider 会回退到默认位置。
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            intent.putExtra(
                DocumentsContract.EXTRA_INITIAL_URI,
                DocumentsContract.buildDocumentUri(
                    "com.android.externalstorage.documents", "primary:Download"
                ),
            )
        }
        try {
            activity.startActivityForResult(intent, REQUEST_CODE_PICK_BACKUP)
            LogFile.info(LogDomain.PLATFORM, "saf pick launched")
        } catch (t: ActivityNotFoundException) {
            LogFile.error(LogDomain.PLATFORM, "saf pick failed: no documents provider", t)
            toast(activity, "没有可用的文件选择器")
        }
    }

    /**
     * 由 HookMain 的 onActivityResult hook 转发（先 proceed 游戏自身逻辑再进这里）。
     * 主线程只做 intent/URI 处理，所有 IO（文件名查询、拷贝、校验、移入）在 executor 内。
     */
    fun onActivityResult(activity: Activity?, requestCode: Int, resultCode: Int, data: Intent?) {
        if (requestCode != REQUEST_CODE_PICK_BACKUP) return
        if (resultCode != Activity.RESULT_OK) return
        val uri = data?.data ?: return
        // activity 参数是宿主回调转发；摄入改用 application context，不挂靠可能正在销毁的 Activity。
        val context = StaticData.context()
        if (context == null) {
            LogFile.error(LogDomain.PLATFORM, "saf import skipped: module context unavailable")
            return
        }
        executor.execute { ingest(context, uri) }
    }

    /** 超出大小上限时在拷贝循环内立即中止的内部信号。 */
    private class FileTooLargeException : RuntimeException()

    private fun ingest(context: Context, uri: Uri) {
        var part: File? = null
        try {
            val targetName = normalizeName(queryDisplayName(context, uri))
            val root = QolPaths.ensureLayout(context)
            if (root == null) {
                LogFile.error(LogDomain.PLATFORM, "saf import failed: qol layout unavailable")
                toast(context, "导入失败：模块目录不可用")
                return
            }
            val backupsDir = File(root, "backups")
            val stagingDir = File(backupsDir, ".incoming")
            if (!stagingDir.isDirectory && !stagingDir.mkdirs()) {
                LogFile.error(LogDomain.PLATFORM, "saf import failed: staging mkdir ${stagingDir.path}")
                toast(context, "导入失败：无法创建临时目录")
                return
            }

            // 局部 val 供 lambda 使用：可变 var 在捕获后无法 smart cast。
            val partFile = File(stagingDir, "${UUID.randomUUID()}.part")
            part = partFile
            val head = ByteArray(6)
            val tail = ByteArray(4)
            val crc = CRC32()
            var carried = 0 // tail 内尚未喂给 CRC 的字节数（流式保留最后 4 字节）
            var size = 0L
            val buffer = ByteArray(DEFAULT_BUFFER_SIZE)

            val input = context.contentResolver.openInputStream(uri)
                ?: run {
                    // provider 崩溃时 openInputStream 返回 null 而不抛异常，必须判空。
                    LogFile.error(LogDomain.PLATFORM, "saf import failed: provider returned null stream")
                    toast(context, "导入失败：无法读取所选文件")
                    return
                }
            input.use { stream ->
                partFile.outputStream().use { output ->
                    while (true) {
                        val read = stream.read(buffer)
                        if (read < 0) break
                        if (read == 0) continue
                        size += read
                        if (size > MAX_BYTES) throw FileTooLargeException()

                        // 头部：前 6 字节（magic + 大端版本号）
                        val headOffset = (size - read).toInt()
                        if (headOffset < head.size) {
                            System.arraycopy(buffer, 0, head, headOffset, minOf(read, head.size - headOffset))
                        }

                        // CRC 只覆盖除最后 4 字节外的全部字节：新数据先喂掉不会被保留的旧尾部，
                        // 再把（旧尾部+新块）的最后 kept 字节留在 tail 作为新尾部。
                        val newTotal = carried + read
                        val kept = minOf(tail.size, newTotal)
                        val feed = newTotal - kept
                        if (feed > 0) {
                            if (feed <= carried) {
                                crc.update(tail, 0, feed)
                            } else {
                                crc.update(tail, 0, carried)
                                crc.update(buffer, 0, feed - carried)
                            }
                        }
                        if (read >= kept) {
                            System.arraycopy(buffer, read - kept, tail, 0, kept)
                        } else {
                            val oldKeep = kept - read
                            System.arraycopy(tail, carried - oldKeep, tail, 0, oldKeep)
                            System.arraycopy(buffer, 0, tail, oldKeep, read)
                        }
                        carried = kept

                        output.write(buffer, 0, read)
                    }
                }
            }

            // 校验顺序固定：大小 → magic → 版本 → 尾部 CRC。
            if (size < MIN_BYTES) {
                LogFile.error(LogDomain.PLATFORM, "saf import rejected: too small size=$size")
                toast(context, "不是有效的备份文件")
                return
            }
            val magicOk = head[0] == 0x51.toByte() && head[1] == 0x53.toByte() &&
                head[2] == 0x42.toByte() && head[3] == 0x31.toByte() // "QSB1"
            val version = ((head[4].toInt() and 0xFF) shl 8) or (head[5].toInt() and 0xFF)
            if (!magicOk || version !in 1..4) {
                LogFile.error(LogDomain.PLATFORM, "saf import rejected: bad header size=$size")
                toast(context, "不是有效的备份文件")
                return
            }
            val trailingCrc = ((tail[0].toLong() and 0xFF) shl 24) or
                ((tail[1].toLong() and 0xFF) shl 16) or
                ((tail[2].toLong() and 0xFF) shl 8) or
                (tail[3].toLong() and 0xFF)
            if (trailingCrc != crc.value) {
                LogFile.error(LogDomain.PLATFORM, "saf import rejected: crc mismatch size=$size")
                toast(context, "备份文件校验失败")
                return
            }

            val target = dedupeTarget(backupsDir, targetName)
            if (target == null) {
                LogFile.error(LogDomain.PLATFORM, "saf import failed: no free name for $targetName")
                toast(context, "导入失败：同名备份过多，请先清理列表")
                return
            }
            val moved = try {
                java.nio.file.Files.move(partFile.toPath(), target.toPath())
                true
            } catch (t: Throwable) {
                // Files.move 失败时回退 File.renameTo：staging 与目标同目录，rename 走原子替换语义。
                LogFile.warn(LogDomain.PLATFORM, "saf import Files.move fallback: ${t.message}")
                partFile.renameTo(target)
            }
            if (!moved) {
                LogFile.error(LogDomain.PLATFORM, "saf import failed: move to ${target.path}")
                toast(context, "导入失败：无法移入备份目录")
                return
            }
            // 已移走：置空防止 finally 的 staging 清理误删目标文件。
            part = null

            LogFile.info(LogDomain.PLATFORM, "saf import ok: ${target.name} size=$size")
            // 刷新面板列表必须在游戏主线程（refresh 内部操作 UI 状态）；调用方经 mainHandler post。
            mainHandler.post {
                try {
                    NativeBridge.nativeSaveBackupUiRefresh()
                } catch (t: Throwable) {
                    LogFile.error(LogDomain.PLATFORM, "saf import ui refresh failed", t)
                }
                toast(context, "已导入 ${target.name}，可在列表中选中后导入")
            }
        } catch (t: FileTooLargeException) {
            LogFile.info(LogDomain.PLATFORM, "saf import rejected: over ${MAX_BYTES} bytes")
            toast(context, "文件超过 64MB")
        } catch (t: Throwable) {
            LogFile.error(LogDomain.PLATFORM, "saf import failed", t)
            toast(context, "导入失败：${t.message ?: t.javaClass.simpleName}")
        } finally {
            // 所有失败分支统一清 staging 残留；成功移动后 part 已置空，不再误删。
            part?.delete()
        }
    }

    /** 读 DISPLAY_NAME；查询异常吞掉并回退固定名（provider 可能拒绝查询）。 */
    private fun queryDisplayName(context: Context, uri: Uri): String {
        val name = try {
            context.contentResolver.query(uri, arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null)
                ?.use { cursor ->
                    val idx = cursor.getColumnIndex(OpenableColumns.DISPLAY_NAME)
                    if (idx >= 0 && cursor.moveToFirst()) cursor.getString(idx) else null
                }
        } catch (t: Throwable) {
            LogFile.warn(LogDomain.PLATFORM, "saf import display name query failed: ${t.message}")
            null
        }
        return name?.takeIf { it.isNotBlank() } ?: "imported.qol.bak"
    }

    /** 去路径段防目录穿越；无已知后缀时补 .qol.bak（保留原名主体）。 */
    private fun normalizeName(displayName: String): String {
        val base = displayName.substringAfterLast('/').substringAfterLast('\\').trim()
        val name = base.ifEmpty { "imported.qol.bak" }
        return if (KNOWN_SUFFIXES.any { name.endsWith(it, true) }) name else "$name.qol.bak"
    }

    /** 目标名去重：原名占用时依次尝试 <base>_1.<ext> … _99；全部占用返回 null。 */
    private fun dedupeTarget(backupsDir: File, name: String): File? {
        val plain = File(backupsDir, name)
        if (!plain.exists()) return plain
        val (base, ext) = splitForDedupe(name)
        for (i in 1..99) {
            val candidate = File(backupsDir, "$base" + "_$i$ext")
            if (!candidate.exists()) return candidate
        }
        return null
    }

    /** 拆主体与后缀：已知后缀整体剥出（save.qol.bak → save / .qol.bak），否则剥最后一个点段。 */
    private fun splitForDedupe(name: String): Pair<String, String> {
        for (suffix in KNOWN_SUFFIXES) {
            if (name.length > suffix.length && name.endsWith(suffix, true)) {
                return name.substring(0, name.length - suffix.length) to suffix
            }
        }
        val dot = name.lastIndexOf('.')
        return if (dot > 0) name.substring(0, dot) to name.substring(dot) else name to ""
    }

    private fun toast(context: Context?, message: String) {
        if (context == null) return
        mainHandler.post {
            try {
                Toast.makeText(context, message, Toast.LENGTH_SHORT).show()
            } catch (t: Throwable) {
                // 宿主窗口不可用时 toast 抛异常；提示失败不影响摄入结果，只落日志。
                LogFile.warn(LogDomain.PLATFORM, "saf import toast failed: $message")
            }
        }
    }
}
