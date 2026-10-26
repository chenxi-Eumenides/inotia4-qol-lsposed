#pragma once

#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "rules.h"

namespace autosell_store_detail {

// 定位顶层/嵌套文本中的 "key" 后冒号后的值起始指针（跳过空白）；未找到返回 nullptr。
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
    const char* value = find_field(json, key);
    if (value == nullptr) return fallback;
    return *value == 't' || *value == '1';
}

inline int parse_int(const char* json, const char* key, int fallback) {
    const char* value = find_field(json, key);
    if (value == nullptr) return fallback;
    char* end = nullptr;
    const long parsed = std::strtol(value, &end, 10);
    if (end == value) return fallback;
    if (parsed < static_cast<long>(INT_MIN)) return INT_MIN;
    if (parsed > static_cast<long>(INT_MAX)) return INT_MAX;
    return static_cast<int>(parsed);
}

inline int clamp_int(int value, int lo, int hi) {
    if (value < lo) return lo;
    if (value > hi) return hi;
    return value;
}

inline bool is_space(char value) {
    return value == ' ' || value == '\t' || value == '\n' || value == '\r';
}

// 取 "key" 的数组值子串（含首尾方括号）；字段缺失或值不是数组时返回空串。
inline std::string find_array(const char* json, const char* key) {
    const char* value = find_field(json, key);
    if (value == nullptr || *value != '[') return std::string();
    const char* end = std::strchr(value, ']');
    if (end == nullptr) return std::string(value);
    return std::string(value, static_cast<size_t>(end - value) + 1);
}

struct SpecialSelections {
    bool backpack = false;
    bool badge = false;
    bool dice = false;
};

// special 是只含类型名的数组；未知名与非字符串元素忽略。
inline SpecialSelections parse_special_array(const char* json_array) {
    SpecialSelections selected{};
    if (json_array == nullptr) return selected;
    const char* p = std::strchr(json_array, '[');
    if (p == nullptr) return selected;
    ++p;
    for (;;) {
        while (is_space(*p) || *p == ',') ++p;
        if (*p == '\0' || *p == ']') break;
        if (*p != '"') {
            while (*p != '\0' && *p != ',' && *p != ']') ++p;
            continue;
        }
        ++p;
        const char* start = p;
        while (*p != '\0' && *p != '"') ++p;
        if (*p != '"') break;
        const char* begin = start;
        while (begin < p && is_space(*begin)) ++begin;
        const char* end = p;
        while (end > begin && is_space(end[-1])) --end;
        const std::string name(begin, static_cast<size_t>(end - begin));
        if (name == "backpack") selected.backpack = true;
        else if (name == "normalSeal") selected.badge = true;
        else if (name == "dice") selected.dice = true;
        ++p;
    }
    return selected;
}

}  // namespace autosell_store_detail

// 将旧 special 名字与新 tier 字段合并到配置。缺失 tier 由 special 回退，显式 tier 优先。
inline bool autosell_apply_special_json(const char* json, autosell::Config* config) {
    if (config == nullptr) return false;
    if (json == nullptr) return true;
    while (autosell_store_detail::is_space(*json)) ++json;
    if (*json == '\0') return true;
    if (std::strncmp(json, "null", 4) == 0) {
        json += 4;
        while (autosell_store_detail::is_space(*json)) ++json;
        return *json == '\0';
    }

    const bool legacy_array = *json == '[';
    if (!legacy_array && *json != '{') return false;
    using namespace autosell_store_detail;
    const SpecialSelections selected = legacy_array
                                           ? parse_special_array(json)
                                           : parse_special_array(find_array(json, "special").c_str());

    const bool has_backpack_tier = !legacy_array && find_field(json, "backpackTier") != nullptr;
    const bool has_badge_tier = !legacy_array && find_field(json, "badgeTier") != nullptr;
    const bool has_dice_tier = !legacy_array && find_field(json, "diceTier") != nullptr;
    config->backpack_tier = has_backpack_tier
                                ? clamp_int(parse_int(json, "backpackTier", 0), 0, 4)
                                : (selected.backpack ? 4 : 0);
    config->badge_tier = has_badge_tier
                             ? clamp_int(parse_int(json, "badgeTier", 0), 0, 4)
                             : (selected.badge ? 1 : 0);
    config->dice_tier = has_dice_tier
                            ? clamp_int(parse_int(json, "diceTier", 0), 0, 5)
                            : (selected.dice ? 4 : 0);
    return true;
}

// 从分档配置派生旧 special 字符串数组；输出顺序固定 backpack -> normalSeal -> dice。
inline std::string autosell_special_to_array(const autosell::Config& config) {
    std::string out = "[";
    bool first = true;
    const auto append = [&out, &first](const char* name) {
        if (!first) out += ",";
        out += "\"";
        out += name;
        out += "\"";
        first = false;
    };
    if (config.backpack_tier > 0) append("backpack");
    if (config.badge_tier > 0) append("normalSeal");
    if (config.dice_tier > 0) append("dice");
    out += "]";
    return out;
}

// Config -> sidecar section v2 JSON；special 保留为兼容字段，tier 键为权威值。
inline std::string autosell_config_to_json(const autosell::Config& config) {
    const std::string special = autosell_special_to_array(config);
    char buf[384];
    std::snprintf(buf, sizeof(buf),
                  "{\"v\":2,\"enabled\":%s,\"rarity\":%d,\"enhance\":%d,"
                  "\"socket\":%d,\"gemTier\":%d,\"gemRange\":%d,"
                  "\"backpackTier\":%d,\"badgeTier\":%d,\"diceTier\":%d,\"special\":%s}",
                  config.enabled ? "true" : "false", config.rarity, config.enhance,
                  config.socket, config.gem_tier, config.gem_range, config.backpack_tier,
                  config.badge_tier, config.dice_tier, special.c_str());
    return std::string(buf);
}

// section JSON -> Config。缺失字段取默认 0 并在边界内钳制；未知键忽略。
// v 缺失按 2；v>2（未知未来版本）/坏 JSON 返回 false 且 out=默认值。
inline bool autosell_config_from_json(const char* json, autosell::Config* out) {
    if (out == nullptr) return false;
    *out = autosell::Config{};
    if (json == nullptr || std::strchr(json, '{') == nullptr) return false;

    using namespace autosell_store_detail;
    if (parse_int(json, "v", 2) > 2) return false;

    autosell::Config config;
    config.enabled = parse_bool(json, "enabled", false);
    config.rarity = clamp_int(parse_int(json, "rarity", 0), 0, 5);
    config.enhance = clamp_int(parse_int(json, "enhance", 0), 0, 32);
    config.socket = clamp_int(parse_int(json, "socket", 0), 0, 16);
    config.gem_tier = clamp_int(parse_int(json, "gemTier", 0), 0, 5);
    config.gem_range = clamp_int(parse_int(json, "gemRange", 0), 0, 5);
    if (!autosell_apply_special_json(json, &config)) return false;
    *out = config;
    return true;
}
