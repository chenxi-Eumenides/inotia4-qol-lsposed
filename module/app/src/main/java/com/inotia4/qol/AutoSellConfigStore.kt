package com.inotia4.qol

import com.inotia4.qol.store.ModuleSaveStore

/**
 * 自动出售按存档配置的 sidecar 桥（section `autosell` v1，UTF-8 JSON，直写）。
 *
 * 与扩展背包 section/journal 完全隔离：只经 [ModuleSaveStore.readSection]/[writeSection]
 * 读写本功能 section，不注册 ModuleSaveCoordinator participant，不触碰
 * `extensionbags.items` / `extensionbags.journal`。
 *
 * native 只做序列化与规则应用；section 容器格式、原子写与损坏隔离由 ModuleSaveStore 负责。
 */
object AutoSellConfigStore {

    private const val SECTION_NAME = "autosell"
    private const val SECTION_VERSION = 1

    /** 与 native autosell_store.h 默认值一致的 v1 配置（值即开关：0=关闭）。 */
    private const val DEFAULT_JSON =
        "{\"v\":1,\"enabled\":false,\"rarity\":0,\"enhance\":0," +
            "\"socket\":0,\"gemTier\":0,\"gemRange\":0,\"specialMask\":0}"

    /** 读取该存档槽的自动出售配置；缺省返回默认 JSON，存储未就绪/异常返回 "error:..."。 */
    @JvmStatic
    fun loadConfigJson(slot: Int): String {
        if (!ModuleSaveStore.isInitialized()) return "error:not_initialized"
        return try {
            val section = ModuleSaveStore.readSection(slot, SECTION_NAME)
                ?: return DEFAULT_JSON
            val raw = section.payload.toString(Charsets.UTF_8)
            if (raw.isBlank()) DEFAULT_JSON else raw
        } catch (t: Throwable) {
            LogFile.logError("autosell config load failed slot=$slot", t)
            "error:storage"
        }
    }

    /** 写入该存档槽的自动出售配置；成功返回 "ok"，失败返回 "error:..."。 */
    @JvmStatic
    fun saveConfigJson(slot: Int, json: String): String {
        return try {
            val saved = ModuleSaveStore.writeSection(
                slot,
                SECTION_NAME,
                SECTION_VERSION,
                json.toByteArray(Charsets.UTF_8),
            )
            if (saved) "ok" else "error:storage"
        } catch (t: Throwable) {
            LogFile.logError("autosell config save failed slot=$slot", t)
            "error:storage"
        }
    }
}
