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
#include "plugins/extension_bag/extension_bag_equip_rules.h"
#include "plugins/extension_bag/extension_bag_geometry.h"
#include "plugins/extension_bag/extension_bag_internal.h"
#include "plugins/extension_bag/extension_bag_logging.h"
#include "plugins/extension_bag/move_merge.h"
#include "plugins/extension_bag/model/ownership_ledger.h"
#include "plugins/extension_bag/model/item_handle_custody.h"
#include "plugins/extension_bag/model/sale_transaction.h"
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
    if (g_game == nullptr || g_character_extras->read.menu_character == nullptr) {
        return nullptr;
    }
    void* character = nullptr;
    return qol::contracts::succeeded(
               g_character_extras->read.menu_character(&character))
               ? character
               : nullptr;
}

ExtensionBagEquipResult equip_extension_bag_item_locked(std::unique_lock<std::mutex>& lock,
                                                        int internal_bag, void* item) {
    ensure_state_loaded_locked();
    if (!virtual_bag::valid_index(internal_bag) || item == nullptr) {
        return ExtensionBagEquipResult::kReject;
    }
    if (g_virtual_bag_state.types[internal_bag] != 0) {
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
        category = g_virtual_bag_state.items[src_bag][src_slot].category;
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
        const virtual_bag::Item& source = g_virtual_bag_state.items[src_bag][src_slot];
        payload = source.payload;
        payload_size = source.payload_size;
    } else if (!serialize_item_payload_locked(item, &payload, &payload_size)) {
        QOL_LOG_WARN(QolDomain::kExtensionBag, "extension equip reject reason=payload_serialize_failed src=%d/%d", src_bag,
                    src_slot);
        return ExtensionBagEquipResult::kFailed;
    }

    if (module_source) {
        const virtual_bag::Item source = g_virtual_bag_state.items[src_bag][src_slot];
        const uint8_t previous_type = g_virtual_bag_state.types[internal_bag];
        if (!virtual_bag::equip_bag(&g_virtual_bag_state, internal_bag, category)) {
            return ExtensionBagEquipResult::kReject;
        }
        g_virtual_bag_state.items[src_bag][src_slot] = {};
        extension_bag_mark_dirty_locked();
        if (persist_state_locked()) {
            if (!free_module_object_locked(src_bag, src_slot)) {
                g_virtual_bag_state.types[internal_bag] = previous_type;
                g_virtual_bag_state.items[src_bag][src_slot] = source;
                g_virtual_bag_state.isolation_now_ms = extension_bag_ui_isolation_now_ms_locked();
                virtual_bag::normalize(&g_virtual_bag_state);
                persist_state_locked();
                QOL_LOG_WARN(QolDomain::kExtensionBag, "extension equip rejected source release bag=%d slot=%d",
                            src_bag, src_slot);
                return ExtensionBagEquipResult::kFailed;
            }
            QOL_LOG_INFO(QolDomain::kExtensionBag, "extension equip ok source=extension bag=%d category=%d src=%d/%d",
                        internal_bag, category, src_bag, src_slot);
            return ExtensionBagEquipResult::kOk;
        }
        g_virtual_bag_state.types[internal_bag] = 0;
        g_virtual_bag_state.items[src_bag][src_slot] = source;
        g_virtual_bag_state.isolation_now_ms = extension_bag_ui_isolation_now_ms_locked();
        virtual_bag::normalize(&g_virtual_bag_state);
        persist_state_locked();
        return ExtensionBagEquipResult::kPersistFailed;
    }

    // P4.3：移除前先分配保管 handle（失败时物品仍在原版库存，无丢失风险）。
    uint32_t custody_handle = 0;
    if (ownership::allocate(&g_ownership_ledger, &custody_handle) != ownership::Outcome::kOk) {
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
        virtual_bag::equip_bag(&g_virtual_bag_state, internal_bag, category);
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
        g_virtual_bag_state.types[internal_bag] = 0;
        g_virtual_bag_state.isolation_now_ms = extension_bag_ui_isolation_now_ms_locked();
        virtual_bag::normalize(&g_virtual_bag_state);
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

bool equip_module_item_on_character_locked(std::unique_lock<std::mutex>& lock,
                                           void* character, void* item, int source_bag,
                                           int source_slot) {
    if (!lock.owns_lock() || character == nullptr || item == nullptr ||
        !virtual_bag::valid_index(source_bag) ||
        source_slot < 0 || source_slot >= virtual_bag::kSlotCount ||
        g_module_objects[source_bag][source_slot] != item ||
        g_game == nullptr || g_character_extras->read.can_equip == nullptr ||
        g_character_extras->read.find_equip_slot == nullptr ||
        extension_bag_write_api() == nullptr ||
        !g_module_view_installed || g_module_view_index != source_bag ||
        g_module_window_original_bag < 0 || g_module_window_original_bag >= 5) {
        return false;
    }
    if (!module_object_replacement_allowed_locked(source_bag, source_slot,
                                                  "character equip")) {
        return false;
    }
    if (!extension_bag_ui_read_char_can_equip(character, item)) return false;

    int target = -1;
    if (!extension_bag_ui_read_char_find_equip_slot(character, item, &target)) return false;
    if (target < 0) return false;
    const int original_bag = g_module_window_original_bag;
    const auto* inventory_write = extension_bag_write_api();
    if (inventory_write == nullptr ||
        inventory_write->inventory.set_physical_item == nullptr) return false;

    // Projection deliberately leaves the real inventory untouched. The original
    // exchange entry point reads INVEN[bag][slot], so expose the projected item
    // only for the duration of that call and restore the real slot afterwards.
    void* original_inventory_item = nullptr;
    qol::contracts::QolItemHandle original_inventory_handle{};
    if (g_item_facts == nullptr || g_item_facts->physical_item_at == nullptr ||
        !qol::contracts::succeeded(g_item_facts->physical_item_at(
            original_bag, source_slot, &original_inventory_handle)) ||
        (original_inventory_handle.slot != 0 &&
         !extension_bag_private_item_for_handle(original_inventory_handle,
                                                &original_inventory_item))) return false;
    void* previous_equipped_item = extension_bag_ui_read_char_equipped_item(character, target);
    std::array<uint8_t, virtual_bag::kSerializedItemBuffer> previous_equipped_payload{};
    int previous_equipped_payload_size = 0;
    if (previous_equipped_item != nullptr &&
        !serialize_item_payload_locked(previous_equipped_item, &previous_equipped_payload,
                                       &previous_equipped_payload_size)) {
        return false;
    }
    const uint32_t source_handle = g_module_object_handles[source_bag][source_slot];
    const uint64_t source_generation = g_module_object_use[source_bag][source_slot].generation;
    const uint64_t source_hash = virtual_bag::payload_hash(
        g_virtual_bag_state.items[source_bag][source_slot]);
    if (!qol::contracts::succeeded(inventory_write->inventory.set_physical_item(
            original_bag, source_slot, item))) return false;

    // 原版交换入口会重入库存 hook，因此临时安装物理槽后必须解锁调用，再重锁复核。
    extension_bag_begin_internal_equip();
    lock.unlock();
    const bool equip_result = extension_bag_write_api() != nullptr &&
                              extension_bag_ui_equip_from_inventory(character, original_bag,
                                                             source_slot, target);
    extension_bag_end_internal_equip();
    lock.lock();
    const QolResult restore_result = inventory_write->inventory.set_physical_item(
        original_bag, source_slot, original_inventory_item);
    if (!qol::contracts::succeeded(restore_result)) {
        QOL_LOG_ERROR(QolDomain::kExtensionBag,
                      "ERROR extension character equip physical slot restore failed bag=%d slot=%d item=%p code=%d",
                      original_bag, source_slot, original_inventory_item, restore_result.code);
        return false;
    }
    const bool source_unchanged = g_module_objects[source_bag][source_slot] == item &&
        g_module_object_handles[source_bag][source_slot] == source_handle &&
        g_module_object_use[source_bag][source_slot].generation == source_generation &&
        virtual_bag::payload_hash(g_virtual_bag_state.items[source_bag][source_slot]) ==
            source_hash &&
        g_module_view_installed && g_module_view_index == source_bag &&
        g_module_window_original_bag == original_bag;
    if (!source_unchanged) {
        QOL_LOG_ERROR(QolDomain::kExtensionBag,
                      "ERROR extension character equip source changed during original exchange bag=%d slot=%d item=%p handle=%u generation=%llu",
                      source_bag, source_slot, item, source_handle,
                      static_cast<unsigned long long>(source_generation));
        return false;
    }
    if (!equip_result || extension_bag_ui_read_char_equipped_item(character, target) != item) {
        QOL_LOG_WARN(QolDomain::kExtensionBag,
                     "extension character equip original exchange failed bag=%d slot=%d target=%d result=%d",
                     source_bag, source_slot, target, equip_result ? 1 : 0);
        return false;
    }
    // CHAR_EquipItemFromInvenToSlot 已把旧装备换入临时投影槽；该槽随后仍由
    // 扩展状态表示，不能让同一个旧指针同时继续留在原版 INVEN 中。
    // 角色装备接管对象所有权，不能走 free_module_object_locked；否则下一帧
    // 会把刚装备到角色槽的对象释放掉。先结束视图借用，再转为游戏侧持有。
    const uint32_t handle = g_module_object_handles[source_bag][source_slot];
    if (ownership::live_state(g_ownership_ledger, handle,
                              ownership::State::kBorrowedForView)) {
        ownership::return_from_view(&g_ownership_ledger, handle);
    }
    if (ownership::live_state(g_ownership_ledger, handle,
                              ownership::State::kModuleOwned)) {
        if (ownership::handover_to_inventory(&g_ownership_ledger, handle) !=
            ownership::Outcome::kOk) {
            QOL_LOG_WARN(QolDomain::kExtensionBag, "extension character equip ownership handover failed bag=%d slot=%d",
                        source_bag, source_slot);
        }
    }
    virtual_bag::Item& descriptor = g_virtual_bag_state.items[source_bag][source_slot];
    if (previous_equipped_item != nullptr) {
        descriptor.payload = previous_equipped_payload;
        descriptor.payload_size = previous_equipped_payload_size;
        (void)read_item_category(previous_equipped_item, &descriptor.category);
        int previous_count = 1;
        descriptor.count = extension_bag_ui_read_item_cumulate_count(previous_equipped_item, &previous_count)
            ? static_cast<int>(virtual_bag_canonical_count(previous_equipped_item, 1)) : previous_count;
        // 旧装备对象被原版 UI（装备槽 desc/控件缓存）持续引用，绝不能释放：
        // 直接收编为扩展槽的物化对象，投影与详情都继续指向同一存活对象。
        if (!module_object_generation_advance_locked(
                source_bag, source_slot, "character equip replacement")) {
            return false;
        }
        g_module_objects[source_bag][source_slot] = previous_equipped_item;
        g_module_object_categories[source_bag][source_slot] = descriptor.category;
        g_module_object_hashes[source_bag][source_slot] = virtual_bag::payload_hash(descriptor);
        uint32_t adopted_handle = 0;
        if (ownership::allocate(&g_ownership_ledger, &adopted_handle) == ownership::Outcome::kOk) {
            g_module_object_handles[source_bag][source_slot] = adopted_handle;
        } else {
            g_module_object_handles[source_bag][source_slot] = 0;
            QOL_LOG_WARN(QolDomain::kExtensionBag, "extension character equip adopt ledger exhausted bag=%d slot=%d",
                        source_bag, source_slot);
        }
    } else {
        descriptor = {};
        if (!module_object_generation_advance_locked(
                source_bag, source_slot, "character equip clear")) {
            return false;
        }
        g_module_objects[source_bag][source_slot] = nullptr;
        g_module_object_categories[source_bag][source_slot] = 0;
        g_module_object_hashes[source_bag][source_slot] = 0;
        g_module_object_handles[source_bag][source_slot] = 0;
    }
    extension_bag_mark_dirty_locked();
    persist_state_locked();
    QOL_LOG_INFO(QolDomain::kExtensionBag, "extension character equip ok bag=%d slot=%d target=%d", source_bag,
                source_slot, target);
    return true;
}

// 装备成功后的 UI 收尾（游戏线程、持锁；先例：draw_end/enter 路径的锁内
// UI 调用）：internal_bag 始终是扩展逻辑袋 0..4。若当前已在另一个扩展视图，
// 先复用 extension_bag_ui_handle_tab_click_locked 切到目标袋，使容量字和投影使用同一
// g_module_view_index，再刷新物品区；随后更新标签对象、复位拖动态并播放音效。
void finish_extension_equip_ui_locked(int internal_bag, void* moving_control) {
    bool switched_view = false;
    refresh_tab_bag_items_locked();
    void* tab = g_extension_tab_buttons[internal_bag];
    if (tab != nullptr) {
        void* data = extension_bag_ui_data(tab);
        if (data != nullptr) {
            *reinterpret_cast<void**>(data) = g_tab_bag_items[internal_bag];
        }
    }
    if (g_module_view_installed && g_virtual_bag_state.mode == virtual_bag::Mode::kModule &&
        g_module_view_index != internal_bag) {
        extension_bag_ui_handle_tab_click_locked(internal_bag);
        switched_view = g_module_view_index == internal_bag;
    }
    if (g_module_view_installed && g_module_view_index == internal_bag) {
        refresh_module_item_area_locked(internal_bag);
    } else if (g_game != nullptr && g_write_port->ui.refresh_item_area != nullptr) {
        virtual_bag_call_original_refresh_item_area_with_guard();
    }
    extension_bag_ui_update_character_equip();
    if (moving_control != nullptr) reset_drag_state_locked(moving_control);
    if (!switched_view) extension_bag_ui_play_switch_sound();
}

bool try_equip_on_extension_tab_drop_locked(std::unique_lock<std::mutex>& lock,
                                            int index, void* moving_control) {
    if (!g_virtual_bag_enabled.load() || !read_world_in_world()) return false;
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
    const auto& session = g_extension_drag_session;
    if (!virtual_bag::valid_index(target_bag) ||
        !virtual_bag::valid_index(session.source_bag) ||
        session.source_slot < 0 || session.source_slot >= virtual_bag::kSlotCount) {
        return false;
    }
    const int category = g_virtual_bag_state.items[session.source_bag][session.source_slot].category;
    return g_virtual_bag_state.types[target_bag] == 0 && extension_bag_runtime_is_backpack_category(category);
}

int first_empty_extension_bag_locked() {
    for (int bag = 0; bag < virtual_bag::kBagCount; ++bag) {
        if (g_virtual_bag_state.types[bag] == 0) return bag;
    }
    return -1;
}

bool equip_extension_source_on_tab_locked(std::unique_lock<std::mutex>& lock, int target_bag,
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
// ---- Path A：装备按钮语义（函数级 hook）----
// 详情"装备"按钮 execute proc 恒为 UIEquip_ButtonEquipExe（0xb7c18），因此
// 其全部扩展语义由 native 第 10 hook（button_equip_exe_wrapper →
// virtual_bag_handle_backpack_button_equip）承担：背包物品（装原版/扩展袋）
// 与扩展槽装备物品（装角色）在原函数内联删源之前分流；其余交还原函数。
// 装备/卸下按钮不再由详情实例绑定器接管，避免与函数级入口 hook 重复执行。
// BL 门禁（0xb9188）：物品 desc 打开 → 调原 MakeDesc（同步生成含装备按钮的
// 菜单）→ 锁内重装装备按钮 hook。物品从触发控件 data[0] 取（MakeDesc 同源）。
void make_desc_equip_gate(void* ctrl, void* arg) {
    const ::qol::contracts::InFlightGuard in_flight_guard(
        g_sdk == nullptr ? nullptr : &g_sdk->in_flight,
        {"patch:P_EXT_DESC_OPEN_GATE"});
    if (ctrl == nullptr || g_game == nullptr ||
        g_write_port->ui.make_item_description == nullptr) return;
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
    (void)g_write_port->ui.make_item_description(ctrl);
    {
        std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
        reset_extension_desc_item_hooks_locked();
        g_extension_desc_item = nullptr;
        g_extension_desc_item_bag = -1;
        g_extension_desc_item_slot = -1;
        if (item != nullptr) {
            int item_bag = -1;
            int item_slot = -1;
            if (module_slot_of_item_locked(item, &item_bag, &item_slot)) {
                g_extension_desc_item = item;
                g_extension_desc_item_bag = item_bag;
                g_extension_desc_item_slot = item_slot;
                // 装备/卸下按钮由函数级 hook
                // （ButtonEquipExe 第 10）承担，观察 hook 只覆盖其他详情按钮。
                install_extension_desc_item_hooks_locked(item);
                QOL_LOG_INFO(QolDomain::kExtensionBag, "extension desc item captured bag=%d slot=%d item=%p", item_bag,
                            item_slot, item);
            }
        }
    }
}

// ---- Path 卸下按钮：函数级 hook ----
// 卸下按钮 execute proc 恒为 UIEquip_ButtonUnequipExe（0xb7e14），扩展语义由
// native 第 11 hook（button_unequip_exe_wrapper →
// virtual_bag_handle_original_bag_unequip）承担：扩展视图内识别扩展袋 desc
// 解除扩展袋；原版视图解除原版袋并在背包满时放回其他袋行/收编扩展空位。
// g_extension_desc_unequip_bag 仍由扩展袋 desc 打开时记录（供函数 hook 识别），
// 详情记录仅供函数级入口 hook 识别。

// virtual_bag_handle_backpack_button_equip 在 public_runtime.cpp 中外部链接，供
// api/api_table.cpp 跨 TU 调用。
