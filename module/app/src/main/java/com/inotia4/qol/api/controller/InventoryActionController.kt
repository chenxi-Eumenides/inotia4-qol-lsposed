package com.inotia4.qol.api.controller

import com.inotia4.qol.service.info.NativeHttpCommand
import com.inotia4.qol.util.ApiException
import com.inotia4.qol.util.JsonUtil
import com.yanzhenjie.andserver.annotation.PathVariable
import com.yanzhenjie.andserver.annotation.PostMapping
import com.yanzhenjie.andserver.annotation.RequestBody
import com.yanzhenjie.andserver.annotation.RestController
import com.yanzhenjie.andserver.http.StatusCode

// 写请求先保留兼容校验，再由 NativeHttpCommand 透传给领域 owner。
@RestController
class InventoryActionController {

    @PostMapping("/api/item/inventory/use_item")
    fun useItem(@RequestBody body: String): String {
        val o = JsonUtil.parseBody(body) ?: throw ApiException(StatusCode.SC_BAD_REQUEST, "bad request")
        val bag = o.optInt("bag", -1)
        val slot = o.optInt("slot", -1)
        if (bag < 0 || slot < 0) throw ApiException(StatusCode.SC_BAD_REQUEST, "bag/slot required")
        return NativeHttpCommand.post("/api/item/inventory/use_item", body)
    }

    @PostMapping("/api/item/inventory/accept_dice")
    fun diceAccept(): String = NativeHttpCommand.post("/api/item/inventory/accept_dice", "{}")

    @PostMapping("/api/item/inventory/reject_dice")
    fun diceReject(): String = NativeHttpCommand.post("/api/item/inventory/reject_dice", "{}")

    @PostMapping("/api/item/inventory/discard_item")
    fun discard(@RequestBody body: String): String {
        val o = JsonUtil.parseBody(body) ?: throw ApiException(StatusCode.SC_BAD_REQUEST, "bad request")
        val bag = o.optInt("bag", -1)
        val slot = o.optInt("slot", -1)
        if (bag < 0 || slot < 0) throw ApiException(StatusCode.SC_BAD_REQUEST, "bag/slot required")
        return NativeHttpCommand.post("/api/item/inventory/discard_item", body)
    }

    @PostMapping("/api/item/inventory/sell_item")
    fun sell(@RequestBody body: String): String {
        val o = JsonUtil.parseBody(body) ?: throw ApiException(StatusCode.SC_BAD_REQUEST, "bad request")
        val bag = o.optInt("bag", -1)
        val slot = o.optInt("slot", -1)
        if (bag < 0 || slot < 0) throw ApiException(StatusCode.SC_BAD_REQUEST, "bag/slot required")
        return NativeHttpCommand.post("/api/item/inventory/sell_item", body)
    }

    @PostMapping("/api/item/inventory/move_item")
    fun move(@RequestBody body: String): String {
        val o = JsonUtil.parseBody(body) ?: throw ApiException(StatusCode.SC_BAD_REQUEST, "bad request")
        val bag = o.optInt("bag", -1)
        val slot = o.optInt("slot", -1)
        val count = o.optInt("count", -1)
        val toBag = o.optInt("to_bag", -1)
        val toSlot = o.optInt("to_slot", -1)
        if (bag < 0 || bag > 10 || slot < 0 || count <= 0 || toBag < 0 || toBag > 10 || toSlot < -1)
            throw ApiException(StatusCode.SC_BAD_REQUEST, "bag/slot/count/to_bag/to_slot required (bag 0-10, 5=task bag excluded for extension)")
        return NativeHttpCommand.post("/api/item/inventory/move_item", body)
    }

    @PostMapping("/api/item/inventory/{role}/put_jewel")
    fun jewel(@PathVariable("role") role: Int, @RequestBody body: String): String {
        val o = JsonUtil.parseBody(body) ?: throw ApiException(StatusCode.SC_BAD_REQUEST, "bad request")
        val bag = o.optInt("bag", -1)
        val slot = o.optInt("slot", -1)
        val equipSlot = o.optInt("equip_slot", -1)
        if (bag < 0 || slot < 0 || equipSlot < 0)
            throw ApiException(StatusCode.SC_BAD_REQUEST, "bag/slot/equip_slot required")
        return NativeHttpCommand.post("/api/item/inventory/$role/put_jewel", body)
    }

    @PostMapping("/api/item/inventory/{role}/enchant")
    fun enchant(@PathVariable("role") role: Int, @RequestBody body: String): String {
        val o = JsonUtil.parseBody(body) ?: throw ApiException(StatusCode.SC_BAD_REQUEST, "bad request")
        val bag = o.optInt("bag", -1)
        val slot = o.optInt("slot", -1)
        val equipSlot = o.optInt("equip_slot", -1)
        if (bag < 0 || slot < 0 || equipSlot < 0)
            throw ApiException(StatusCode.SC_BAD_REQUEST, "bag/slot/equip_slot required")
        return NativeHttpCommand.post("/api/item/inventory/$role/enchant", body)
    }

    @PostMapping("/api/item/inventory/{role}/equip_item")
    fun equip(@PathVariable("role") role: Int, @RequestBody body: String): String {
        val o = JsonUtil.parseBody(body) ?: throw ApiException(StatusCode.SC_BAD_REQUEST, "bad request")
        val bag = o.optInt("bag", -1)
        val slot = o.optInt("slot", -1)
        val category = o.optInt("category", -1)
        return if (category >= 0) {
            NativeHttpCommand.post("/api/item/inventory/$role/equip_item", body)
        } else if (bag >= 0 && slot >= 0) {
            NativeHttpCommand.post("/api/item/inventory/$role/equip_item", body)
        } else {
            throw ApiException(StatusCode.SC_BAD_REQUEST, "bag+slot or category required")
        }
    }

    @PostMapping("/api/item/inventory/{role}/unequip_item")
    fun unequip(@PathVariable("role") role: Int, @RequestBody body: String): String {
        val o = JsonUtil.parseBody(body) ?: throw ApiException(StatusCode.SC_BAD_REQUEST, "bad request")
        val slot = o.optInt("slot", -1)
        if (slot < 0) throw ApiException(StatusCode.SC_BAD_REQUEST, "slot required")
        return NativeHttpCommand.post("/api/item/inventory/$role/unequip_item", body)
    }
}
