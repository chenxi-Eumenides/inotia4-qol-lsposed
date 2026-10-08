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
     * 规范化字节 → 规范化字节：解析并校验（无效 payload 隔离告警保留审计痕迹），
     * 输出与 C++ codec 相同的规范化 JSON 文本字节。解析失败返回 null。
     */
    @JvmStatic
    fun normalizeStateBytes(bytes: ByteArray): ByteArray? {
        return parseStateBytes(bytes, rejectPayloadlessItems = false)?.toString()?.toByteArray(Charsets.UTF_8)
    }

    /**
     * 读面规范化：读档侧拒绝无 payload 的非空物品（与迁移前 loadStateJson 的
     * rejectPayloadlessItems 语义一致），供等价性测试对照 C++ load 行为。
     */
    @JvmStatic
    fun normalizeLoadedStateBytes(bytes: ByteArray): ByteArray? {
        return parseStateBytes(bytes, rejectPayloadlessItems = true)?.toString()?.toByteArray(Charsets.UTF_8)
    }

    private fun parseStateBytes(bytes: ByteArray, rejectPayloadlessItems: Boolean): JSONObject? {
        return parseState(String(bytes, Charsets.UTF_8), rejectPayloadlessItems)
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

    private fun parseState(raw: String, rejectPayloadlessItems: Boolean): JSONObject? {
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
                    //（受限原始编码入日志；落盘时机归 core.save 保存事务）。
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
