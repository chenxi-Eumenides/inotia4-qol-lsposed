package com.inotia4.qol

import android.content.Context
import com.inotia4.qol.store.ModuleSaveStore
import com.inotia4.qol.store.ModuleSaveCoordinator
import org.json.JSONArray
import org.json.JSONObject

/**
 * Module-owned extension backpack data. It persists stable item descriptors plus lossless native
 * serialization payloads (SAVE_SaveItem records, base64-encoded). It never persists native item
 * pointers or original inventory records.
 */
object ExtensionBagUiBridge {

    private const val SECTION_NAME = "extensionbags.items"
    private const val SECTION_VERSION = 4
    private const val BAG_COUNT = 5
    private const val SLOT_COUNT = 16
    private const val MAX_PAYLOAD_CHARS = 512

    private val payloadPattern = Regex("^[A-Za-z0-9+/]*={0,2}$")

    /** Idempotent; ApiServer restart may call it again. 无自有状态，保留入口稳定调用方。 */
    @JvmStatic
    @Suppress("UNUSED_PARAMETER")
    fun initialize(context: Context) {
        // sidecar IO 与格式校验统一经 ModuleSaveStore；本桥不再持有上下文或游戏签名身份。
    }

    @JvmStatic
    fun loadStateJson(slot: Int): String {
        val section = try {
            ModuleSaveStore.readSection(slot, SECTION_NAME)
        } catch (t: Throwable) {
            LogFile.error(LogDomain.EXTENSION_BAG, "extension bag UI state load failed", t)
            return defaultStateJson()
        } ?: return defaultStateJson()
        val raw = section.payload.toString(Charsets.UTF_8)
        // v4-only：读取只认当前 section 名与 v4 布局；未知版本也按当前布局尽力解析，
        // 解析失败（parseState 返回 null）才回退空状态；不做旧版本迁移/回写，不使用身份门禁。
        val parsed = parseState(raw, rejectPayloadlessItems = true)
        return parsed?.toString() ?: defaultStateJson()
    }

    @JvmStatic
    fun saveStateJson(slot: Int, stateJson: String): String {
        val normalized = parseState(stateJson) ?: return "error:invalid_state"
        return try {
            val saved = ModuleSaveStore.writeSection(
                slot,
                SECTION_NAME,
                SECTION_VERSION,
                normalized.toString().toByteArray(Charsets.UTF_8),
            )
            if (saved) "ok" else "error:storage"
        } catch (t: Throwable) {
            LogFile.error(LogDomain.EXTENSION_BAG, "extension bag UI state save failed", t)
            "error:storage"
        }
    }

    /** Prepare the extension section; commit is allowed only after complete SAVE_Save success. */
    @JvmStatic
    fun prepareSave(slot: Int, transactionId: String, stateJson: String): String {
        val normalized = parseState(stateJson) ?: return "error:invalid_state"
        val prepared = ModuleSaveCoordinator.prepare(
            slot,
            transactionId,
            listOf(
                ModuleSaveCoordinator.PreparedSection(
                    SECTION_NAME,
                    SECTION_VERSION,
                    normalized.toString().toByteArray(Charsets.UTF_8),
                ),
            ),
        )
        return if (prepared) "ok" else "error:storage"
    }

    @JvmStatic
    fun commitSave(slot: Int, transactionId: String, stateJson: String): String {
        val normalized = parseState(stateJson) ?: return "error:invalid_state"
        val committed = ModuleSaveCoordinator.commitAfterOriginalSave(
            slot,
            transactionId,
            listOf(
                ModuleSaveCoordinator.PreparedSection(
                    SECTION_NAME,
                    SECTION_VERSION,
                    normalized.toString().toByteArray(Charsets.UTF_8),
                ),
            ),
        )
        return if (committed) "ok" else "error:storage"
    }

    @JvmStatic
    fun abortKnownFailedSave(slot: Int, transactionId: String): String {
        return if (ModuleSaveCoordinator.abortKnownFailedSave(slot, transactionId)) "ok" else "error:storage"
    }

    private fun defaultStateJson(): String = stateJson(
        mode = "original",
        originalSelected = 0,
        types = IntArray(BAG_COUNT),
        selected = -1,
        inspected = -1,
        items = emptyItems(),
        pending = null,
    ).toString()

    private fun parseState(raw: String, rejectPayloadlessItems: Boolean = false): JSONObject? {
        return try {
            val source = JSONObject(raw)
            val sourceTypes = source.getJSONArray("types")
            if (sourceTypes.length() != BAG_COUNT) return null
            val types = IntArray(BAG_COUNT) { index -> sourceTypes.getInt(index) }
            if (types.any { it !in 0..4 }) return null
            val mode = source.optString("mode", "original")
            if (mode != "original" && mode != "module") return null
            val originalSelected = source.optInt("originalSelected", 0)
            if (originalSelected !in 0..5) return null
            var selected = source.optInt("selected", -1)
            if (selected !in types.indices) selected = -1
            var normalizedMode = mode
            if (normalizedMode == "module" && selected < 0) {
                normalizedMode = "original"
                selected = -1
            }
            var inspected = source.optInt("inspected", -1)
            if (inspected != selected) inspected = -1
            val sourceItems = source.getJSONArray("items")
            if (sourceItems.length() != BAG_COUNT) return null
            val items = Array(BAG_COUNT) { bag ->
                val sourceBag = sourceItems.getJSONArray(bag)
                if (sourceBag.length() != SLOT_COUNT) throw IllegalArgumentException("invalid slot count")
                Array(SLOT_COUNT) { slot ->
                    val sourceItem = sourceBag.getJSONObject(slot)
                    val category = sourceItem.optInt("category", 0)
                    val count = sourceItem.optInt("count", 0)
                    if (category < 0 || count < 0) throw IllegalArgumentException("invalid item")
                    val hasPayload = sourceItem.has("payload")
                    val payload = sourceItem.optString("payload", "")
                    val safePayload = if (payload.length in 1..MAX_PAYLOAD_CHARS && payloadPattern.matches(payload)) {
                        payload
                    } else {
                        ""
                    }
                    // P4.5：无效 payload 不得静默转 ItemData(0,0)——隔离告警保留审计痕迹
                    //（受限原始编码入日志；section 写入保持净荷，落盘时机归 P7）。
                    if (hasPayload && safePayload.isEmpty()) {
                        val preview = payload.take(64).map {
                            if (it.isWhitespace() || it == '"') '?' else it
                        }.joinToString("")
                        LogFile.info(LogDomain.EXTENSION_BAG, "extension bag isolated reason=invalid_payload " +
                            "bag=${bag + 6} slot=$slot payloadLength=${payload.length} " +
                            "payloadPreview=$preview")
                    }
                    if (rejectPayloadlessItems && category > 0 && count > 0 && !hasPayload) {
                        LogFile.info(LogDomain.EXTENSION_BAG, "extension bag isolated reason=payloadless_rejected " +
                            "bag=${bag + 6} slot=$slot category=$category count=$count")
                    }
                    when {
                        hasPayload && safePayload.isEmpty() -> ItemData(0, 0)
                        rejectPayloadlessItems && category > 0 && count > 0 && !hasPayload -> ItemData(0, 0)
                        else -> ItemData(category, count, safePayload)
                    }
                }
            }
            val pending = if (source.has("pending")) source.optJSONObject("pending") else null
            stateJson(normalizedMode, originalSelected, types, selected, inspected, items, pending)
        } catch (t: Throwable) {
            LogFile.error(LogDomain.EXTENSION_BAG, "extension bag UI state parse failed", t)
            null
        }
    }

    private data class ItemData(val category: Int, val count: Int, val payload: String = "")

    private fun emptyItems(): Array<Array<ItemData>> = Array(BAG_COUNT) {
        Array(SLOT_COUNT) { ItemData(0, 0) }
    }

    private fun stateJson(
        mode: String,
        originalSelected: Int,
        types: IntArray,
        selected: Int,
        inspected: Int,
        items: Array<Array<ItemData>>,
        pending: JSONObject?,
    ): JSONObject {
        val itemJson = JSONArray()
        items.forEach { bag ->
            val bagJson = JSONArray()
            bag.forEach { item ->
                val itemObject = JSONObject()
                    .put("category", item.category)
                    .put("count", item.count)
                if (item.payload.isNotEmpty()) itemObject.put("payload", item.payload)
                bagJson.put(itemObject)
            }
            itemJson.put(bagJson)
        }
        val result = JSONObject()
            .put("mode", mode)
            .put("originalSelected", originalSelected)
            .put("types", JSONArray(types.toList()))
            .put("selected", selected)
            .put("inspected", inspected)
            .put("items", itemJson)
        if (pending != null) result.put("pending", pending)
        return result
    }

}
