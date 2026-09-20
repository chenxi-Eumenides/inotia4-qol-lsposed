// inventory_trade.cpp —— 背包处置原语（自动出售阶段 A 基础设施）。
//
// 语义严格等同原 data_op_sell_item / data_op_discard_item；data_op_* 已改为委托
// 本原语（source="api"）。行为对 API 零变更：错误分支顺序、日志文案与退款路径
// 均逐行对应。

#include "inventory_trade.h"

#include "contracts/extension_bag.h"
#include "contracts/item_count.h"
#include "core/native/qol_log.h"
#include "contracts/sell_price.h"
#include "game_access.h"
#include "game_state.h"
#include "game_symbols.h"
#include "infra/capabilities.h"

#include <cstdint>
#include <mutex>

namespace inventory_trade {

namespace {

std::mutex g_inventory_trade_mtx;

// D2b：extension_bag 能力每次调用现取（不缓存跨帧指针）。能力缺失 = EB disabled。
const qol::contracts::ExtensionBagApi* extension_bag_api() {
    return static_cast<const qol::contracts::ExtensionBagApi*>(qol::infra::require_capability(
        qol::contracts::kExtensionBagCapabilityId,
        QolVersionRange{static_cast<uint16_t>(qol::contracts::kExtensionBagVersionMajor),
                        static_cast<uint16_t>(qol::contracts::kExtensionBagVersionMinor)}));
}

// 能力可用时走 provider 的 sell_price；缺失时按 provider 同一算法复现。
// 原因：port 的 sell_price 内部没有 enabled 门禁，是纯价格计算（变体折扣 R-23、堆叠
// 收敛与边界 R-37），EB 关闭时物理袋出售/自动出售仍需结算；直接降级为 -1 会破坏行为中性。
int64_t sell_price_with_fallback(const qol::contracts::ExtensionBagApi* api, void* item, int count) {
    if (api != nullptr && api->sell_price != nullptr) {
        return api->sell_price(item, count, true);
    }
    if (item == nullptr || count <= 0 || fn_item_get_sell_price == nullptr) return -1;
    const int64_t unit_price = static_cast<int64_t>(fn_item_get_sell_price(item));
    uint32_t legal_count = static_cast<uint32_t>(count);
    const qol::contracts::QolItemCountApi* item_count = qol::contracts::item_count_api();
    if (item_count != nullptr && item_count->effective != nullptr) {
        uint32_t effective = 0;
        const QolResult result = item_count->effective(item, &effective);
        if (qol::contracts::succeeded(result)) legal_count = effective;
    }
    int64_t final_price = 0;
    if (!qol::contracts::sell_price::calculate(unit_price, legal_count, true, &final_price)) {
        return -1;
    }
    return final_price;
}

// 投影同步：port 在 EB disabled 时实际为 no-op（无模块视图）；能力缺失时跳过等价。
void sync_projected_slot_if_available(const qol::contracts::ExtensionBagApi* api, int bag, int slot) {
    if (api != nullptr && api->sync_projected_slot != nullptr) api->sync_projected_slot(bag, slot);
}

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
    // 弹窗链检查（API 直连确认回调核心，不调用确认弹窗）：原版丢弃/出售按钮
    // UIEquip_ButtonDestroyExe(0xb6240) 仅做金额预演（x23=0x666 标记）+ 创建确认弹窗；
    // 弹窗 OK 回调 UIEquip_OKDestroyItem(0xb83d0) 才是真实结算（背包分支走 0x1261c4
    // 删物品 + 加钱，game_symbols.h 反汇编结论）。弹窗前步骤无业务副作用，API 在此
    // 直连结算语义：金额计算 + INVEN_RemoveItem（经 Stage4 Hook，扩展物品透明路由）。
    if (fn_item_is_no_sell(category) != 0) {
        QOL_LOG_WARN(QolDomain::kInventory,
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
    const qol::contracts::ExtensionBagApi* eb = extension_bag_api();
    const int64_t price = sell_price_with_fallback(eb, item, count);  // 默认变体折扣 70%
    if (price < 0) {
        QOL_LOG_WARN(QolDomain::kInventory,
                     "sell price reject reason=price_bounds source=%s category=%d count=%d "
                     "unit=%lld variant=100 payload=0 gen=0",
                     source, category, count, static_cast<long long>(unit_price));
        result.status = Status::kPriceInvalid;
        return result;
    }
    uint32_t log_count = static_cast<uint32_t>(count > 0 ? count : 0);
    const qol::contracts::QolItemCountApi* log_item_count = qol::contracts::item_count_api();
    if (log_item_count != nullptr && log_item_count->effective != nullptr) {
        uint32_t effective = 0;
        const QolResult effective_result = log_item_count->effective(item, &effective);
        if (qol::contracts::succeeded(effective_result)) log_count = effective;
    }
    QOL_LOG_INFO(QolDomain::kInventory,
                 "sell price source=%s category=%d count=%d unit=%lld variant=100 "
                 "final=%lld payload=0 gen=0",
                 source,
                 category,
                 static_cast<int>(log_count),
                 static_cast<long long>(unit_price), static_cast<long long>(price));
    result.price = price;
    // 与原版 UIStore_SellItem(0xd25f0) 的对照：
    //   原版链 = INVEN_RemoveItem(item) 成功后 INVEN_AddMoney(价格)，无回购列表（普通
    //   NPC 商店卖出的物品不进入货架）；本实现先加钱后删除、删除失败回退金币——顺序
    //   相反但最终状态一致，先加钱是为了并发下失败路径无需触碰已删除对象。
    //   原版 UI 按件出售：可堆叠物品数量 >1 时先 ITEMSYSTEM_Divide 拆分再删（0xd26b8
    //   分支），一次只卖 1 件；本原语语义为整槽出售（单价 × 数量），属 API 明确选择的
    //   语义差异（调用方需要按件卖可自行配合 move_item 拆分槽位）。
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
    sync_projected_slot_if_available(eb, bag, slot);
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
    sync_projected_slot_if_available(extension_bag_api(), bag, slot);
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
