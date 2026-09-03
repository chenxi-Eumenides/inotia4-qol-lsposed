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

internal object ExtensionBagActions {
    fun extensionBagEnterView(bag: Int): String =
        LogFile.op("POST /api/debug/extension_bag/enter_view", "bag=$bag") { NativeBridge.nativeOpExtensionBagEnterView(bag) }
    fun extensionBagExitView(): String =
        LogFile.op("POST /api/debug/extension_bag/exit_view", "") { NativeBridge.nativeOpExtensionBagExitView() }
    fun extensionBagUnequip(bag: Int): String =
        LogFile.op("POST /api/debug/extension_bag/unequip", "bag=$bag") { NativeBridge.nativeOpExtensionBagUnequip(bag) }
    fun extensionBagSelectBag(bag: Int): String =
        LogFile.op("POST /api/debug/extension_bag/select_bag", "bag=$bag") { NativeBridge.nativeOpExtensionBagSelectBag(bag) }
    fun extensionBagClickItem(bag: Int, slot: Int): String =
        LogFile.op("POST /api/debug/extension_bag/click_item", "bag=$bag,slot=$slot") { NativeBridge.nativeOpExtensionBagClickItem(bag, slot) }
    fun extensionBagMoveItem(fromBag: Int, fromSlot: Int, toBag: Int, toSlot: Int): String =
        LogFile.op("POST /api/extension_bag/move_item", "fromBag=$fromBag,fromSlot=$fromSlot,toBag=$toBag,toSlot=$toSlot") {
            NativeBridge.nativeOpExtensionBagMoveItem(fromBag, fromSlot, toBag, toSlot)
        }
}
