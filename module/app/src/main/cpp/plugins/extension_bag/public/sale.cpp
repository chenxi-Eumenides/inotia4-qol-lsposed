#include "game_ui_virtbag.h"
#include "plugins/extension_bag/interface/state_access.h"
#include "plugins/extension_bag/runtime/reentry_lock.h"

#include "contracts/game_host.h"
#include "contracts/host_sdk.h"
#include "contracts/ui.h"
#include "contracts/ownership.h"
#include "contracts/inventory_refresh.h"
#include "contracts/item_facts.h"
#include "plugins/extension_bag/model/equip_rules.h"
#include "plugins/extension_bag/extension_bag_port.h"
#include "plugins/extension_bag/model/move_merge.h"
#include "plugins/extension_bag/model/ownership_ledger.h"
#include "plugins/extension_bag/model/item_handle_custody.h"
#include "plugins/extension_bag/model/sale_transaction.h"
#include "plugins/extension_bag/model/draw_state_lock.h"
#include "plugins/extension_bag/model/main_menu_cleanup_edge.h"
#include "plugins/extension_bag/model/virtual_bag_state.h"
#include "contracts/module_save_port.h"
#include "contracts/popup_enter_alias.h"
#include "plugins/extension_bag/extension_bag_context.h"
#include "plugins/extension_bag/extension_bag_geometry.h"
#include "plugins/extension_bag/runtime/logging.h"
#include "plugins/extension_bag/runtime/item_access.h"
#include "plugins/extension_bag/runtime/lifecycle.h"
#include "plugins/extension_bag/runtime/ownership.h"
#include "plugins/extension_bag/runtime/projection.h"
#include "plugins/extension_bag/runtime/shared.h"
#include "plugins/extension_bag/runtime/tabs.h"
#include "plugins/extension_bag/ui/host_ui.h"
#include "plugins/extension_bag/plugin.h"
#include "plugins/extension_bag/ui/store_sell_rules.h"

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

bool virtual_bag_identify_native_item(void* item, int* out_bag, int* out_slot) {
    if (item == nullptr || out_bag == nullptr || out_slot == nullptr) return false;
    ::qol::extension_bag::ReentryLock lock;
    return module_slot_of_item_locked(item, out_bag, out_slot);
}
void* virtual_bag_find_native_item(int category) {
    if (category <= 0) return nullptr;
    ::qol::extension_bag::ReentryLock lock;
    ensure_state_loaded_locked();
    for (int bag = 0; bag < virtual_bag::active_extension_bags(); ++bag) {
        for (int slot = 0; slot < ::qol::extension_bag::access::bag().capacities[bag]; ++slot) {
            const virtual_bag::Item& descriptor = ::qol::extension_bag::access::bag().items[bag][slot];
            if (descriptor.category != category || descriptor.count <= 0) continue;
            void* item = module_item_locked(bag, slot);
            if (item != nullptr) return item;
            VIRTBAG_LOG("native find extension materialize failed bag=%d slot=%d category=%d",
                        bag, slot, category);
        }
    }
    return nullptr;
}

bool virtual_bag_remove_native_item(void* item, void* raw_use_token) {
    if (item == nullptr) return false;
    ::qol::extension_bag::ReentryLock lock;
    ensure_state_loaded_locked();
    int bag = -1;
    int slot = -1;
    if (!module_slot_of_item_locked(item, &bag, &slot) ||
        ::qol::extension_bag::access::objects().module_objects[bag][slot] != item) {
        return false;
    }
    ModuleUseToken* use_token = static_cast<ModuleUseToken*>(raw_use_token);
    if (!module_object_release_allowed_locked(
            bag, slot, item, "native remove", true, use_token)) {
        return false;
    }
    const virtual_bag::Item previous_item = ::qol::extension_bag::access::bag().items[bag][slot];
    ::qol::extension_bag::access::bag().items[bag][slot] = {};
    extension_bag_mark_dirty_locked();
    if (!free_module_object_locked(bag, slot, use_token)) {
        ::qol::extension_bag::access::bag().items[bag][slot] = previous_item;
        VIRTBAG_LOG("native remove rejected object release bag=%d slot=%d", bag, slot);
        return false;
    }
    const bool persisted = persist_state_locked();
    if (::qol::extension_bag::access::objects().module_view_installed && ::qol::extension_bag::access::objects().module_view_index == bag) {
        refresh_module_item_area_locked(bag);
    }
    VIRTBAG_LOG("native remove extension bag=%d slot=%d persisted=%d", bag, slot,
                persisted ? 1 : 0);
    return true;
}
namespace {

struct SaleReleaseContext {
    qol::contracts::QolItemHandle expected;
};

bool sale_clear_projection(void*, void* control, void* item) {
    if (control == nullptr || extension_bag_ui_item(control) != item) return false;
    return extension_bag_ui_set_control_item(control, nullptr);
}

bool sale_restore_projection(void*, void* control, void* item) {
    if (control == nullptr || item == nullptr) return false;
    const void* current = extension_bag_ui_item(control);
    if (current != nullptr && current != item) return false;
    return extension_bag_ui_set_control_item(control, item);
}

bool sale_release_object(void* raw, void* item, qol::contracts::QolItemHandle expected) {
    auto* context = static_cast<SaleReleaseContext*>(raw);
    if (context == nullptr || item == nullptr || expected.slot != context->expected.slot ||
        expected.generation != context->expected.generation ||
        !extension_bag_sale_item_begin_release(expected, item)) return false;
    const bool freed = extension_free_item(item);
    if (!extension_bag_sale_item_finish_release(expected, freed)) {
        QOL_LOG_ERROR(QolDomain::kExtensionBag,
                      "ERROR sale custody finalize failed slot=%u generation=%u freed=%d",
                      expected.slot, expected.generation, freed ? 1 : 0);
        // A successful pool free is irreversible; report it as committed and leave an
        // ownership diagnostic rather than refunding against a removed object.
        return freed;
    }
    return freed;
}

}  // namespace

bool virtual_bag_consume_native_item(void* item, void* raw_use_token) {
    if (item == nullptr) return false;
    void* detail_control = nullptr;
    void* detail_item = nullptr;
    {
        ::qol::extension_bag::ReentryLock lock;
        ensure_state_loaded_locked();
        int bag = -1;
        int slot = -1;
        if (!module_slot_of_item_locked(item, &bag, &slot) || ::qol::extension_bag::access::objects().module_objects[bag][slot] != item) {
            return false;
        }
        ModuleUseToken* use_token = static_cast<ModuleUseToken*>(raw_use_token);
        if (!module_object_release_allowed_locked(
                bag, slot, item, "native consume", true,
                use_token)) {
            return false;
        }
        // 消耗前数量取模式视图（R-47 决策 b）：启用态 canonical、关闭态 b，
        // 与 getter 的 observed_before 同域参与扣减判定。
        const int before_count = static_cast<int>(virtual_bag_effective_count(
            item, static_cast<uint32_t>(::qol::extension_bag::access::bag().items[bag][slot].count)));
        int observed_before = before_count;
        (void)read_item_cumulate_count(item, &observed_before);
        if (!consume_extension_item_after_native_locked(
                bag, slot, item, before_count, observed_before, "native_consume", use_token)) {
            return false;
        }
        extension_bag_mark_dirty_locked();
        persist_state_locked();
        if (::qol::extension_bag::access::objects().module_view_installed && ::qol::extension_bag::access::objects().module_view_index == bag) {
            refresh_module_item_area_locked(bag);
        }
        if (::qol::extension_bag::access::ui().pending_jewel_detail_item != nullptr &&
            virtual_bag::valid_index(::qol::extension_bag::access::objects().module_view_index) &&
            ::qol::extension_bag::access::ui().pending_jewel_detail_bag == ::qol::extension_bag::access::objects().module_view_index &&
            ::qol::extension_bag::access::ui().pending_jewel_detail_slot >= 0 &&
            ::qol::extension_bag::access::ui().pending_jewel_detail_slot < ::qol::extension_bag::access::bag().capacities[::qol::extension_bag::access::objects().module_view_index] &&
            ::qol::extension_bag::access::objects().module_view_installed && ::qol::extension_bag::access::objects().projected_item_root != nullptr &&
            ::qol::extension_bag::access::host().sdk != nullptr && ::qol::extension_bag::access::host().sdk->ui != nullptr) {
            detail_control = valid_child_locked(::qol::extension_bag::access::objects().projected_item_root,
                                                ::qol::extension_bag::access::ui().pending_jewel_detail_slot);
            detail_item = ::qol::extension_bag::access::ui().pending_jewel_detail_item;
        }
        ::qol::extension_bag::access::ui().pending_jewel_detail_item = nullptr;
        ::qol::extension_bag::access::ui().pending_jewel_detail_bag = -1;
        ::qol::extension_bag::access::ui().pending_jewel_detail_slot = -1;
        VIRTBAG_LOG("native consume extension bag=%d slot=%d before=%d", bag, slot, before_count);
    }
    if (detail_control != nullptr && ::qol::extension_bag::access::host().sdk != nullptr && ::qol::extension_bag::access::host().sdk->ui != nullptr) {
        (void)extension_bag_ui_set_control_item(detail_control, detail_item);
    }
    if (detail_control != nullptr && ::qol::extension_bag::access::host().game != nullptr &&
        ::qol::extension_bag::access::host().write_port->ui.make_item_description != nullptr) {
        (void)::qol::extension_bag::access::host().write_port->ui.make_item_description(detail_control);
    }
    return true;
}
void* virtual_bag_current_use_token() {
    return g_active_module_use_token;
}
QolResult virtual_bag_remove_for_sale(int32_t logical_bag, int32_t slot,
                                      qol::contracts::QolItemHandle expected_item) {
    const int bag = virtual_bag::extension_internal_bag(logical_bag);
    if (!virtual_bag::valid_index(bag) || slot < 0 || slot >= virtual_bag::kSlotCount) {
        return qol::contracts::error(kQolErrInvalidArgument, "invalid extension sale slot", 27);
    }
    ::qol::extension_bag::ReentryLock lock;
    if (!::qol::extension_bag::access::enabled().load(std::memory_order_acquire) ||
        !extension_bag_session_admitted_locked()) {
        return qol::contracts::error(kQolErrNotReady, "extension sale session unavailable", 34);
    }
    if (object_touch_window_active_locked()) {
        return qol::contracts::error(kQolErrBusy, "extension sale during touch window", 34);
    }

    virtual_bag::Item& descriptor = ::qol::extension_bag::access::bag().items[bag][slot];
    void* const item = ::qol::extension_bag::access::objects().module_objects[bag][slot];
    int item_bag = -1;
    int item_slot = -1;
    qol::contracts::QolItemHandle current_item{};
    const bool located = item != nullptr && module_slot_of_item_locked(item, &item_bag, &item_slot) &&
                         item_bag == bag && item_slot == slot;
    const bool custody_matches = located &&
        extension_bag_sale_item_matches(expected_item, item, &current_item);
    const bool descriptor_match = located && descriptor.category > 0 && descriptor.count > 0 &&
                                  ::qol::extension_bag::access::objects().module_object_categories[bag][slot] == descriptor.category;
    const bool hash_matches = descriptor_match &&
        ::qol::extension_bag::access::objects().module_object_hashes[bag][slot] == virtual_bag::payload_hash(descriptor);
    const bool release_allowed = located &&
        module_object_release_allowed_locked(bag, slot, item, "inventory sale", false, nullptr);
    if (!descriptor_match || !hash_matches || !custody_matches || !release_allowed) {
        return qol::contracts::error(kQolErrConflict, "extension sale identity stale", 29);
    }
    void* projection_control = nullptr;
    if (::qol::extension_bag::access::objects().module_view_installed && ::qol::extension_bag::access::objects().module_view_index == bag) {
        if (::qol::extension_bag::access::objects().projected_item_root == nullptr ||
            (projection_control = valid_child_locked(::qol::extension_bag::access::objects().projected_item_root, slot)) == nullptr ||
            extension_bag_ui_item(projection_control) != item) {
            return qol::contracts::error(kQolErrConflict, "extension sale projection stale", 31);
        }
    }

    ModuleUseState& use = ::qol::extension_bag::access::objects().module_object_use[bag][slot];
    const int capacity = ::qol::extension_bag::access::bag().capacities[bag];
    qol::features::extension_bag::model::SaleSlotState state{};
    state.logical_bag = logical_bag;
    state.slot = slot;
    state.capacity = capacity;
    state.session_admitted = extension_bag_session_admitted_locked();
    state.descriptor_matches_object = located && ::qol::extension_bag::access::objects().module_objects[bag][slot] == item;
    state.descriptor_hash_matches = hash_matches;
    state.handle_custody_matches = custody_matches;
    state.expected_item = expected_item;
    state.current_item = current_item;
    state.descriptor = &descriptor;
    state.object = &::qol::extension_bag::access::objects().module_objects[bag][slot];
    state.object_category = &::qol::extension_bag::access::objects().module_object_categories[bag][slot];
    state.object_hash = &::qol::extension_bag::access::objects().module_object_hashes[bag][slot];
    state.object_handle = &::qol::extension_bag::access::objects().module_object_handles[bag][slot];
    state.ledger_handle = ::qol::extension_bag::access::objects().module_object_handles[bag][slot];
    state.ledger = &::qol::extension_bag::access::objects().ownership_ledger;
    state.use_active = &use.active;
    state.use_consumed = &use.consumed;
    state.use_pending_release = &use.pending_release;
    state.use_item = &use.item;
    state.use_generation = &use.generation;
    state.use_owner = &use.owner;
    state.dirty = &::qol::extension_bag::access::persistence().dirty;
    state.revision = &::qol::extension_bag::access::persistence().revision;
    state.projection_control = projection_control;
    SaleReleaseContext release_context{expected_item};
    const qol::features::extension_bag::model::SaleRemovalOps ops{
        &release_context, &sale_clear_projection,
        &sale_restore_projection, &sale_release_object};
    const auto removed = qol::features::extension_bag::model::remove_for_sale(state, ops);
    if (removed != qol::features::extension_bag::model::SaleRemovalResult::kRemoved &&
        removed != qol::features::extension_bag::model::SaleRemovalResult::kRemovedLedgerDiagnostic) {
        const int error = removed == qol::features::extension_bag::model::SaleRemovalResult::kNotAdmitted
                              ? kQolErrNotReady
                          : removed == qol::features::extension_bag::model::SaleRemovalResult::kActiveUse
                              ? kQolErrBusy
                              : kQolErrConflict;
        QOL_LOG_WARN(QolDomain::kExtensionBag,
                     "extension sale removal rejected logical_bag=%d slot=%d result=%u",
                     logical_bag, slot, static_cast<unsigned>(removed));
        return qol::contracts::error(error, "extension sale removal rejected", 31);
    }
    if (removed == qol::features::extension_bag::model::SaleRemovalResult::kRemovedLedgerDiagnostic) {
        QOL_LOG_ERROR(QolDomain::kExtensionBag,
                      "ERROR sale removed with ownership ledger diagnostic logical_bag=%d slot=%d",
                      logical_bag, slot);
    }
    if (::qol::extension_bag::access::ui().extension_desc_item == item) {
        reset_extension_desc_item_hooks_locked();
        ::qol::extension_bag::access::ui().extension_desc_item = nullptr;
        ::qol::extension_bag::access::ui().extension_desc_item_bag = -1;
        ::qol::extension_bag::access::ui().extension_desc_item_slot = -1;
        clear_original_desc_locked();
    }
    QOL_LOG_INFO(QolDomain::kExtensionBag, "extension sale removed logical_bag=%d slot=%d",
                 logical_bag, slot);
    return qol::contracts::ok();
}
bool virtual_bag_handle_confirm_use_item(void* item) {
    return extension_confirm_use_item(item);
}

bool virtual_bag_handle_use_mercenary_seal() {
    return extension_use_mercenary_seal();
}
