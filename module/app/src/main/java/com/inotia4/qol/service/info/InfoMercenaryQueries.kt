package com.inotia4.qol.service.info

import com.inotia4.qol.NativeBridge
import com.inotia4.qol.util.ApiException
import com.inotia4.qol.util.JsonUtil
import com.yanzhenjie.andserver.http.StatusCode
import org.json.JSONArray

/** Mercenary query endpoints. */
internal object InfoMercenaryQueries {
    fun all(): String = pass(NativeBridge.nativeGetMercenariesJson())

    fun slots(): String {
        val json = NativeBridge.nativeGetMercenariesJson()
        if (error(json)) return json
        val arr = JsonUtil.parseArr(json) ?: return JsonUtil.wrap("slots", JSONArray())
        val slots = JSONArray()
        for (i in 0 until arr.length()) {
            arr.optJSONObject(i)?.optInt("slot", -1)?.takeIf { it >= 0 }?.let(slots::put)
        }
        return JsonUtil.wrap("slots", slots)
    }

    fun slot(slot: Int): String {
        val json = NativeBridge.nativeGetMercenariesJson()
        if (error(json)) return json
        val arr = JsonUtil.parseArr(json) ?: notFound()
        for (i in 0 until arr.length()) {
            val mercenary = arr.optJSONObject(i) ?: continue
            if (mercenary.optInt("slot", -1) == slot) return mercenary.toString()
        }
        return notFound()
    }

    private fun pass(json: String): String = json
    private fun error(json: String): Boolean = JsonUtil.parseObj(json)?.has("error") == true
    private fun notFound(): Nothing = throw ApiException(StatusCode.SC_NOT_FOUND, "not found")
}
