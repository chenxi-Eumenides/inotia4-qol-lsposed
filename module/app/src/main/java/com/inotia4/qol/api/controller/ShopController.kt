package com.inotia4.qol.api.controller

import com.inotia4.qol.service.ApiServices
import com.inotia4.qol.service.info.NativeHttpCommand
import com.inotia4.qol.util.ControllerGuard
import com.yanzhenjie.andserver.annotation.GetMapping
import com.yanzhenjie.andserver.annotation.PostMapping
import com.yanzhenjie.andserver.annotation.RequestBody
import com.yanzhenjie.andserver.annotation.RestController

// 商店读取仍走 ApiServices，购买写请求由 NativeHttpCommand 透传给领域 owner。
@RestController
class ShopController {

    @GetMapping("/api/item/shop/items")
    fun items(): String = ControllerGuard.guard(ApiServices.info::shopItems)

    @PostMapping("/api/item/shop/buy_item")
    fun buy(@RequestBody body: String): String =
        NativeHttpCommand.post("/api/item/shop/buy_item", body)
}
