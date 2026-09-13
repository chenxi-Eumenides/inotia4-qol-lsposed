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

internal object PartyActions {
    fun includeParty(mercenarySlot: Int): String =
        LogFile.op(LogDomain.API, "POST /api/character/party/include", mapOf("mercSlot" to "$mercenarySlot")) { ActionSupport.attachParty(NativeBridge.nativeOpIncludeParty(mercenarySlot)) }
    fun excludeParty(mercenarySlot: Int): String =
        LogFile.op(LogDomain.API, "POST /api/character/party/exclude", mapOf("mercSlot" to "$mercenarySlot")) { ActionSupport.attachParty(NativeBridge.nativeOpExcludeParty(mercenarySlot)) }
    fun discharge(mercenarySlot: Int): String =
        LogFile.op(LogDomain.API, "POST /api/character/party/discharge", mapOf("mercSlot" to "$mercenarySlot")) { ActionSupport.attachParty(NativeBridge.nativeOpDischarge(mercenarySlot)) }
    fun withdraw(mercenarySlot: Int, equipSlot: Int): String =
        LogFile.op(LogDomain.API, "POST /api/character/party/withdraw", mapOf("mercSlot" to "$mercenarySlot", "equipSlot" to "$equipSlot")) {
            ActionSupport.attachParty(NativeBridge.nativeOpWithdraw(mercenarySlot, equipSlot))
        }
}
