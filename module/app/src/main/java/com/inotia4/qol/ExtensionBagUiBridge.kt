package com.inotia4.qol

import org.json.JSONArray
import org.json.JSONObject

/**
 * Extension backpack payload 规范化/校验纯函数（bytes ↔ EB 状态）。
 *
 * P3c：容器 IO 与保存协调已收口到 core.save 的 save.slot_data（C++ 侧 EB 内部 codec，
 * 见 plugins/extension_bag/extension_bag_persistence.cpp）；本桥不再持有 section 读写、
 * 事务 prepare/commit 或 slot 参数——只保留 payload 规范化/校验的纯函数语义
 *（无 IO、无槽位、无 ModuleSaveStore / ModuleSaveCoordinator 依赖），供既有调用面与
 * 等价性测试使用。
 */
object ExtensionBagUiBridge {

    private const val SECTION_NAME = "extensionbags.items"
    private const val BAG_COUNT = 5
    private const val SLOT_COUNT = 16
    private const val MAX_PAYLOAD_CHARS = 512

    private val payloadPattern = Regex("^[A-Za-z0-9+/]*={0,2}$")

    /** 幂等；ApiServer 重启可重复调用。无自有状态，保留入口稳定调用方。 */
    @JvmStatic
    @Suppress("UNUSED_PARAMETER")
    fun initialize(context: android.content.Context) {
        // sidecar IO 已随 P3c 收口到 save.slot_data；本桥仅存留纯函数。
    }

    /** section 名/版本常量：与 C++ 侧 EB codec（extensionbags.items v4）保持同源。 */
    val sectionName: String get() = SECTION_NAME

    /**
     * 规范化字节 → 规范化字节：解析并校验，输出与 C++ codec 相同的规范化 JSON 文本字节。
     * 解析失败返回 null。本函数是纯函数：不写日志、不做 IO；诊断信息经
     * [normalizeStateBytesDetailed] 返回给调用层记录。
     */
    @JvmStatic
    fun normalizeStateBytes(bytes: ByteArray): ByteArray? {
        return parseState(String(bytes, Charsets.UTF_8), rejectPayloadlessItems = false).json
            ?.toString()?.toByteArray(Charsets.UTF_8)
    }

    /**
     * 读面规范化：读档侧拒绝无 payload 的非空物品（与迁移前 loadStateJson 的
     * rejectPayloadlessItems 语义一致），供等价性测试对照 C++ load 行为。
     */
    @JvmStatic
    fun normalizeLoadedStateBytes(bytes: ByteArray): ByteArray? {
        return parseState(String(bytes, Charsets.UTF_8), rejectPayloadlessItems = true).json
            ?.toString()?.toByteArray(Charsets.UTF_8)
    }

    /** 规范化结果：bytes 为规范化 JSON 字节（失败为 null）；diagnostics 供调用层记录。 */
    data class NormalizeResult(val bytes: ByteArray?, val diagnostics: List<String>)

    /**
     * 带诊断的规范化入口：解析函数返回诊断信息，由调用层决定如何记录；
     * codec 自身不调用 LogFile/native/logcat。
     */
    @JvmStatic
    fun normalizeStateBytesDetailed(bytes: ByteArray, rejectPayloadlessItems: Boolean): NormalizeResult {
        val parsed = parseState(String(bytes, Charsets.UTF_8), rejectPayloadlessItems)
        return NormalizeResult(parsed.json?.toString()?.toByteArray(Charsets.UTF_8), parsed.diagnostics)
    }

    private data class ParsedState(val json: JSONObject?, val diagnostics: List<String>)

    private fun parseState(raw: String, rejectPayloadlessItems: Boolean): ParsedState {
        val diagnostics = ArrayList<String>()
        return try {
            val source = JSONObject(raw)
            val sourceTypes = source.getJSONArray("types")
            if (sourceTypes.length() != BAG_COUNT) return ParsedState(null, diagnostics)
            val types = IntArray(BAG_COUNT) { index -> sourceTypes.getInt(index) }
            if (types.any { it !in 0..4 }) return ParsedState(null, diagnostics)
            val mode = source.optString("mode", "original")
            if (mode != "original" && mode != "module") return ParsedState(null, diagnostics)
            val originalSelected = source.optInt("originalSelected", 0)
            if (originalSelected !in 0..5) return ParsedState(null, diagnostics)
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
            if (sourceItems.length() != BAG_COUNT) return ParsedState(null, diagnostics)
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
                    // P4.5：无效 payload 不得静默转 ItemData(0,0)——诊断信息交调用层记录，
                    // codec 自身不写日志（受限编码预览）。
                    if (hasPayload && safePayload.isEmpty()) {
                        val preview = payload.take(64).map {
                            if (it.isWhitespace() || it == '"') '?' else it
                        }.joinToString("")
                        diagnostics.add("extension bag isolated reason=invalid_payload " +
                            "bag=${bag + 6} slot=$slot payloadLength=${payload.length} " +
                            "payloadPreview=$preview")
                    }
                    if (rejectPayloadlessItems && category > 0 && count > 0 && !hasPayload) {
                        diagnostics.add("extension bag isolated reason=payloadless_rejected " +
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
            ParsedState(stateJson(normalizedMode, originalSelected, types, selected, inspected, items, pending), diagnostics)
        } catch (t: Throwable) {
            diagnostics.add("extension bag UI state parse failed: ${t.javaClass.simpleName}: ${t.message}")
            ParsedState(null, diagnostics)
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
