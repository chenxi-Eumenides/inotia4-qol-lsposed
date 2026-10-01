package com.inotia4.qol.api.controller

import com.inotia4.qol.service.info.NativeHttpCommand
import com.yanzhenjie.andserver.annotation.PathVariable
import com.yanzhenjie.andserver.annotation.PostMapping
import com.yanzhenjie.andserver.annotation.RequestBody
import com.yanzhenjie.andserver.annotation.RestController

@RestController
class InventoryActionController {

    @PostMapping("/api/item/inventory/use_item")
    fun useItem(@RequestBody body: String): String =
        NativeHttpCommand.post("/api/item/inventory/use_item", body)

    @PostMapping("/api/item/inventory/accept_dice")
    fun diceAccept(): String = NativeHttpCommand.post("/api/item/inventory/accept_dice", "{}")

    @PostMapping("/api/item/inventory/reject_dice")
    fun diceReject(): String = NativeHttpCommand.post("/api/item/inventory/reject_dice", "{}")

    @PostMapping("/api/item/inventory/discard_item")
    fun discard(@RequestBody body: String): String =
        NativeHttpCommand.post("/api/item/inventory/discard_item", body)

    @PostMapping("/api/item/inventory/sell_item")
    fun sell(@RequestBody body: String): String =
        NativeHttpCommand.post("/api/item/inventory/sell_item", body)

    @PostMapping("/api/item/inventory/move_item")
    fun move(@RequestBody body: String): String =
        NativeHttpCommand.post("/api/item/inventory/move_item", body)

    @PostMapping("/api/item/inventory/{role}/put_jewel")
    fun jewel(@PathVariable("role") role: Int, @RequestBody body: String): String =
        NativeHttpCommand.post("/api/item/inventory/$role/put_jewel", body)

    @PostMapping("/api/item/inventory/{role}/enchant")
    fun enchant(@PathVariable("role") role: Int, @RequestBody body: String): String =
        NativeHttpCommand.post("/api/item/inventory/$role/enchant", body)

    @PostMapping("/api/item/inventory/{role}/equip_item")
    fun equip(@PathVariable("role") role: Int, @RequestBody body: String): String =
        NativeHttpCommand.post("/api/item/inventory/$role/equip_item", body)

    @PostMapping("/api/item/inventory/{role}/unequip_item")
    fun unequip(@PathVariable("role") role: Int, @RequestBody body: String): String =
        NativeHttpCommand.post("/api/item/inventory/$role/unequip_item", body)
}
