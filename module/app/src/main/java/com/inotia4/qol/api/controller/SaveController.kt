package com.inotia4.qol.api.controller

import com.inotia4.qol.NativeBridge
import com.inotia4.qol.service.ApiServices
import com.inotia4.qol.service.info.NativeHttpCommand
import com.inotia4.qol.service.info.NativeHttpQuery
import com.inotia4.qol.util.ApiException
import com.inotia4.qol.util.ControllerGuard
import com.inotia4.qol.util.JsonUtil
import com.yanzhenjie.andserver.annotation.GetMapping
import com.yanzhenjie.andserver.annotation.PostMapping
import com.yanzhenjie.andserver.annotation.RequestBody
import com.yanzhenjie.andserver.annotation.RestController
import com.yanzhenjie.andserver.http.StatusCode
import org.json.JSONObject

// controller: 路由层，业务走 ApiServices。路径首段必须静态（AndServer 处理器约束，architecture §3）
@RestController
class SaveController {

    @PostMapping("/api/system/save")
    fun save(): String = ControllerGuard.guard { ApiServices.action.save() }

    @PostMapping("/api/system/enter_slot")
    fun enterSlot(@RequestBody body: String): String {
        val slot = JsonUtil.parseBody(body)?.optInt("slot", -1) ?: -1
        if (slot < 0) throw ApiException(StatusCode.SC_BAD_REQUEST, "slot required (0-2)")
        return ControllerGuard.guard { ApiServices.action.enterSlot(slot) }
    }

    @PostMapping("/api/system/create_slot")
    fun create(@RequestBody body: String): String {
        val o = JsonUtil.parseBody(body) ?: throw ApiException(StatusCode.SC_BAD_REQUEST, "bad request")
        val slot = o.optInt("slot", -1)
        val classIdx = o.optInt("class_idx", -1)
        if (slot < 0) throw ApiException(StatusCode.SC_BAD_REQUEST, "slot required (0-2)")
        if (classIdx < 0) throw ApiException(StatusCode.SC_BAD_REQUEST, "class_idx required (0-5)")
        return ControllerGuard.guard { ApiServices.action.createSlot(slot, classIdx) }
    }

    // ---- 存档管理器备份（checksum = sha256(origPlain‖module) 前 12 位小写 hex，为备份标识）----

    @PostMapping("/api/system/backup/export")
    fun backupExport(@RequestBody body: String): String {
        val o = JsonUtil.parseBody(body)
        strictBackupSlot(o)
        return ControllerGuard.guard {
            postAndAwait("/api/system/backup/export", o?.toString() ?: "{}")
        }
    }

    @GetMapping("/api/system/backup/list")
    fun backupList(): String =
        ControllerGuard.guard { NativeHttpQuery.get("/api/system/backup/list") }

    @PostMapping("/api/system/backup/import")
    fun backupImport(@RequestBody body: String): String {
        val o = JsonUtil.parseBody(body) ?: throw ApiException(StatusCode.SC_BAD_REQUEST, "bad request")
        strictBackupSlot(o)
        val checksum = o.optString("checksum")
        if (!CHECKSUM_PATTERN.matches(checksum)) throw ApiException(StatusCode.SC_BAD_REQUEST, "bad checksum")
        return ControllerGuard.guard {
            postAndAwait("/api/system/backup/import", o.toString())
        }
    }

    @PostMapping("/api/system/backup/delete")
    fun backupDelete(@RequestBody body: String): String {
        val o = JsonUtil.parseBody(body)
        val checksum = o?.optString("checksum").orEmpty()
        if (!CHECKSUM_PATTERN.matches(checksum)) throw ApiException(StatusCode.SC_BAD_REQUEST, "bad checksum")
        return ControllerGuard.guard {
            postAndAwait("/api/system/backup/delete", o?.toString() ?: "{}")
        }
    }

    /**
     * 写端点票据化：native 立即返回 {"ok":true,"ticket":N}，主线程执行完成后经
     * GET /api/system/tickets/{id} 回取原 JSON 结果。对外响应形状保持不变。
     */
    private fun postAndAwait(path: String, body: String): String {
        val ack = JsonUtil.parseObj(NativeHttpCommand.post(path, body))
            ?: throw ApiException(StatusCode.SC_SERVICE_UNAVAILABLE, "not ready")
        val ticket = ack.optLong("ticket", 0L)
        if (ticket <= 0L) return ack.toString()
        val deadline = System.currentTimeMillis() + 15_000L
        while (System.currentTimeMillis() < deadline) {
            val state = JsonUtil.parseObj(NativeBridge.nativeTicketStateJson(ticket))
                ?: throw ApiException(StatusCode.SC_SERVICE_UNAVAILABLE, "ticket unavailable")
            when (state.optString("state")) {
                "done", "failed" -> {
                    val result = state.opt("result")
                    if (result != null) return result.toString()
                    return if (state.optString("state") == "failed") {
                        JsonUtil.err("backup command failed")
                    } else {
                        "{\"ok\":true}"
                    }
                }
                "unknown", "expired" -> throw ApiException(
                    StatusCode.SC_SERVICE_UNAVAILABLE,
                    "ticket ${state.optString("state")}",
                )
            }
            Thread.sleep(20L)
        }
        throw ApiException(StatusCode.SC_SERVICE_UNAVAILABLE, "ticket timeout")
    }

    companion object {
        private val CHECKSUM_PATTERN = Regex("^[0-9a-f]{12}$")

        /** slot 必须是 JSON 整数（Int/Long）且落在 0..2；小数、字符串与越界值一律 bad slot。 */
        private fun strictBackupSlot(o: JSONObject?): Int {
            val raw = o?.opt("slot")
            val value = when (raw) {
                is Int -> raw.toLong()
                is Long -> raw
                else -> throw ApiException(StatusCode.SC_BAD_REQUEST, "bad slot")
            }
            if (value !in 0L..2L) throw ApiException(StatusCode.SC_BAD_REQUEST, "bad slot")
            return value.toInt()
        }
    }

}
