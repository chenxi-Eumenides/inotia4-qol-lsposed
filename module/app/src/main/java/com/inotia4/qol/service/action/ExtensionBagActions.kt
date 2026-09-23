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
    fun extensionBagMoveItem(fromBag: Int, fromSlot: Int, toBag: Int, toSlot: Int): String =
        LogFile.op(LogDomain.EXTENSION_BAG, "POST /api/extension_bag/move_item", mapOf("fromBag" to "$fromBag", "fromSlot" to "$fromSlot", "toBag" to "$toBag", "toSlot" to "$toSlot")) {
            NativeBridge.nativeOpExtensionBagMoveItem(fromBag, fromSlot, toBag, toSlot)
        }
}
