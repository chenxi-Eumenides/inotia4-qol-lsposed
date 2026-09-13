#pragma once

#include <jni.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "feature/autosell/autosell_rules.h"

// 自动出售按存档配置持久化（sidecar section `autosell` v1，UTF-8 JSON，直写）。
//
// 隔离边界：只读写模块 sidecar 的 `autosell` section，不进 ModuleSaveCoordinator
// 的 participant/journal，不触碰 `extensionbags.items` / `extensionbags.journal`，
// 不持 g_virtual_bag_mtx。
//
// 纯序列化函数为 inline（可 host 测，零 Android/游戏依赖）；JNI 桥与直写在 .cpp。

namespace autosell_store_detail {

// 定位顶层 `"key"` 后冒号后的值起始指针（跳过空白）；未找到返回 nullptr。
inline const char* find_field(const char* json, const char* key) {
    if (json == nullptr || key == nullptr) return nullptr;
    const std::string token = std::string("\"") + key + "\"";
    const char* p = std::strstr(json, token.c_str());
    if (p == nullptr) return nullptr;
    const char* colon = std::strchr(p + token.size(), ':');
    if (colon == nullptr) return nullptr;
    ++colon;
    while (*colon == ' ' || *colon == '\t' || *colon == '\n' || *colon == '\r') ++colon;
    return colon;
}

inline bool parse_bool(const char* json, const char* key, bool fallback) {
    const char* v = find_field(json, key);
    if (v == nullptr) return fallback;
    return *v == 't' || *v == '1';
}

inline int parse_int(const char* json, const char* key, int fallback) {
    const char* v = find_field(json, key);
    if (v == nullptr) return fallback;
    char* end = nullptr;
    const long n = std::strtol(v, &end, 10);
    if (end == v) return fallback;
    return static_cast<int>(n);
}

inline int clamp_int(int value, int lo, int hi) {
    if (value < lo) return lo;
    if (value > hi) return hi;
    return value;
}

}  // namespace autosell_store_detail

// Config -> section v1 JSON（specialMask 为 uint32 位掩码，与 autosell_rules.h 位一致）。
inline std::string autosell_config_to_json(const autosell::Config& config) {
    char buf[512];
    std::snprintf(buf, sizeof(buf),
                  "{\"v\":1,\"enabled\":%s,\"rarityEnabled\":%s,\"rarityThreshold\":%d,"
                  "\"enhanceEnabled\":%s,\"enhanceThreshold\":%d,"
                  "\"socketEnabled\":%s,\"socketThreshold\":%d,"
                  "\"gemTierEnabled\":%s,\"gemTierThreshold\":%d,"
                  "\"specialMask\":%u}",
                  config.enabled ? "true" : "false",
                  config.rarity_enabled ? "true" : "false", config.rarity_threshold,
                  config.enhance_enabled ? "true" : "false", config.enhance_threshold,
                  config.socket_enabled ? "true" : "false", config.socket_threshold,
                  config.gem_tier_enabled ? "true" : "false", config.gem_tier_threshold,
                  static_cast<unsigned>(config.special_mask));
    return std::string(buf);
}

// section JSON -> Config。缺失字段取默认并在边界内钳制；未知键忽略。
// v 缺失或 <=1 按 v1；v>1（未知未来版本）/坏 JSON 返回 false 且 out=默认值。
inline bool autosell_config_from_json(const char* json, autosell::Config* out) {
    if (out == nullptr) return false;
    *out = autosell::Config{};
    if (json == nullptr || std::strchr(json, '{') == nullptr) return false;

    using namespace autosell_store_detail;
    if (parse_int(json, "v", 1) > 1) return false;  // 未知未来版本：回退默认

    autosell::Config cfg;
    cfg.enabled = parse_bool(json, "enabled", false);
    cfg.rarity_enabled = parse_bool(json, "rarityEnabled", false);
    cfg.rarity_threshold = clamp_int(parse_int(json, "rarityThreshold", 0), 0, 4);
    cfg.enhance_enabled = parse_bool(json, "enhanceEnabled", false);
    const int enhance = parse_int(json, "enhanceThreshold", 0);
    cfg.enhance_threshold = enhance < 0 ? 0 : enhance;
    cfg.socket_enabled = parse_bool(json, "socketEnabled", false);
    cfg.socket_threshold = clamp_int(parse_int(json, "socketThreshold", 0), 0, 15);
    cfg.gem_tier_enabled = parse_bool(json, "gemTierEnabled", false);
    cfg.gem_tier_threshold = clamp_int(parse_int(json, "gemTierThreshold", 0), 0, 4);
    const int mask = parse_int(json, "specialMask", 0);
    cfg.special_mask = static_cast<uint32_t>(mask < 0 ? 0 : mask);
    cfg.special_enabled = cfg.special_mask != 0u;

    *out = cfg;
    return true;
}

// 注册 Kotlin AutoSellConfigStore（jclass + load/save 静态方法 id）。
void autosell_store_register_bridge(JNIEnv* env, jclass bridge_class);

// 【预留·当前未接线】进档时经桥读取 `autosell` section 并应用到运行时配置；非法 slot 不读写。
// 进档自动注册任务的时机待定（见 autosell_scan.h 说明）；接线时应改走 autosell_apply_config，
// 以便按 enabled 注册 / 删除扫描任务。
void autosell_store_ensure_loaded(int slot);

// 序列化并写回 `autosell` section；slot 非法或桥失败返回 false。
bool autosell_store_persist(int slot, const autosell::Config& config);
