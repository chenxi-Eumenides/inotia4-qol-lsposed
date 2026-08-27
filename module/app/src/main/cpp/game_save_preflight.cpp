// game_save_preflight.cpp —— 存档预检纯逻辑（parse 域）。
// 判决来源：原版 SAVE_LoadSaveSlot 写入的槽结构（docs/system/save.md §4），
// 本文件只做字节 → 判决/JSON 的映射，不读存档文件、不调游戏函数。

#include "game_save_preflight.h"

#include <cstddef>

#include "game_json.h"

SavePreflightVerdict save_preflight_classify(uint8_t slot_state, uint8_t slot_err) {
    switch (slot_state) {
        case 0:
            return SavePreflightVerdict::kMissing;
        case 2:
            return SavePreflightVerdict::kValid;
        case 1:
            // 阶段码 3 = IsValidInformation 失败（版本 >5 或槽位号不匹配）
            return (slot_err & 0x7) == 3 ? SavePreflightVerdict::kIncompatible
                                         : SavePreflightVerdict::kCorrupt;
        default:
            return SavePreflightVerdict::kUnknown;
    }
}

const char* save_preflight_verdict_name(SavePreflightVerdict v) {
    switch (v) {
        case SavePreflightVerdict::kValid: return "valid";
        case SavePreflightVerdict::kCorrupt: return "corrupt";
        case SavePreflightVerdict::kMissing: return "missing";
        case SavePreflightVerdict::kIncompatible: return "incompatible";
        default: return "unknown";
    }
}

const char* save_preflight_stage_name(uint8_t err_code) {
    // docs/system/save.md §4：SAVE_LoadSaveSlot 逐级短路写入的阶段码
    static const char* kNames[] = {
        "load_data",      // 0 文件打开/读取/解密/校验和
        "block_table",    // 1 块 0 目录项不可读
        "information",    // 2 块 0（Information）解析失败
        "validation",     // 3 版本 >5 或槽位号不匹配
        "block_table",    // 4 块 1 目录项不可读
        "player",         // 5 Player 块解析失败
        "mercenary_slot", // 6 主佣兵槽索引 <0
        "character",      // 7 角色 0 加载失败
        "character",      // 8 角色 1（3 位字段截断为 0，与码 0 不可区分）
        "character",      // 9 角色 2（截断为 1）
    };
    if (err_code < sizeof(kNames) / sizeof(kNames[0])) return kNames[err_code];
    return "unknown";
}

std::string save_preflight_json(int32_t slot, uint8_t slot_state, uint8_t slot_err,
                                int map_id, int hero_level, int hero_index,
                                const char* detail) {
    SavePreflightVerdict v = save_preflight_classify(slot_state, slot_err);
    std::string s = "{\"ok\":true,\"slot\":" + std::to_string(slot);
    s += ",\"verdict\":\"";
    s += save_preflight_verdict_name(v);
    s += "\"";
    s += ",\"enter\":";
    s += (v == SavePreflightVerdict::kValid ? "true" : "false");
    s += ",\"slot_state\":" + std::to_string(slot_state);
    if (v == SavePreflightVerdict::kCorrupt || v == SavePreflightVerdict::kIncompatible) {
        uint8_t code = slot_err & 0x7;
        s += ",\"stage\":\"";
        s += save_preflight_stage_name(code);
        s += "\",\"error_code\":" + std::to_string(code);
    }
    if (v == SavePreflightVerdict::kValid) {
        s += ",\"map_id\":" + std::to_string(map_id);
        s += ",\"hero_level\":" + std::to_string(hero_level);
        s += ",\"hero_index\":" + std::to_string(hero_index);
    }
    if (detail != nullptr && detail[0] != '\0') {
        s += ",\"detail\":\"";
        s += json_escape(detail);
        s += "\"";
    }
    s += "}";
    return s;
}

std::string save_preflight_error_json(int32_t slot, uint8_t slot_state, uint8_t slot_err) {
    SavePreflightVerdict v = save_preflight_classify(slot_state, slot_err);
    uint8_t code = slot_err & 0x7;
    std::string s = "{\"ok\":false,\"error\":\"slot ";
    s += save_preflight_verdict_name(v);
    s += "\"";
    s += ",\"slot\":" + std::to_string(slot);
    s += ",\"verdict\":\"";
    s += save_preflight_verdict_name(v);
    s += "\"";
    s += ",\"stage\":\"";
    s += save_preflight_stage_name(code);
    s += "\",\"error_code\":" + std::to_string(code);
    s += ",\"enter\":false}";
    return s;
}

std::string save_preflight_semantic_error_json(int32_t slot, const char* stage,
                                               uint8_t error_code, const char* detail) {
    std::string s = "{\"ok\":false,\"error\":\"slot corrupt\",\"slot\":";
    s += std::to_string(slot);
    s += ",\"verdict\":\"corrupt\",\"stage\":\"";
    s += stage != nullptr ? stage : "unknown";
    s += "\",\"error_code\":" + std::to_string(error_code) + ",\"enter\":false";
    if (detail != nullptr && detail[0] != '\0') {
        s += ",\"detail\":\"";
        s += json_escape(detail);
        s += "\"";
    }
    s += "}";
    return s;
}
