package com.inotia4.qol

import android.content.Context
import java.io.File
import java.io.IOException

object QolPaths {
    const val DIR_NAME = "qol"
    private const val LEGACY_SIDECAR_SUFFIX = ".module-save"
    private const val SIDECAR_SUFFIX = ".qol-save"
    private val layoutPlaceholders = mutableSetOf<String>()

    fun root(context: Context): File? =
        context.getExternalFilesDir(null)?.let { File(it, DIR_NAME) }

    /** 创建 qol/ 及 saves、backups、plugins、resources；失败返回 null。幂等。 */
    @Synchronized
    fun ensureLayout(context: Context): File? {
        return try {
            val root = root(context) ?: return null
            if (root.exists() && !root.isDirectory) return null
            if (!root.exists() && !root.mkdirs() && !root.isDirectory) return null

            for (name in listOf("saves", "backups", "plugins", "resources")) {
                val directory = File(root, name)
                if (directory.exists() && !directory.isDirectory) return null
                if (!directory.exists()) {
                    if (!directory.mkdirs() && !directory.isDirectory) return null
                    synchronized(layoutPlaceholders) { layoutPlaceholders.add(directory.absolutePath) }
                }
            }
            root
        } catch (e: Exception) {
            LogFile.error(LogDomain.PLATFORM, "qol layout creation failed", e)
            null
        }
    }

    /** 一次性把旧位置搬进 qol/；幂等，目标已存在时不覆盖，失败时保留源。 */
    fun migrateLegacy(context: Context) {
        val externalFiles = context.getExternalFilesDir(null)
        if (externalFiles == null) {
            LogFile.info(LogDomain.PLATFORM, "legacy storage migration skipped: external files directory unavailable")
            return
        }
        val root = ensureLayout(context)
        if (root == null) {
            LogFile.error(LogDomain.PLATFORM, "legacy storage migration failed: qol layout unavailable")
            return
        }

        migratePath(File(externalFiles, "config.json"), File(root, "config.json"), isDirectory = false)
        migratePath(File(externalFiles, "module-saves"), File(root, "saves"), isDirectory = true)
        migratePath(File(externalFiles, "save_backup"), File(root, "backups"), isDirectory = true)
        // sidecar 后缀已改为 .qol-save；对（无论本次还是更早搬迁的）saves/ 内旧后缀文件重命名，幂等。
        renameLegacySidecars(File(root, "saves"))
        removeLegacyFile(File(externalFiles, "skip_hive_block.flag"))
        removeLegacyFile(File(externalFiles, "inotia4-export.log"))
    }

    private fun migratePath(source: File, destination: File, isDirectory: Boolean) {
        if (!source.exists() || (isDirectory && !source.isDirectory) || (!isDirectory && !source.isFile)) {
            LogFile.info(LogDomain.PLATFORM, "legacy migration skipped: ${source.name} is absent or has the wrong type")
            return
        }

        if (destination.exists()) {
            val createdByLayout = synchronized(layoutPlaceholders) {
                layoutPlaceholders.remove(destination.absolutePath)
            }
            // ensureLayout 本次预建的空目录是占位目录；其他已存在目标保持不变。
            val emptyDirectoryPlaceholder =
                createdByLayout && isDirectory && destination.isDirectory && destination.list()?.isEmpty() == true
            if (!emptyDirectoryPlaceholder) {
                LogFile.info(LogDomain.PLATFORM, "legacy migration skipped: ${destination.path} already exists")
                return
            }
            if (!destination.delete()) {
                LogFile.error(LogDomain.PLATFORM, "legacy migration failed: cannot replace empty ${destination.path}")
                return
            }
        }

        if (source.renameTo(destination)) {
            LogFile.info(LogDomain.PLATFORM, "legacy migration moved ${source.name} to ${destination.path}")
            return
        }

        try {
            copyRecursively(source, destination)
            if (!source.deleteRecursively()) {
                try {
                    copyRecursively(destination, source, overwrite = true)
                    if (!destination.deleteRecursively()) {
                        throw IOException("could not remove copied destination ${destination.path}")
                    }
                } catch (restoreFailure: Exception) {
                    LogFile.error(
                        LogDomain.PLATFORM,
                        "legacy source cleanup failed; recovery copy retained at ${destination.path}",
                        restoreFailure,
                    )
                    return
                }
                throw IOException("could not remove legacy source ${source.path}")
            }
            LogFile.info(LogDomain.PLATFORM, "legacy migration copied ${source.name} to ${destination.path}")
        } catch (t: Exception) {
            destination.deleteRecursively()
            LogFile.error(LogDomain.PLATFORM, "legacy migration failed for ${source.path}; source retained", t)
        }
    }

    private fun copyRecursively(source: File, destination: File, overwrite: Boolean = false) {
        if (source.isDirectory) {
            if (destination.exists() && !destination.isDirectory) {
                throw IOException("${destination.path} is not a directory")
            }
            if (!destination.exists() && !destination.mkdir() && !destination.isDirectory) {
                throw IOException("could not create ${destination.path}")
            }
            val children = source.listFiles() ?: throw IOException("could not list ${source.path}")
            children.forEach { child -> copyRecursively(child, File(destination, child.name), overwrite) }
            return
        }

        if (destination.exists() && !overwrite) {
            throw IOException("${destination.path} already exists")
        }
        val parent = destination.parentFile
        if (parent != null && !parent.isDirectory && !parent.mkdirs() && !parent.isDirectory) {
            throw IOException("could not create ${parent.path}")
        }
        source.inputStream().use { input ->
            destination.outputStream().use { output -> input.copyTo(output) }
        }
    }

    /**
     * 把 saves/ 内旧后缀 `.module-save`（含 `.last-good`、`.corrupt.<ms>` 等派生名）改为
     * `.qol-save`，使旧 sidecar 在新命名下仍可被 native store 读取。目标已存在时不覆盖。
     */
    private fun renameLegacySidecars(savesDir: File) {
        if (!savesDir.isDirectory) return
        for (file in savesDir.listFiles() ?: return) {
            if (!file.isFile || !file.name.contains(LEGACY_SIDECAR_SUFFIX)) continue
            val target = File(savesDir, file.name.replace(LEGACY_SIDECAR_SUFFIX, SIDECAR_SUFFIX))
            if (target.exists()) {
                LogFile.info(LogDomain.PLATFORM, "legacy sidecar rename skipped: ${target.name} already exists")
                continue
            }
            if (file.renameTo(target)) {
                LogFile.info(LogDomain.PLATFORM, "legacy sidecar renamed ${file.name} -> ${target.name}")
            } else {
                LogFile.error(LogDomain.PLATFORM, "legacy sidecar rename failed: ${file.path}")
            }
        }
    }

    private fun removeLegacyFile(file: File) {
        if (!file.exists()) return
        if (file.delete()) {
            LogFile.info(LogDomain.PLATFORM, "legacy file removed: ${file.name}")
        } else {
            LogFile.error(LogDomain.PLATFORM, "legacy file removal failed: ${file.path}")
        }
    }
}
