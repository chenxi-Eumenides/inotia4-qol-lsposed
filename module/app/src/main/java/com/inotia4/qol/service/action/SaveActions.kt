package com.inotia4.qol.service.action

import com.inotia4.qol.LogDomain
import com.inotia4.qol.LogFile
import com.inotia4.qol.NativeBridge
import com.inotia4.qol.UiActivityTracker
import com.inotia4.qol.patch.AgreementGate
import com.inotia4.qol.service.info.NativeHttpCommand
import com.inotia4.qol.util.JsonUtil

/**
 * 存档写端点（save/enter_slot/create_slot）。
 *
 * 游戏写已迁 owner（save_backup）的**主线程命令**：HTTP 层只做平台门禁与参数透传，投递后立即
 * 返回票据；结果（原同步响应）经 `GET /api/system/tickets/{id}` 回取。模块 sidecar 的槽位
 * 生命周期操作随命令在主线程成功后执行（原 ModuleSaveStore 副作用已下沉 native）。
 */
internal object SaveActions {
    fun save(): String =
        LogFile.op(LogDomain.SAVE, "POST /api/system/save", emptyMap<String, String>()) {
            NativeHttpCommand.post("/api/system/save", "{}")
        }

    // 归 ui 域（/api/ui/go_main_menu），待 ui 批次一并迁移；本批保持原行为。
    fun mainMenu(): String =
        LogFile.op(LogDomain.SAVE, "POST /api/ui/go_main_menu", emptyMap<String, String>()) {
            ActionSupport.attachPlayer(NativeBridge.nativeOpMainMenu())
        }

    fun enterSlot(slot: Int): String =
        LogFile.op(LogDomain.SAVE, "POST /api/system/enter_slot", mapOf("slot" to "$slot")) {
            uiGate() ?: run {
                AgreementGate.beginWorldLoad()
                NativeHttpCommand.post("/api/system/enter_slot", "{\"slot\":$slot}")
            }
        }

    fun createSlot(slot: Int, classIdx: Int): String =
        LogFile.op(
            LogDomain.SAVE,
            "POST /api/system/create_slot",
            mapOf("slot" to "$slot", "classIdx" to "$classIdx"),
        ) {
            uiGate() ?: run {
                AgreementGate.beginWorldLoad()
                NativeHttpCommand.post(
                    "/api/system/create_slot",
                    "{\"slot\":$slot,\"class_idx\":$classIdx}",
                )
            }
        }

    /**
     * 平台门禁（JVM 侧，不能下沉 native）：ActivityThread 反射不可用或同意页遮挡时，与迁移前
     * 一致地返回同一错误体（HTTP 200 业务错误），不投递命令。
     */
    private fun uiGate(): String? {
        val activityCheck = UiActivityTracker.check()
        return when {
            activityCheck.failed -> JsonUtil.err("ui state unavailable")
            activityCheck.blockingActivityName != null -> JsonUtil.err("ui occupied: agreement")
            else -> null
        }
    }
}
