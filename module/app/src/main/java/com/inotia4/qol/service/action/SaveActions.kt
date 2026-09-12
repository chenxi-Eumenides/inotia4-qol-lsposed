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
import com.inotia4.qol.service.contract.ActionApiService

internal object SaveActions {
    fun save(): String =
        LogFile.op("POST /api/system/save", "") {
            ActionSupport.attachPlayer(ActionSupport.afterSave(NativeBridge.nativeOpSave()))
        }
    fun mainMenu(): String =
        LogFile.op("POST /api/ui/go_main_menu", "") { ActionSupport.attachPlayer(NativeBridge.nativeOpMainMenu()) }
    fun enterSlot(slot: Int): String =
        LogFile.op("POST /api/system/enter_slot", "slot=$slot") {
            val activityCheck = UiActivityTracker.check()
            if (activityCheck.failed) {
                JsonUtil.err("ui state unavailable")
            } else if (activityCheck.blockingActivityName != null) {
                JsonUtil.err("ui occupied: agreement")
            } else {
                AgreementGate.beginWorldLoad()
                ActionSupport.attachPlayer(ActionSupport.afterNativeSuccess(NativeBridge.nativeOpEnterSlot(slot)) { ModuleSaveStore.ensureSlot(slot) })
            }
        }
    fun createSlot(slot: Int, classIdx: Int): String =
        LogFile.op("POST /api/system/create_slot", "slot=$slot,classIdx=$classIdx") {
            val activityCheck = UiActivityTracker.check()
            if (activityCheck.failed) {
                JsonUtil.err("ui state unavailable")
            } else if (activityCheck.blockingActivityName != null) {
                JsonUtil.err("ui occupied: agreement")
            } else {
                AgreementGate.beginWorldLoad()
                ActionSupport.attachPlayer(ActionSupport.afterNativeSuccess(NativeBridge.nativeOpCreateSlot(slot, classIdx)) { ModuleSaveStore.resetSlot(slot) })
            }
        }
}
