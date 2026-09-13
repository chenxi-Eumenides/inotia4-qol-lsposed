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

internal object UiActions {
    fun panelClose(): String =
        LogFile.op(LogDomain.API, "POST /api/ui/close_panel", emptyMap<String, String>()) { ActionSupport.attachUi(NativeBridge.nativeOpPanelClose()) }
    fun panelOpen(panel: String): String =
        LogFile.op(LogDomain.API, "POST /api/ui/open_panel", mapOf("panel" to "$panel")) { ActionSupport.attachUi(NativeBridge.nativeOpPanelOpen(panel)) }
    fun npcInteract(): String =
        LogFile.op(LogDomain.API, "POST /api/ui/start_interact", emptyMap<String, String>()) { NativeBridge.nativeOpNpcInteract() }
    fun dialogSelect(action: String, index: Int): String =
        LogFile.op(LogDomain.API, "POST /api/ui/dialog/select", mapOf("action" to "$action", "index" to "$index")) {
            val activityCheck = UiActivityTracker.check()
            if (activityCheck.blockingActivityName != null) {
                // agreement 域（Java 同意页，仅主菜单）：select 不 fail-closed——检测失败时落回
                // native（其自带 in_world 守卫），避免 Java 反射故障拖垮 world 内全部 select。
                if (action == "ok") {
                    val activity = UiActivityTracker.agreementActivity()
                    if (activity == null || !AgreementPopup.dismiss(activity)) {
                        JsonUtil.err("agreement window unavailable")
                    } else {
                        JSONObject().put("ok", true).put("result", "tap_dispatched").toString()
                    }
                } else {
                    JsonUtil.err("no such option in agreement")
                }
            } else {
                NativeBridge.nativeOpDialogSelect(action, index)
            }
        }
}
