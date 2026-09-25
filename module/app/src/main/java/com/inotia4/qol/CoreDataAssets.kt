package com.inotia4.qol

import android.content.Context
import java.io.IOException

/** Transfers packaged JSON asset bytes to the native static-data owner without parsing them here. */
internal object CoreDataAssets {
    private const val ROOT = "static-data"
    private const val MAX_FILE_BYTES = 16 * 1024 * 1024
    private const val MAX_TOTAL_BYTES = 32 * 1024 * 1024

    fun load(context: Context) {
        val paths = listJsonAssets(context, "").sorted()
        var totalBytes = 0L
        for (path in paths) {
            totalBytes += countAssetBytes(context, path)
        }
        require(totalBytes <= MAX_TOTAL_BYTES) { "static assets exceed total size limit" }
        check(NativeBridge.nativeCoreDataBegin(paths.size, totalBytes.toInt())) {
            "native static-data import begin failed"
        }
        try {
            for (path in paths) {
                val bytes = context.assets.open("$ROOT/$path").use { it.readBytes() }
                check(bytes.size <= MAX_FILE_BYTES) { "static asset exceeds per-file limit: $path" }
                check(NativeBridge.nativeCoreDataAsset(path, bytes)) {
                    "native static-data asset import failed: $path"
                }
            }
            check(NativeBridge.nativeCoreDataFinish()) { "native static-data import finish failed" }
        } catch (error: Throwable) {
            NativeBridge.nativeCoreDataCancel()
            throw error
        }
    }

    private fun listJsonAssets(context: Context, directory: String): List<String> {
        val assetDirectory = if (directory.isEmpty()) ROOT else "$ROOT/$directory"
        val children = context.assets.list(assetDirectory)
            ?: throw IOException("cannot list static-data asset directory: $directory")
        val paths = ArrayList<String>()
        for (child in children) {
            val relativePath = if (directory.isEmpty()) child else "$directory/$child"
            val entries = context.assets.list("$ROOT/$relativePath")
            if (!entries.isNullOrEmpty()) {
                paths += listJsonAssets(context, relativePath)
            } else if (child.endsWith(".json")) {
                paths += relativePath
            }
        }
        return paths
    }

    private fun countAssetBytes(context: Context, path: String): Long =
        context.assets.open("$ROOT/$path").use { stream ->
            val buffer = ByteArray(8 * 1024)
            var count = 0L
            while (true) {
                val read = stream.read(buffer)
                if (read < 0) break
                count += read
                require(count <= MAX_FILE_BYTES) { "static asset exceeds per-file limit: $path" }
            }
            count
        }
}
