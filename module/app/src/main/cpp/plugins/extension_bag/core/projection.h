#pragma once

#include "plugins/extension_bag/runtime/state.h"
#include "plugins/extension_bag/runtime/reentry_lock.h"
#include "plugins/extension_bag/runtime/projection_controller.h"

struct NativeCallScope {
    const bool previous = g_in_native_call;
    const qol::contracts::QolGameHost* game = runtime_state().host.game;

    NativeCallScope() {
        g_in_native_call = true;
        if (game != nullptr && game->item_abi.begin_original_only != nullptr) {
            game->item_abi.begin_original_only();
        }
    }
    ~NativeCallScope() {
        if (game != nullptr && game->item_abi.end_original_only != nullptr) {
            game->item_abi.end_original_only();
        }
        g_in_native_call = previous;
    }
};

void refresh_module_item_area_locked(int index);
void virtual_bag_draw_original_bag_wrapper();
void virtual_bag_draw_inven_item_wrapper();
void clear_original_bag_selection_locked();
void clear_original_desc_locked();
int original_bag_button_index(int64_t x, int64_t y);
bool bind_original_exit_display_bag_locked(int bag);
void* module_item_locked(int bag, int slot);
uint32_t save_inventory_wrapper(uint8_t* cursor);
uint32_t item_draw_porting_gate(void* item, int32_t x, int32_t y,
                                int32_t a, int32_t b, int32_t c);
int32_t save_item_on_empty_gate(void* item, int32_t bag);
bool install_module_view_locked(int bag);
bool ensure_module_view_root_current_locked();
void refresh_projection_if_overwritten_locked();
bool sync_projected_slot_locked(int bag, int slot);
void refresh_projected_module_view_after_move_locked();
// 跨空间移动后刷新仍处于原生视图的袋。扩展视图安装时只刷新投影，
// 避免原版刷新把投影控件覆盖回空槽。
void refresh_native_item_area_after_move_locked(int native_bag);
void restore_module_view_locked();
bool complete_original_exit_locked();
void cancel_original_exit_locked();
void virtual_bag_draw_end_locked(int inventory_state, ::qol::extension_bag::ReentryLock& lock);
void virtual_bag_draw_end_wrapper();
void virtual_bag_call_original_refresh_item_area_with_guard();
bool virtual_bag_sync_projected_bag();
bool item_pointer_known_locked(void* item);
