#include "plugins/extension_bag/api/bag_source.h"
#include "plugins/extension_bag/core/ui_state.h"
#include "game_ui_virtbag.h"
#include "plugins/extension_bag/runtime/state_access.h"
#include "plugins/extension_bag/runtime/reentry_lock.h"
#include "plugins/extension_bag/runtime/inventory_access.h"
#include "plugins/extension_bag/runtime/item_access.h"
#include "plugins/extension_bag/runtime/inventory_action.h"
#include "plugins/extension_bag/core/lifecycle.h"
#include "plugins/extension_bag/runtime/ownership_port.h"
#include "plugins/extension_bag/core/projection.h"
#include "plugins/extension_bag/runtime/shared.h"
#include "plugins/extension_bag/transaction/transaction.h"
#include "plugins/extension_bag/core/host_adapter.h"
#include "plugins/extension_bag/ui/host_ui.h"
#include "contracts/game_host.h"
#include "contracts/host_sdk.h"
#include "contracts/ui.h"
#include "contracts/ownership.h"
#include "contracts/inventory_refresh.h"
#include "contracts/item_facts.h"
#include "contracts/module_save_port.h"
#include "contracts/popup_enter_alias.h"
#include "plugins/extension_bag/model/equip_rules.h"
#include "plugins/extension_bag/core/geometry.h"
#include "plugins/extension_bag/runtime/logging.h"
#include "plugins/extension_bag/core/move_merge.h"
#include "plugins/extension_bag/model/ownership_ledger.h"
#include "plugins/extension_bag/model/item_handle_custody.h"
#include "plugins/extension_bag/model/sale_transaction.h"
#include "plugins/extension_bag/model/draw_state_lock.h"
#include "plugins/extension_bag/model/main_menu_cleanup_edge.h"
#include "plugins/extension_bag/plugin.h"
#include "plugins/extension_bag/model/store_sell_rules.h"

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
    if (::qol::extension_bag::access::host().sdk != nullptr && ::qol::extension_bag::access::host().sdk->ui != nullptr && ::qol::extension_bag::access::host().sdk->ui->show_text_popup != nullptr) {
        (void)::qol::extension_bag::access::host().sdk->ui->show_text_popup(static_cast<int32_t>(text_id));
    }
}

}  // namespace

int64_t store_button_sell_exe_wrapper() {
    using ButtonSellFn = int64_t (*)();
    const auto backup = reinterpret_cast<ButtonSellFn>(
        qol::extension_bag::ui_state().ui.backup_store_button_sell_exe);
    if (backup == nullptr) return 1;
    if (!::qol::extension_bag::access::enabled().load()) return backup();

    void* item = nullptr;
    int count = 0;
    bool module_item = false;
    {
        ::qol::extension_bag::ReentryLock lock;
        if (qol::extension_bag::ui_state().store.view_installed && ::qol::extension_bag::access::host().game != nullptr &&
            ::qol::extension_bag::access::host().inventory_extras->read.cursor_item != nullptr) {
            void* ctrl = extension_bag_ui_root(qol::contracts::QolUiRootKind::kStoreItemRoot);
            void* cursor = nullptr;
            if (ctrl != nullptr) {
                (void)::qol::extension_bag::access::host().inventory_extras->read.cursor_item(ctrl, &cursor);
            }
            item = cursor != nullptr ? extension_bag_ui_item(cursor) : nullptr;
            int bag = -1;
            int slot = -1;
            if (item != nullptr && module_slot_of_item_locked(item, &bag, &slot) &&
                bag == qol::extension_bag::ui_state().store.view_bag && virtual_bag::valid_index(bag) &&
                slot >= 0 && slot < virtual_bag::kSlotCount &&
                qol::extension_bag::ui_state().objects.module_objects[bag][slot] == item) {
                const virtual_bag::Item& descriptor =
                    ::qol::extension_bag::access::bag().items[bag][slot];
                count = static_cast<int>(virtual_bag_effective_count(
                    item, static_cast<uint32_t>(descriptor.count)));
                module_item = descriptor.count > 0;
            }
        }
    }
    VIRTBAG_LOG("store button sell view=%d module=%d count=%d item=%p",
                qol::extension_bag::ui_state().store.view_installed ? 1 : 0, module_item ? 1 : 0, count, item);
    if (!module_item || count <= 1) return backup();

    const auto* ui = ::qol::extension_bag::access::host().write_port->ui.create_item_count_popup != nullptr
                         ? &::qol::extension_bag::access::host().write_port->ui
                         : nullptr;
    void* callback = ui != nullptr && ui->sell_ok_input_item_count != nullptr
                         ? ui->sell_ok_input_item_count()
                         : nullptr;
    if (ui == nullptr || callback == nullptr ||
        !qol::contracts::succeeded(
            ui->create_item_count_popup(1, item, 1, count, callback))) {
        VIRTBAG_LOG("store quantity dialog unavailable; falling back count=%d", count);
        return backup();
    }
    return 1;
}

int64_t store_sell_item_wrapper(int64_t displayed_price) {
    const ::qol::contracts::InFlightGuard in_flight_guard(
        ::qol::extension_bag::access::host().sdk == nullptr ? nullptr : &::qol::extension_bag::access::host().sdk->in_flight,
        {"hook:UIStore_SellItem"});
    if (qol::extension_bag::ui_state().ui.backup_store_sell_item == nullptr) return 0;
    if (!::qol::extension_bag::access::enabled().load()) return reinterpret_cast<StoreSellItemFn>(qol::extension_bag::ui_state().ui.backup_store_sell_item)(displayed_price);

    void* item = nullptr;
    int bag = -1;
    int slot = -1;
    int view = 0;
    bool item_encoded = false;
    bool module_item = false;
    bool reject = false;
    qol::contracts::QolItemHandle expected_item{};
    qol::store_sell::Plan plan{};
    {
        ::qol::extension_bag::ReentryLock lock;
        if (qol::extension_bag::ui_state().store.view_installed && ::qol::extension_bag::access::host().game != nullptr &&
            ::qol::extension_bag::access::host().inventory_extras->read.cursor_item != nullptr &&
            ::qol::extension_bag::access::host().item_facts != nullptr && ::qol::extension_bag::access::host().item_facts->sell_price != nullptr) {
            void* ctrl = extension_bag_ui_root(qol::contracts::QolUiRootKind::kStoreItemRoot);
            void* cursor = nullptr;
            if (ctrl != nullptr) {
                (void)::qol::extension_bag::access::host().inventory_extras->read.cursor_item(ctrl, &cursor);
            }
            item = cursor != nullptr ? extension_bag_ui_item(cursor) : nullptr;
            if (item != nullptr && module_slot_of_item_locked(item, &bag, &slot)) {
                module_item = true;
                const bool identity_ok =
                    bag == qol::extension_bag::ui_state().store.view_bag && virtual_bag::valid_index(bag) && slot >= 0 &&
                    slot < virtual_bag::kSlotCount && qol::extension_bag::ui_state().objects.module_objects[bag][slot] == item &&
                    module_object_release_allowed_locked(bag, slot, item, "store sell", false,
                                                         nullptr);
                const virtual_bag::Item& descriptor = ::qol::extension_bag::access::bag().items[bag][slot];
                const bool descriptor_ok =
                    descriptor.category > 0 && descriptor.count > 0 &&
                    qol::extension_bag::ui_state().objects.module_object_categories[bag][slot] == descriptor.category &&
                    qol::extension_bag::ui_state().objects.module_object_hashes[bag][slot] == virtual_bag::payload_hash(descriptor);
                if (!identity_ok || !descriptor_ok) {
                    reject = true;
                } else {
                    if (!extension_bag_item_handle_for_pointer(item, &expected_item)) {
                        reject = true;
                    }
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
    if (!module_item) return reinterpret_cast<StoreSellItemFn>(qol::extension_bag::ui_state().ui.backup_store_sell_item)(displayed_price);
    if (reject) {
        VIRTBAG_LOG("store sell reject reason=identity_or_plan bag=%d slot=%d", bag, slot);
        store_sell_popup(0xc);
        return 0;
    }

    const int count = plan.kind == qol::store_sell::PlanKind::kWhole ? 1 : plan.count;
    const int64_t price = extension_bag_sell_price(item, count, false);
    if (price < 0 || price != displayed_price) {
        VIRTBAG_LOG("store sell reject reason=price_mismatch bag=%d slot=%d count=%d price=%lld displayed=%lld",
                    bag, slot, count, static_cast<long long>(price),
                    static_cast<long long>(displayed_price));
        store_sell_popup(0xc);
        return 0;
    }
    const auto* action = ::qol::features::extension_bag::inventory_action_api();
    if (action == nullptr) {
        store_sell_popup(0xc);
        return 0;
    }
    const auto result = action->sell({qol::features::extension_bag::kExtensionBagSpaceId, bag, slot}, expected_item, count, "store");
    if (result.status != qol::contracts::InventorySellStatus::kOk) {
        VIRTBAG_LOG("store sell action rejected bag=%d slot=%d count=%d status=%d",
                    bag, slot, count, static_cast<int>(result.status));
        store_sell_popup(0xc);
        return 0;
    }
    {
        ::qol::extension_bag::ReentryLock lock;
        if (qol::extension_bag::ui_state().store.view_installed &&
            qol::extension_bag::ui_state().store.view_bag == bag) {
            // The core action changed sidecar state through QolBagSource; this refresh only
            // rebinds the store's independent display projection.
            store_refresh_projection_locked();
        }
    }
    store_sell_popup(0x1f);
    VIRTBAG_LOG("store sell committed through inventory.action bag=%d slot=%d count=%d view=%d price=%lld",
                bag, slot, count, view, static_cast<long long>(result.price));
    return 0;
}

namespace {
enum class StoreSaveSlotLookup : uint8_t { kFound, kNotFound, kError };

StoreSaveSlotLookup store_find_original_slot_with_real_capacity(void* item,
                                                                 int32_t* out_slot) {
    if (item == nullptr || out_slot == nullptr || ::qol::extension_bag::access::host().game == nullptr ||
        ::qol::extension_bag::access::host().inventory_extras == nullptr || ::qol::extension_bag::access::host().inventory_extras->read.find_save_slot == nullptr) {
        VIRTBAG_LOG("store buy gate original lookup unavailable item=%p", item);
        return StoreSaveSlotLookup::kError;
    }

    ::qol::extension_bag::ReentryLock lock;
    const bool projected = qol::extension_bag::ui_state().store.view_installed;
    const int original_bag = qol::extension_bag::ui_state().store.window_original_bag;
    const uint64_t view_generation = qol::extension_bag::ui_state().store.view_generation;
    uint32_t projected_size_word = 0;
    if (projected) {
        if (!virtual_bag::valid_original_transaction_bag(original_bag) ||
            !bag_size_word_locked(original_bag, &projected_size_word)) {
            VIRTBAG_LOG("store buy gate cannot read projected capacity window=%d", original_bag);
            return StoreSaveSlotLookup::kError;
        }
        if (!set_bag_size_word_locked(original_bag, qol::extension_bag::ui_state().store.original_size_word)) {
            VIRTBAG_LOG("store buy gate cannot restore original capacity window=%d", original_bag);
            return StoreSaveSlotLookup::kError;
        }
    }

    lock.unlock();
    int32_t slot = -1;
    const QolResult result = ::qol::extension_bag::access::host().inventory_extras->read.find_save_slot(item, &slot);
    lock.lock();

    if (projected) {
        if (qol::extension_bag::ui_state().store.view_generation != view_generation || !qol::extension_bag::ui_state().store.view_installed ||
            qol::extension_bag::ui_state().store.window_original_bag != original_bag) {
            VIRTBAG_LOG("store buy gate view changed during original lookup generation=%llu->%llu",
                        static_cast<unsigned long long>(view_generation),
                        static_cast<unsigned long long>(qol::extension_bag::ui_state().store.view_generation));
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
        ::qol::extension_bag::access::bag().items, ::qol::extension_bag::access::bag().types,
        ::qol::extension_bag::access::bag().capacities, source, /*allow_new_slot=*/true,
        virtual_bag_extended_mode(item), virtual_bag_category_uses_stack_count,
        &module_slot_is_assignable_locked);
    return plan.outcome == virtual_bag::PlaceOutcome::kMerged ||
           plan.outcome == virtual_bag::PlaceOutcome::kPlacedNewSlot;
}
}  // namespace

uint8_t store_buy_find_slot_gate(void* item, void* out_slot) {
    const ::qol::contracts::InFlightGuard in_flight_guard(
        ::qol::extension_bag::access::host().sdk == nullptr ? nullptr : &::qol::extension_bag::access::host().sdk->in_flight,
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
    if (!::qol::extension_bag::access::enabled().load()) return 0;
    ::qol::extension_bag::ReentryLock lock;
    return store_extension_can_accept_item_locked(item) ? 1 : 0;
}
