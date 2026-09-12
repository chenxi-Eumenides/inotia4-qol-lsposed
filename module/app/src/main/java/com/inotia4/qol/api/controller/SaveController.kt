package com.inotia4.qol.api.controller

import com.inotia4.qol.service.ApiServices
import com.inotia4.qol.util.ApiException
import com.inotia4.qol.util.ControllerGuard
import com.inotia4.qol.util.JsonUtil
import com.yanzhenjie.andserver.annotation.GetMapping
import com.yanzhenjie.andserver.annotation.PostMapping
import com.yanzhenjie.andserver.annotation.RequestBody
import com.yanzhenjie.andserver.annotation.RestController
import com.yanzhenjie.andserver.http.StatusCode

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
        val slot = JsonUtil.parseBody(body)?.optInt("slot", -1) ?: -1
        if (slot !in 0..2) throw ApiException(StatusCode.SC_BAD_REQUEST, "bad slot")
        return ControllerGuard.guard { ApiServices.action.backupExport(slot) }
    }

    @GetMapping("/api/system/backup/list")
    fun backupList(): String = ControllerGuard.guard { ApiServices.action.backupList() }

    @PostMapping("/api/system/backup/import")
    fun backupImport(@RequestBody body: String): String {
        val o = JsonUtil.parseBody(body) ?: throw ApiException(StatusCode.SC_BAD_REQUEST, "bad request")
        val slot = o.optInt("slot", -1)
        val checksum = o.optString("checksum")
        if (slot !in 0..2) throw ApiException(StatusCode.SC_BAD_REQUEST, "bad slot")
        if (!CHECKSUM_PATTERN.matches(checksum)) throw ApiException(StatusCode.SC_BAD_REQUEST, "bad checksum")
        return ControllerGuard.guard { ApiServices.action.backupImport(checksum, slot) }
    }

    @PostMapping("/api/system/backup/delete")
    fun backupDelete(@RequestBody body: String): String {
        val checksum = JsonUtil.parseBody(body)?.optString("checksum").orEmpty()
        if (!CHECKSUM_PATTERN.matches(checksum)) throw ApiException(StatusCode.SC_BAD_REQUEST, "bad checksum")
        return ControllerGuard.guard { ApiServices.action.backupDelete(checksum) }
    }

    companion object {
        private val CHECKSUM_PATTERN = Regex("^[0-9a-f]{12}$")
    }

}
