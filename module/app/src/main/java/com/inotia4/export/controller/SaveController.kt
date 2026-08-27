package com.inotia4.export.controller

import com.inotia4.export.service.ApiServices
import com.inotia4.export.util.ApiException
import com.inotia4.export.util.ControllerGuard
import com.inotia4.export.util.JsonUtil
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

}
