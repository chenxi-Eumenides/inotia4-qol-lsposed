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

internal object ActionSupport {
fun attachPlayer(op: String): String =
        attach(op) { NativeBridge.nativeGetPlayerJson() }

    fun attachParty(op: String): String =
        attach(op) { NativeBridge.nativeGetPartyJson() }

    fun attachInventory(op: String): String =
        attach(op) { NativeBridge.nativeGetInventoryJson() }

    fun attachSkills(op: String): String =
        attach(op) { NativeBridge.nativeGetSkillsJson() }

    fun attachUi(op: String): String =
        attach(op) { NativeBridge.nativeGetGamestateJson() }

    fun currentSaveSlot(): Int = try {
        JSONObject(NativeBridge.nativeCurrentSaveSlot()).optInt("current_save_slot", -1)
    } catch (e: Exception) {
        -1
    }

    fun afterSave(op: String): String = afterNativeSuccess(op) {
        val current = currentSaveSlot()
        if (current in 0..2) ModuleSaveStore.ensureSlot(current)
    }

    fun afterNativeSuccess(op: String, action: () -> Unit): String {
        val succeeded = try {
            JSONObject(op).optBoolean("ok", false)
        } catch (e: Exception) {
            false
        }
        if (succeeded) action()
        return op
    }

    fun findItemSlot(category: Int): Pair<Int, Int>? {
        val inv = try {
            JSONObject(NativeBridge.nativeGetInventoryJson())
        } catch (e: Exception) {
            return null
        }
        val bags = inv.optJSONArray("bags") ?: return null
        for (b in 0 until bags.length()) {
            val bag = bags.getJSONObject(b)
            val items = bag.optJSONArray("items") ?: continue
            for (i in 0 until items.length()) {
                val item = items.getJSONObject(i)
                if (item.optInt("category", -1) == category) return b to item.optInt("slot", -1)
            }
        }
        return null
    }

    fun attach(op: String, latest: () -> String): String {
        return try {
            val obj = JSONObject(op)
            if (obj.optBoolean("ok", false)) obj.put("state", JSONObject(latest()))
            obj.toString()
        } catch (e: Exception) {
            op
        }
    }
}
