#include "game_ui_virtbag.h"

#include "contracts/game_host.h"
#include "contracts/host_sdk.h"
#include "contracts/ui.h"
#include "contracts/ownership.h"
#include "contracts/inventory_refresh.h"
#include "contracts/item_facts.h"
#include "plugins/extension_bag/model/equip_rules.h"
#include "plugins/extension_bag/extension_bag_port.h"
#include "plugins/extension_bag/assembly/move_merge.h"
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
#include "plugins/extension_bag/plugin.h"
#include "plugins/extension_bag/ui/store_sell_rules.h"
#include "plugins/extension_bag/extension_bag_internal.h"

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

namespace {

const char* extension_recovery_action_name_locked() {
    const virtual_bag::RecoveryAction action =
        virtual_bag::recovery_action(runtime_state().objects.bag, runtime_state().objects.bag.pending);
    if (action == virtual_bag::RecoveryAction::kComplete) return "complete";
    if (action == virtual_bag::RecoveryAction::kRollback) return "rollback";
    return "none";
}

std::string extension_bag_status_json_locked() {
    return "{\"enabled\":" + std::string(runtime_state().enabled.load() ? "true" : "false") +
           ",\"injected\":" + std::string(runtime_state().persistence.state_entry != nullptr ? "true" : "false") +
           ",\"save_panel_hook\":" +
           std::string(runtime_state().persistence.save_panel_hook_installed ? "true" : "false") +
           ",\"extension_tab_button\":" +
           std::string(runtime_state().ui.extension_tab_buttons[0] != nullptr ? "true" : "false") +
           ",\"inventory_frame_active\":" +
           std::string(runtime_state().ui.inventory_frame_active ? "true" : "false") +
           ",\"recovery_action\":\"" + extension_recovery_action_name_locked() + "\"" +
           ",\"state\":" + virtual_bag::state_json(runtime_state().objects.bag, true) + "}";
}

std::string extension_bag_not_ready_error_locked() {
    if (!runtime_state().enabled.load()) return op_err("extension bag disabled");
    return op_err("not in game");
}

bool extension_bag_ready_locked() {
    // 操作准入与扫描同门：扩展开关 + world + 当前 epoch 已加载（未加载/降级/epoch 未登记
    // 时拒绝物品操作，不惰性 IO 兜底）。session 状态由 provider 回调维护。
    return runtime_state().enabled.load() && read_world_in_world() &&
           extension_bag_session_admitted_locked();
}

int extension_internal_bag(int logical_bag) {
    return virtual_bag::extension_internal_bag(logical_bag);
}

std::string extension_bag_view_result_json(bool ok, const char* error) {
    if (!ok) return op_err(error);
    return "{\"ok\":true,\"state\":" + extension_bag_status_json_locked() + "}";
}
}  // namespace

// P3 袋解除（对齐原版 UIEquip_ButtonUnequipExe desc_type=1 语义）：
// 非空 → kNotEmpty（弹窗 7，b8084）；原版袋 0..4 全满 → kNoSpace（弹窗 6，
// b809c）；成功 = 按袋类型重建 count-1 背包物品入库（源袋优先、显式跳过
// 任务袋 5/ADR-006）→ 清装备态 → 切到接收袋（原版 b804c-b805c 双字节写）。
// 顺序铁律：先预检空袋再建对象（原版同序 b7f94→b7fa8），否则非空路径会把
// 已入库物品滞留 g_inven 造成复制；转移成功后仅 persist 失败需回滚
// （INVEN_RemoveItemDirect 返回值不可信 → 槽位重读确认后再真释放）。
ExtensionBagUnequipResult unequip_extension_bag_locked(std::unique_lock<std::mutex>& lock,
                                                       int internal_bag) {
    ensure_state_loaded_locked();
    if (runtime_state().objects.bag.capacities[internal_bag] == 0) {
        return ExtensionBagUnequipResult::kNotEquipped;
    }
    for (const virtual_bag::Item& item : runtime_state().objects.bag.items[internal_bag]) {
        if (item.category > 0 || item.count > 0) {
            return ExtensionBagUnequipResult::kNotEmpty;
        }
    }
    if (runtime_state().host.game == nullptr || runtime_state().host.write_port->inventory.create_item == nullptr ||
        runtime_state().host.write_port->inventory.save_item_on_empty == nullptr) {
        VIRTBAG_LOG("extension unequip symbols unavailable bag=%d", internal_bag);
        return ExtensionBagUnequipResult::kFailed;
    }
    void* item = nullptr;
    if (!qol::contracts::succeeded(runtime_state().host.write_port->inventory.create_item(
            static_cast<int32_t>(runtime_state().objects.bag.types[internal_bag]), 0, 0, 0, &item)) ||
        item == nullptr) {
        VIRTBAG_LOG("extension unequip create item failed bag=%d", internal_bag);
        return ExtensionBagUnequipResult::kFailed;
    }
    // P4.3：全新对象立即入账本；分配失败立即真释放（尚未暴露，安全）。
    uint32_t item_handle = 0;
    if (ownership::allocate(&runtime_state().objects.ownership_ledger, &item_handle) != ownership::Outcome::kOk) {
        release_temporary_item_locked(item);
        VIRTBAG_LOG("extension unequip ledger exhausted bag=%d", internal_bag);
        return ExtensionBagUnequipResult::kFailed;
    }
    // 袋对象 +0x10 的 bit0..24 是容量，ITEMSYSTEM_CreateItem（0x10be9c）已按
    // ITEMSTATICBASE[category]（1→4、2→8、3→12、4→16）自动写入。只改
    // bit25..31，native bag object 只写 marker；禁止先做 S2 编码再叠加 marker，
    // 因为当前 marker mask 与 S2 B 段重叠。
    {
        uint32_t flags = 0;
        const auto* count_api = item_count_api();
        if (count_api == nullptr || count_api->read_encoded_field == nullptr ||
            count_api->write_encoded_field == nullptr ||
            !qol::contracts::succeeded(count_api->read_encoded_field(item, &flags)) ||
            !qol::contracts::succeeded(count_api->write_encoded_field(
                item, ::qol::features::extension_bag::write_native_bag_object_marker(flags)))) {
            release_temporary_item_locked(item);
            VIRTBAG_LOG("extension unequip bag marker write failed bag=%d", internal_bag);
            return ExtensionBagUnequipResult::kFailed;
        }
    }

    int preferred = original_bag_locked();
    if (preferred < 0 || preferred > 4) preferred = runtime_state().objects.bag.original_selected;
    if (preferred < 0 || preferred > 4) preferred = 0;
    // 排除 internal_bag 自身：正在解除的袋不得作为接收方——袋对象若放回
    // 自己的行/槽，随后 unequip_bag 清袋会把刚放回的物品一起清掉
    // （真机实证：extension unequip adopt bag=N dst=N/0 后物品丢失/解除失败）。
    int order[5];
    int order_count = 0;
    for (int bag = 0; bag < 5; ++bag) {
        if (bag == internal_bag || bag == preferred) continue;
        order[order_count++] = bag;
    }
    if (preferred != internal_bag) {
        for (int i = order_count; i > 0; --i) order[i] = order[i - 1];
        order[0] = preferred;
        ++order_count;
    }
    int receiving_bag = -1;
    for (int i = 0; i < order_count && receiving_bag < 0; ++i) {
        int save_result = 0;
        if (qol::contracts::succeeded(runtime_state().host.write_port->inventory.save_item_on_empty(
                item, order[i], &save_result)) && save_result != 0) {
            receiving_bag = order[i];
        }
    }
    if (receiving_bag < 0) {
        // 原版袋 0..4 全满：袋对象不回原版背包，转而收编为扩展袋物品槽的
        // 普通袋物品（与装备卸下收编 virtual_bag_adopt_unequipped_item 同
        // 模式）。收编后用户可把它拖回原版背包，或拖到空扩展标签重新装备。
        int adopt_bag = -1;
        int adopt_slot = -1;
        for (int bag = 0; bag < virtual_bag::kBagCount && adopt_bag < 0; ++bag) {
            if (bag == internal_bag) continue;  // 排除正在解除的袋（见上）
            if (runtime_state().objects.bag.types[bag] == 0) continue;
            const int capacity = runtime_state().objects.bag.capacities[bag];
            for (int slot = 0; slot < capacity; ++slot) {
                if (module_slot_is_assignable_locked(bag, slot)) {
                    adopt_bag = bag;
                    adopt_slot = slot;
                    break;
                }
            }
        }
        if (adopt_bag < 0) {
            // 原版背包与所有扩展袋物品槽都满：真无去处，释放并保持原版弹窗。
            release_tracked_item_locked(item_handle, item, "unequip no space");
            return ExtensionBagUnequipResult::kNoSpace;
        }
        std::array<uint8_t, virtual_bag::kSerializedItemBuffer> payload{};
        int payload_size = 0;
        if (!serialize_item_payload_locked(item, &payload, &payload_size)) {
            release_tracked_item_locked(item_handle, item, "unequip adopt serialize");
            return ExtensionBagUnequipResult::kFailed;
        }
        virtual_bag::Item& descriptor = runtime_state().objects.bag.items[adopt_bag][adopt_slot];
        descriptor.payload = payload;
        descriptor.payload_size = payload_size;
        (void)read_item_category(item, &descriptor.category);
        // 袋对象 count 恒为 1（本路径只处理袋物品）。不能用
        // fn_get_cumulate_count：CreateItem 后 +0x10 高位 count 区为 0，
        // 读得 0 会让收编袋显示为空槽（下次收编仍会选中它）。
        descriptor.count = 1;
        // 对象移交目标扩展槽保管（item_handle 早前已 allocate，沿用）。
        if (!module_object_generation_advance_locked(adopt_bag, adopt_slot, "unequip adopt")) {
            release_tracked_item_locked(item_handle, item, "unequip adopt slot protected");
            return ExtensionBagUnequipResult::kFailed;
        }
        if (!extension_bag_rebind_item_handle(item)) {
            release_tracked_item_locked(item_handle, item, "unequip adopt handle registration");
            return ExtensionBagUnequipResult::kFailed;
        }
        runtime_state().objects.module_objects[adopt_bag][adopt_slot] = item;
        runtime_state().objects.module_object_categories[adopt_bag][adopt_slot] = descriptor.category;
        runtime_state().objects.module_object_hashes[adopt_bag][adopt_slot] = virtual_bag::payload_hash(descriptor);
        runtime_state().objects.module_object_handles[adopt_bag][adopt_slot] = item_handle;
        const uint8_t adopted_type = runtime_state().objects.bag.types[internal_bag];
        virtual_bag::unequip_bag(&runtime_state().objects.bag, internal_bag);
        extension_bag_mark_dirty_locked();
        if (persist_state_locked()) {
            if (runtime_state().objects.module_view_installed && runtime_state().objects.module_view_index == adopt_bag) {
                refresh_module_item_area_locked(adopt_bag);
            }
            VIRTBAG_LOG("extension unequip adopt bag=%d type=%d dst=%d/%d", internal_bag,
                        static_cast<int>(adopted_type), adopt_bag, adopt_slot);
            return ExtensionBagUnequipResult::kOk;
        }
        // persist 失败：回滚收编与袋装备态，对象真释放。
        if (!module_object_generation_advance_locked(adopt_bag, adopt_slot, "unequip rollback")) {
            VIRTBAG_LOG("extension unequip rollback blocked by active object bag=%d slot=%d",
                        adopt_bag, adopt_slot);
            return ExtensionBagUnequipResult::kPersistFailed;
        }
        runtime_state().objects.module_objects[adopt_bag][adopt_slot] = nullptr;
        runtime_state().objects.module_object_categories[adopt_bag][adopt_slot] = 0;
        runtime_state().objects.module_object_hashes[adopt_bag][adopt_slot] = 0;
        runtime_state().objects.module_object_handles[adopt_bag][adopt_slot] = 0;
        descriptor = virtual_bag::Item{};
        runtime_state().objects.bag.types[internal_bag] = adopted_type;
        runtime_state().objects.bag.isolation_now_ms = isolation_now_ms_locked();
        virtual_bag::normalize(&runtime_state().objects.bag);
        release_tracked_item_locked(item_handle, item, "unequip adopt rollback");
        persist_state_locked();
        VIRTBAG_LOG("extension unequip adopt persist failed bag=%d dst=%d/%d", internal_bag,
                    adopt_bag, adopt_slot);
        return ExtensionBagUnequipResult::kPersistFailed;
    }

    // 入库成功后 g_inven 拥有对象，模块不得再释放；预检后 unequip_bag 不会
    // 因非空失败。UI 恢复仍由 virtual_bag_draw_end_wrapper 下一帧完成。
    const uint8_t saved_type = runtime_state().objects.bag.types[internal_bag];
    virtual_bag::unequip_bag(&runtime_state().objects.bag, internal_bag);
    virtual_bag::enter_original(&runtime_state().objects.bag, receiving_bag);
    set_original_bag_locked(receiving_bag);
    extension_bag_mark_dirty_locked();
    if (persist_state_locked()) {
        // P4.3：persist 成功 = 提交点，此刻才移交原版库存（终态）。
        handover_tracked_item_locked(item_handle, item, "unequip handover");
        VIRTBAG_LOG("extension unequip ok bag=%d type=%d receiving=%d", internal_bag,
                    static_cast<int>(saved_type), receiving_bag);
        return ExtensionBagUnequipResult::kOk;
    }
    bool rolled_back = false;
    for (int slot = 0; slot < virtual_bag::kSlotCount; ++slot) {
            void* slot_item = nullptr;
            if (!inventory_slot_locked(receiving_bag, slot, &slot_item) ||
                slot_item != item) {
                continue;
            }
            if (remove_item_direct_unlocked(lock, receiving_bag, slot, item,
                                             "unequip rollback")) {
                rolled_back = true;
            }
            break;
        }
    if (rolled_back) {
        // P4.3：回滚已确认（脱离 INVEN）→ 账本终止保管 + 真释放。
        release_tracked_item_locked(item_handle, item, "unequip rollback");
        runtime_state().objects.bag.types[internal_bag] = saved_type;
        runtime_state().objects.bag.isolation_now_ms = isolation_now_ms_locked();
        virtual_bag::normalize(&runtime_state().objects.bag);
        persist_state_locked();
        VIRTBAG_LOG("extension unequip persist failed; rolled back bag=%d", internal_bag);
        return ExtensionBagUnequipResult::kPersistFailed;
    }
    // 回滚未确认：对象去向不明，按 handover 记账（inventory-owned 终态，审计可见）。
    handover_tracked_item_locked(item_handle, item, "unequip rollback uncertain");
    VIRTBAG_LOG("extension unequip rollback failed bag=%d receiving=%d", internal_bag,
                receiving_bag);
    return ExtensionBagUnequipResult::kFailed;
}

namespace {
void show_extension_bag_not_empty_popup() {
    if (runtime_state().host.sdk != nullptr && runtime_state().host.sdk->ui != nullptr && runtime_state().host.sdk->ui->show_text_popup != nullptr) {
        (void)runtime_state().host.sdk->ui->show_text_popup(7);
    }
}

void show_extension_bag_no_space_popup() {
    if (runtime_state().host.sdk != nullptr && runtime_state().host.sdk->ui != nullptr && runtime_state().host.sdk->ui->show_text_popup != nullptr) {
        (void)runtime_state().host.sdk->ui->show_text_popup(6);
    }
}
}  // namespace

std::string extension_bag_status_json_locked_bridge() {
    return extension_bag_status_json_locked();
}
std::string extension_bag_not_ready_error_locked_bridge() {
    return extension_bag_not_ready_error_locked();
}
bool extension_bag_ready_locked_bridge() { return extension_bag_ready_locked(); }
int extension_internal_bag_bridge(int logical_bag) {
    return extension_internal_bag(logical_bag);
}
std::string extension_bag_view_result_json_bridge(bool ok, const char* error) {
    return extension_bag_view_result_json(ok, error);
}
ExtensionBagUnequipResult unequip_extension_bag_locked_bridge(
    std::unique_lock<std::mutex>& lock, int internal_bag) {
    return unequip_extension_bag_locked(lock, internal_bag);
}
