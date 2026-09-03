package com.inotia4.qol.service.action

import com.inotia4.qol.AgreementPopup
import com.inotia4.qol.LogFile
import com.inotia4.qol.NativeBridge
import com.inotia4.qol.UiActivityTracker
import com.inotia4.qol.patch.AgreementGate
import com.inotia4.qol.store.ModuleSaveStore
import com.inotia4.qol.util.ApiException
import com.inotia4.qol.util.JsonUtil
import com.yanzhenjie.andserver.http.StatusCode
import org.json.JSONObject
import com.inotia4.qol.service.contract.ActionApiService

internal object InventoryActions {
    fun useItem(bag: Int, slot: Int): String =
        LogFile.op("POST /api/item/inventory/use_item", "bag=$bag,slot=$slot") { ActionSupport.attachInventory(NativeBridge.nativeOpUseItem(bag, slot)) }
    fun diceAccept(): String =
        LogFile.op("POST /api/item/inventory/accept_dice", "") { ActionSupport.attachPlayer(NativeBridge.nativeOpDiceAccept()) }
    fun diceReject(): String =
        LogFile.op("POST /api/item/inventory/reject_dice", "") { NativeBridge.nativeOpDiceReject() }
    fun sellItem(bag: Int, slot: Int): String =
        LogFile.op("POST /api/item/inventory/sell_item", "bag=$bag,slot=$slot") { ActionSupport.attachInventory(NativeBridge.nativeOpSellItem(bag, slot)) }
    fun moveItem(bag: Int, slot: Int, count: Int, toBag: Int, toSlot: Int): String =
        LogFile.op("POST /api/item/inventory/move_item", "bag=$bag,slot=$slot,count=$count,toBag=$toBag,toSlot=$toSlot") {
            val extensionInvolved = bag in 6..10 || toBag in 6..10
            if (extensionInvolved) {
                NativeBridge.nativeOpExtensionBagMoveItem(bag, slot, toBag, toSlot)
            } else {
                ActionSupport.attachInventory(NativeBridge.nativeOpMoveItem(bag, slot, count, toBag, toSlot))
            }
        }
    fun equip(role: Int, bag: Int, slot: Int): String =
        LogFile.op("POST /api/item/inventory/{role}/equip_item", "role=$role,bag=$bag,slot=$slot") { ActionSupport.attachParty(NativeBridge.nativeOpEquip(role, bag, slot)) }
    fun equipByCategory(role: Int, category: Int): String =
        LogFile.op("POST /api/item/inventory/{role}/equip_item", "role=$role,category=$category") {
            val pos = ActionSupport.findItemSlot(category) ?: throw ApiException(StatusCode.SC_NOT_FOUND, "item not found")
            ActionSupport.attachParty(NativeBridge.nativeOpEquip(role, pos.first, pos.second))
        }
    fun unequip(role: Int, slot: Int): String =
        LogFile.op("POST /api/item/inventory/{role}/unequip_item", "role=$role,slot=$slot") { ActionSupport.attachParty(NativeBridge.nativeOpUnequip(role, slot)) }
    fun shopBuy(slot: Int): String =
        LogFile.op("POST /api/item/shop/buy_item", "slot=$slot") { ActionSupport.attachInventory(NativeBridge.nativeOpShopBuy(slot)) }
    fun jewel(role: Int, bag: Int, slot: Int, equipSlot: Int): String =
        LogFile.op("POST /api/item/inventory/{role}/put_jewel", "role=$role,bag=$bag,slot=$slot,equipSlot=$equipSlot") {
            ActionSupport.attachParty(NativeBridge.nativeOpJewel(role, bag, slot, equipSlot))
        }
    fun enchant(role: Int, bag: Int, slot: Int, equipSlot: Int): String =
        LogFile.op("POST /api/item/inventory/{role}/enchant", "role=$role,bag=$bag,slot=$slot,equipSlot=$equipSlot") {
            ActionSupport.attachParty(NativeBridge.nativeOpEnchant(role, bag, slot, equipSlot))
        }
    fun discardItem(bag: Int, slot: Int): String =
        LogFile.op("POST /api/item/inventory/discard_item", "bag=$bag,slot=$slot") { ActionSupport.attachInventory(NativeBridge.nativeOpDiscardItem(bag, slot)) }
}
