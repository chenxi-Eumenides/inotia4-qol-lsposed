package com.inotia4.qol.service.info

import com.inotia4.qol.NativeBridge
import com.inotia4.qol.util.JsonUtil

/** Thin system/debug query endpoints kept out of the aggregate information service. */
internal object InfoSystemQueries {
    // system 快照查询已迁 owner（system_facts）：Kotlin 只做 nativeHttpDispatch 透传，
    // 成功响应（含快照重排/名称注入/info 拼装）完全由 native owner 生成。
    fun snapshot(): String = NativeHttpQuery.get("/api/system/snapshot")
    fun game(): String = NativeHttpQuery.get("/api/system/game")
    fun gameInfo(): String = NativeHttpQuery.get("/api/system/info")
    fun gameFrame(): String = NativeHttpQuery.get("/api/system/game_frame")

    fun debugUi(): String = NativeBridge.nativeGetDebugUiJson()
    fun debugPath(tx: Int, ty: Int): String = NativeBridge.nativeDebugPathJson(tx, ty)
    fun debugItemRaw(bag: Int, slot: Int): String = NativeBridge.nativeDebugItemRawJson(bag, slot)
    fun settingsUiStatus(): String = NativeBridge.nativeSettingsUiStatus()
    fun extensionBagStatusJson(): String = NativeBridge.nativeExtensionBagStatusJson()
    fun health(): String = JsonUtil.wrap(
        "ok" to true,
        "ready" to NativeBridge.ready,
        "variant" to JsonUtil.parseObj(NativeBridge.nativeGetVariantJson())
    )
}
