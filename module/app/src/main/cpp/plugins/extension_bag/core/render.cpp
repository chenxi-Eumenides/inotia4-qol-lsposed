#include "plugins/extension_bag/core/ui_state.h"
#include "plugins/extension_bag/core/geometry.h"
#include "plugins/extension_bag/core/lifecycle.h"
#include "plugins/extension_bag/runtime/ownership_port.h"
#include "plugins/extension_bag/core/projection.h"
#include "plugins/extension_bag/runtime/shared.h"
#include "plugins/extension_bag/core/observation.h"
#include "plugins/extension_bag/runtime/state.h"
#include "plugins/extension_bag/runtime/inventory_access.h"
#include "plugins/extension_bag/runtime/projection_controller.h"
#include "plugins/extension_bag/core/tabs.h"
#include "plugins/extension_bag/runtime/reentry_lock.h"

#include "plugins/extension_bag/runtime/logging.h"
#include "plugins/extension_bag/core/game_ui_port.h"
#include "plugins/extension_bag/runtime/bag_state_store.h"

#include <cstring>

void prepare_c18_native_bag_layout_locked();

namespace {

bool is_c18_native_bag_layout_candidate() {
    const auto* game = runtime_state().host.game;
    return game != nullptr && game->target.has_capability != nullptr &&
        game->target.has_capability(
            qol::contracts::kQolTargetCapabilityC18NativeBagLayoutCandidate) != 0;
}

// bag 页上的原生页签 = 原版袋 + 该改版追加的袋，全部是袋容器的 type-2 直接子控件
// （模块页签是后挂的锚定控件，必须排除）。该数量正好是最后一个页签的下一个索引，
// 因此把它写成当前袋时没有任何原生页签匹配选中，模块自己的选中环是唯一高亮。
// v514 的 DrawInvenBag 选中分支只对 child<6 生效（ccmp lt），改版追加页签
// 6..N-1 永远不会画成“选中”样子。这里用同一套原生素材补上选中页的亮图标：
// 图标位置与 UIEquip_DrawInvenBag 相同（rect 原点 +5），不改变任何袋状态。
void draw_c18_active_native_tab_locked() {
    if (!is_c18_native_bag_layout_candidate() || qol::extension_bag::ui_state().store.view_installed) return;
    const int raw_bag = raw_current_bag_locked();
    if (raw_bag < virtual_bag::kOriginalNativeBagCount) return;
    void* container = extension_bag_ui_root(qol::contracts::QolUiRootKind::kEquipBagContainer);
    if (container == nullptr) return;
    int child_count = 0;
    if (!extension_bag_ui_child_count(container, &child_count) || raw_bag >= child_count) return;
    void* child = extension_bag_ui_child_at(container, raw_bag);
    if (child == nullptr || extension_bag_ui_parent(child) != container ||
        extension_bag_ui_user_type(child) != 2) return;
    for (const auto& anchor : qol::extension_bag::ui_state().ui.extension_tab_anchors) {
        if (anchor.mounted != 0 && anchor.handle == child) return;
    }
    int64_t ax = 0;
    int64_t ay = 0;
    if (!extension_bag_ui_control_origin(child, &ax, &ay)) return;
    (void)extension_bag_ui_draw_part(0xf, 6, static_cast<int32_t>(ax + 5),
                                     static_cast<int32_t>(ay + 5), 0, 1);
}

int c18_native_highlight_suppression_bag_locked() {
    void* container = extension_bag_ui_root(qol::contracts::QolUiRootKind::kEquipBagContainer);
    if (container == nullptr) return -1;
    int child_count = 0;
    if (!extension_bag_ui_child_count(container, &child_count) || child_count <= 0) return -1;
    constexpr int kMaxScan = 32;
    const int limit = child_count < kMaxScan ? child_count : kMaxScan;
    int native_tabs = 0;
    for (int index = 0; index < limit; ++index) {
        void* child = extension_bag_ui_child_at(container, index);
        if (child == nullptr || extension_bag_ui_user_type(child) != 2) continue;
        bool module_tab = false;
        for (const auto& anchor : qol::extension_bag::ui_state().ui.extension_tab_anchors) {
            if (anchor.mounted != 0 && anchor.handle == child) {
                module_tab = true;
                break;
            }
        }
        if (!module_tab) ++native_tabs;
    }
    return native_tabs > 0 ? native_tabs : -1;
}

// 原版 DrawInvenBag 的选中高亮是叠加绘制，未选中分支只重画图标，不会擦掉
// 上一帧的 0xf/0x13。扩展页选中时即使临时 current_bag 已指向无匹配值，旧高亮
// 仍可能残留。这里对原版 0..5 重画底框和未选中图标，不写任何袋状态，也不影响
// core.item。
void clear_native_tab_highlight_locked(int bag) {
    if (bag < 0 || bag >= virtual_bag::kOriginalNativeBagCount) return;
    void* container = extension_bag_ui_root(qol::contracts::QolUiRootKind::kEquipBagContainer);
    if (container == nullptr) return;
    void* tab = extension_bag_ui_child_at(container, bag);
    if (tab == nullptr || extension_bag_ui_parent(tab) != container ||
        extension_bag_ui_user_type(tab) != 2) {
        return;
    }
    int64_t ax = 0;
    int64_t ay = 0;
    extension_bag_control_abs_pos(tab, &ax, &ay);
    (void)extension_bag_ui_draw_part(0xf, 0x14, static_cast<int32_t>(ax - 5),
                                     static_cast<int32_t>(ay - 12), 0, 1);
    uint32_t size_word = 0;
    // 原生对袋 0 和任务袋 5 走特殊分支，固定使用已装备袋图标。
    const bool special = bag == 0 || bag == virtual_bag::kOriginalTaskBag;
    const bool equipped = special ||
                          (bag_size_word_locked(bag, &size_word) &&
                           (size_word & ((1u << 25) - 1u)) != 0);
    if (equipped) {
        if (!extension_bag_ui_draw_part_alpha(0xf, 6, static_cast<int32_t>(ax + 5),
                                              static_cast<int32_t>(ay + 5), 0, 1, 0x28)) {
            (void)extension_bag_ui_draw_part(0xf, 6, static_cast<int32_t>(ax + 5),
                                             static_cast<int32_t>(ay + 5), 0, 1);
        }
    } else {
        if (!extension_bag_ui_draw_part_alpha(0xf, 9, static_cast<int32_t>(ax + 5),
                                              static_cast<int32_t>(ay + 5), 0, 1, 0x28)) {
            (void)extension_bag_ui_draw_part(0xf, 9, static_cast<int32_t>(ax + 5),
                                             static_cast<int32_t>(ay + 5), 0, 1);
        }
    }
}

void clear_native_tab_highlights_locked() {
    // 只处理原版 0..5。C18 追加页签由原生绘制和 current_bag 抑制负责，
    // EB 不能再叠加图标，否则会把所有追加袋画成“已选中”。
    for (int bag = 0; bag < virtual_bag::kOriginalNativeBagCount; ++bag) {
        clear_native_tab_highlight_locked(bag);
    }
}

void draw_tab_buttons_in_frame_locked() {
    // 配置门控：扩展背包关闭时不得绘制/重装页签，并清除已挂载的页签控件。
    if (!runtime_state().enabled.load()) {
        if (qol::extension_bag::ui_state().ui.extension_tab_root != nullptr || qol::extension_bag::ui_state().ui.extension_tab_buttons[0] != nullptr) {
            delete_extension_tab_buttons_locked();
            disable_extension_tab_buttons_locked();
        }
        return;
    }
    // Store owns its own tab column. The equip-page tab controls stay mounted but
    // must not be drawn over the store, otherwise two EB tab columns appear.
    if (qol::extension_bag::ui_state().store.view_installed) return;
    const bool can_draw_original_button = extension_bag_ui_can_draw();
    // 标签挂袋容器（与原版 6 袋标签同父）：比对基准同步为袋容器句柄。
    void* current_bag_container = extension_bag_ui_root(qol::contracts::QolUiRootKind::kEquipBagContainer);
    // 容器就位重试：enter 时 install 可能因占位容器（child count<6）被跳过；
    // 每帧检测真容器就位且尚未挂载（root 变化）时重新 install。
    int child_count = 0;
    if (current_bag_container != nullptr && extension_bag_ui_child_count(current_bag_container, &child_count) &&
        child_count >= virtual_bag::kOriginalNativeBagCount) {
        // 每帧 ensure：游戏切页/关闭面板会销毁页签子树，锚定层负责证明并重建。
        install_extension_tab_buttons_locked();
        current_bag_container = extension_bag_ui_root(qol::contracts::QolUiRootKind::kEquipBagContainer);
    }
    const bool tabs_are_current =
        current_bag_container != nullptr && current_bag_container == qol::extension_bag::ui_state().ui.extension_tab_root &&
        qol::extension_bag::ui_state().ui.extension_tab_generation == qol::extension_bag::ui_state().ui.inventory_generation;
    if (!tabs_are_current || !can_draw_original_button) return;
    const int active = virtual_bag::active_extension_bags();
    const int visible = active < virtual_bag::kMaxRenderedExtensionBagTabs
                            ? active : virtual_bag::kMaxRenderedExtensionBagTabs;
    static int last_pending_multi_column_count = 0;
    if (active > virtual_bag::kMaxRenderedExtensionBagTabs &&
        last_pending_multi_column_count != active) {
        VIRTBAG_LOG("tab layout pending multi-column count=%d", active);
        last_pending_multi_column_count = active;
    } else if (active <= virtual_bag::kMaxRenderedExtensionBagTabs) {
        last_pending_multi_column_count = 0;
    }
    // 第一遍：底框 loc20，最多绘制已有单列布局可容纳的前五个页签。
    for (int index = visible - 1; index >= 0; --index) {
        void* button = qol::extension_bag::ui_state().ui.extension_tab_buttons[index];
        if (button == nullptr) continue;
        int64_t ax = 0, ay = 0;
        extension_bag_control_abs_pos(button, &ax, &ay);
        (void)extension_bag_ui_draw_part(0xf, 0x14, static_cast<int32_t>(ax - 5),
                                         static_cast<int32_t>(ay - 12), 0, 1);
    }
    // 第二遍：袋图标 loc6/loc9 + 选中高亮 loc19。
    for (int index = 0; index < visible; ++index) {
        void* button = qol::extension_bag::ui_state().ui.extension_tab_buttons[index];
        if (button == nullptr) continue;
        const bool selected = runtime_state().objects.bag.mode == virtual_bag::Mode::kModule &&
                              runtime_state().objects.bag.selected == index;
        const bool equipped = runtime_state().objects.bag.capacities[index] != 0;
        int64_t ax = 0, ay = 0;
        extension_bag_control_abs_pos(button, &ax, &ay);
        if (selected) {
            (void)extension_bag_ui_draw_part(0xf, 0x13, static_cast<int32_t>(ax - 5),
                                             static_cast<int32_t>(ay - 12), 0, 1);
            (void)extension_bag_ui_draw_part(0xf, 6, static_cast<int32_t>(ax + 5),
                                             static_cast<int32_t>(ay + 5), 0, 1);
        } else if (equipped) {
            if (!extension_bag_ui_draw_part_alpha(0xf, 6, static_cast<int32_t>(ax + 5),
                                                  static_cast<int32_t>(ay + 5), 0, 1, 0x28)) {
                (void)extension_bag_ui_draw_part(0xf, 6, static_cast<int32_t>(ax + 5),
                                                 static_cast<int32_t>(ay + 5), 0, 1);
            }
        } else {
            if (!extension_bag_ui_draw_part_alpha(0xf, 9, static_cast<int32_t>(ax + 5),
                                                  static_cast<int32_t>(ay + 5), 0, 1, 0x28)) {
                (void)extension_bag_ui_draw_part(0xf, 9, static_cast<int32_t>(ax + 5),
                                                 static_cast<int32_t>(ay + 5), 0, 1);
            }
        }
    }
}

}

namespace {

void draw_cells_in_frame_locked() {
    const bool can_draw_original_button = extension_bag_ui_can_draw();
    if (runtime_state().objects.bag.mode != virtual_bag::Mode::kModule ||
        !virtual_bag::valid_index(runtime_state().objects.bag.selected) || qol::extension_bag::ui_state().objects.module_view_installed) {
        return;
    }

    const int bag = runtime_state().objects.bag.selected;
    const int capacity = runtime_state().objects.bag.capacities[bag];
    for (int slot = 0; slot < capacity; ++slot) {
        const int column = slot % 4;
        const int row = slot / 4;
        const qol::contracts::UiRect rect{kGridX + column * kGridStep, kGridY + row * kGridStep,
                          kGridCell, kGridCell};
        const bool occupied = slot < capacity &&
                              runtime_state().objects.bag.items[bag][slot].category > 0 &&
                              runtime_state().objects.bag.items[bag][slot].count > 0;
        const bool dragging_source = qol::extension_bag::ui_state().ui.extension_drag.active &&
                                     qol::extension_bag::ui_state().ui.extension_drag.bag == bag &&
                                     qol::extension_bag::ui_state().ui.extension_drag.slot == slot;
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
        if (runtime_state().objects.bag.inspected == slot) {
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
    if (qol::extension_bag::ui_state().ui.extension_drag.active && qol::extension_bag::ui_state().ui.extension_drag.bag == bag) {
        void* item = module_item_locked(bag, qol::extension_bag::ui_state().ui.extension_drag.slot);
        if (item != nullptr) {
            const int32_t ghost_x = static_cast<int32_t>(qol::extension_bag::ui_state().ui.extension_drag.current_x - kGridCell / 2);
            const int32_t ghost_y = static_cast<int32_t>(qol::extension_bag::ui_state().ui.extension_drag.current_y - kGridCell / 2);
            (void)extension_bag_ui_draw_item(item, ghost_x, ghost_y, 0, 1);
        }
    }
}

}  // namespace
void virtual_bag_draw_original_bag_wrapper() {
    const ::qol::contracts::InFlightGuard in_flight_guard(
        runtime_state().host.sdk == nullptr ? nullptr : &runtime_state().host.sdk->in_flight,
        {"patch:P_EXT_INVEN_BAG_DRAW"});
    if (runtime_state().host.game == nullptr || runtime_state().host.write_port->ui.draw_original_bag == nullptr) return;
    ::qol::extension_bag::ReentryLock lock;
    const bool module_view = runtime_state().objects.bag.mode == virtual_bag::Mode::kModule &&
                             virtual_bag::valid_index(runtime_state().objects.bag.selected);
    static int last_module_view = -1;
    if (last_module_view != static_cast<int>(module_view)) {
        log_exit_trace_locked("draw_bag", 0, 0, 0);
        last_module_view = module_view ? 1 : 0;
    }
    // 进入扩展选中态（kModule）或商店扩展视图时，原版袋标签全部取消高亮
    // （互斥）：只在本帧原版 DrawInvenBag 前后临时改 current_bag，画完恢复。
    // 不能在整个商店视图期间改 current_bag，否则商店投影会按“无当前袋”刷新，
    // 右侧 EB 袋物品区会变空。
    const bool mask_original_highlight = module_view || qol::extension_bag::ui_state().store.view_installed;
    // C18 改版袋把追加页签放在原生袋数之后；不使用固定袋号表示无选中。
    // 用“原版袋 + 追加袋”的总数作临时当前袋，索引越界即无页签匹配高亮。
    const bool c18_native_module_view =
        module_view && is_c18_native_bag_layout_candidate();
    int suppression_bag = -1;
    if (mask_original_highlight) {
        suppression_bag = c18_native_module_view
            ? c18_native_highlight_suppression_bag_locked()
            : native_bag_count_locked();
    }
    const int saved_current = raw_current_bag_locked();
    // 退出扩展视图的首帧：current_bag 可能仍是投影窗口袋（通常是 0），
    // 但用户点击的是原版袋 X。绘制袋列前临时切到 exit_display_bag，
    // 避免先闪一帧原版袋 0 的选中态。
    const bool exiting_to_original =
        runtime_state().objects.bag.mode == virtual_bag::Mode::kExitingModule &&
        qol::extension_bag::ui_state().ui.exit_display_bag >= 0;
    const bool write_exit_display =
        exiting_to_original && saved_current >= 0 &&
        runtime_state().host.write_port->ui.set_current_bag != nullptr;
    // 读不到原值时不写临时袋号：否则可能留下无效 current_bag。
    const bool write_highlight_suppression =
        suppression_bag >= 0 && saved_current >= 0 &&
        runtime_state().host.write_port->ui.set_current_bag != nullptr;
    if (write_exit_display) {
        (void)runtime_state().host.write_port->ui.set_current_bag(
            qol::extension_bag::ui_state().ui.exit_display_bag);
    } else if (write_highlight_suppression) {
        (void)runtime_state().host.write_port->ui.set_current_bag(suppression_bag);
    }
    // Native bag controls consume CO_RECT during DrawInvenBag. Apply any
    // collision-driven C18 shift before the original draw reads those rects.
    prepare_c18_native_bag_layout_locked();
    if (module_view && qol::extension_bag::ui_state().objects.module_view_installed) {
        (void)runtime_state().host.write_port->ui.draw_original_bag();
        if ((write_highlight_suppression || write_exit_display) && saved_current >= 0) {
            (void)runtime_state().host.write_port->ui.set_current_bag(saved_current);
        }
        if (module_view && !qol::extension_bag::ui_state().store.view_installed) clear_native_tab_highlights_locked();
        draw_tab_buttons_in_frame_locked();
        return;
    }
    (void)runtime_state().host.write_port->ui.draw_original_bag();
    // 退出扩展视图时 current_bag 已临时切到 exit_display_bag；C18 选中图标
    // 必须在恢复 current_bag 之前补画，否则会画成上一个 C18 袋。
    if (!module_view) draw_c18_active_native_tab_locked();
    if ((write_highlight_suppression || write_exit_display) && saved_current >= 0) {
        (void)runtime_state().host.write_port->ui.set_current_bag(saved_current);
    }
    if (module_view && !qol::extension_bag::ui_state().store.view_installed) clear_native_tab_highlights_locked();
    if (!qol::extension_bag::ui_state().store.view_installed) draw_tab_buttons_in_frame_locked();
}
void virtual_bag_draw_inven_item_wrapper() {
    const ::qol::contracts::InFlightGuard in_flight_guard(
        runtime_state().host.sdk == nullptr ? nullptr : &runtime_state().host.sdk->in_flight,
        {"patch:P_EXT_INVEN_ITEM_DRAW"});
    if (runtime_state().host.game == nullptr || runtime_state().host.write_port->ui.draw_original_items == nullptr) return;
    ::qol::extension_bag::ReentryLock lock;
    if (runtime_state().objects.bag.mode == virtual_bag::Mode::kModule &&
        virtual_bag::valid_index(runtime_state().objects.bag.selected)) {
        if (qol::extension_bag::ui_state().objects.module_view_installed) {
            (void)runtime_state().host.write_port->ui.draw_original_items();
            return;
        }
        // Extension items occupy the original inventory rectangle without
        // entering g_inven. Mask the original bag only for this draw call so
        // the underlying ControlItem visuals do not show through the overlay.
        const int saved_current = original_bag_locked();
        const bool write_legacy_sentinel = !is_c18_native_bag_layout_candidate();
        if (write_legacy_sentinel) {
            (void)runtime_state().host.write_port->ui.set_current_bag(native_bag_count_locked());
        }
        (void)runtime_state().host.write_port->ui.draw_original_items();
        if (write_legacy_sentinel) {
            (void)runtime_state().host.write_port->ui.set_current_bag(saved_current);
        }
        return;
    }
    // Raw 值必须原样恢复：C18 改版袋的当前袋是 6..N-1，原版投影的 0..5 clamp
    // 会把刚由原生回调选中的页签写回 0，表现为点追加页签后又高亮回原版袋。
    const int saved_current = raw_current_bag_locked();
    if (runtime_state().objects.bag.mode == virtual_bag::Mode::kExitingModule &&
        qol::extension_bag::ui_state().ui.exit_display_bag >= 0) {
        (void)runtime_state().host.write_port->ui.set_current_bag(qol::extension_bag::ui_state().ui.exit_display_bag);
    }
    (void)runtime_state().host.write_port->ui.draw_original_items();
    if (saved_current >= 0) {
        (void)runtime_state().host.write_port->ui.set_current_bag(saved_current);
    }
}
void virtual_bag_draw_end_locked(int inventory_state, ::qol::extension_bag::ReentryLock& lock) {
    static int previous_state = -1;
    const int old_state = previous_state;
    if (old_state != inventory_state) {
        VIRTBAG_LOG("draw-end state edge previous=%d current=%d gen=%llu", old_state,
                    inventory_state, static_cast<unsigned long long>(qol::extension_bag::ui_state().ui.inventory_generation));
    }
    previous_state = inventory_state;
    if (inventory_state >= 0 && inventory_state != 5) {
        // 页签点击只登记 pending，实际提交在 draw-end。装备页/扩展视图的
        // inventory_state 为 4，不能因为非 5 就跳过 pending 提交。
        if (qol::extension_bag::ui_state().ui.pending_extension_tab >= 0) {
            commit_pending_extension_tab_locked(lock);
        }
        // Empty EB bag unequip switches to original mode while the inventory
        // panel is still state 4. This branch used to return before the mode
        // check below, leaving the projected item controls installed and the
        // original bag looking empty until a bag switch forced a refresh.
        if (qol::extension_bag::ui_state().objects.module_view_installed) {
            const bool module_view_expected =
                runtime_state().objects.bag.mode == virtual_bag::Mode::kModule &&
                virtual_bag::valid_index(runtime_state().objects.bag.selected);
            if (!module_view_expected) restore_module_view_locked();
        }
        if (runtime_state().host.sdk != nullptr && runtime_state().host.sdk->ui != nullptr && runtime_state().host.sdk->ui->end_frame != nullptr) {
            (void)runtime_state().host.sdk->ui->end_frame();
        }
        if (extension_bag_session_admitted_locked()) drain_deferred_frees_locked();
        return;
    }
    if (!extension_bag_session_admitted_locked()) {
        if (runtime_state().host.sdk != nullptr && runtime_state().host.sdk->ui != nullptr && runtime_state().host.sdk->ui->end_frame != nullptr)
            (void)runtime_state().host.sdk->ui->end_frame();
        return;
    }
    int32_t game_state_open = 1;
    const auto* world_facts = extension_bag_world_facts();
    if (world_facts != nullptr && world_facts->game_state_gate != nullptr &&
        qol::contracts::succeeded(world_facts->game_state_gate(&game_state_open)) &&
        game_state_open == 0) {
        restore_module_view_locked();
        if (runtime_state().host.sdk != nullptr && runtime_state().host.sdk->ui != nullptr && runtime_state().host.sdk->ui->end_frame != nullptr) {
            (void)runtime_state().host.sdk->ui->end_frame();
        }
        drain_deferred_frees_locked();
        return;
    }
    if (!qol::extension_bag::ui_state().ui.inventory_frame_active) {
        restore_module_view_locked();
        set_original_bag_locked(0);
        virtual_bag::enter_original(&runtime_state().objects.bag, 0);
        qol::extension_bag::ui_state().ui.inventory_frame_active = true;
    }
    if (!runtime_state().enabled.load()) {
        restore_module_view_locked();
        if (runtime_state().host.sdk != nullptr && runtime_state().host.sdk->ui != nullptr && runtime_state().host.sdk->ui->end_frame != nullptr) {
            (void)runtime_state().host.sdk->ui->end_frame();
        }
        drain_deferred_frees_locked();
        return;
    }
    commit_pending_extension_tab_locked(lock);
    if (qol::extension_bag::ui_state().objects.module_view_installed) ensure_module_view_root_current_locked();
    // 投影常驻（P2/P3）：module 视图有效期间保持安装；仅在状态不再指向
    // 模块视图（退出/切档/异常）时兜底恢复，防止遗留投影污染原版窗口。
    const bool module_view_expected =
        runtime_state().objects.bag.mode == virtual_bag::Mode::kModule &&
        virtual_bag::valid_index(runtime_state().objects.bag.selected);
    if (qol::extension_bag::ui_state().objects.module_view_installed && !module_view_expected) restore_module_view_locked();
    if (qol::extension_bag::ui_state().objects.module_view_installed) {
        clear_stale_projected_moving_locked();
    }
    static int last_mode = -1;
    static int last_selected = -2;
    static int last_overlay = -1;
    static int last_overlay_index = -2;
    if (last_mode != static_cast<int>(runtime_state().objects.bag.mode) ||
        last_selected != runtime_state().objects.bag.selected ||
        last_overlay != static_cast<int>(qol::extension_bag::ui_state().objects.module_view_installed) ||
        last_overlay_index != qol::extension_bag::ui_state().objects.module_view_index) {
        log_exit_trace_locked("draw_end", 0, 0, 0);
        last_mode = static_cast<int>(runtime_state().objects.bag.mode);
        last_selected = runtime_state().objects.bag.selected;
        last_overlay = qol::extension_bag::ui_state().objects.module_view_installed ? 1 : 0;
        last_overlay_index = qol::extension_bag::ui_state().objects.module_view_index;
    }
    draw_cells_in_frame_locked();
    if (runtime_state().host.sdk != nullptr && runtime_state().host.sdk->ui != nullptr && runtime_state().host.sdk->ui->end_frame != nullptr) {
        (void)runtime_state().host.sdk->ui->end_frame();
    }
    drain_deferred_frees_locked();
}

void virtual_bag_draw_end_wrapper() {
    const ::qol::contracts::InFlightGuard in_flight_guard(
        runtime_state().host.sdk == nullptr ? nullptr : &runtime_state().host.sdk->in_flight,
        {"patch:P_EXT_INVEN_DRAW_END"});
    // Read the panel-owned state before acquiring the runtime lock, then pass an unlockable
    // lock to pending-tab commit so UI callbacks run outside the critical section.
    const int inventory_state = virtual_bag_inventory_state_id();
    ::qol::extension_bag::ReentryLock lock;
    // 主线程绘制收尾：revision 变化时刷新轻量快照，读侧无需再加锁。
    qol::extension_bag::publish_extension_bag_snapshot_if_changed();
    virtual_bag_draw_end_locked(inventory_state, lock);
}
