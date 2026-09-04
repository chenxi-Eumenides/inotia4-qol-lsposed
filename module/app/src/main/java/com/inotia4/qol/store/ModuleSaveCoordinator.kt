package com.inotia4.qol.store

import android.util.Base64
import org.json.JSONObject
import java.util.LinkedHashMap

/**
 * Coordinates module sections around a confirmed original-game save.
 *
 * The coordinator owns save ordering only. It does not interpret a feature payload or decide
 * whether a feature transaction should be replayed or rolled back.
 */
object ModuleSaveCoordinator {
    private const val JOURNAL_SECTION = "module.save.journal"
    private const val JOURNAL_VERSION = 1

    data class PreparedSection(
        val name: String,
        val version: Int,
        val payload: ByteArray,
    )

    fun prepare(slot: Int, transactionId: String, sections: List<PreparedSection>): Boolean {
        require(transactionId.isNotEmpty()) { "transaction id must not be empty" }
        require(sections.isNotEmpty()) { "at least one section is required" }
        val journal = JSONObject()
            .put("transactionId", transactionId)
            .put("slot", slot)
            .put("stage", 0)
        val entries = org.json.JSONArray()
        sections.forEach { section ->
            entries.put(
                JSONObject()
                    .put("name", section.name)
                    .put("version", section.version)
                    .put("payload", Base64.encodeToString(section.payload, Base64.NO_WRAP)),
            )
        }
        journal.put("sections", entries)
        return ModuleSaveStore.writeSection(
            slot,
            JOURNAL_SECTION,
            JOURNAL_VERSION,
            journal.toString().toByteArray(Charsets.UTF_8),
        )
    }

    fun commitAfterOriginalSave(
        slot: Int,
        transactionId: String,
        sections: List<PreparedSection>,
    ): Boolean {
        require(transactionId.isNotEmpty()) { "transaction id must not be empty" }
        require(sections.isNotEmpty()) { "at least one section is required" }
        val journal = ModuleSaveStore.readSection(slot, JOURNAL_SECTION) ?: return false
        if (journal.version != JOURNAL_VERSION) return false
        val json = try {
            JSONObject(journal.payload.toString(Charsets.UTF_8))
        } catch (_: Exception) {
            return false
        }
        if (json.optString("transactionId") != transactionId || json.optInt("slot", -1) != slot ||
            json.optInt("stage", -1) != 0) {
            return false
        }
        val preparedSections = json.optJSONArray("sections") ?: return false
        if (preparedSections.length() != sections.size) return false
        val preparedNames = HashSet<String>()
        for (index in 0 until preparedSections.length()) {
            val entry = preparedSections.optJSONObject(index) ?: return false
            val name = entry.optString("name")
            val version = entry.optInt("version", 0)
            if (name.isEmpty() || version <= 0 || !preparedNames.add(name)) return false
            val current = sections.firstOrNull { it.name == name } ?: return false
            if (current.version != version) return false
        }
        val replacements = LinkedHashMap<String, ModuleSaveStore.Section>()
        sections.forEach { section ->
            if (replacements.put(
                    section.name,
                    ModuleSaveStore.Section(section.version, section.payload.copyOf()),
                ) != null
            ) return false
        }
        return replacements.isNotEmpty() && ModuleSaveStore.replaceSections(
            slot,
            replacements,
            setOf(JOURNAL_SECTION),
        )
    }

    fun abortKnownFailedSave(slot: Int, transactionId: String): Boolean {
        val journal = ModuleSaveStore.readSection(slot, JOURNAL_SECTION) ?: return true
        return try {
            if (JSONObject(journal.payload.toString(Charsets.UTF_8)).optString("transactionId") != transactionId) {
                false
            } else {
                ModuleSaveStore.removeSection(slot, JOURNAL_SECTION)
            }
        } catch (_: Exception) {
            false
        }
    }
}
