package com.inotia4.qol.storage

import android.content.ContentResolver
import android.content.ContentUris
import android.content.ContentValues
import android.content.Context
import android.net.Uri
import android.os.Build
import android.provider.MediaStore
import com.inotia4.qol.QolPaths
import java.io.File
import java.io.FileInputStream
import java.io.FileNotFoundException
import java.io.IOException

/**
 * 把模块 qol/ 目录内的单个文件复制到公共 Download（MediaStore.Downloads）。
 *
 * 只做导出复制面：无删除/改名管理能力；写入策略是「先查后写」——同名同目录目标
 * 已存在时截断重写，不重新 insert（MediaProvider 会把重复 insert 改名为 `xxx (1).ext`）。
 */
internal object PublicDownloads {

    private const val MAX_SOURCE_BYTES = 64L * 1024 * 1024

    fun copy(
        context: Context,
        source: File,
        relativeDir: String,
        displayName: String,
        mime: String,
    ): Result<Unit> {
        // RELATIVE_PATH/IS_PENDING 写入自 API 29 起可用；更低版本没有无权限的公共
        // Download 写入通道，直接失败（调用方负责把原因提示给用户）。
        if (Build.VERSION.SDK_INT < 29) {
            return Result.failure(
                UnsupportedOperationException("公共 Download 导出需要 Android 10（API 29）及以上")
            )
        }
        if (!relativeDir.endsWith("/")) {
            return Result.failure(IllegalArgumentException("relativeDir 必须以 / 结尾"))
        }
        // 源校验先于目标目录操作：越界路径（symlink、路径拼接）必须在任何写入前拒绝。
        if (!source.isFile) {
            return Result.failure(FileNotFoundException("源文件不存在：${source.path}"))
        }
        if (source.length() > MAX_SOURCE_BYTES) {
            return Result.failure(IllegalArgumentException("文件超过 64MB，未导出：${source.name}"))
        }
        val root = QolPaths.root(context)?.canonicalPath
            ?: return Result.failure(IllegalStateException("模块目录不可用"))
        val sourcePath = source.canonicalPath
        if (sourcePath != root && !sourcePath.startsWith("$root/")) {
            return Result.failure(SecurityException("源路径越出模块目录：${source.path}"))
        }
        val resolver = context.contentResolver
        val collection = MediaStore.Downloads.getContentUri(MediaStore.VOLUME_EXTERNAL_PRIMARY)
        return try {
            val existing = queryExisting(resolver, collection, displayName, relativeDir)
            if (existing != null) {
                writeInto(resolver, existing, source, "wt")
                Result.success(Unit)
            } else {
                insertAndPublish(resolver, collection, source, displayName, mime, relativeDir)
            }
        } catch (e: FileNotFoundException) {
            Result.failure(e)
        } catch (e: SecurityException) {
            Result.failure(e)
        } catch (e: IOException) {
            Result.failure(e)
        }
    }

    /** 按（DISPLAY_NAME, RELATIVE_PATH）查目标条目；命中返回对应 uri，未命中返回 null。 */
    private fun queryExisting(
        resolver: ContentResolver,
        collection: Uri,
        displayName: String,
        relativeDir: String,
    ): Uri? {
        resolver.query(
            collection,
            arrayOf(MediaStore.MediaColumns._ID),
            "${MediaStore.MediaColumns.DISPLAY_NAME}=? AND ${MediaStore.MediaColumns.RELATIVE_PATH}=?",
            arrayOf(displayName, relativeDir),
            null,
        )?.use { cursor ->
            if (cursor.moveToFirst()) {
                val id = cursor.getLong(0)
                return ContentUris.withAppendedId(collection, id)
            }
        }
        return null
    }

    private fun insertAndPublish(
        resolver: ContentResolver,
        collection: Uri,
        source: File,
        displayName: String,
        mime: String,
        relativeDir: String,
    ): Result<Unit> {
        val values = ContentValues().apply {
            put(MediaStore.MediaColumns.DISPLAY_NAME, displayName)
            put(MediaStore.MediaColumns.MIME_TYPE, mime)
            put(MediaStore.MediaColumns.RELATIVE_PATH, relativeDir)
            put(MediaStore.MediaColumns.IS_PENDING, 1)
        }
        val uri = resolver.insert(collection, values)
            ?: return Result.failure(IOException("Download 目录拒绝新建条目"))
        writeInto(resolver, uri, source, "w")
        // 发布失败必须返回失败：IS_PENDING 行约 7 天后被系统回收，用户会误以为文件存在。
        val pending = ContentValues().apply { put(MediaStore.MediaColumns.IS_PENDING, 0) }
        if (resolver.update(uri, pending, null, null) <= 0) {
            return Result.failure(IOException("Download 目录发布失败"))
        }
        return Result.success(Unit)
    }

    /** 打开输出流并流式复制；流为 null（目标不可写）按 IOException 语义失败，异常由 copy 统一转换。 */
    private fun writeInto(resolver: ContentResolver, uri: Uri, source: File, mode: String) {
        val output = resolver.openOutputStream(uri, mode)
            ?: throw IOException("无法打开目标输出流")
        FileInputStream(source).use { input ->
            output.use { out -> input.copyTo(out) }
        }
    }
}
