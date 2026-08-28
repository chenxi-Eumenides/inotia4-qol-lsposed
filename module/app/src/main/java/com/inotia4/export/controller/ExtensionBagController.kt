package com.inotia4.export.controller

import com.inotia4.export.service.ApiServices
import com.inotia4.export.util.ApiException
import com.inotia4.export.util.ControllerGuard
import com.inotia4.export.util.JsonUtil
import com.yanzhenjie.andserver.annotation.GetMapping
import com.yanzhenjie.andserver.annotation.PostMapping
import com.yanzhenjie.andserver.annotation.RequestBody
import com.yanzhenjie.andserver.annotation.RestController
import com.yanzhenjie.andserver.http.StatusCode

// controller: 扩展背包正式操作面（control-plane §4.1 / ADR-008）。路径首段静态（architecture §3）
@RestController
class ExtensionBagController {

    @GetMapping("/api/extension_bag/status")
    fun status(): String = ControllerGuard.guard { ApiServices.info.extensionBagStatusJson() }

    @PostMapping("/api/extension_bag/enter_view")
    fun enterView(@RequestBody body: String): String {
        val o = JsonUtil.parseBody(body) ?: throw ApiException(StatusCode.SC_BAD_REQUEST, "bad request")
        val bag = o.optInt("bag", -1)
        if (bag < 6 || bag > 10) throw ApiException(StatusCode.SC_BAD_REQUEST, "bag required (6-10)")
        return ControllerGuard.guard { ApiServices.action.extensionBagEnterView(bag) }
    }

    @PostMapping("/api/extension_bag/exit_view")
    fun exitView(): String = ControllerGuard.guard { ApiServices.action.extensionBagExitView() }

    @PostMapping("/api/extension_bag/select_bag")
    fun selectBag(@RequestBody body: String): String {
        val o = JsonUtil.parseBody(body) ?: throw ApiException(StatusCode.SC_BAD_REQUEST, "bad request")
        val bag = o.optInt("bag", -1)
        if (bag < 6 || bag > 10) throw ApiException(StatusCode.SC_BAD_REQUEST, "bag required (6-10)")
        return ControllerGuard.guard { ApiServices.action.extensionBagSelectBag(bag) }
    }

    @PostMapping("/api/extension_bag/click_item")
    fun clickItem(@RequestBody body: String): String {
        val o = JsonUtil.parseBody(body) ?: throw ApiException(StatusCode.SC_BAD_REQUEST, "bad request")
        val bag = o.optInt("bag", -1)
        val slot = o.optInt("slot", -1)
        if (bag < 6 || bag > 10) throw ApiException(StatusCode.SC_BAD_REQUEST, "bag required (6-10)")
        if (slot < 0 || slot > 15) throw ApiException(StatusCode.SC_BAD_REQUEST, "slot required (0-15)")
        return ControllerGuard.guard { ApiServices.action.extensionBagClickItem(bag, slot) }
    }

    @PostMapping("/api/extension_bag/move_item")
    fun moveItem(@RequestBody body: String): String {
        val o = JsonUtil.parseBody(body) ?: throw ApiException(StatusCode.SC_BAD_REQUEST, "bad request")
        val fromBag = o.optInt("from_bag", -1)
        val fromSlot = o.optInt("from_slot", -1)
        val toBag = o.optInt("to_bag", -1)
        val toSlot = o.optInt("to_slot", -1)
        if (fromBag < 0 || fromSlot < 0 || toBag < 0)
            throw ApiException(StatusCode.SC_BAD_REQUEST, "from_bag/from_slot/to_bag required")
        if (toSlot < -1) throw ApiException(StatusCode.SC_BAD_REQUEST, "bad to_slot")
        return ControllerGuard.guard {
            ApiServices.action.extensionBagMoveItem(fromBag, fromSlot, toBag, toSlot)
        }
    }
}
