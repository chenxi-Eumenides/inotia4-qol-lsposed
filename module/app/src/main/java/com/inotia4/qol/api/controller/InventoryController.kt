package com.inotia4.qol.api.controller

import com.inotia4.qol.service.ApiServices
import com.inotia4.qol.util.ControllerGuard
import com.yanzhenjie.andserver.annotation.GetMapping
import com.yanzhenjie.andserver.annotation.PathVariable
import com.yanzhenjie.andserver.annotation.RestController

/**
 * 背包：/api/item/inventory（api-reference §5.1）。复合 + money/items/bag/{i}/info + bag/{i}/{slot}。
 * 路由统一全路径写法（无类级 RequestMapping，见 architecture §3）。
 */
@RestController
class InventoryController {

    @GetMapping("/api/item/inventory")
        fun composite(): String = ControllerGuard.guard(ApiServices.info::inventory)

    @GetMapping("/api/item/inventory/money")
    fun money(): String = ControllerGuard.guard(ApiServices.info::inventoryMoney)

    @GetMapping("/api/item/inventory/items")
    fun items(): String = ControllerGuard.guard(ApiServices.info::inventoryItems)

    @GetMapping("/api/item/inventory/bag/{bag}/info")
    fun bagInfo(@PathVariable("bag") bag: Int): String =
        ControllerGuard.guard { ApiServices.info.bagInfo(bag) }

    @GetMapping("/api/item/inventory/bag/{bag}/{slot}")
    fun bagSlot(@PathVariable("bag") bag: Int, @PathVariable("slot") slot: Int): String =
        ControllerGuard.guard { ApiServices.info.bagSlot(bag, slot) }
}
