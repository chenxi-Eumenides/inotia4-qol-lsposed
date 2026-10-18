#include "plugins/extension_bag/extension_bag_port.h"
#include "plugins/extension_bag/extension_bag_bridge.h"
#include "plugins/extension_bag/extension_bag_context.h"
#include "plugins/extension_bag/game_ui_virtbag.h"
#include "plugins/extension_bag/model/virtual_bag_state.h"
#include "plugins/extension_bag/model/item_handle_custody.h"
#include "plugins/extension_bag/api/port_internal.h"
#include "contracts/inventory_extension.h"
#include "contracts/sell_price.h"
#include "contracts/host_sdk.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <string>

namespace qol::features::extension_bag {
// ===========================================================================
// ITM-13：中立物品域扩展 provider —— 扩展袋语义唯一的对外实现点
// ===========================================================================
// core.item / game 物品读路径只经此中立表感知扩展物品；此表不含 ExtensionBag 名称、逻辑袋
// 硬编码或业务实现细节（袋号换算在此内部完成）。全部成员仅游戏主线程调用；持
// g_virtual_bag_mtx 时不得重入本表的加锁成员。
namespace {

QolResult neutral_error(int32_t code, const char* message) {
    return qol::contracts::error(code, message, static_cast<uint32_t>(std::strlen(message)));
}

int32_t neutral_owns_bag(int32_t bag) {
    return virtual_bag::valid_extension_logical_bag(bag) ? 1 : 0;
}

QolResult neutral_item_ref_at(int32_t bag, int32_t slot,
                              qol::contracts::QolInventoryExtensionItemRef* out) {
    if (out == nullptr) return neutral_error(kQolErrInvalidArgument, "invalid argument");
    *out = {};
    struct Ctx {
        int32_t bag;
        int32_t slot;
         qol::contracts::QolInventoryExtensionItemRef* out;
        bool found;
    } ctx{bag, slot, out, false};
    virtual_bag_for_each_logical_item(
        [](int32_t item_bag, int32_t item_slot, int32_t category, int32_t count,
           qol::contracts::QolItemHandle item_handle,
           void* raw) -> bool {
            auto* p = static_cast<Ctx*>(raw);
            if (item_bag != p->bag || item_slot != p->slot) return false;
             *p->out = {static_cast<int32_t>(qol::contracts::QolInventoryItemKind::kProvider),
                        item_bag, item_slot, category, count, item_handle};
            p->found = true;
            return true;
        },
        &ctx);
    if (!ctx.found) return neutral_error(kQolErrNotFound, "not found");
    const int internal_bag = virtual_bag::extension_internal_bag(bag);
    if (internal_bag < 0) return neutral_error(kQolErrNotFound, "not found");
    void* item = virtual_bag_item_at(internal_bag, slot);
    if (item == nullptr || !extension_bag_register_item_handle(item) ||
        !extension_bag_item_handle_for_pointer(item, &out->item)) {
        return neutral_error(kQolErrNotFound, "not found");
    }
    return qol::contracts::ok();
}

QolResult neutral_for_each_item(qol::contracts::QolInventoryExtensionItemFn fn, void* ctx) {
    if (fn == nullptr) return neutral_error(kQolErrInvalidArgument, "invalid argument");
    struct Bridge {
        qol::contracts::QolInventoryExtensionItemFn fn;
        void* ctx;
        bool stopped;
    } bridge{fn, ctx, false};
    virtual_bag_for_each_logical_item(
        [](int32_t bag, int32_t slot, int32_t category, int32_t count,
           qol::contracts::QolItemHandle item_handle, void* raw) -> bool {
            auto* p = static_cast<Bridge*>(raw);
            // Enumeration stays allocation-free; consumers request a materialized handle by slot.
            const qol::contracts::QolInventoryExtensionItemRef ref{
                static_cast<int32_t>(qol::contracts::QolInventoryItemKind::kProvider), bag, slot,
                category, count, item_handle};
            p->stopped = p->fn(&ref, p->ctx) != 0;
            return p->stopped;
        },
        &bridge);
    return qol::contracts::ok();
}

QolResult neutral_bag_capacity(int32_t bag, int32_t* out_capacity) {
    if (out_capacity == nullptr) return neutral_error(kQolErrInvalidArgument, "invalid argument");
    *out_capacity = 0;
    if (!virtual_bag_logical_capacity(bag, out_capacity)) {
        return neutral_error(kQolErrNotFound, "not found");
    }
    return qol::contracts::ok();
}

QolResult neutral_inventory_fragment(char* out, uint32_t capacity, uint32_t* out_size) {
    if (out == nullptr || out_size == nullptr) {
        return neutral_error(kQolErrInvalidArgument, "invalid argument");
    }
    *out_size = 0;
    // R-47b 真值表与袋号/capacity/slot_count 组装唯一实现点在扩展袋序列化内。
    const std::string fragment = extension_bag_inventory_bags_json();
    if (fragment.empty()) return qol::contracts::ok();
    if (fragment.size() >= capacity) return neutral_error(kQolErrBlocked, "buffer too small");
    std::memcpy(out, fragment.data(), fragment.size());
    out[fragment.size()] = '\0';
    *out_size = static_cast<uint32_t>(fragment.size());
    return qol::contracts::ok();
}

int32_t neutral_move_item(int32_t from_bag, int32_t from_slot, int32_t to_bag, int32_t to_slot,
                          char* out, uint32_t capacity, uint32_t* out_size) {
    return provider_move_item(from_bag, from_slot, to_bag, to_slot, out, capacity, out_size);
}

int32_t neutral_use_item(int32_t bag, int32_t slot, char* out, uint32_t capacity,
                         uint32_t* out_size) {
    if (out_size != nullptr) *out_size = 0;
    if (out == nullptr || out_size == nullptr) return 0;
    const std::string body = extension_bag_api_use_item_impl(bag, slot);
    const uint32_t copied = static_cast<uint32_t>(std::min<std::size_t>(body.size(), capacity));
    if (copied != 0) std::memcpy(out, body.data(), copied);
    *out_size = static_cast<uint32_t>(body.size());
    return 1;
}

QolResult neutral_sell_price(qol::contracts::QolItemHandle item, int32_t count,
                             int32_t apply_discount,
                             int64_t* out_price) {
    if (out_price == nullptr) return neutral_error(kQolErrInvalidArgument, "invalid argument");
    *out_price = 0;
    const int64_t price = provider_sell_price(item, count, apply_discount != 0);
    if (price < 0) return neutral_error(kQolErrInvalidArgument, "invalid sell price");
    *out_price = price;
    return qol::contracts::ok();
}

QolResult neutral_sync_projected_slot(int32_t bag, int32_t slot) {
    provider_sync_projected_slot(bag, slot);
    return qol::contracts::ok();
}

QolResult neutral_sync_projected_bag() {
    provider_sync_projected_bag();
    return qol::contracts::ok();
}

int32_t neutral_module_view_installed() {
    return provider_module_view_installed() ? 1 : 0;
}

int32_t neutral_identify_native_item(qol::contracts::QolItemHandle item,
                                     int32_t* out_bag, int32_t* out_slot) {
    if (out_bag == nullptr || out_slot == nullptr) return 0;
    int internal_bag = -1;
    int slot = -1;
    if (!provider_identify_native_item(item, &internal_bag, &slot)) return 0;
    if (internal_bag < 0 || internal_bag >= virtual_bag::kBagCount) return 0;
    // provider 内部完成袋号换算：对外只输出公共逻辑袋号。
    *out_bag = virtual_bag::kExtensionLogicalBagFirst + internal_bag;
    *out_slot = slot;
    return 1;
}

int32_t neutral_remove_native_item(qol::contracts::QolItemHandle item) {
    return provider_remove_native_item(item) ? 1 : 0;
}

QolResult neutral_put_jewel(int32_t role, int32_t bag, int32_t slot, int32_t equip_slot) {
    return extension_bag_api_put_jewel_typed(role, bag, slot, equip_slot);
}

const qol::contracts::QolInventoryExtensionProvider kInventoryExtensionProvider{
    &neutral_owns_bag,
    &neutral_item_ref_at,
    &neutral_for_each_item,
    &neutral_bag_capacity,
    &neutral_inventory_fragment,
    &neutral_move_item,
    &neutral_sell_price,
    &neutral_sync_projected_slot,
    &neutral_sync_projected_bag,
    &neutral_module_view_installed,
    &neutral_identify_native_item,
    &neutral_remove_native_item,
    &neutral_put_jewel,
    &neutral_use_item,
    &provider_remove_for_sale,
};

}  // namespace

const qol::contracts::QolInventoryExtensionProvider* inventory_extension_provider_table() {
    return &kInventoryExtensionProvider;
}

}  // namespace qol::features::extension_bag
