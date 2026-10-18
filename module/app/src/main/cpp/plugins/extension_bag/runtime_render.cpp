#include "plugins/extension_bag/extension_bag_internal.h"

#include "plugins/extension_bag/extension_bag_context.h"
#include "plugins/extension_bag/extension_bag_logging.h"
#include "plugins/extension_bag/game_ui_virtbag.h"
#include "plugins/extension_bag/plugin.h"

#include <cstring>

void log_exit_trace_locked(const char* phase, uint64_t event, uint64_t param,
                           uint64_t param2, int64_t x = -1, int64_t y = -1);

namespace {

void draw_tab_buttons_in_frame_locked() {
    // 配置门控：扩展背包关闭时不得绘制/重装页签，并清除已挂载的页签控件。
    if (!g_virtual_bag_enabled.load()) {
        if (g_extension_tab_root != nullptr || g_extension_tab_buttons[0] != nullptr) {
            delete_extension_tab_buttons_locked();
            disable_extension_tab_buttons_locked();
        }
        return;
    }
    const bool can_draw_original_button = extension_bag_ui_can_draw();
    // 标签挂袋容器（与原版 6 袋标签同父）：比对基准同步为袋容器句柄。
    void* current_bag_container = extension_bag_ui_root(qol::contracts::QolUiRootKind::kEquipBagContainer);
    // 容器就位重试：enter 时 install 可能因占位容器（child count<6）被跳过；
    // 每帧检测真容器就位且尚未挂载（root 变化）时重新 install。
    int child_count = 0;
    if (current_bag_container != nullptr && extension_bag_ui_child_count(current_bag_container, &child_count) &&
        child_count >= 6 && current_bag_container != g_extension_tab_root) {
        install_extension_tab_buttons_locked();
        current_bag_container = extension_bag_ui_root(qol::contracts::QolUiRootKind::kEquipBagContainer);
    }
    const bool tabs_are_current =
        current_bag_container != nullptr && current_bag_container == g_extension_tab_root &&
        g_extension_tab_generation == g_inventory_generation;
    if (!tabs_are_current || !can_draw_original_button) return;
    // 第一遍：底框 loc20（72x84），倒序 index 4→0（对齐原版 DrawInvenBackground 的 5→0）。
    for (int index = virtual_bag::kBagCount - 1; index >= 0; --index) {
        void* button = g_extension_tab_buttons[index];
        if (button == nullptr) continue;
        int64_t ax = 0, ay = 0;
        extension_bag_control_abs_pos(button, &ax, &ay);
        (void)extension_bag_ui_draw_part(0xf, 0x14, static_cast<int32_t>(ax - 5),
                                         static_cast<int32_t>(ay - 12), 0, 1);
    }
    // 第二遍：袋图标 loc6/loc9 + 选中高亮 loc19，正序 index 0→4（对齐原版 DrawInvenBag 的 0→5）。
    for (int index = 0; index < virtual_bag::kBagCount; ++index) {
        void* button = g_extension_tab_buttons[index];
        if (button == nullptr) continue;
        const bool selected = g_virtual_bag_state.mode == virtual_bag::Mode::kModule &&
                              g_virtual_bag_state.selected == index;
        const bool equipped = g_virtual_bag_state.capacities[index] != 0;
        int64_t ax = 0, ay = 0;
        extension_bag_control_abs_pos(button, &ax, &ay);
        if (selected) {
            (void)extension_bag_ui_draw_part(0xf, 0x13, static_cast<int32_t>(ax - 5),
                                             static_cast<int32_t>(ay - 12), 0, 1);
            (void)extension_bag_ui_draw_part(0xf, 6, static_cast<int32_t>(ax + 5),
                                             static_cast<int32_t>(ay + 5), 0, 1);
        } else if (equipped) {
            (void)extension_bag_ui_draw_part(0xf, 6, static_cast<int32_t>(ax + 5),
                                             static_cast<int32_t>(ay + 5), 0, 0x28);
        } else {
            (void)extension_bag_ui_draw_part(0xf, 9, static_cast<int32_t>(ax + 5),
                                             static_cast<int32_t>(ay + 5), 0, 0x28);
        }
    }
}

}

namespace {

void draw_cells_in_frame_locked() {
    const bool can_draw_original_button = extension_bag_ui_can_draw();
    if (g_virtual_bag_state.mode != virtual_bag::Mode::kModule ||
        !virtual_bag::valid_index(g_virtual_bag_state.selected) || g_module_view_installed) {
        return;
    }

    const int bag = g_virtual_bag_state.selected;
    const int capacity = g_virtual_bag_state.capacities[bag];
    for (int slot = 0; slot < capacity; ++slot) {
        const int column = slot % 4;
        const int row = slot / 4;
        const UiRect rect{kGridX + column * kGridStep, kGridY + row * kGridStep,
                          kGridCell, kGridCell};
        const bool occupied = slot < capacity &&
                              g_virtual_bag_state.items[bag][slot].category > 0 &&
                              g_virtual_bag_state.items[bag][slot].count > 0;
        const bool dragging_source = g_extension_drag.active &&
                                     g_extension_drag.bag == bag &&
                                     g_extension_drag.slot == slot;
        if (can_draw_original_button) {
            (void)extension_bag_ui_draw_part(0xf, 0x29, static_cast<int32_t>(rect.x),
                                             static_cast<int32_t>(rect.y), 0, 1);
        }
        if (occupied && !dragging_source) {
            void* item = module_item_locked(bag, slot);
            if (item != nullptr) {
                // ITEM_DrawPorting applies the original +5 inset, rarity
                // frame, icon mapping and stack count itself.
                (void)extension_bag_ui_draw_item(item, static_cast<int32_t>(rect.x),
                                                 static_cast<int32_t>(rect.y), 0, 1);
            }
        }
        if (g_virtual_bag_state.inspected == slot) {
            constexpr int32_t kInspectBorder = 3;
            constexpr uint32_t kInspectColor = 0xff75d8ff;
            (void)extension_bag_ui_fill_rect({rect.x, rect.y, kGridCell, kInspectBorder}, kInspectColor);
            (void)extension_bag_ui_fill_rect(
                {rect.x, rect.y + kGridCell - kInspectBorder, kGridCell, kInspectBorder}, kInspectColor);
            (void)extension_bag_ui_fill_rect({rect.x, rect.y, kInspectBorder, kGridCell}, kInspectColor);
            (void)extension_bag_ui_fill_rect(
                {rect.x + kGridCell - kInspectBorder, rect.y, kInspectBorder, kGridCell}, kInspectColor);
        }
    }
    if (g_extension_drag.active && g_extension_drag.bag == bag) {
        void* item = module_item_locked(bag, g_extension_drag.slot);
        if (item != nullptr) {
            const int32_t ghost_x = static_cast<int32_t>(g_extension_drag.current_x - kGridCell / 2);
            const int32_t ghost_y = static_cast<int32_t>(g_extension_drag.current_y - kGridCell / 2);
            (void)extension_bag_ui_draw_item(item, ghost_x, ghost_y, 0, 1);
        }
    }
}

}  // namespace
void virtual_bag_draw_original_bag_wrapper() {
    const ::qol::contracts::InFlightGuard in_flight_guard(
        g_sdk == nullptr ? nullptr : &g_sdk->in_flight,
        {"patch:P_EXT_INVEN_BAG_DRAW"});
    if (g_game == nullptr || g_write_port->ui.draw_original_bag == nullptr) return;
    std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
    ensure_state_loaded_locked();
    const bool module_view = g_virtual_bag_state.mode == virtual_bag::Mode::kModule &&
                             virtual_bag::valid_index(g_virtual_bag_state.selected);
    static int last_module_view = -1;
    if (last_module_view != static_cast<int>(module_view)) {
        log_exit_trace_locked("draw_bag", 0, 0, 0);
        last_module_view = module_view ? 1 : 0;
    }
    // 进入扩展选中态（kModule）时，原版袋标签全部取消高亮（互斥）：
    // 画原版 DrawInvenBag 前把当前袋 GOT 临时设为 kNoOriginalBagSelected(6)，
    // 画完恢复。此前只在 !installed 分支屏蔽，installed 分支漏掉了。
    const int saved_current = original_bag_locked();
    if (module_view && g_write_port->ui.set_current_bag != nullptr) {
        (void)g_write_port->ui.set_current_bag(kNoOriginalBagSelected);
    }
    if (module_view && g_module_view_installed) {
        (void)g_write_port->ui.draw_original_bag();
        if (module_view) (void)g_write_port->ui.set_current_bag(saved_current);
        draw_tab_buttons_in_frame_locked();
        return;
    }
    (void)g_write_port->ui.draw_original_bag();
    if (module_view) (void)g_write_port->ui.set_current_bag(saved_current);
    draw_tab_buttons_in_frame_locked();
}
void virtual_bag_draw_inven_item_wrapper() {
    const ::qol::contracts::InFlightGuard in_flight_guard(
        g_sdk == nullptr ? nullptr : &g_sdk->in_flight,
        {"patch:P_EXT_INVEN_ITEM_DRAW"});
    if (g_game == nullptr || g_write_port->ui.draw_original_items == nullptr) return;
    std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
    if (g_virtual_bag_state.mode == virtual_bag::Mode::kModule &&
        virtual_bag::valid_index(g_virtual_bag_state.selected)) {
        if (g_module_view_installed) {
            (void)g_write_port->ui.draw_original_items();
            return;
        }
        // Extension items occupy the original inventory rectangle without
        // entering g_inven. Mask the original bag only for this draw call so
        // the underlying ControlItem visuals do not show through the overlay.
        const int saved_current = original_bag_locked();
        (void)g_write_port->ui.set_current_bag(kNoOriginalBagSelected);
        (void)g_write_port->ui.draw_original_items();
        (void)g_write_port->ui.set_current_bag(saved_current);
        return;
    }
    const int saved_current = original_bag_locked();
    if (g_virtual_bag_state.mode == virtual_bag::Mode::kExitingModule &&
        g_exit_display_bag < kNoOriginalBagSelected) {
        (void)g_write_port->ui.set_current_bag(g_exit_display_bag);
    }
    (void)g_write_port->ui.draw_original_items();
    (void)g_write_port->ui.set_current_bag(saved_current);
}
void virtual_bag_draw_end_locked(int inventory_state, std::unique_lock<std::mutex>& lock) {
    static int previous_state = -1;
    const int old_state = previous_state;
    if (old_state != inventory_state) {
        VIRTBAG_LOG("draw-end state edge previous=%d current=%d gen=%llu", old_state,
                    inventory_state, static_cast<unsigned long long>(g_inventory_generation));
    }
    previous_state = inventory_state;
    if (inventory_state >= 0 && inventory_state != 5) {
        if (g_sdk != nullptr && g_sdk->ui != nullptr && g_sdk->ui->end_frame != nullptr) {
            (void)g_sdk->ui->end_frame();
        }
        if (extension_bag_session_admitted_locked()) drain_deferred_frees_locked();
        return;
    }
    if (!extension_bag_session_admitted_locked()) {
        if (g_sdk != nullptr && g_sdk->ui != nullptr && g_sdk->ui->end_frame != nullptr)
            (void)g_sdk->ui->end_frame();
        return;
    }
    int32_t game_state_open = 1;
    const auto* world_facts = qol::features::extension_bag::world_facts();
    if (world_facts != nullptr && world_facts->game_state_gate != nullptr &&
        qol::contracts::succeeded(world_facts->game_state_gate(&game_state_open)) &&
        game_state_open == 0) {
        restore_module_view_locked();
        if (g_sdk != nullptr && g_sdk->ui != nullptr && g_sdk->ui->end_frame != nullptr) {
            (void)g_sdk->ui->end_frame();
        }
        drain_deferred_frees_locked();
        return;
    }
    if (!g_inventory_frame_active) {
        restore_module_view_locked();
        set_original_bag_locked(0);
        virtual_bag::enter_original(&g_virtual_bag_state, 0);
        g_inventory_frame_active = true;
    }
    if (!g_virtual_bag_enabled.load()) {
        restore_module_view_locked();
        if (g_sdk != nullptr && g_sdk->ui != nullptr && g_sdk->ui->end_frame != nullptr) {
            (void)g_sdk->ui->end_frame();
        }
        drain_deferred_frees_locked();
        return;
    }
    ensure_state_loaded_locked();
    commit_pending_extension_tab_locked(lock);
    if (g_module_view_installed) ensure_module_view_root_current_locked();
    // 投影常驻（P2/P3）：module 视图有效期间保持安装；仅在状态不再指向
    // 模块视图（退出/切档/异常）时兜底恢复，防止遗留投影污染原版窗口。
    const bool module_view_expected =
        g_virtual_bag_state.mode == virtual_bag::Mode::kModule &&
        virtual_bag::valid_index(g_virtual_bag_state.selected);
    if (g_module_view_installed && !module_view_expected) restore_module_view_locked();
    if (g_module_view_installed) {
        clear_stale_projected_moving_locked();
    }
    static int last_mode = -1;
    static int last_selected = -2;
    static int last_overlay = -1;
    static int last_overlay_index = -2;
    if (last_mode != static_cast<int>(g_virtual_bag_state.mode) ||
        last_selected != g_virtual_bag_state.selected ||
        last_overlay != static_cast<int>(g_module_view_installed) ||
        last_overlay_index != g_module_view_index) {
        log_exit_trace_locked("draw_end", 0, 0, 0);
        last_mode = static_cast<int>(g_virtual_bag_state.mode);
        last_selected = g_virtual_bag_state.selected;
        last_overlay = g_module_view_installed ? 1 : 0;
        last_overlay_index = g_module_view_index;
    }
    draw_cells_in_frame_locked();
    if (g_sdk != nullptr && g_sdk->ui != nullptr && g_sdk->ui->end_frame != nullptr) {
        (void)g_sdk->ui->end_frame();
    }
    drain_deferred_frees_locked();
}

void virtual_bag_draw_end_wrapper() {
    const ::qol::contracts::InFlightGuard in_flight_guard(
        g_sdk == nullptr ? nullptr : &g_sdk->in_flight,
        {"patch:P_EXT_INVEN_DRAW_END"});
    // Read the panel-owned state before acquiring the runtime lock, then pass an unlockable
    // lock to pending-tab commit so UI callbacks run outside the critical section.
    const int inventory_state = virtual_bag_inventory_state_id();
    std::unique_lock<std::mutex> lock(g_virtual_bag_mtx);
    virtual_bag_draw_end_locked(inventory_state, lock);
}
