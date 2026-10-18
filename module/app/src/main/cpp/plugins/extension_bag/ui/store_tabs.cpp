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
uint64_t store_tab_item_proc(void*, uint64_t, void*, void*);
int store_tab_index(void* ctrl) {
    for (int index = 0; index < virtual_bag::kBagCount; ++index) {
        if (runtime_state().store.tab_buttons[index] == ctrl) return index;
    }
    return -1;
}

void store_refresh_tab_bag_items_locked() {
    for (int index = 0; index < virtual_bag::kBagCount; ++index) {
        const uint8_t type = runtime_state().objects.bag.types[index];
        void*& item = runtime_state().store.tab_bag_items[index];
        if (type == 0) {
            if (item != nullptr) {
                if (defer_item_free_locked(item)) item = nullptr;
            }
            continue;
        }
        if (item != nullptr) {
            int category = -1;
            if (!read_item_category(item, &category) || category != static_cast<int>(type)) {
                if (defer_item_free_locked(item)) item = nullptr;
            }
        }
        if (item == nullptr) {
            item = extension_create_item(static_cast<int32_t>(type), 0, 0, 0);
            if (item != nullptr) {
                uint32_t flags = 0;
                const auto* count_api = item_count_api();
                if (count_api != nullptr && count_api->read_encoded_field != nullptr &&
                    count_api->write_encoded_field != nullptr &&
                    qol::contracts::succeeded(count_api->read_encoded_field(item, &flags))) {
                    (void)count_api->write_encoded_field(
                        item, ::qol::features::extension_bag::write_native_bag_object_marker(flags));
                }
            }
        }
    }
}
bool store_all_tab_buttons_installed_locked() {
    for (void* button : runtime_state().store.tab_buttons) {
        if (button == nullptr) return false;
    }
    return true;
}

bool store_has_tab_buttons_locked() {
    for (void* button : runtime_state().store.tab_buttons) {
        if (button != nullptr) return true;
    }
    return false;
}

void store_discard_new_tab_controls_locked(
    std::array<void*, virtual_bag::kBagCount>& controls, int failed_index,
    void* failed_control, const char* stage) {
    VIRTBAG_LOG("store tab install failed index=%d stage=%s", failed_index, stage);
    if (failed_control != nullptr && !extension_bag_ui_delete_control(failed_control)) {
        VIRTBAG_LOG("store tab cleanup failed index=%d control=%p", failed_index,
                    failed_control);
    }
    for (int index = 0; index < virtual_bag::kBagCount; ++index) {
        void*& control = controls[index];
        if (control == nullptr) continue;
        if (!extension_bag_ui_delete_control(control)) {
            VIRTBAG_LOG("store tab cleanup failed index=%d control=%p", index, control);
        }
        control = nullptr;
    }
}
uint64_t store_tab_item_proc(void* ctrl, uint64_t event, void*, void*) {
    if (event == 0x02) {
        std::lock_guard<std::mutex> lock(runtime_state().mtx);
        if (!extension_bag_session_admitted_locked()) return 1;
        const int index = store_tab_index(ctrl);
        if (index >= 0 && runtime_state().store.tab_generation != 0) {
            runtime_state().store.pending_tab = index;
            runtime_state().store.pending_tab_generation = runtime_state().store.tab_generation;
        }
        return 1;
    }
    // 商店宿主不接管拖拽，避免扩展标签成为拖动源或跨袋目标。
    return 0;
}
}  // namespace

void store_disable_tab_buttons_locked() {
    ++runtime_state().store.tab_generation;
    runtime_state().store.pending_tab = -1;
    runtime_state().store.pending_tab_generation = 0;
    runtime_state().store.tab_buttons.fill(nullptr);
    runtime_state().store.tab_root = nullptr;
}

void store_delete_tab_buttons_locked() {
    for (void*& button : runtime_state().store.tab_buttons) {
        if (button != nullptr && !extension_bag_ui_delete_control(button)) {
            VIRTBAG_LOG("store tab delete failed control=%p", button);
        }
        button = nullptr;
    }
}
void store_install_tab_buttons_locked() {
    // 配置门控：扩展背包关闭时商店页签不得挂载（3 个宿主之一）。
    if (!runtime_state().enabled.load() || !extension_bag_session_admitted_locked()) return;
    if (runtime_state().host.sdk == nullptr) return;
    void* bag_container = extension_bag_ui_root(qol::contracts::QolUiRootKind::kStoreBagContainer);
    int bag_child_count = 0;
    if (bag_container == nullptr || !extension_bag_ui_child_count(bag_container, &bag_child_count) ||
        bag_child_count < 6) return;
    if (runtime_state().store.tab_root == bag_container && store_all_tab_buttons_installed_locked()) return;

    if (runtime_state().store.tab_root != nullptr || store_has_tab_buttons_locked()) {
        store_delete_tab_buttons_locked();
        store_disable_tab_buttons_locked();
    }
    store_refresh_tab_bag_items_locked();

    int64_t bag0_abs_x = 0;
    int64_t bag0_abs_y = 0;
    void* bag0 = valid_child_locked(bag_container, 0);
    if (bag0 == nullptr) return;
    extension_bag_control_abs_pos(bag0, &bag0_abs_x, &bag0_abs_y);
    UiRect bag0_rect{};
    if (!extension_bag_ui_rect(bag0, &bag0_rect) || bag0_rect.w <= 0) return;

    int64_t mount_abs_x = 0;
    int64_t mount_abs_y = 0;
    extension_bag_control_abs_pos(bag_container, &mount_abs_x, &mount_abs_y);
    // 这里沿用原版袋列的右侧间距 68、下移 2；起点仍来自商店实时袋 0 rect。
    const int64_t tab_rel_x = bag0_abs_x + 68 - mount_abs_x;
    const int64_t tab_rel_y = bag0_abs_y + 2 - mount_abs_y;
    std::array<void*, virtual_bag::kBagCount> controls{};
    for (int index = 0; index < virtual_bag::kBagCount; ++index) {
        void* control = nullptr;
        if (!extension_bag_ui_create_control(bag_container, &control)) {
            store_discard_new_tab_controls_locked(controls, index, control, "create");
            return;
        }
        if (control == nullptr) {
            store_discard_new_tab_controls_locked(controls, index, nullptr, "create-null");
            return;
        }
        controls[index] = control;
        if (!extension_bag_ui_set_control_proc(control, &store_tab_item_proc)) {
            store_discard_new_tab_controls_locked(controls, index, nullptr, "proc");
            return;
        }
        if (!extension_bag_ui_set_control_user_type(control, 2)) {
            store_discard_new_tab_controls_locked(controls, index, nullptr, "user-type");
            return;
        }
        if (!extension_bag_ui_unuse_control_event_move(control)) {
            store_discard_new_tab_controls_locked(controls, index, nullptr, "event-move");
            return;
        }
        if (!extension_bag_ui_set_rect(control, {tab_rel_x, tab_rel_y + index * 70, 57, 57})) {
            store_discard_new_tab_controls_locked(controls, index, nullptr, "rect");
            return;
        }
        void* data = nullptr;
        if (!extension_bag_ui_allocate_control_data(0x10, &data)) {
            store_discard_new_tab_controls_locked(controls, index, nullptr, "data-allocate");
            return;
        }
        if (data == nullptr) {
            store_discard_new_tab_controls_locked(controls, index, nullptr, "data-null");
            return;
        }
        *reinterpret_cast<void**>(data) = runtime_state().store.tab_bag_items[index];
        if (!extension_bag_ui_set_control_data(control, data)) {
            store_discard_new_tab_controls_locked(controls, index, nullptr, "data-set");
            return;
        }
    }
    runtime_state().store.tab_buttons = controls;
    runtime_state().store.tab_root = bag_container;
    ++runtime_state().store.tab_generation;
    VIRTBAG_LOG("store tabs installed root=%p generation=%llu", bag_container,
                static_cast<unsigned long long>(runtime_state().store.tab_generation));
}
void store_commit_pending_tab_locked() {
    if (!extension_bag_session_admitted_locked()) {
        runtime_state().store.pending_tab = -1;
        runtime_state().store.pending_tab_generation = 0;
        return;
    }
    if (!virtual_bag::valid_index(runtime_state().store.pending_tab) ||
        runtime_state().store.pending_tab_generation != runtime_state().store.tab_generation) {
        runtime_state().store.pending_tab = -1;
        runtime_state().store.pending_tab_generation = 0;
        return;
    }
    const int bag = runtime_state().store.pending_tab;
    runtime_state().store.pending_tab = -1;
    runtime_state().store.pending_tab_generation = 0;
    if (runtime_state().store.view_installed && runtime_state().store.view_bag == bag) return;
    store_install_module_view_locked(bag);
}
void store_draw_tab_buttons_locked() {
    if (!extension_bag_ui_can_draw()) return;
    for (int index = virtual_bag::kBagCount - 1; index >= 0; --index) {
        void* button = runtime_state().store.tab_buttons[index];
        if (button == nullptr) continue;
        int64_t x = 0;
        int64_t y = 0;
        extension_bag_control_abs_pos(button, &x, &y);
        (void)extension_bag_ui_draw_part_alpha(0xf, 0x14, static_cast<int32_t>(x - 5),
                                               static_cast<int32_t>(y - 12), 0, 1, 0);
    }
    for (int index = 0; index < virtual_bag::kBagCount; ++index) {
        void* button = runtime_state().store.tab_buttons[index];
        if (button == nullptr) continue;
        int64_t x = 0;
        int64_t y = 0;
        extension_bag_control_abs_pos(button, &x, &y);
        const bool selected = runtime_state().store.view_installed && runtime_state().store.view_bag == index;
        const bool equipped = runtime_state().objects.bag.capacities[index] != 0;
        if (selected) {
            (void)extension_bag_ui_draw_part_alpha(0xf, 0x13, static_cast<int32_t>(x - 5),
                                                   static_cast<int32_t>(y - 12), 0, 1, 0);
            (void)extension_bag_ui_draw_part_alpha(0xf, 6, static_cast<int32_t>(x + 5),
                                                   static_cast<int32_t>(y + 5), 0, 1, 0);
        } else if (equipped) {
            (void)extension_bag_ui_draw_part_alpha(0xf, 6, static_cast<int32_t>(x + 5),
                                                   static_cast<int32_t>(y + 5), 0, 1, 0x28);
        } else {
            (void)extension_bag_ui_draw_part_alpha(0xf, 9, static_cast<int32_t>(x + 5),
                                                   static_cast<int32_t>(y + 5), 0, 1, 0x28);
        }
    }
}
// 商店袋列入口 hook：与背包页 virtual_bag_draw_original_bag_wrapper 同法，在
// UIStore_DrawInvenBagGroup 真正读取当前袋之前写成 6，画完恢复 → 原版袋不再高亮。
void store_draw_inven_bag_group_entry() {
    using DrawFn = void (*)();
    const auto backup = reinterpret_cast<DrawFn>(
        runtime_state().ui.backup_store_draw_inven_bag_group);
    if (backup == nullptr) return;
    const ::qol::contracts::InFlightGuard in_flight_guard(
        runtime_state().host.sdk == nullptr ? nullptr : &runtime_state().host.sdk->in_flight,
        {"hook:UIStore_DrawInvenBagGroup"});
    std::lock_guard<std::mutex> lock(runtime_state().mtx);
    int32_t saved_current = 0;
    bool masked = false;
    if (runtime_state().store.view_installed &&
        runtime_state().host.write_port->ui.current_bag != nullptr &&
        runtime_state().host.write_port->ui.set_current_bag != nullptr &&
        qol::contracts::succeeded(runtime_state().host.write_port->ui.current_bag(&saved_current)) &&
        saved_current != kNoOriginalBagSelected) {
        (void)runtime_state().host.write_port->ui.set_current_bag(kNoOriginalBagSelected);
        masked = true;
    }
    backup();
    if (masked) (void)runtime_state().host.write_port->ui.set_current_bag(saved_current);
}
void store_draw_equip_background_entry(void* a, void* b, int32_t c, void* d, int32_t e) {
    using DrawFn = void (*)(void*, void*, int32_t, void*, int32_t);
    const auto backup = reinterpret_cast<DrawFn>(
        runtime_state().ui.backup_ui_equip_draw_equip_background);
    if (backup == nullptr) return;
    const ::qol::contracts::InFlightGuard in_flight_guard(
        runtime_state().host.sdk == nullptr ? nullptr : &runtime_state().host.sdk->in_flight,
        {"hook:UIEquip_DrawEquipBackground"});
    std::lock_guard<std::mutex> lock(runtime_state().mtx);
    int32_t saved_current = 0;
    bool masked = false;
    const bool module_view = runtime_state().objects.bag.mode == virtual_bag::Mode::kModule;
    if ((runtime_state().store.view_installed || module_view) &&
        runtime_state().host.write_port->ui.current_bag != nullptr &&
        runtime_state().host.write_port->ui.set_current_bag != nullptr &&
        qol::contracts::succeeded(runtime_state().host.write_port->ui.current_bag(&saved_current)) &&
        saved_current != kNoOriginalBagSelected) {
        (void)runtime_state().host.write_port->ui.set_current_bag(kNoOriginalBagSelected);
        masked = true;
    }
    backup(a, b, c, d, e);
    if (masked) (void)runtime_state().host.write_port->ui.set_current_bag(saved_current);
}
// R-57 原版袋列高亮遮蔽（商店）：进入扩展视图时，画原版 UIStore_DrawInvenBagGroup
// 前把当前袋 GOT 临时置 kNoOriginalBagSelected(6)，画完恢复 → 原版 6 袋不再高亮
// （与扩展页 virtual_bag_draw_original_bag_wrapper 同法）。扩展页签高亮由 draw_end
// 的 store_draw_tab_buttons_locked 在袋列绘制之后补画。
void store_draw_inven_bag_group_wrapper() {
    const ::qol::contracts::InFlightGuard in_flight_guard(
        runtime_state().host.sdk == nullptr ? nullptr : &runtime_state().host.sdk->in_flight,
        {"patch:P_EXT_STORE_BAG_DRAW"});
    if (runtime_state().host.game == nullptr ||
        runtime_state().host.write_port->ui.draw_original_store_bag_group == nullptr) return;
    // The entry hook owns masking. Do not take runtime_state().mtx here: the
    // entry hook runs while this wrapper calls the function and uses the same
    // non-recursive mutex.
    (void)runtime_state().host.write_port->ui.draw_original_store_bag_group();
}
