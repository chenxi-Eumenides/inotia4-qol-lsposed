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
class ShopController {

    @GetMapping("/api/item/shop/items")
    fun items(): String = ControllerGuard.guard(ApiServices.info::shopItems)

    @PostMapping("/api/item/shop/buy_item")
    fun buy(@RequestBody body: String): String {
        val o = JsonUtil.parseBody(body) ?: throw ApiException(StatusCode.SC_BAD_REQUEST, "bad request")
        val slot = o.optInt("slot", -1)
        if (slot < 0) throw ApiException(StatusCode.SC_BAD_REQUEST, "slot required")
        return ControllerGuard.guard { ApiServices.action.shopBuy(slot) }
    }
}
