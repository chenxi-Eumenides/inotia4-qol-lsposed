package com.inotia4.qol.controller

import com.inotia4.qol.service.ApiServices
import com.inotia4.qol.util.ApiException
import com.inotia4.qol.util.ControllerGuard
import com.inotia4.qol.util.JsonUtil
import com.yanzhenjie.andserver.annotation.PostMapping
import com.yanzhenjie.andserver.annotation.RequestBody
import com.yanzhenjie.andserver.annotation.RestController
import com.yanzhenjie.andserver.http.StatusCode

// controller: 路由层，业务走 ApiServices。路径首段必须静态（AndServer 处理器约束，architecture §3）
@RestController
class PartyActionController {

    @PostMapping("/api/character/party/include")
    fun include(@RequestBody body: String): String {
        val o = JsonUtil.parseBody(body) ?: throw ApiException(StatusCode.SC_BAD_REQUEST, "bad request")
        val mercSlot = o.optInt("mercenary_slot", -1)
        if (mercSlot < 0) throw ApiException(StatusCode.SC_BAD_REQUEST, "mercenary_slot required")
        return ControllerGuard.guard { ApiServices.action.includeParty(mercSlot) }
    }

    @PostMapping("/api/character/party/exclude")
    fun exclude(@RequestBody body: String): String {
        val o = JsonUtil.parseBody(body) ?: throw ApiException(StatusCode.SC_BAD_REQUEST, "bad request")
        val mercSlot = o.optInt("mercenary_slot", -1)
        if (mercSlot < 0) throw ApiException(StatusCode.SC_BAD_REQUEST, "mercenary_slot required")
        return ControllerGuard.guard { ApiServices.action.excludeParty(mercSlot) }
    }

    @PostMapping("/api/character/party/discharge")
    fun discharge(@RequestBody body: String): String {
        val o = JsonUtil.parseBody(body) ?: throw ApiException(StatusCode.SC_BAD_REQUEST, "bad request")
        val mercSlot = o.optInt("mercenary_slot", -1)
        if (mercSlot < 0) throw ApiException(StatusCode.SC_BAD_REQUEST, "mercenary_slot required")
        return ControllerGuard.guard { ApiServices.action.discharge(mercSlot) }
    }

    @PostMapping("/api/character/party/withdraw")
    fun withdraw(@RequestBody body: String): String {
        val o = JsonUtil.parseBody(body) ?: throw ApiException(StatusCode.SC_BAD_REQUEST, "bad request")
        val mercSlot = o.optInt("mercenary_slot", -1)
        val equipSlot = o.optInt("equip_slot", -1)
        if (mercSlot < 0 || equipSlot < 0) throw ApiException(StatusCode.SC_BAD_REQUEST, "mercenarySlot/equip_slot required")
        return ControllerGuard.guard { ApiServices.action.withdraw(mercSlot, equipSlot) }
    }
}
