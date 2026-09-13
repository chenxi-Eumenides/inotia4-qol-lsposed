package com.inotia4.qol

/**
 * 统一日志域 token（P1）。
 *
 * token 文本是 Kotlin ↔ native 的唯一契约，与 `core/native/qol_log.h` 的
 * `QolDomain` 词表逐项对应（小写）。新增域必须两侧同步。
 */
enum class LogDomain(val token: String) {
    PLATFORM("platform"),
    CORE("core"),
    HTTP("http"),
    API("api"),
    OP("op"),
    INVENTORY("inventory"),
    EXTENSION_BAG("extension_bag"),
    SAVE("save"),
    SAVE_BACKUP("save_backup"),
    AUTOSELL("autosell"),
    GEM_CRAFT("gem_craft"),
    ATTR_RANGE("attr_range"),
    UI("ui"),
    CONFIG("config"),
    CATALOG("catalog");
}
