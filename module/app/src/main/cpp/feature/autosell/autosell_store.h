#pragma once

#include <jni.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "feature/autosell/autosell_rules.h"

// 自动出售按存档配置持久化（sidecar section `autosell` v2，UTF-8 JSON，直写）。
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

// —— 特殊类型「名字 <-> 位」映射（全仓唯一来源；sidecar/JNI/状态 JSON 一律经此转换）——
// v2 对外契约为类型名集合；`autosell::SpecialType` 位掩码只是内部实现细节，不再对外承诺。
// 数组顺序 = special 输出顺序（固定 backpack -> normalSeal -> dice）。

struct SpecialTypeName {
    uint32_t bit;
    const char* name;
};

inline constexpr SpecialTypeName kSpecialNames[] = {
    {autosell::kSpecialBackpack, "backpack"},
    {autosell::kSpecialNormalSeal, "normalSeal"},
    {autosell::kSpecialDice, "dice"},
};
inline constexpr int kSpecialNameCount =
    static_cast<int>(sizeof(kSpecialNames) / sizeof(kSpecialNames[0]));

inline bool is_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

// 取 `"key"` 的数组值子串（含首尾方括号）；字段缺失或值不是数组时返回空串。
// 数组元素只允许字符串（特殊类型名），故以首个 ']' 收尾；未闭合时容错取到结尾。
inline std::string find_array(const char* json, const char* key) {
    const char* v = find_field(json, key);
    if (v == nullptr || *v != '[') return std::string();
    const char* end = std::strchr(v, ']');
    if (end == nullptr) return std::string(v);
    return std::string(v, static_cast<size_t>(end - v) + 1);
}

}  // namespace autosell_store_detail

// 位 -> 名字（仅接受单个已定义位；不在表内返回 nullptr）。
inline const char* autosell_special_name(uint32_t bit) {
    for (int i = 0; i < autosell_store_detail::kSpecialNameCount; ++i) {
        if (autosell_store_detail::kSpecialNames[i].bit == bit) {
            return autosell_store_detail::kSpecialNames[i].name;
        }
    }
    return nullptr;
}

// 名字 -> 位（NUL 结尾精确比较；未知名字/空/nullptr 返回 0）。
inline uint32_t autosell_special_bit(const char* name) {
    if (name == nullptr || name[0] == '\0') return 0;
    for (int i = 0; i < autosell_store_detail::kSpecialNameCount; ++i) {
        if (std::strcmp(autosell_store_detail::kSpecialNames[i].name, name) == 0) {
            return autosell_store_detail::kSpecialNames[i].bit;
        }
    }
    return 0;
}

// 解析 JSON 字符串数组文本（如 ["backpack", "dice"]）为位掩码。
// 容错：忽略 '[' 前内容；跳过元素间空白与逗号；未知名忽略；非字符串元素跳过；
// 引号未闭合按已有内容收敛；nullptr / 坏串（无 '['）-> 空集 0。
inline uint32_t autosell_special_parse_array(const char* json_array) {
    if (json_array == nullptr) return 0;
    const char* p = std::strchr(json_array, '[');
    if (p == nullptr) return 0;
    ++p;
    uint32_t mask = 0;
    for (;;) {
        while (autosell_store_detail::is_space(*p) || *p == ',') ++p;
        if (*p == '\0' || *p == ']') break;
        if (*p != '"') {  // 非字符串元素：跳到下一分隔符
            while (*p != '\0' && *p != ',' && *p != ']') ++p;
            continue;
        }
        ++p;  // 开引号
        const char* start = p;
        while (*p != '\0' && *p != '"') ++p;
        if (*p != '"') break;  // 字符串未闭合：容错按已有结果结束
        const char* s = start;
        while (s < p && autosell_store_detail::is_space(*s)) ++s;  // 容错引号内首尾空白
        const char* e = p;
        while (e > s && autosell_store_detail::is_space(e[-1])) --e;
        const std::string token(s, static_cast<size_t>(e - s));
        mask |= autosell_special_bit(token.c_str());
        ++p;  // 闭引号
    }
    return mask;
}

// 位掩码 -> JSON 字符串数组文本；只输出已选名字，顺序固定 backpack -> normalSeal -> dice；0 -> []。
inline std::string autosell_special_to_array(uint32_t mask) {
    std::string out = "[";
    bool first = true;
    for (int i = 0; i < autosell_store_detail::kSpecialNameCount; ++i) {
        if ((mask & autosell_store_detail::kSpecialNames[i].bit) == 0) continue;
        if (!first) out += ",";
        out += "\"";
        out += autosell_store_detail::kSpecialNames[i].name;
        out += "\"";
        first = false;
    }
    out += "]";
    return out;
}

// Config -> section v2 JSON（值即开关：0=关，正整数为 1-based 档位；special 为类型名数组）。
inline std::string autosell_config_to_json(const autosell::Config& config) {
    const std::string special = autosell_special_to_array(config.special_mask);
    char buf[256];
    std::snprintf(buf, sizeof(buf),
                  "{\"v\":2,\"enabled\":%s,\"rarity\":%d,\"enhance\":%d,"
                  "\"socket\":%d,\"gemTier\":%d,\"gemRange\":%d,\"special\":%s}",
                  config.enabled ? "true" : "false", config.rarity, config.enhance,
                  config.socket, config.gem_tier, config.gem_range, special.c_str());
    return std::string(buf);
}

// section JSON -> Config。缺失字段取默认 0 并在边界内钳制；未知键忽略。
// 钳制：rarity/gemTier/gemRange 0..5、socket 0..16、enhance 0..32。
// special：v2 名字数组；未知名忽略；缺失/非数组 -> 空集。旧 v1 键 specialMask 不再读取（不做迁移）。
// v 缺失按 2；v>2（未知未来版本）/坏 JSON 返回 false 且 out=默认值。
inline bool autosell_config_from_json(const char* json, autosell::Config* out) {
    if (out == nullptr) return false;
    *out = autosell::Config{};
    if (json == nullptr || std::strchr(json, '{') == nullptr) return false;

    using namespace autosell_store_detail;
    if (parse_int(json, "v", 2) > 2) return false;  // 未知未来版本：回退默认

    autosell::Config cfg;
    cfg.enabled = parse_bool(json, "enabled", false);
    cfg.rarity = clamp_int(parse_int(json, "rarity", 0), 0, 5);
    cfg.enhance = clamp_int(parse_int(json, "enhance", 0), 0, 32);
    cfg.socket = clamp_int(parse_int(json, "socket", 0), 0, 16);
    cfg.gem_tier = clamp_int(parse_int(json, "gemTier", 0), 0, 5);
    cfg.gem_range = clamp_int(parse_int(json, "gemRange", 0), 0, 5);
    const std::string special = find_array(json, "special");
    cfg.special_mask = autosell_special_parse_array(special.c_str());

    *out = cfg;
    return true;
}

// 注册 Kotlin AutoSellConfigStore（jclass + load/save 静态方法 id）。
void autosell_store_register_bridge(JNIEnv* env, jclass bridge_class);

// 进档时经桥读取 `autosell` section 并应用到运行时配置；非法 slot 不读写。
// 由 `autosell_register_save_enter` 的 save-enter 回调（游戏主线程）调用一次；任务
// 生命周期见 autosell_scan.h（全局开关 + 已进档）。
void autosell_store_ensure_loaded(int slot);

// 序列化并写回 `autosell` section；slot 非法或桥失败返回 false。
bool autosell_store_persist(int slot, const autosell::Config& config);
