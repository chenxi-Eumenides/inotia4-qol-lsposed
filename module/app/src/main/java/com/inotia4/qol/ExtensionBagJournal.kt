package com.inotia4.qol

import com.inotia4.qol.store.ModuleSaveStore
import org.json.JSONObject

/**
 * Sidecar prepare journal section (ADR-007 v1, `extensionbags.journal`).
 *
 * The journal lives in its own section, separate from the committed state section
 * (`extensionbags.items`), so a crash between the original save and the sidecar commit can be
 * resolved on next load: stage × committed state × original-world probe decide replay/rollback.
 * Record format is produced/consumed by native `journal_json`/`parse_journal_json`
 * (virtual_bag_state.h); this object only handles section storage and id generation.
 */
object ExtensionBagJournal {

    private const val SECTION_VERSION = 1

    /** Unique per-transaction id, e.g. "j-1724800000-42". Generated here, validated natively. */
    private val counter = java.util.concurrent.atomic.AtomicLong(0)

    fun sectionVersion(): Int = SECTION_VERSION

    fun nextTransactionId(): String = "j-${System.currentTimeMillis()}-${counter.incrementAndGet()}"

    fun read(slot: Int): Pair<Int, ByteArray>? {
        return try {
            ModuleSaveStore.readSection(slot, JOURNAL_SECTION_NAME)?.let { it.version to it.payload }
        } catch (t: Throwable) {
            LogFile.error(LogDomain.EXTENSION_BAG, "extension bag journal read failed", t)
            null
        }
    }

    fun write(slot: Int, journalJson: String): Boolean {
        return try {
            ModuleSaveStore.writeSection(
                slot,
                JOURNAL_SECTION_NAME,
                SECTION_VERSION,
                journalJson.toByteArray(Charsets.UTF_8),
            )
        } catch (t: Throwable) {
            LogFile.error(LogDomain.EXTENSION_BAG, "extension bag journal write failed", t)
            false
        }
    }

    fun clear(slot: Int): Boolean {
        return try {
            ModuleSaveStore.removeSection(slot, JOURNAL_SECTION_NAME)
        } catch (t: Throwable) {
            LogFile.error(LogDomain.EXTENSION_BAG, "extension bag journal clear failed", t)
            false
        }
    }

    fun parsePayload(payload: ByteArray): JSONObject? {
        return try {
            val json = JSONObject(payload.toString(Charsets.UTF_8))
            val id = json.optString("transactionId", "")
            if (id.isEmpty() || id.length > 64) null else json
        } catch (t: Throwable) {
            LogFile.error(LogDomain.EXTENSION_BAG, "extension bag journal parse failed", t)
            null
        }
    }

    private const val JOURNAL_SECTION_NAME = "extensionbags.journal"
}
