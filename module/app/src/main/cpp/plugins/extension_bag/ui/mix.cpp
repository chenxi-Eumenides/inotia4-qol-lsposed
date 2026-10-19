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

// 合成器（UIMix）宿主：只管理 UIMix 自己的页签、投影和刷新，不复用 UIEquip/商店视图
// 状态。结构参考 ui_store.cpp：
//   - PopupState ENTER/F3 回调包装（enter 装页签、f3 teardown）；
//   - Scene_Draw_POPUP_SC_MIX 内 bl GRPX_End 调用点 BL patch 挂 draw_end wrapper；
//   - 自建 5 个扩展袋页签挂到 UIMix 袋组 [g_uimix+UIMIX_SLOT_BAG_GROUP]（原版 6 袋按钮同父）；
//   - 投影安装/恢复写 UIMix 物品网格组 [g_uimix+UIMIX_SLOT_ITEM_GROUP] 的 16 个子控件。
// 宿主物理袋沿用 original_bag_locked()（UIMix 与 UIEquip 共用 [0x2f56d8] 当前袋 GOT，
// UIMix_RefreshInvenItem 反汇编确认），合法 0..4；落在任务袋 5/非法时回退 0。

namespace {

bool mix_view_installed = false;
int mix_view_bag = -1;
int mix_window_original_bag = -1;
uint32_t mix_original_size_word = 0;
uint32_t mix_original_bag_size = 0;
void* mix_projected_item_root = nullptr;
std::array<void*, virtual_bag::kBagCount> mix_tab_buttons{};
std::array<void*, virtual_bag::kBagCount> mix_tab_bag_items{};
void* mix_tab_root = nullptr;
uint64_t mix_tab_generation = 0;
int mix_pending_tab = -1;
uint64_t mix_pending_tab_generation = 0;
bool mix_frame_active = false;
uint64_t mix_view_generation = 0;
uint64_t mix_refresh_generation = 0;
bool mix_refresh_pending = false;

uint64_t mix_tab_item_proc(void* ctrl, uint64_t event, void*, void*);
void mix_install_module_view_locked(int bag);
void mix_restore_module_view_locked();

void* mix_bag_container_locked() {
    return extension_bag_ui_root(qol::contracts::QolUiRootKind::kMixBagContainer);
}

void* mix_item_root_locked() {
    return extension_bag_ui_root(qol::contracts::QolUiRootKind::kMixItemRoot);
}

// 面板状态：0=配方菜单（原版不画背包列），1=背包/合成视图。扩展页签只在 state!=0
// 时挂载/绘制，避免在配方菜单阶段早于原版背包出现（UIMix_Draw 状态门控反汇编确认）。
int mix_state_locked() {
    if (runtime_state().host.craft_facts == nullptr || runtime_state().host.craft_facts->read_only.panel_state == nullptr) return 0;
    int32_t state = 0;
    return qol::contracts::succeeded(runtime_state().host.craft_facts->read_only.panel_state(&state)) ? state : 0;
}

int mix_tab_index(void* ctrl) {
    for (int index = 0; index < virtual_bag::kBagCount; ++index) {
        if (mix_tab_buttons[index] == ctrl) return index;
    }
    return -1;
}

void mix_refresh_tab_bag_items_locked() {
    for (int index = 0; index < virtual_bag::kBagCount; ++index) {
        const uint8_t type = runtime_state().objects.bag.types[index];
        void*& item = mix_tab_bag_items[index];
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

void mix_disable_tab_buttons_locked() {
    ++mix_tab_generation;
    mix_pending_tab = -1;
    mix_pending_tab_generation = 0;
    mix_tab_buttons.fill(nullptr);
    mix_tab_root = nullptr;
}

void mix_delete_tab_buttons_locked() {
    for (void*& button : mix_tab_buttons) {
        if (button != nullptr && !extension_bag_ui_delete_control(button)) {
            VIRTBAG_LOG("mix tab delete failed control=%p", button);
        }
        button = nullptr;
    }
}

bool mix_all_tab_buttons_installed_locked() {
    for (void* button : mix_tab_buttons) {
        if (button == nullptr) return false;
    }
    return true;
}

bool mix_has_tab_buttons_locked() {
    for (void* button : mix_tab_buttons) {
        if (button != nullptr) return true;
    }
    return false;
}

void mix_discard_new_tab_controls_locked(
    std::array<void*, virtual_bag::kBagCount>& controls, int failed_index,
    void* failed_control, const char* stage) {
    VIRTBAG_LOG("mix tab install failed index=%d stage=%s", failed_index, stage);
    if (failed_control != nullptr && !extension_bag_ui_delete_control(failed_control)) {
        VIRTBAG_LOG("mix tab cleanup failed index=%d control=%p", failed_index,
                    failed_control);
    }
    for (int index = 0; index < virtual_bag::kBagCount; ++index) {
        void*& control = controls[index];
        if (control == nullptr) continue;
        if (!extension_bag_ui_delete_control(control)) {
            VIRTBAG_LOG("mix tab cleanup failed index=%d control=%p", index, control);
        }
        control = nullptr;
    }
}

void mix_install_tab_buttons_locked() {
    // 配置门控：扩展背包关闭时合成器页签不得挂载（3 个宿主之一）。
    if (!runtime_state().enabled.load() || !extension_bag_session_admitted_locked()) return;
    if (runtime_state().host.sdk == nullptr) return;
    void* bag_container = mix_bag_container_locked();
    int bag_child_count = 0;
    if (bag_container == nullptr || !extension_bag_ui_child_count(bag_container, &bag_child_count) ||
        bag_child_count < 6) return;
    if (mix_tab_root == bag_container && mix_all_tab_buttons_installed_locked()) return;

    if (mix_tab_root != nullptr || mix_has_tab_buttons_locked()) {
        mix_delete_tab_buttons_locked();
        mix_disable_tab_buttons_locked();
    }
    mix_refresh_tab_bag_items_locked();

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
    // 沿用扩展页/商店的原版袋列右侧间距 68、下移 2；起点来自 UIMix 实时袋 0 rect。
    const int64_t tab_rel_x = bag0_abs_x + 68 - mount_abs_x;
    const int64_t tab_rel_y = bag0_abs_y + 2 - mount_abs_y;
    std::array<void*, virtual_bag::kBagCount> controls{};
    for (int index = 0; index < virtual_bag::kBagCount; ++index) {
        void* control = nullptr;
        if (!extension_bag_ui_create_control(bag_container, &control)) {
            mix_discard_new_tab_controls_locked(controls, index, control, "create");
            return;
        }
        if (control == nullptr) {
            mix_discard_new_tab_controls_locked(controls, index, nullptr, "create-null");
            return;
        }
        controls[index] = control;
        if (!extension_bag_ui_set_control_proc(control, &mix_tab_item_proc)) {
            mix_discard_new_tab_controls_locked(controls, index, nullptr, "proc");
            return;
        }
        if (!extension_bag_ui_set_control_user_type(control, 2)) {
            mix_discard_new_tab_controls_locked(controls, index, nullptr, "user-type");
            return;
        }
        if (!extension_bag_ui_unuse_control_event_move(control)) {
            mix_discard_new_tab_controls_locked(controls, index, nullptr, "event-move");
            return;
        }
        if (!extension_bag_ui_set_rect(control, {tab_rel_x, tab_rel_y + index * 70, 57, 57})) {
            mix_discard_new_tab_controls_locked(controls, index, nullptr, "rect");
            return;
        }
        void* data = nullptr;
        if (!extension_bag_ui_allocate_control_data(0x10, &data)) {
            mix_discard_new_tab_controls_locked(controls, index, nullptr, "data-allocate");
            return;
        }
        if (data == nullptr) {
            mix_discard_new_tab_controls_locked(controls, index, nullptr, "data-null");
            return;
        }
        *reinterpret_cast<void**>(data) = mix_tab_bag_items[index];
        if (!extension_bag_ui_set_control_data(control, data)) {
            mix_discard_new_tab_controls_locked(controls, index, nullptr, "data-set");
            return;
        }
    }
    mix_tab_buttons = controls;
    mix_tab_root = bag_container;
    ++mix_tab_generation;
    VIRTBAG_LOG("mix tabs installed root=%p bag0=(%lld,%lld) mount=(%lld,%lld) generation=%llu",
                bag_container, static_cast<long long>(bag0_abs_x),
                static_cast<long long>(bag0_abs_y), static_cast<long long>(mount_abs_x),
                static_cast<long long>(mount_abs_y),
                static_cast<unsigned long long>(mix_tab_generation));
}

uint64_t mix_tab_item_proc(void* ctrl, uint64_t event, void*, void*) {
    if (event == 0x02) {
        std::lock_guard<std::mutex> lock(runtime_state().mtx);
        if (!extension_bag_session_admitted_locked()) return 1;
        const int index = mix_tab_index(ctrl);
        if (index >= 0 && mix_tab_generation != 0) {
            mix_pending_tab = index;
            mix_pending_tab_generation = mix_tab_generation;
        }
        return 1;
    }
    // 合成器宿主不接管拖拽，避免扩展标签成为拖动源或跨袋目标。
    return 0;
}

void mix_commit_pending_tab_locked() {
    if (!extension_bag_session_admitted_locked()) {
        mix_pending_tab = -1;
        mix_pending_tab_generation = 0;
        return;
    }
    if (!virtual_bag::valid_index(mix_pending_tab) ||
        mix_pending_tab_generation != mix_tab_generation) {
        mix_pending_tab = -1;
        mix_pending_tab_generation = 0;
        return;
    }
    const int bag = mix_pending_tab;
    mix_pending_tab = -1;
    mix_pending_tab_generation = 0;
    if (mix_view_installed && mix_view_bag == bag) return;
    mix_install_module_view_locked(bag);
}

void mix_refresh_projection_locked() {
    if (!mix_view_installed || mix_projected_item_root == nullptr ||
        !virtual_bag::valid_index(mix_view_bag)) return;
    const int capacity = runtime_state().objects.bag.capacities[mix_view_bag];
    for (int slot = 0; slot < capacity; ++slot) {
        void* control = valid_child_locked(mix_projected_item_root, slot);
        if (control == nullptr) continue;
        void* expected = module_item_locked(mix_view_bag, slot);
        void* data = extension_bag_ui_data(control);
        void* current = data != nullptr ? *reinterpret_cast<void**>(data) : nullptr;
        if (current != expected) (void)extension_bag_ui_set_control_item(control, expected);
    }
}

void mix_request_item_refresh_locked() {
    mix_refresh_pending = true;
    ++mix_refresh_generation;
}

void mix_refresh_item_after_unlock() {
    std::unique_lock<std::mutex> lock(runtime_state().mtx);
    if (!mix_refresh_pending) return;
    const uint64_t refresh_generation = mix_refresh_generation;
    const uint64_t view_generation = mix_view_generation;
    mix_refresh_pending = false;
    lock.unlock();

    const auto* api = extension_bag_write_api();
    if (api == nullptr || api->ui.refresh_mix_item == nullptr) {
        VIRTBAG_LOG("mix item refresh failed reason=missing-adapter");
        lock.lock();
        if (refresh_generation == mix_refresh_generation) mix_request_item_refresh_locked();
        return;
    }
    const QolResult refresh_result = api->ui.refresh_mix_item();
    if (!qol::contracts::succeeded(refresh_result)) {
        VIRTBAG_LOG("mix item refresh failed code=%d", refresh_result.code);
        lock.lock();
        if (refresh_generation == mix_refresh_generation) mix_request_item_refresh_locked();
        return;
    }

    lock.lock();
    if (refresh_generation != mix_refresh_generation ||
        view_generation != mix_view_generation) return;
    if (!mix_frame_active || !runtime_state().enabled.load() ||
        !extension_bag_session_admitted_locked() || mix_state_locked() == 0) {
        if (mix_view_installed) mix_restore_module_view_locked();
        return;
    }
    if (!mix_view_installed) return;
    void* live_root = mix_item_root_locked();
    if (live_root == nullptr) {
        VIRTBAG_LOG("mix item refresh completed without item root; retry scheduled");
        mix_request_item_refresh_locked();
        return;
    }
    mix_projected_item_root = live_root;
    mix_refresh_projection_locked();
}

void mix_install_module_view_locked(int bag) {
    if (!extension_bag_session_admitted_locked() || !virtual_bag::valid_index(bag) ||
        runtime_state().objects.bag.capacities[bag] == 0 ||
        runtime_state().host.game == nullptr || runtime_state().host.write_port->ui.refresh_mix_item == nullptr) return;
    // UIMix 与 UIEquip 共用当前袋 GOT；落在任务袋 5/非法时回退原版袋 0。
    int host = original_bag_locked();
    if (!virtual_bag::valid_original_transaction_bag(host)) host = 0;
    if (mix_view_installed) mix_restore_module_view_locked();
    uint32_t size_word = 0;
    if (!bag_size_word_locked(host, &size_word)) {
        VIRTBAG_LOG("mix view rejected host bag=%d", host);
        return;
    }

    mix_window_original_bag = host;
    mix_original_size_word = size_word;
    mix_original_bag_size = size_word;
    constexpr uint32_t kCapacityMask = (1u << 25) - 1u;
    if (!set_bag_size_word_locked(
            host, (size_word & ~kCapacityMask) |
                      (static_cast<uint32_t>(runtime_state().objects.bag.capacities[bag]) & kCapacityMask))) {
        VIRTBAG_LOG("mix view rejected capacity write bag=%d host=%d", bag, host);
        mix_original_size_word = 0;
        mix_original_bag_size = 0;
        mix_window_original_bag = -1;
        return;
    }
    set_original_bag_locked(host);

    mix_projected_item_root = mix_item_root_locked();
    if (mix_projected_item_root == nullptr) {
        if (!set_bag_size_word_locked(
                host, (mix_original_size_word & ~kCapacityMask) |
                          (mix_original_bag_size & kCapacityMask))) {
            VIRTBAG_LOG("mix view rollback capacity failed host=%d", host);
        }
        mix_original_size_word = 0;
        mix_original_bag_size = 0;
        mix_window_original_bag = -1;
        mix_projected_item_root = nullptr;
        mix_request_item_refresh_locked();
        return;
    }
    const int capacity = runtime_state().objects.bag.capacities[bag];
    for (int slot = 0; slot < capacity; ++slot) {
        if (module_item_locked(bag, slot) != nullptr) {
            ownership::borrow_for_view(&runtime_state().objects.ownership_ledger,
                                       runtime_state().objects.module_object_handles[bag][slot]);
        }
    }
    mix_view_bag = bag;
    mix_view_installed = true;
    ++mix_view_generation;
    mix_request_item_refresh_locked();
    VIRTBAG_LOG("mix view installed bag=%d window=%d capacity=%d", bag, host,
                runtime_state().objects.bag.capacities[bag]);
}

void mix_restore_module_view_locked() {
    if (!mix_view_installed) return;
    if (virtual_bag::valid_index(mix_view_bag)) {
        for (int slot = 0; slot < virtual_bag::kSlotCount; ++slot) {
            const uint32_t handle = runtime_state().objects.module_object_handles[mix_view_bag][slot];
            if (ownership::live_state(runtime_state().objects.ownership_ledger, handle,
                                      ownership::State::kBorrowedForView)) {
                ownership::return_from_view(&runtime_state().objects.ownership_ledger, handle);
            }
        }
    }
    if (mix_original_size_word != 0) {
        constexpr uint32_t kCapacityMask = (1u << 25) - 1u;
        if (!set_bag_size_word_locked(
                mix_window_original_bag,
                (mix_original_size_word & ~kCapacityMask) | (mix_original_bag_size & kCapacityMask))) {
            VIRTBAG_LOG("mix restore capacity failed host=%d", mix_window_original_bag);
        }
    }
    mix_original_size_word = 0;
    mix_original_bag_size = 0;
    mix_window_original_bag = -1;
    mix_projected_item_root = nullptr;
    mix_view_bag = -1;
    mix_view_installed = false;
    ++mix_view_generation;
    mix_request_item_refresh_locked();
}

}  // namespace

void mix_enter_wrapper() {
    const ::qol::contracts::InFlightGuard in_flight_guard(
        runtime_state().host.sdk == nullptr ? nullptr : &runtime_state().host.sdk->in_flight,
        {"own:popup.mix"});
    if (runtime_state().ui.mix_original_enter != nullptr) runtime_state().ui.mix_original_enter();
    {
        std::lock_guard<std::mutex> lock(runtime_state().mtx);
        ensure_state_loaded_locked();
        mix_teardown_locked();
        // Enter 时面板处于 state 0（配方菜单，原版尚未画背包列）；页签等 state!=0 的
        // draw_end 再挂载，避免早于原版背包出现。
        mix_frame_active = true;
    }
    mix_refresh_item_after_unlock();
}

void mix_f3_wrapper() {
    const ::qol::contracts::InFlightGuard in_flight_guard(
        runtime_state().host.sdk == nullptr ? nullptr : &runtime_state().host.sdk->in_flight,
        {"own:popup.mix"});
    MixSceneNoArgFn original = nullptr;
    {
        std::lock_guard<std::mutex> lock(runtime_state().mtx);
        mix_teardown_locked();
        mix_frame_active = false;
        original = runtime_state().ui.mix_original_f3;
    }
    mix_refresh_item_after_unlock();
    if (original != nullptr) original();
}

namespace {

void mix_draw_tab_buttons_locked() {
    if (!extension_bag_ui_can_draw()) return;
    for (int index = virtual_bag::kBagCount - 1; index >= 0; --index) {
        void* button = mix_tab_buttons[index];
        if (button == nullptr) continue;
        int64_t x = 0;
        int64_t y = 0;
        extension_bag_control_abs_pos(button, &x, &y);
        (void)extension_bag_ui_draw_part_alpha(0xf, 0x14, static_cast<int32_t>(x - 5),
                                                 static_cast<int32_t>(y - 12), 0, 1, 0);
    }
    for (int index = 0; index < virtual_bag::kBagCount; ++index) {
        void* button = mix_tab_buttons[index];
        if (button == nullptr) continue;
        int64_t x = 0;
        int64_t y = 0;
        extension_bag_control_abs_pos(button, &x, &y);
        const bool selected = mix_view_installed && mix_view_bag == index;
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

}  // namespace

void mix_draw_end_wrapper() {
    const ::qol::contracts::InFlightGuard in_flight_guard(
        runtime_state().host.sdk == nullptr ? nullptr : &runtime_state().host.sdk->in_flight,
        {"patch:P_EXT_MIX_DRAW_END"});
    {
        std::lock_guard<std::mutex> lock(runtime_state().mtx);
        if (mix_frame_active) {
            const bool interactive = runtime_state().enabled.load() &&
                                     extension_bag_session_admitted_locked() &&
                                     mix_state_locked() != 0;
            if (interactive) {
                ensure_state_loaded_locked();
                mix_install_tab_buttons_locked();
                mix_commit_pending_tab_locked();
                // 点原版袋按钮 / 切配方类型会改写当前袋（UIMix_InvenBagControlEventProc /
                // UIMix_ButtonMenuListExe 置 0）；当前袋偏离投影宿主即退出扩展视图。
                if (mix_view_installed && original_bag_locked() != mix_window_original_bag) {
                    mix_restore_module_view_locked();
                }
                mix_refresh_projection_locked();
                mix_draw_tab_buttons_locked();
            } else {
                // state 0（配方菜单）/扩展禁用：卸载投影与页签，保留 frame_active 待
                // 选配方（state 1）后重新挂载。
                mix_restore_module_view_locked();
                if (mix_tab_buttons[0] != nullptr) {
                    mix_delete_tab_buttons_locked();
                    mix_disable_tab_buttons_locked();
                }
            }
        }
    }
    mix_refresh_item_after_unlock();
    if (runtime_state().host.sdk != nullptr && runtime_state().host.sdk->ui != nullptr && runtime_state().host.sdk->ui->end_frame != nullptr) {
        (void)runtime_state().host.sdk->ui->end_frame();
    }
}

void mix_teardown_locked() {
    mix_restore_module_view_locked();
    mix_disable_tab_buttons_locked();
    for (void*& item : mix_tab_bag_items) {
        if (item != nullptr) {
            if (defer_item_free_locked(item)) item = nullptr;
        }
    }
    mix_frame_active = false;
}


// R-57 原版袋列高亮遮蔽：进入扩展视图时，画原版 UIMix_DrawInvenBagGroup 前把当前袋
// GOT 临时置 kNoOriginalBagSelected(6)，画完恢复 → 原版 6 袋不再高亮（与扩展页
// virtual_bag_draw_original_bag_wrapper 同法）。扩展页签高亮由 draw_end 的
// mix_draw_tab_buttons_locked 在袋列绘制之后补画。
void mix_draw_inven_bag_group_wrapper() {
    const ::qol::contracts::InFlightGuard in_flight_guard(
        runtime_state().host.sdk == nullptr ? nullptr : &runtime_state().host.sdk->in_flight,
        {"patch:P_EXT_MIX_BAG_DRAW"});
    if (runtime_state().host.game == nullptr ||
        runtime_state().host.write_port->ui.draw_original_mix_bag_group == nullptr ||
        runtime_state().host.write_port->ui.current_bag == nullptr ||
        runtime_state().host.write_port->ui.set_current_bag == nullptr) return;
    std::lock_guard<std::mutex> lock(runtime_state().mtx);
    int32_t saved_current = 0;
    bool masked = false;
    if (mix_view_installed &&
        qol::contracts::succeeded(runtime_state().host.write_port->ui.current_bag(&saved_current)) &&
        saved_current != kNoOriginalBagSelected) {
        (void)runtime_state().host.write_port->ui.set_current_bag(kNoOriginalBagSelected);
        masked = true;
    }
    (void)runtime_state().host.write_port->ui.draw_original_mix_bag_group();
    if (masked) (void)runtime_state().host.write_port->ui.set_current_bag(saved_current);
}
