#include "game_ui_virtbag.h"
#include "contracts/game_host.h"
#include "contracts/host_sdk.h"
#include "contracts/ui.h"
#include "contracts/ownership.h"
#include "contracts/inventory_refresh.h"
#include "contracts/item_facts.h"
#include "contracts/module_save_port.h"
#include "contracts/popup_enter_alias.h"
#include "plugins/extension_bag/extension_bag_context.h"
#include "plugins/extension_bag/model/equip_rules.h"
#include "plugins/extension_bag/extension_bag_geometry.h"
#include "plugins/extension_bag/extension_bag_internal.h"
#include "plugins/extension_bag/runtime/logging.h"
#include "plugins/extension_bag/assembly/move_merge.h"
#include "plugins/extension_bag/model/ownership_ledger.h"
#include "plugins/extension_bag/model/item_handle_custody.h"
#include "plugins/extension_bag/model/sale_transaction.h"
#include "plugins/extension_bag/model/draw_state_lock.h"
#include "plugins/extension_bag/model/main_menu_cleanup_edge.h"
#include "plugins/extension_bag/plugin.h"
#include "plugins/extension_bag/ui/store_sell_rules.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <functional>
#include <mutex>
#include <thread>
#include <unistd.h>

// 商店宿主只管理 UIStore 自己的控件、投影和详情事务，不复用 UIEquip 的视图状态。

namespace {
using StoreSellItemFn = int64_t (*)(int64_t);

void store_sell_popup(uint32_t text_id) {
    if (runtime_state().host.sdk != nullptr && runtime_state().host.sdk->ui != nullptr && runtime_state().host.sdk->ui->show_text_popup != nullptr) {
        (void)runtime_state().host.sdk->ui->show_text_popup(static_cast<int32_t>(text_id));
    }
}

// W1 部分出售：把模式视图数量从 view 递减 count（不调用原版 ITEMSYSTEM_Divide）。
// 写回经 item-count facade（R-47 决策 b：启用态拆段进位、关闭态只写 b 保留 a），
// 随后重序列化 payload 并把 descriptor.count 回写为 canonical（R-38/R-48）。
bool decrement_extension_item_view_locked(int bag, int slot, void* item, int count,
                                          const char* context) {
    if (!virtual_bag::valid_index(bag) || slot < 0 || slot >= virtual_bag::kSlotCount ||
        item == nullptr || count <= 0) {
        return false;
    }
    virtual_bag::Item& descriptor = runtime_state().objects.bag.items[bag][slot];
    const int view = static_cast<int>(virtual_bag_effective_count(
        item, static_cast<uint32_t>(descriptor.count)));
    if (count >= view) return false;  // 整堆由调用方走删除分支
    const auto* count_api = item_count_api();
    uint32_t old_field = 0;
    if (count_api == nullptr || count_api->read_encoded_field == nullptr ||
        count_api->write_encoded_field == nullptr ||
        !qol::contracts::succeeded(count_api->read_encoded_field(item, &old_field))) return false;
    if (!virtual_bag_write_effective(item, static_cast<uint32_t>(view - count))) return false;
    std::array<uint8_t, virtual_bag::kSerializedItemBuffer> payload{};
    int payload_size = 0;
    if (!serialize_item_payload_locked(item, &payload, &payload_size)) {
        (void)count_api->write_encoded_field(item, old_field);
        VIRTBAG_LOG("store sell reserialize failed context=%s bag=%d slot=%d", context, bag, slot);
        return false;
    }
    descriptor.payload = payload;
    descriptor.payload_size = payload_size;
    descriptor.count = static_cast<int>(virtual_bag_canonical_count(
        item, virtual_bag_canonical_from_field(old_field, static_cast<uint32_t>(descriptor.count))));
    runtime_state().objects.module_object_categories[bag][slot] = descriptor.category;
    runtime_state().objects.module_object_hashes[bag][slot] = virtual_bag::payload_hash(descriptor);
    extension_bag_mark_dirty_locked();
    return true;
}
}  // namespace

int64_t store_sell_item_wrapper(int64_t displayed_price) {
    const ::qol::contracts::InFlightGuard in_flight_guard(
        runtime_state().host.sdk == nullptr ? nullptr : &runtime_state().host.sdk->in_flight,
        {"hook:UIStore_SellItem"});
    if (runtime_state().ui.backup_store_sell_item == nullptr) return 0;
    if (!runtime_state().enabled.load()) return reinterpret_cast<StoreSellItemFn>(runtime_state().ui.backup_store_sell_item)(displayed_price);

    void* item = nullptr;
    int bag = -1;
    int slot = -1;
    int view = 0;
    bool item_encoded = false;
    bool module_item = false;
    bool reject = false;
    qol::store_sell::Plan plan{};
    {
        std::lock_guard<std::mutex> lock(runtime_state().mtx);
        if (runtime_state().store.view_installed && runtime_state().host.game != nullptr &&
            runtime_state().host.inventory_extras->read.cursor_item != nullptr &&
            runtime_state().host.item_facts != nullptr && runtime_state().host.item_facts->sell_price != nullptr) {
            void* ctrl = extension_bag_ui_root(qol::contracts::QolUiRootKind::kStoreItemRoot);
            void* cursor = nullptr;
            if (ctrl != nullptr) {
                (void)runtime_state().host.inventory_extras->read.cursor_item(ctrl, &cursor);
            }
            item = cursor != nullptr ? extension_bag_ui_item(cursor) : nullptr;
            if (item != nullptr && module_slot_of_item_locked(item, &bag, &slot)) {
                module_item = true;
                const bool identity_ok =
                    bag == runtime_state().store.view_bag && virtual_bag::valid_index(bag) && slot >= 0 &&
                    slot < virtual_bag::kSlotCount && runtime_state().objects.module_objects[bag][slot] == item &&
                    module_object_release_allowed_locked(bag, slot, item, "store sell", false,
                                                         nullptr);
                const virtual_bag::Item& descriptor = runtime_state().objects.bag.items[bag][slot];
                const bool descriptor_ok =
                    descriptor.category > 0 && descriptor.count > 0 &&
                    runtime_state().objects.module_object_categories[bag][slot] == descriptor.category &&
                    runtime_state().objects.module_object_hashes[bag][slot] == virtual_bag::payload_hash(descriptor);
                if (!identity_ok || !descriptor_ok) {
                    reject = true;
                } else {
                    view = static_cast<int>(virtual_bag_effective_count(
                        item, static_cast<uint32_t>(descriptor.count)));
                    item_encoded = extension_bag_ui_item_count_encoding(item) ==
                                   ::qol::contracts::QolItemCountEncoding::kEncoded;
                    int64_t unit = 0;
                    if (!extension_bag_ui_read_item_sell_price(item, &unit)) {
                        reject = true;
                    } else {
                        plan = qol::store_sell::plan(item_encoded, view, unit, displayed_price);
                        if (plan.kind == qol::store_sell::PlanKind::kReject) reject = true;
                    }
                }
            }
        }
    }
    if (!module_item) return reinterpret_cast<StoreSellItemFn>(runtime_state().ui.backup_store_sell_item)(displayed_price);
    if (reject) {
        VIRTBAG_LOG("store sell reject reason=identity_or_plan bag=%d slot=%d", bag, slot);
        store_sell_popup(0xc);
        return 0;
    }

    const int count = plan.kind == qol::store_sell::PlanKind::kWhole ? 1 : plan.count;
    const int64_t price = extension_bag_sell_price(item, count, false);
    if (price < 0 || price != displayed_price || extension_bag_write_api() == nullptr ||
        extension_bag_write_api()->equipment.add_money == nullptr ||
        extension_bag_write_api()->equipment.minus_money == nullptr) {
        VIRTBAG_LOG("store sell reject reason=price_mismatch bag=%d slot=%d count=%d price=%lld displayed=%lld",
                    bag, slot, count, static_cast<long long>(price),
                    static_cast<long long>(displayed_price));
        store_sell_popup(0xc);
        return 0;
    }
    if (!extension_bag_ui_add_money(price)) {
        VIRTBAG_LOG("store sell credit failed bag=%d slot=%d price=%lld", bag, slot,
                    static_cast<long long>(price));
        store_sell_popup(0xc);
        return 0;
    }

    bool committed = false;
    bool whole = false;
    {
        std::lock_guard<std::mutex> lock(runtime_state().mtx);
        const bool current =
            virtual_bag::valid_index(bag) && slot >= 0 && slot < virtual_bag::kSlotCount &&
            runtime_state().objects.module_objects[bag][slot] == item &&
            module_object_release_allowed_locked(bag, slot, item, "store sell", false, nullptr) &&
            (!item_encoded ||
             static_cast<int>(virtual_bag_effective_count(
                 item, static_cast<uint32_t>(runtime_state().objects.bag.items[bag][slot].count))) == view);
        if (current) {
            whole = plan.kind == qol::store_sell::PlanKind::kWhole || count >= view;
            if (whole) {
                const virtual_bag::Item previous_item = runtime_state().objects.bag.items[bag][slot];
                runtime_state().objects.bag.items[bag][slot] = {};
                extension_bag_mark_dirty_locked();
                if (!free_module_object_locked(bag, slot)) {
                    runtime_state().objects.bag.items[bag][slot] = previous_item;
                    persist_state_locked();
                    VIRTBAG_LOG("store sell rejected object release bag=%d slot=%d", bag, slot);
                } else {
                    persist_state_locked();
                    committed = true;
                }
            } else if (decrement_extension_item_view_locked(bag, slot, item, count, "store sell")) {
                persist_state_locked();
                committed = true;
            }
        }
        if (committed && runtime_state().store.view_installed && runtime_state().store.view_bag == bag) {
            // R-12：只用商店投影刷新；调用原版 UIStore_RefreshInvenItem 会把整列刷回
            // 窗口原版袋内容，破坏扩展投影。
            store_refresh_projection_locked();
        }
    }
    if (!committed) {
        const bool refunded = extension_bag_ui_minus_money(price);
        VIRTBAG_LOG("store sell remove failed bag=%d slot=%d count=%d price=%lld refunded=%d",
                    bag, slot, count, static_cast<long long>(price), refunded ? 1 : 0);
        store_sell_popup(0xc);
        return 0;
    }
    store_sell_popup(0x1f);
    VIRTBAG_LOG("store sell committed bag=%d slot=%d count=%d whole=%d view=%d price=%lld",
                bag, slot, count, whole ? 1 : 0, view, static_cast<long long>(price));
    return 0;
}

namespace {
enum class StoreSaveSlotLookup : uint8_t { kFound, kNotFound, kError };

StoreSaveSlotLookup store_find_original_slot_with_real_capacity(void* item,
                                                                 int32_t* out_slot) {
    if (item == nullptr || out_slot == nullptr || runtime_state().host.game == nullptr ||
        runtime_state().host.inventory_extras == nullptr || runtime_state().host.inventory_extras->read.find_save_slot == nullptr) {
        VIRTBAG_LOG("store buy gate original lookup unavailable item=%p", item);
        return StoreSaveSlotLookup::kError;
    }

    std::unique_lock<std::mutex> lock(runtime_state().mtx);
    const bool projected = runtime_state().store.view_installed;
    const int original_bag = runtime_state().store.window_original_bag;
    const uint64_t view_generation = runtime_state().store.view_generation;
    uint32_t projected_size_word = 0;
    if (projected) {
        if (!virtual_bag::valid_original_transaction_bag(original_bag) ||
            !bag_size_word_locked(original_bag, &projected_size_word)) {
            VIRTBAG_LOG("store buy gate cannot read projected capacity window=%d", original_bag);
            return StoreSaveSlotLookup::kError;
        }
        if (!set_bag_size_word_locked(original_bag, runtime_state().store.original_size_word)) {
            VIRTBAG_LOG("store buy gate cannot restore original capacity window=%d", original_bag);
            return StoreSaveSlotLookup::kError;
        }
    }

    lock.unlock();
    int32_t slot = -1;
    const QolResult result = runtime_state().host.inventory_extras->read.find_save_slot(item, &slot);
    lock.lock();

    if (projected) {
        if (runtime_state().store.view_generation != view_generation || !runtime_state().store.view_installed ||
            runtime_state().store.window_original_bag != original_bag) {
            VIRTBAG_LOG("store buy gate view changed during original lookup generation=%llu->%llu",
                        static_cast<unsigned long long>(view_generation),
                        static_cast<unsigned long long>(runtime_state().store.view_generation));
            return StoreSaveSlotLookup::kError;
        }
        if (!set_bag_size_word_locked(original_bag, projected_size_word)) {
            VIRTBAG_LOG("store buy gate failed to restore projected capacity window=%d", original_bag);
            return StoreSaveSlotLookup::kError;
        }
    }

    if (qol::contracts::succeeded(result)) {
        if (slot < 0 || slot > UINT8_MAX) {
            VIRTBAG_LOG("store buy gate received invalid original slot=%d", slot);
            return StoreSaveSlotLookup::kError;
        }
        *out_slot = slot;
        return StoreSaveSlotLookup::kFound;
    }
    if (result.code == kQolErrNotFound) return StoreSaveSlotLookup::kNotFound;
    VIRTBAG_LOG("store buy gate original lookup failed code=%d", result.code);
    return StoreSaveSlotLookup::kError;
}

bool store_extension_can_accept_item_locked(void* item) {
    if (item == nullptr || !extension_bag_session_admitted_locked()) return false;
    ensure_state_loaded_locked();

    std::array<uint8_t, virtual_bag::kSerializedItemBuffer> payload{};
    int payload_size = 0;
    int category = 0;
    if (!serialize_item_payload_locked(item, &payload, &payload_size) ||
        !read_item_category(item, &category)) {
        VIRTBAG_LOG("store buy gate extension preflight rejected item=%p reason=source-read", item);
        return false;
    }
    int canonical_count = static_cast<int>(virtual_bag_canonical_count(item, 1));
    if (canonical_count <= 0) canonical_count = 1;
    virtual_bag::Item source{};
    source.category = category;
    source.count = canonical_count;
    source.payload = payload;
    source.payload_size = payload_size;

    const virtual_bag::PlacePlan plan = virtual_bag::place_plan(
        runtime_state().objects.bag.items, runtime_state().objects.bag.types,
        runtime_state().objects.bag.capacities, source, /*allow_new_slot=*/true,
        virtual_bag_extended_mode(item), virtual_bag_category_uses_stack_count,
        &module_slot_is_assignable_locked);
    return plan.outcome == virtual_bag::PlaceOutcome::kMerged ||
           plan.outcome == virtual_bag::PlaceOutcome::kPlacedNewSlot;
}
}  // namespace

uint8_t store_buy_find_slot_gate(void* item, void* out_slot) {
    const ::qol::contracts::InFlightGuard in_flight_guard(
        runtime_state().host.sdk == nullptr ? nullptr : &runtime_state().host.sdk->in_flight,
        {"patch:P_EXT_STORE_BUY_GATE_1"});
    int32_t slot = -1;
    const StoreSaveSlotLookup lookup = store_find_original_slot_with_real_capacity(item, &slot);
    if (lookup == StoreSaveSlotLookup::kFound) {
        if (out_slot != nullptr) {
            const uint8_t encoded_slot = static_cast<uint8_t>(slot);
            std::memcpy(out_slot, &encoded_slot, sizeof(encoded_slot));
        }
        return 1;
    }
    if (lookup == StoreSaveSlotLookup::kError) return 0;
    if (!runtime_state().enabled.load()) return 0;
    std::lock_guard<std::mutex> lock(runtime_state().mtx);
    return store_extension_can_accept_item_locked(item) ? 1 : 0;
}
