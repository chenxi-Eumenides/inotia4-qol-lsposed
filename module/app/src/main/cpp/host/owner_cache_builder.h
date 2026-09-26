#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "contracts/character_state.h"
#include "contracts/item_state.h"
#include "contracts/result.h"

namespace qol::host {

// T2h-B1（v3 A-simple）：owner 优先的 cache builder 适配器。
// 只做字节转发：api 现取（调用方不缓存）、成功即返回 owner 组装 JSON；四类失败
// （api 空/写失败/缓冲不足）逐字 fallback 到同槽 legacy builder；不解析 JSON。
// inline 头：assembly.cpp 与 host 测试共用同一实现（W2 可测钩子），无导出符号。
inline std::string build_character_state_json_via_owner(
    const qol::contracts::CharacterStateApi* api, int32_t kind, uint32_t capacity,
    std::string (*legacy)()) {
    if (api == nullptr || api->write_state_json == nullptr || legacy == nullptr) {
        return legacy != nullptr ? legacy() : std::string();
    }
    thread_local std::vector<char> buffer;
    if (buffer.size() < capacity) buffer.resize(capacity);
    uint32_t size = 0;
    if (!qol::contracts::succeeded(api->write_state_json(
            static_cast<qol::contracts::QolCharacterStateKind>(kind), buffer.data(),
            static_cast<uint32_t>(buffer.size()), &size)) ||
        size > buffer.size()) {
        return legacy();
    }
    return std::string(buffer.data(), size);
}

// T2h-B2：item.state 适配器（同形态）；kind 当前仅 kRawInventory。
inline std::string build_item_state_json_via_owner(const qol::contracts::ItemStateApi* api,
                                                   int32_t kind, uint32_t capacity,
                                                   std::string (*legacy)()) {
    if (api == nullptr || api->write_state_json == nullptr || legacy == nullptr) {
        return legacy != nullptr ? legacy() : std::string();
    }
    thread_local std::vector<char> buffer;
    if (buffer.size() < capacity) buffer.resize(capacity);
    uint32_t size = 0;
    if (!qol::contracts::succeeded(
            api->write_state_json(kind, buffer.data(), static_cast<uint32_t>(buffer.size()), &size)) ||
        size > buffer.size()) {
        return legacy();
    }
    return std::string(buffer.data(), size);
}

}  // namespace qol::host
