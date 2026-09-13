package com.inotia4.qol.service.action

import com.inotia4.qol.LogDomain
import com.inotia4.qol.LogFile
import com.inotia4.qol.NativeBridge
import com.inotia4.qol.util.JsonUtil

/**
 * 存档管理器备份（api/system/backup 四端点）服务适配。
 *
 * bundle 组装/解析、导出去重、导入事务与回滚全部收口 native feature/save_backup
 * （NativeBridge.nativeBackup*），本层仅保留 LogFile.op 端点日志包装与入参防御校验，
 * 直接返回 native 的响应 JSON（与迁移前逐字段一致）。
 */
internal object SaveBackupActions {

    private val CHECKSUM = Regex("^[0-9a-f]{12}$")

    fun backupExport(slot: Int): String =
        LogFile.op(LogDomain.SAVE_BACKUP, "POST /api/system/backup/export", mapOf("slot" to "$slot")) {
            if (slot !in 0..2) JsonUtil.err("bad slot") else NativeBridge.nativeBackupExport(slot)
        }

    fun backupImport(checksum: String, slot: Int): String =
        LogFile.op(LogDomain.SAVE_BACKUP, "POST /api/system/backup/import", mapOf("checksum" to "$checksum", "slot" to "$slot")) {
            when {
                !CHECKSUM.matches(checksum) -> JsonUtil.err("bad checksum")
                slot !in 0..2 -> JsonUtil.err("bad slot")
                else -> NativeBridge.nativeBackupImport(checksum, slot)
            }
        }

    fun backupList(): String =
        LogFile.op(LogDomain.SAVE_BACKUP, "GET /api/system/backup/list", emptyMap<String, String>()) { NativeBridge.nativeBackupList() }

    fun backupDelete(checksum: String): String =
        LogFile.op(LogDomain.SAVE_BACKUP, "POST /api/system/backup/delete", mapOf("checksum" to "$checksum")) {
            if (CHECKSUM.matches(checksum)) NativeBridge.nativeBackupDelete(checksum)
            else JsonUtil.err("bad checksum")
        }
}
