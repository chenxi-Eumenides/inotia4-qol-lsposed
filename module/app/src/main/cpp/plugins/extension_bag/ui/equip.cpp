#include "plugins/extension_bag/transaction/transaction.h"
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
#include "plugins/extension_bag/core/tabs.h"
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
#include "plugins/extension_bag/model/draw_state_lock.h"
#include "plugins/extension_bag/model/main_menu_cleanup_edge.h"
#include "plugins/extension_bag/plugin.h"

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

void* extension_menu_character() {
    if (::qol::extension_bag::access::host().game == nullptr || ::qol::extension_bag::access::host().character_extras->read.menu_character == nullptr) {
        return nullptr;
    }
    void* character = nullptr;
    return qol::contracts::succeeded(
               ::qol::extension_bag::access::host().character_extras->read.menu_character(&character))
               ? character
               : nullptr;
}

ExtensionBagEquipResult equip_extension_bag_item_locked(::qol::extension_bag::ReentryLock& lock,
                                                        int internal_bag, void* item) {
    if (!virtual_bag::valid_index(internal_bag) || item == nullptr) {
        return ExtensionBagEquipResult::kReject;
    }
    if (::qol::extension_bag::access::bag().types[internal_bag] != 0) {
        return ExtensionBagEquipResult::kReject;
    }
    int category = 0;
    int src_bag = -1;
    int src_slot = -1;
    for (int bag = 0; bag < virtual_bag::kOriginalTransactionBagCount && src_bag < 0; ++bag) {
        for (int slot = 0; slot < virtual_bag::kSlotCount; ++slot) {
            void* current = nullptr;
            if (inventory_slot_locked(bag, slot, &current) && current == item) {
                src_bag = bag;
                src_slot = slot;
                break;
            }
        }
    }
    const bool module_source = src_bag < 0 && module_slot_of_item_locked(item, &src_bag, &src_slot);
    if (module_source) {
        category = ::qol::extension_bag::access::bag().items[src_bag][src_slot].category;
    } else {
        if (src_bag < 0 || !read_item_category(item, &category)) {
            return ExtensionBagEquipResult::kReject;
        }
    }
    if (!extension_bag_runtime_is_backpack_category(category)) return ExtensionBagEquipResult::kReject;
    const auto* write_api = extension_bag_write_api();
    if (!module_source &&
        (write_api == nullptr || write_api->inventory.remove_item_direct == nullptr ||
         write_api->inventory.save_item_on_empty == nullptr ||
         write_api->inventory.load_item == nullptr || write_api->inventory.free_item == nullptr)) {
        return ExtensionBagEquipResult::kFailed;
    }

    std::array<uint8_t, virtual_bag::kSerializedItemBuffer> payload{};
    int payload_size = 0;
    if (module_source) {
        const virtual_bag::Item& source = ::qol::extension_bag::access::bag().items[src_bag][src_slot];
        payload = source.payload;
        payload_size = source.payload_size;
    } else if (!serialize_item_payload_locked(item, &payload, &payload_size)) {
        QOL_LOG_WARN(QolDomain::kExtensionBag, "extension equip reject reason=payload_serialize_failed src=%d/%d", src_bag,
                    src_slot);
        return ExtensionBagEquipResult::kFailed;
    }

    if (module_source) {
        const virtual_bag::Item source = ::qol::extension_bag::access::bag().items[src_bag][src_slot];
        const uint8_t previous_type = ::qol::extension_bag::access::bag().types[internal_bag];
        if (!virtual_bag::equip_bag(&::qol::extension_bag::access::bag(), internal_bag, category)) {
            return ExtensionBagEquipResult::kReject;
        }
        ::qol::extension_bag::access::bag().items[src_bag][src_slot] = {};
        extension_bag_mark_dirty_locked();
        if (persist_state_locked()) {
            if (!free_module_object_locked(src_bag, src_slot)) {
                ::qol::extension_bag::access::bag().types[internal_bag] = previous_type;
                ::qol::extension_bag::access::bag().items[src_bag][src_slot] = source;
                ::qol::extension_bag::access::bag().isolation_now_ms = extension_bag_ui_isolation_now_ms_locked();
                virtual_bag::normalize(&::qol::extension_bag::access::bag());
                persist_state_locked();
                QOL_LOG_WARN(QolDomain::kExtensionBag, "extension equip rejected source release bag=%d slot=%d",
                            src_bag, src_slot);
                return ExtensionBagEquipResult::kFailed;
            }
            QOL_LOG_INFO(QolDomain::kExtensionBag, "extension equip ok source=extension bag=%d category=%d src=%d/%d",
                        internal_bag, category, src_bag, src_slot);
            return ExtensionBagEquipResult::kOk;
        }
        ::qol::extension_bag::access::bag().types[internal_bag] = 0;
        ::qol::extension_bag::access::bag().items[src_bag][src_slot] = source;
        ::qol::extension_bag::access::bag().isolation_now_ms = extension_bag_ui_isolation_now_ms_locked();
        virtual_bag::normalize(&::qol::extension_bag::access::bag());
        persist_state_locked();
        return ExtensionBagEquipResult::kPersistFailed;
    }

    // P4.3：移除前先分配保管 handle（失败时物品仍在原版库存，无丢失风险）。
    uint32_t custody_handle = 0;
    if (ownership::allocate(&qol::extension_bag::ui_state().objects.ownership_ledger, &custody_handle) != ownership::Outcome::kOk) {
        QOL_LOG_WARN(QolDomain::kExtensionBag, "extension equip ledger exhausted src=%d/%d", src_bag, src_slot);
        return ExtensionBagEquipResult::kFailed;
    }

    if (!remove_item_direct_unlocked(lock, src_bag, src_slot, item, "extension equip")) {
        QOL_LOG_WARN(QolDomain::kExtensionBag, "extension equip remove verify failed bag=%d slot=%d", src_bag, src_slot);
        // 物品仍被 INVEN 持有（remove 未生效）→ 不得释放，记 inventory 终态。
        handover_tracked_item_locked(custody_handle, item,
                                     "equip remove verify failed; retained in inventory");
        return ExtensionBagEquipResult::kFailed;
    }
    // 移除已确认：此后任何失败（state 拒绝 / persist 失败）都必须回插物品，
    // 否则真实丢失。equip_bag 前置已查 types==0，此处拒绝仅剩理论可能。
    const bool state_equipped =
        virtual_bag::equip_bag(&::qol::extension_bag::access::bag(), internal_bag, category);
    if (state_equipped) {
        extension_bag_mark_dirty_locked();
        if (persist_state_locked()) {
            // P4.3：装备对象已被状态 payload 取代（原指针无人引用），终止保管；
            // drop 路径处于触摸窗口内 → 隔离区，API 路径窗口外 → 真释放。
            retire_custody_item_locked(custody_handle, item, "equip custody retired");
            QOL_LOG_INFO(QolDomain::kExtensionBag, "extension equip ok bag=%d category=%d src=%d/%d", internal_bag,
                        category, src_bag, src_slot);
            return ExtensionBagEquipResult::kOk;
        }
        ::qol::extension_bag::access::bag().types[internal_bag] = 0;
        ::qol::extension_bag::access::bag().isolation_now_ms = extension_bag_ui_isolation_now_ms_locked();
        virtual_bag::normalize(&::qol::extension_bag::access::bag());
    } else {
        QOL_LOG_WARN(QolDomain::kExtensionBag, "extension equip state reject bag=%d category=%d", internal_bag, category);
    }
    bool restored = false;
    uint32_t rebuilt_handle = 0;
    void* rebuilt = load_item_payload_tracked_locked(payload.data(), payload_size,
                                                     "equip_rollback", &rebuilt_handle);
    if (rebuilt != nullptr) {
        if (extension_save_item_on_empty(rebuilt, src_bag)) {
            handover_tracked_item_locked(rebuilt_handle, rebuilt, "equip rollback handover");
            restored = true;
        } else {
            release_tracked_item_locked(rebuilt_handle, rebuilt, "equip rollback insert failed");
        }
    }
    retire_custody_item_locked(custody_handle, item, "equip rollback custody retired");
    persist_state_locked();
    QOL_LOG_WARN(QolDomain::kExtensionBag, "extension equip persist failed bag=%d restored=%d", internal_bag,
                restored ? 1 : 0);
    return restored ? ExtensionBagEquipResult::kPersistFailed : ExtensionBagEquipResult::kFailed;
}

bool equip_module_item_on_character_locked(::qol::extension_bag::ReentryLock& lock,
                                           void* character, void* item, int source_bag,
                                           int source_slot) {
    if (!lock.owns_lock() || character == nullptr || item == nullptr ||
        !virtual_bag::valid_index(source_bag) || source_slot < 0 ||
        source_slot >= virtual_bag::kSlotCount ||
        qol::extension_bag::ui_state().objects.module_objects[source_bag][source_slot] != item ||
        ::qol::extension_bag::access::host().character_extras == nullptr ||
        ::qol::extension_bag::access::host().character_extras->read.can_equip == nullptr ||
        ::qol::extension_bag::access::host().character_extras->read.find_equip_slot == nullptr ||
        ::qol::extension_bag::access::host().character_extras->read.party_index == nullptr ||
        !qol::extension_bag::ui_state().objects.module_view_installed ||
        qol::extension_bag::ui_state().objects.module_view_index != source_bag ||
        qol::extension_bag::ui_state().objects.module_window_original_bag < 0 ||
        qol::extension_bag::ui_state().objects.module_window_original_bag >= 5) {
        return false;
    }
    if (!module_object_replacement_allowed_locked(source_bag, source_slot,
                                                   "character equip")) {
        return false;
    }
    if (!extension_bag_ui_read_char_can_equip(character, item)) return false;

    int role = -1;
    int equip_slot = -1;
    if (!qol::contracts::succeeded(
            ::qol::extension_bag::access::host().character_extras->read.party_index(
                character, &role)) ||
        role < 0 || !extension_bag_ui_read_char_find_equip_slot(character, item, &equip_slot) ||
        equip_slot < 0) {
        return false;
    }
    qol::contracts::QolItemHandle expected_item{};
    if (!extension_bag_item_handle_for_pointer(item, &expected_item)) return false;
    const auto* action = ::qol::features::extension_bag::inventory_action_api();
    if (action == nullptr) return false;

    lock.unlock();
    const auto outcome = action->equip(role, {2, source_bag, source_slot}, equip_slot);
    lock.lock();
    if (outcome.status != qol::contracts::InventoryActionStatus::kOk) {
        QOL_LOG_WARN(QolDomain::kExtensionBag,
                     "extension character equip action rejected role=%d bag=%d slot=%d equip_slot=%d status=%d",
                     role, source_bag, source_slot, equip_slot,
                     static_cast<int>(outcome.status));
        return false;
    }
    return true;
}

// 装备成功后的 UI 收尾（游戏线程、持锁；先例：draw_end/enter 路径的锁内
// UI 调用）：internal_bag 始终是扩展逻辑袋 0..4。若当前已在另一个扩展视图，
// 先复用 extension_bag_ui_handle_tab_click_locked 切到目标袋，使容量字和投影使用同一
// g_module_view_index，再刷新物品区；随后更新标签对象、复位拖动态并播放音效。
void finish_extension_equip_ui_locked(int internal_bag, void* moving_control) {
    bool switched_view = false;
    refresh_tab_bag_items_locked();
    void* tab = qol::extension_bag::ui_state().ui.extension_tab_buttons[internal_bag];
    if (tab != nullptr) {
        void* data = extension_bag_ui_data(tab);
        if (data != nullptr) {
            *reinterpret_cast<void**>(data) = qol::extension_bag::ui_state().ui.tab_bag_items[internal_bag];
        }
    }
    if (qol::extension_bag::ui_state().objects.module_view_installed && ::qol::extension_bag::access::bag().mode == virtual_bag::Mode::kModule &&
        qol::extension_bag::ui_state().objects.module_view_index != internal_bag) {
        extension_bag_ui_handle_tab_click_locked(internal_bag);
        switched_view = qol::extension_bag::ui_state().objects.module_view_index == internal_bag;
    }
    if (qol::extension_bag::ui_state().objects.module_view_installed && qol::extension_bag::ui_state().objects.module_view_index == internal_bag) {
        refresh_module_item_area_locked(internal_bag);
    } else if (::qol::extension_bag::access::host().game != nullptr && ::qol::extension_bag::access::host().write_port->ui.refresh_item_area != nullptr) {
        virtual_bag_call_original_refresh_item_area_with_guard();
    }
    extension_bag_ui_update_character_equip();
    if (moving_control != nullptr) reset_drag_state_locked(moving_control);
    if (!switched_view) extension_bag_ui_play_switch_sound();
}

bool try_equip_on_extension_tab_drop_locked(::qol::extension_bag::ReentryLock& lock,
                                            int index, void* moving_control) {
    if (!::qol::extension_bag::access::enabled().load() || !read_world_in_world()) return false;
    void* data = extension_bag_ui_data(moving_control);
    void* item = data != nullptr ? *reinterpret_cast<void**>(data) : nullptr;
    if (item == nullptr) return false;
    const ExtensionBagEquipResult result = equip_extension_bag_item_locked(lock, index, item);
    if (result != ExtensionBagEquipResult::kOk) {
        QOL_LOG_WARN(QolDomain::kExtensionBag, "extension tab drop equip result=%d index=%d", static_cast<int>(result),
                    index);
        return false;
    }
    finish_extension_equip_ui_locked(index, moving_control);
    QOL_LOG_INFO(QolDomain::kExtensionBag, "extension tab drop equipped index=%d", index);
    return true;
}

bool extension_source_should_equip_locked(int target_bag) {
    const auto& session = qol::extension_bag::ui_state().ui.extension_drag_session;
    if (!virtual_bag::valid_index(target_bag) ||
        !virtual_bag::valid_index(session.source_bag) ||
        session.source_slot < 0 || session.source_slot >= virtual_bag::kSlotCount) {
        return false;
    }
    const int category = ::qol::extension_bag::access::bag().items[session.source_bag][session.source_slot].category;
    return ::qol::extension_bag::access::bag().types[target_bag] == 0 && extension_bag_runtime_is_backpack_category(category);
}

int first_empty_extension_bag_locked() {
    for (int bag = 0; bag < virtual_bag::active_extension_bags(); ++bag) {
        if (::qol::extension_bag::access::bag().types[bag] == 0) return bag;
    }
    return -1;
}

bool equip_extension_source_on_tab_locked(::qol::extension_bag::ReentryLock& lock, int target_bag,
                                          int source_bag, int source_slot) {
    void* source_item = module_item_locked(source_bag, source_slot);
    if (source_item == nullptr) return false;
    const ExtensionBagEquipResult result =
        equip_extension_bag_item_locked(lock, target_bag, source_item);
    if (result != ExtensionBagEquipResult::kOk) {
        QOL_LOG_WARN(QolDomain::kExtensionBag, "extension source tab equip result=%d target=%d source=%d/%d",
                    static_cast<int>(result), target_bag, source_bag, source_slot);
        return false;
    }
    finish_extension_equip_ui_locked(target_bag, nullptr);
    return true;
}
void make_desc_equip_gate(void* ctrl, void* arg) {
    const ::qol::contracts::InFlightGuard in_flight_guard(
        ::qol::extension_bag::access::host().sdk == nullptr ? nullptr : &::qol::extension_bag::access::host().sdk->in_flight,
        {"patch:P_EXT_DESC_OPEN_GATE"});
    if (ctrl == nullptr || ::qol::extension_bag::access::host().game == nullptr ||
        ::qol::extension_bag::access::host().write_port->ui.make_item_description == nullptr) return;
    // UIEquip_RefreshItemArea may have restored the original inventory object
    // immediately before the original event reaches MakeDesc. Rebind only
    // projected controls so the original MakeDesc reads the extension item.
    virtual_bag_sync_projected_item_control(ctrl);
    void* item = nullptr;
    {
        void* data = extension_bag_ui_data(ctrl);
        if (data != nullptr) item = *reinterpret_cast<void**>(data);
    }
    (void)arg;
    (void)::qol::extension_bag::access::host().write_port->ui.make_item_description(ctrl);
    {
        ::qol::extension_bag::ReentryLock lock;
        int item_bag = -1;
        int item_slot = -1;
        int item_category = -1;
        const bool module_item =
            item != nullptr && module_slot_of_item_locked(item, &item_bag, &item_slot) &&
            read_item_category(item, &item_category);
        const bool extension_bag_item =
            module_item && category_is_extension_backpack(item_category);
        if (module_item && !extension_bag_item) {
            // Reuse the execute-slot bindings across consecutive EB item
            // details. Unbinding and resolving all slots on every click is the
            // dominant extra latency versus the original detail path.
            qol::extension_bag::ui_state().ui.extension_desc_item = item;
            qol::extension_bag::ui_state().ui.extension_desc_item_bag = item_bag;
            qol::extension_bag::ui_state().ui.extension_desc_item_slot = item_slot;
            install_extension_desc_item_hooks_locked(item);
            QOL_LOG_INFO(QolDomain::kExtensionBag,
                         "extension desc item captured bag=%d slot=%d item=%p",
                         item_bag, item_slot, item);
        } else {
            reset_extension_desc_item_hooks_locked();
            qol::extension_bag::ui_state().ui.extension_desc_item = nullptr;
            qol::extension_bag::ui_state().ui.extension_desc_item_bag = -1;
            qol::extension_bag::ui_state().ui.extension_desc_item_slot = -1;
            if (extension_bag_item) {
                // 袋详情保留原版 UnequipExe；卸袋由函数级入口识别，
                // 不能安装普通物品的 V2 详情 hook（否则会落到分解分支）。
                QOL_LOG_INFO(QolDomain::kExtensionBag,
                             "extension bag desc keeps original unequip bag=%d slot=%d item=%p",
                             item_bag, item_slot, item);
            }
        }
    }
}
