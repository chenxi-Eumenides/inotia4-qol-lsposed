package com.inotia4.qol.api.controller

import com.inotia4.qol.service.ApiServices
import com.inotia4.qol.util.ControllerGuard
import com.yanzhenjie.andserver.annotation.GetMapping
import com.yanzhenjie.andserver.annotation.PathVariable
import com.yanzhenjie.andserver.annotation.RequestParam
import com.yanzhenjie.andserver.annotation.RestController

/**
 * 背包：/api/item/inventory（api-reference §4.1）。复合 + money/items/bag/{i}/info + bag/{i}/{slot}。
 * 路由统一全路径写法（无类级 RequestMapping，见 architecture §3）。
 *
 * 列表端点分页：cursor 由 native 生成、对调用方不透明；limit 默认/上限由 owner 校验。
 * space 省略时 bag 按扁平袋号解析（原版→改版→注册源顺序），给出时按该空间内索引解析。
 */
@RestController
class InventoryController {

    @GetMapping("/api/item/inventory")
    fun composite(
        @RequestParam(value = "space", required = false) space: String?,
        @RequestParam(value = "cursor", required = false) cursor: String?,
        @RequestParam(value = "limit", required = false) limit: String?,
    ): String = ControllerGuard.guard {
        ApiServices.info.inventory(space?.toIntOrNull(), cursor, limit?.toIntOrNull())
    }

    @GetMapping("/api/item/inventory/money")
    fun money(): String = ControllerGuard.guard(ApiServices.info::inventoryMoney)

    @GetMapping("/api/item/inventory/items")
    fun items(
        @RequestParam(value = "space", required = false) space: String?,
        @RequestParam(value = "cursor", required = false) cursor: String?,
        @RequestParam(value = "limit", required = false) limit: String?,
    ): String = ControllerGuard.guard {
        ApiServices.info.inventoryItems(space?.toIntOrNull(), cursor, limit?.toIntOrNull())
    }

    @GetMapping("/api/item/inventory/bag/{bag}/info")
    fun bagInfo(
        @PathVariable("bag") bag: Int,
        @RequestParam(value = "space", required = false) space: String?,
    ): String = ControllerGuard.guard { ApiServices.info.bagInfo(bag, space?.toIntOrNull()) }

    @GetMapping("/api/item/inventory/bag/{bag}/{slot}")
    fun bagSlot(
        @PathVariable("bag") bag: Int,
        @PathVariable("slot") slot: Int,
        @RequestParam(value = "space", required = false) space: String?,
    ): String = ControllerGuard.guard { ApiServices.info.bagSlot(bag, slot, space?.toIntOrNull()) }
}
