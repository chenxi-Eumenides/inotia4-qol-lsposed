#pragma once

#include <string>

#include "contracts/result.h"
#include "contracts/game_ports/item_dice_write_port.h"
#include "contracts/game_ports/item_inventory_state_write_port.h"
#include "core/native/inventory_trade.h"

namespace qol::bridge {

// JNI item 写入口旧信封的逐字等价（对照 api/native 各 data_op_* 的 op_ok/op_err 字节）。
// helper 只做 JSON 字节构造，不调用 frame_cache_force_refresh；refresh 纪律（discard 补刷、
// sell/dice_accept 不刷等）由各 wrapper 侧处理。

// 与 game/ops/game_ops_common.cpp 的 op_err 同格式。message 由调用方保证非空。
inline std::string item_error_json(const char* message) {
    return std::string("{\"ok\":false,\"error\":\"") + message + "\"}";
}

// 纯信封（set_money/add_item/jewel/enchant/equip/unequip/dice_reject 等）：
// 成功 {"ok":true}；失败优先 result.message，空则 fallback。
inline std::string item_write_result_json(const QolResult& result, const char* fallback) {
    if (contracts::succeeded(result)) return "{\"ok\":true}";
    return item_error_json(result.message != nullptr ? result.message : fallback);
}

// use_item：成功无 gained → {"ok":true}；有 gained → gained 数组（字段序 bag,slot,category,count）。
inline std::string item_use_result_json(const QolResult& result,
                                        const contracts::QolItemInventoryStateWriteResult& out) {
    if (!contracts::succeeded(result)) {
        return item_error_json(result.message != nullptr ? result.message : "item use failed");
    }
    if ((out.flags & contracts::kQolItemInventoryStateHasGained) == 0) return "{\"ok\":true}";
    std::string s = "{\"ok\":true,\"gained\":[";
    for (uint32_t i = 0; i < out.gained_count; ++i) {
        if (i != 0) s.push_back(',');
        const auto& item = out.gained[i];
        s += "{\"bag\":" + std::to_string(item.bag) +
             ",\"slot\":" + std::to_string(item.slot) +
             ",\"category\":" + std::to_string(item.category) +
             ",\"count\":" + std::to_string(item.count) + "}";
    }
    s += "]}";
    return s;
}

// dice_accept：成功 → base/applied/delta 三个定长数组（kQolItemDiceStatCount=5）。
inline std::string item_dice_accept_result_json(const QolResult& result,
                                                const contracts::QolItemDiceAcceptResult& out) {
    if (!contracts::succeeded(result)) {
        return item_error_json(result.message != nullptr ? result.message : "dice accept failed");
    }
    std::string s = "{\"ok\":true,\"base\":[";
    for (uint32_t i = 0; i < contracts::kQolItemDiceStatCount; ++i) {
        if (i > 0) s += ",";
        s += std::to_string(out.base[i]);
    }
    s += "],\"applied\":[";
    for (uint32_t i = 0; i < contracts::kQolItemDiceStatCount; ++i) {
        if (i > 0) s += ",";
        s += std::to_string(out.applied[i]);
    }
    s += "],\"delta\":[";
    for (uint32_t i = 0; i < contracts::kQolItemDiceStatCount; ++i) {
        if (i > 0) s += ",";
        s += std::to_string(out.delta[i]);
    }
    s += "]}";
    return s;
}

// shop_buy：成功 → {"ok":true,"price":<out.value>}。
inline std::string item_shop_buy_result_json(const QolResult& result,
                                             const contracts::QolItemInventoryStateWriteResult& out) {
    if (!contracts::succeeded(result)) {
        return item_error_json(result.message != nullptr ? result.message : "shop buy failed");
    }
    return "{\"ok\":true,\"price\":" + std::to_string(out.value) + "}";
}

// discard：Status → 信封（kOk 的 cache refresh 由 wrapper 侧补）。
inline std::string item_discard_result_json(inventory_trade::Status status) {
    switch (status) {
        case inventory_trade::Status::kOk:
            return "{\"ok\":true}";
        case inventory_trade::Status::kNotInWorld:
            return item_error_json("not in game");
        case inventory_trade::Status::kTaskBagExcluded:
            return item_error_json("task bag excluded");
        case inventory_trade::Status::kEmptySlot:
        case inventory_trade::Status::kStale:
            return item_error_json("slot empty");
        case inventory_trade::Status::kNoNativeItem:
            return item_error_json("extension item unavailable");
        case inventory_trade::Status::kSymbolsUnresolved:
            return item_error_json("symbol not resolved");
        case inventory_trade::Status::kRemoveFailed:
        default:
            return item_error_json("discard failed");
    }
}

// sell：Result → 信封或 {"ok":true,"price":N}（不刷新）。
inline std::string item_sell_result_json(const inventory_trade::Result& result) {
    switch (result.status) {
        case inventory_trade::Status::kOk:
            return "{\"ok\":true,\"price\":" + std::to_string(result.price) + "}";
        case inventory_trade::Status::kNotInWorld:
            return item_error_json("not in game");
        case inventory_trade::Status::kTaskBagExcluded:
            return item_error_json("task bag excluded");
        case inventory_trade::Status::kEmptySlot:
        case inventory_trade::Status::kStale:
            return item_error_json("slot empty");
        case inventory_trade::Status::kNoNativeItem:
            return item_error_json("extension item unavailable");
        case inventory_trade::Status::kSymbolsUnresolved:
            return item_error_json("symbol not resolved");
        case inventory_trade::Status::kNoSell:
            return item_error_json("item cannot be sold");
        case inventory_trade::Status::kPriceInvalid:
            return item_error_json("invalid sell price");
        case inventory_trade::Status::kCreditFailed:
            return item_error_json("sell money update failed");
        case inventory_trade::Status::kRemoveFailed:
        default:
            return item_error_json("sell failed");
    }
}

}  // namespace qol::bridge
