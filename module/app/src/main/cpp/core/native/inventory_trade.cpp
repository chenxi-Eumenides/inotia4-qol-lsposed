// inventory_trade.cpp —— 背包处置原语（自动出售阶段 A 基础设施）。
//
// 语义严格等同原 data_op_sell_item / data_op_discard_item；data_op_* 已改为委托
// 本原语（source="api"）。行为对 API 零变更：错误分支顺序、日志文案与退款路径
// 均逐行对应。

#include "inventory_trade.h"

#include "core/native/extension_bag_port.h"
#include "core/native/stack_codec.h"
#include "core/native/stack_limit_port.h"
#include "game_access.h"
#include "game_state.h"
#include "game_symbols.h"

#include <android/log.h>

#include <cstdint>
#include <mutex>

namespace inventory_trade {

namespace {

std::mutex g_inventory_trade_mtx;

Result sell_locked(int bag, int slot, const void* expected_item, const char* source) {
    Result result;
    if (!game_in_world()) {
        result.status = Status::kNotInWorld;
        return result;
    }
    if (bag == 5) {
        result.status = Status::kTaskBagExcluded;
        return result;
    }
    if (fn_remove_item == nullptr || fn_add_money == nullptr ||
        fn_minus_money == nullptr || fn_item_get_sell_price == nullptr ||
        fn_item_is_no_sell == nullptr || fn_get_bit == nullptr) {
        result.status = Status::kSymbolsUnresolved;
        return result;
    }
    InventoryItemRef item_ref;
    if (!inventory_item_ref_at(bag, slot, &item_ref)) {
        result.status = Status::kEmptySlot;
        return result;
    }
    if (item_ref.native_item == nullptr) {
        result.status = Status::kNoNativeItem;
        return result;
    }
    // 处置身份复核（I-1）：解析后物品与调用方期望不一致即视为槽位已变，不结算。
    if (expected_item != nullptr && static_cast<const void*>(item_ref.native_item) != expected_item) {
        result.status = Status::kStale;
        return result;
    }
    void* item = item_ref.native_item;
    const uint16_t flags = *reinterpret_cast<uint16_t*>(
        reinterpret_cast<uint8_t*>(item) + I_TYPE);
    const int category = fn_get_bit(flags, 15, 6);
    result.category = category;
    result.count = item_ref.count;
    if (fn_item_is_no_sell(category) != 0) {
        __android_log_print(ANDROID_LOG_WARN, "Inotia4VirtBag",
                            "sell price reject reason=no_sell source=%s category=%d count=%d "
                            "variant=100 payload=0 gen=0",
                            source, category, item_ref.count);
        result.status = Status::kNoSell;
        return result;
    }
    const int count = fn_get_cumulate_count != nullptr
        ? fn_get_cumulate_count(item) : 1;
    result.count = count;
    const int64_t unit_price = static_cast<int64_t>(fn_item_get_sell_price(item));
    const int64_t price = extension_bag_sell_price(item, count);  // 默认变体折扣 70%
    if (price < 0) {
        __android_log_print(ANDROID_LOG_WARN, "Inotia4VirtBag",
                            "sell price reject reason=price_bounds source=%s category=%d count=%d "
                            "unit=%lld variant=100 payload=0 gen=0",
                            source, category, count, static_cast<long long>(unit_price));
        result.status = Status::kPriceInvalid;
        return result;
    }
    __android_log_print(ANDROID_LOG_INFO, "Inotia4VirtBag",
                        "sell price source=%s category=%d count=%d unit=%lld variant=100 "
                        "final=%lld payload=0 gen=0",
                        source,
                        category,
                        static_cast<int>(stack_codec::effective_clamp(
                            static_cast<uint32_t>(count), stack_limit_enabled())),
                        static_cast<long long>(unit_price), static_cast<long long>(price));
    result.price = price;
    if (!fn_add_money(price)) {
        result.status = Status::kCreditFailed;
        return result;
    }
    // 统一走已 Hook 的 INVEN_RemoveItem：扩展物品由 Stage4 识别后走扩展删除，原版物品透传原版。
    // 删除失败回退金币，保持扩展物品原有退款路径语义。
    if (fn_remove_item(item) == 0) {
        fn_minus_money(price);
        result.status = Status::kRemoveFailed;
        return result;
    }
    extension_bag_sync_projected_slot(bag, slot);
    result.status = Status::kOk;
    return result;
}

Status discard_locked(int bag, int slot, const void* expected_item) {
    if (!game_in_world()) return Status::kNotInWorld;
    if (bag == 5) return Status::kTaskBagExcluded;
    InventoryItemRef item_ref;
    if (!inventory_item_ref_at(bag, slot, &item_ref)) return Status::kEmptySlot;
    if (item_ref.native_item == nullptr) return Status::kNoNativeItem;
    if (fn_remove_item == nullptr) return Status::kSymbolsUnresolved;
    // 处置身份复核（I-1）：与 sell 同口径；置于符号检查之后以保持原有错误优先级。
    if (expected_item != nullptr && static_cast<const void*>(item_ref.native_item) != expected_item) {
        return Status::kStale;
    }
    // 统一走已 Hook 的 INVEN_RemoveItem：扩展物品由 Stage4 识别后走扩展删除，原版物品透传原版。
    if (fn_remove_item(item_ref.native_item) == 0) return Status::kRemoveFailed;
    extension_bag_sync_projected_slot(bag, slot);
    return Status::kOk;
}

}  // namespace

Result sell(int bag, int slot, const void* expected_item, const char* source) {
    std::lock_guard<std::mutex> lock(g_inventory_trade_mtx);
    return sell_locked(bag, slot, expected_item, source != nullptr ? source : "");
}

Status discard(int bag, int slot, const void* expected_item) {
    std::lock_guard<std::mutex> lock(g_inventory_trade_mtx);
    return discard_locked(bag, slot, expected_item);
}

}  // namespace inventory_trade
