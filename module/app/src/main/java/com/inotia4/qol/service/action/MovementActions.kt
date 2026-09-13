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

internal object MovementActions {
    fun move(x: Int, y: Int): String =
        LogFile.op(LogDomain.API, "POST /api/world/movement/move_to", mapOf("x" to "$x", "y" to "$y")) { ActionSupport.attachPlayer(NativeBridge.nativeOpMove(x, y)) }
    fun walk(direction: Int): String =
        LogFile.op(LogDomain.API, "POST /api/world/movement/walk_dir", mapOf("dir" to "$direction")) { ActionSupport.attachPlayer(NativeBridge.nativeOpWalk(direction)) }
    fun walkStop(): String =
        LogFile.op(LogDomain.API, "POST /api/world/movement/stop_move", emptyMap<String, String>()) { NativeBridge.nativeOpWalkStop() }
    fun interact(): String =
        LogFile.op(LogDomain.API, "POST /api/world/movement/interact_with", emptyMap<String, String>()) { ActionSupport.attachPlayer(NativeBridge.nativeOpInteract()) }
}
