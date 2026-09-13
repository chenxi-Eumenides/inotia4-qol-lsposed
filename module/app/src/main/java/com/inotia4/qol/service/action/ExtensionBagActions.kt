package com.inotia4.qol.service.action

import com.inotia4.qol.AgreementPopup
import com.inotia4.qol.LogDomain
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

internal object ExtensionBagActions {
    fun extensionBagEnterView(bag: Int): String =
        LogFile.op(LogDomain.EXTENSION_BAG, "POST /api/debug/extension_bag/enter_view", mapOf("bag" to "$bag")) { NativeBridge.nativeOpExtensionBagEnterView(bag) }
    fun extensionBagExitView(): String =
        LogFile.op(LogDomain.EXTENSION_BAG, "POST /api/debug/extension_bag/exit_view", emptyMap<String, String>()) { NativeBridge.nativeOpExtensionBagExitView() }
    fun extensionBagUnequip(bag: Int): String =
        LogFile.op(LogDomain.EXTENSION_BAG, "POST /api/debug/extension_bag/unequip", mapOf("bag" to "$bag")) { NativeBridge.nativeOpExtensionBagUnequip(bag) }
    fun extensionBagSelectBag(bag: Int): String =
        LogFile.op(LogDomain.EXTENSION_BAG, "POST /api/debug/extension_bag/select_bag", mapOf("bag" to "$bag")) { NativeBridge.nativeOpExtensionBagSelectBag(bag) }
    fun extensionBagClickItem(bag: Int, slot: Int): String =
        LogFile.op(LogDomain.EXTENSION_BAG, "POST /api/debug/extension_bag/click_item", mapOf("bag" to "$bag", "slot" to "$slot")) { NativeBridge.nativeOpExtensionBagClickItem(bag, slot) }
    fun extensionBagMoveItem(fromBag: Int, fromSlot: Int, toBag: Int, toSlot: Int): String =
        LogFile.op(LogDomain.EXTENSION_BAG, "POST /api/extension_bag/move_item", mapOf("fromBag" to "$fromBag", "fromSlot" to "$fromSlot", "toBag" to "$toBag", "toSlot" to "$toSlot")) {
            NativeBridge.nativeOpExtensionBagMoveItem(fromBag, fromSlot, toBag, toSlot)
        }
}
