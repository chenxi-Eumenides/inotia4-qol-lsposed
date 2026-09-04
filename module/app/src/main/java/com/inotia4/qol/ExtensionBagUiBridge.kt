package com.inotia4.qol

import android.content.Context
import android.content.pm.PackageManager
import com.inotia4.qol.store.ModuleSaveStore
import com.inotia4.qol.store.ModuleSaveCoordinator
import org.json.JSONArray
import org.json.JSONObject
import java.security.MessageDigest

/**
 * Module-owned extension backpack data. It persists stable item descriptors plus lossless native
 * serialization payloads (SAVE_SaveItem records, base64-encoded). It never persists native item
 * pointers or original inventory records.
 */
object ExtensionBagUiBridge {

    private const val SECTION_NAME = "extensionbags.items"
    private const val LEGACY_SECTION_NAME = "virtualbags.items"
    private const val SECTION_VERSION = 4
    private const val BAG_COUNT = 5
    private const val SLOT_COUNT = 16
    private const val MAX_PAYLOAD_CHARS = 512
    private const val IDENTITY_KEY = "gameIdentity"

    private val payloadPattern = Regex("^[A-Za-z0-9+/]*={0,2}$")

    @Volatile
    private var appContext: Context? = null

    /** Idempotent; ApiServer restart may call it again. */
    @JvmStatic
    fun initialize(context: Context) {
        appContext = context.applicationContext
    }

    /**
     * Host-game identity for sidecar deserialization gating (control-plane §9 P1): signature
     * digest prefix changes only when the APK binary identity changes; same-signature game
     * updates keep sidecar data readable.
     */
    private fun gameIdentity(): String? {
        val context = appContext ?: return null
        return try {
            val info = context.packageManager.getPackageInfo(
                context.packageName,
                PackageManager.GET_SIGNING_CERTIFICATES,
            )
            val signatures = info.signingInfo?.apkContentsSigners ?: return null
            val signature = signatures.firstOrNull() ?: return null
            val digest = MessageDigest.getInstance("SHA-256").digest(signature.toByteArray())
            digest.take(8).joinToString("") { "%02x".format(it) }
        } catch (t: Throwable) {
            LogFile.logError("extension bag identity unavailable", t)
            null
        }
    }

    @JvmStatic
    fun loadStateJson(slot: Int): String {
        val section = try {
            ModuleSaveStore.readSection(slot, SECTION_NAME)
                ?: ModuleSaveStore.readSection(slot, LEGACY_SECTION_NAME)
        } catch (t: Throwable) {
            LogFile.logError("extension bag UI state load failed", t)
            return defaultStateJson()
        } ?: return defaultStateJson()
        val raw = section.payload.toString(Charsets.UTF_8)
        val payload = when (section.version) {
            2 -> migrateLegacyState(raw)
            3 -> raw // v3 旧描述符：仅 category/count，无序列化载荷，解析后按 v4 重写
            SECTION_VERSION -> raw
            else -> return defaultStateJson()
        }
        if (!identityAcceptable(payload)) {
            LogFile.log("extension bag slot=$slot rejected: incompatible game identity")
            return defaultStateJson()
        }
        val parsed = payload?.let {
            parseState(it, rejectPayloadlessItems = section.version == SECTION_VERSION)
        }
        if (section.version != SECTION_VERSION && parsed != null) {
            try {
                ModuleSaveStore.writeSection(
                    slot,
                    SECTION_NAME,
                    SECTION_VERSION,
                    parsed.toString().toByteArray(Charsets.UTF_8),
                )
            } catch (t: Throwable) {
                LogFile.logError("extension bag UI state migration failed", t)
            }
        }
        return parsed?.toString() ?: defaultStateJson()
    }

    private fun identityAcceptable(payload: String?): Boolean {
        if (payload == null) return true
        val stored = try {
            JSONObject(payload).optString(IDENTITY_KEY, "")
        } catch (t: Throwable) {
            LogFile.logError("extension bag state unreadable", t)
            return false
        }
        if (stored.isEmpty()) return true // 旧格式或 unknown 身份：按可读处理
        val current = gameIdentity() ?: return true // 身份不可得时不阻断（host/诊断期）
        return stored == current
    }

    @JvmStatic
    fun saveStateJson(slot: Int, stateJson: String): String {
        val normalized = parseState(stateJson) ?: return "error:invalid_state"
        gameIdentity()?.let { normalized.put(IDENTITY_KEY, it) }
        return try {
            val saved = ModuleSaveStore.writeSection(
                slot,
                SECTION_NAME,
                SECTION_VERSION,
                normalized.toString().toByteArray(Charsets.UTF_8),
            )
            if (saved) "ok" else "error:storage"
        } catch (t: Throwable) {
            LogFile.logError("extension bag UI state save failed", t)
            "error:storage"
        }
    }

    /** Prepare the extension section; commit is allowed only after complete SAVE_Save success. */
    @JvmStatic
    fun prepareSave(slot: Int, transactionId: String, stateJson: String): String {
        val normalized = parseState(stateJson) ?: return "error:invalid_state"
        gameIdentity()?.let { normalized.put(IDENTITY_KEY, it) }
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
        gameIdentity()?.let { normalized.put(IDENTITY_KEY, it) }
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
                        LogFile.log("extension bag isolated reason=invalid_payload " +
                            "bag=${bag + 6} slot=$slot payloadLength=${payload.length} " +
                            "payloadPreview=$preview")
                    }
                    if (rejectPayloadlessItems && category > 0 && count > 0 && !hasPayload) {
                        LogFile.log("extension bag isolated reason=payloadless_rejected " +
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
            LogFile.logError("extension bag UI state parse failed", t)
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

    private fun migrateLegacyState(raw: String): String? {
        return try {
            val source = JSONObject(raw)
            val capacities = source.getJSONArray("capacities")
            if (capacities.length() != BAG_COUNT) return null
            val types = JSONArray()
            for (index in 0 until BAG_COUNT) {
                val capacity = capacities.getInt(index)
                if (capacity !in 0..16) return null
                val type = when {
                    capacity == 0 -> 0
                    capacity <= 4 -> 1
                    capacity <= 8 -> 2
                    capacity <= 12 -> 3
                    else -> 4
                }
                types.put(type)
            }
            source.put("types", types)
            source.remove("capacities")
            source.toString()
        } catch (t: Throwable) {
            LogFile.logError("extension bag UI legacy state migration parse failed", t)
            null
        }
    }
}
