#pragma once

#include "contracts/craft_facts.h"
#include "contracts/game_host.h"
#include "contracts/game_ports/extension_bag_write_port.h"
#include "contracts/game_ports/character_extras_port.h"
#include "contracts/game_ports/inventory_extras_port.h"
#include "contracts/host_sdk.h"
#include "contracts/item_facts.h"
#include "contracts/ownership.h"
#include "plugins/extension_bag/extension_bag_geometry.h"
#include "plugins/extension_bag/extension_bag_port.h"
#include "plugins/extension_bag/model/ownership_ledger.h"
#include "plugins/extension_bag/model/virtual_bag_state.h"
#include "plugins/extension_bag/runtime_state.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

#include <jni.h>

enum class ExtensionBagEquipResult {
    kOk,
    kReject,         // 前置不满足：非背包物品 / 扩展位已装备 / 不在原版库存
    kFailed,         // 移除或回滚失败（状态可能不一致，强日志）
    kPersistFailed,  // persist 失败且已完整回滚
};

enum class ExtensionBagUnequipResult {
    kOk,
    kNotEquipped,
    kNotEmpty,
    kNoSpace,
    kPersistFailed,
    kFailed,
};

struct NativeCallScope {
    const bool previous = g_in_native_call;
    const qol::contracts::QolGameHost* game = g_game;

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

// 触摸事件使用 Scene_Draw 的绝对逻辑坐标；真机运行时宽度为 1408。
constexpr int64_t kCellX = 0x4c4;
constexpr int64_t kCellY = 0x86;
// Phase one uses an independent read-only overlay. These coordinates stay in
// the same absolute logical space as the original equipment scene.
// UIEquip_CreateInvenControl absolute origin: bag button origin minus the
// bag container offset plus the item-container offset.
constexpr int64_t kGridX = 0x45c - 0x185 + 0x22; // 761
constexpr int64_t kGridY = 0x91 - 0x0c + 0x1b;   // 160
constexpr int64_t kGridCell = 0x4a;
constexpr int64_t kGridStep = 0x53;
constexpr int64_t kOriginalGridX = kGridX;
constexpr int64_t kOriginalGridY = kGridY;
constexpr int kEquipSlotCount = 10;
void extension_bag_mark_dirty_locked();
uint64_t extension_bag_item_revision_locked();
const qol::contracts::QolExtensionBagWritePort* extension_bag_write_api();
bool read_item_category(const void* item, int* out_category);
bool read_item_is_jewel(int category);
bool read_item_is_enchant_scroll(int category);
bool read_item_is_mercenary_seal(int category);
bool read_item_is_usable(int category);
bool item_is_equip(const void* item);
void* extension_create_item(int category, int arg1, int arg2, int arg3);
bool mark_native_bag_object(void* item);
bool extension_free_item(void* item);
const qol::contracts::QolItemCountApi* item_count_api();
bool read_world_state_id(int* out_state);
bool read_world_in_world();
int virtual_bag_inventory_state_id();
bool module_slot_of_item_locked(void* item, int* out_bag, int* out_slot);
void* valid_child_locked(void* root, int slot);
bool defer_item_free_locked(void* item);
void drain_deferred_frees_locked();
void log_ownership_audit_locked(const char* context);
bool object_touch_window_active_locked();
bool retire_custody_item_locked(uint32_t handle, void* item, const char* context);
void* load_item_payload_tracked_locked(const uint8_t* payload, int payload_size,
                                       const char* context, uint32_t* out_handle);
void handover_tracked_item_locked(uint32_t handle, void* item, const char* context);
void release_tracked_item_locked(uint32_t handle, void* item, const char* context);
bool module_slot_is_assignable_locked(int bag, int slot);
bool module_object_replacement_allowed_locked(int bag, int slot, const char* context);
bool module_object_release_allowed_locked(int bag, int slot, void* item, const char* context,
                                          bool allow_owner, const ModuleUseToken* token);
bool module_object_generation_advance_locked(int bag, int slot, const char* context);
bool module_use_begin_locked(int bag, int slot, void* item, ModuleUseToken* out_token);
bool module_use_finish_locked(const ModuleUseToken& token, bool* out_consumed,
                              ModuleUseReleasePlan* out_release);
bool module_use_abort_locked(const ModuleUseToken& token);
bool module_use_abort_isolate_locked(const ModuleUseToken& token);
bool module_use_release_native(const ModuleUseReleasePlan& release);
bool module_use_finalize_release_locked(const ModuleUseReleasePlan& release);
bool free_module_object_locked(int bag, int slot, void* raw_use_token = nullptr);

bool p5_observation_enabled();
bool p5_panel_observation_enabled();
bool p5_should_begin_item_observation(uint64_t event);
uint64_t log_p5_observation_locked(const char* phase, void* target_control, uint64_t event,
                                   bool x2_present, bool param_present, void* source_control,
                                   bool result_known, uint64_t result, int64_t event_x = -1,
                                   int64_t event_y = -1, uint64_t related_sequence = 0);

int extension_tab_index(void* ctrl);
void refresh_tab_bag_items_locked();
void begin_projected_drag_session_locked(int source_slot);
void advance_projected_drag_session_locked();
bool projected_drag_session_active_locked();
void cancel_projected_drag_session_locked(const char* reason);
void* touch_moving_item_control_locked();
void reset_drag_state_locked(void* moving_control);
void clear_stale_projected_moving_locked();
void complete_original_release_cleanup_locked(void* moving_control, int64_t rel_x, int64_t rel_y,
                                              uint64_t release_param2, bool handled);
void clear_original_item_selection_locked();
int original_item_category_locked(void* item);
int drag_source_category_locked();

void draw_cells_in_frame_locked();
bool bag_size_word_locked(int bag, uint32_t* out_word);
bool set_bag_size_word_locked(int bag, uint32_t word);
void refresh_module_item_area_locked(int index);
void virtual_bag_draw_original_bag_wrapper();
void virtual_bag_draw_inven_item_wrapper();
int original_bag_locked();
void set_original_bag_locked(int bag);
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
void restore_module_view_locked();
bool complete_original_exit_locked();
void cancel_original_exit_locked();
void virtual_bag_f3_wrapper();
void virtual_bag_inventory_enter_wrapper();
void virtual_bag_save_panel_enter_wrapper();
void virtual_bag_draw_end_locked(int inventory_state, std::unique_lock<std::mutex>& lock);
void virtual_bag_draw_end_wrapper();
void log_exit_trace_locked(const char* phase, uint64_t event, uint64_t param,
                           uint64_t param2, int64_t x, int64_t y);

uint64_t virtual_bag_event(uint64_t event, uint64_t param, uint64_t param2);
void ensure_lifecycle_task();
void remove_lifecycle_task();
bool prepare_main_menu_locked();
void teardown_extension_bag_hosts_locked();
bool clear_module_cache_locked();
bool module_cache_clear_preflight_locked();
void commit_pending_extension_tab_locked(std::unique_lock<std::mutex>& lock);
void reset_extension_desc_unequip_hook_locked();
void reset_extension_desc_item_hooks_locked();
void restore_extension_desc_popup_callbacks_locked();
PhysicalInventorySnapshot capture_physical_inventory_snapshot_locked();
void ensure_state_loaded_locked();
bool persist_state_locked(bool force = false);
void virtual_bag_call_original_refresh_item_area_with_guard();
bool virtual_bag_sync_projected_bag();
void delete_extension_tab_buttons_locked();
void disable_extension_tab_buttons_locked();
void install_extension_tab_buttons_locked();
bool extension_tab_buttons_complete_locked();
void* materialize_module_item_locked(const virtual_bag::Item& descriptor);
void release_temporary_item_locked(void* item);
bool item_pointer_known_locked(void* item);

bool move_original_to_extension_slot_locked(
    std::unique_lock<std::mutex>& lock, int src_bag, int src_slot, int dst_bag,
    qol::contracts::QolItemHandle expected_item = qol::contracts::kInvalidItemHandle);
bool move_original_to_extension_locked(std::unique_lock<std::mutex>& lock, int dst_bag,
                                       void* moving_control);
bool move_extension_to_original_locked(std::unique_lock<std::mutex>& lock, int src_bag,
                                       int src_slot, int target_bag);
bool move_extension_to_extension_locked(int src_bag, int src_slot, int dst_bag,
                                        int requested_dst_slot = -1);
bool route_projected_session_to_tab_locked(std::unique_lock<std::mutex>& lock, int target_bag);
bool extension_save_item_on_empty(void* item, int bag);
bool serialize_item_payload_locked(
    void* item, std::array<uint8_t, virtual_bag::kSerializedItemBuffer>* out, int* out_size);
void* load_item_payload_locked(const uint8_t* payload, int payload_size, const char* context);
void generate_transaction_id_locked(char* out);
bool txn_record_pending_locked(virtual_bag::TransactionContext* txn);
void txn_abort_locked(const virtual_bag::TransactionContext& txn,
                      virtual_bag::TxnStage failed_at, const char* reason);
void isolate_pending_locked(const char* reason, const char* detail);
void isolate_ext2orig_failure_locked(const virtual_bag::TransactionContext& txn,
                                    virtual_bag::TxnStage phase, const char* reason,
                                    const char* detail);
bool original_inventory_contains_payload_locked(const uint8_t* payload, int payload_size,
                                                int target_bag);
int original_inventory_payload_slot_locked(const uint8_t* payload, int payload_size,
                                           int target_bag);
int original_inventory_handle_slot_locked(qol::contracts::QolItemHandle expected,
                                          int target_bag);
void recover_pending_transaction_locked(std::unique_lock<std::mutex>& lock);
bool extension_tab_hit(int index, int64_t x, int64_t y);
bool try_equip_on_extension_tab_drop_locked(std::unique_lock<std::mutex>& lock,
                                             int index, void* moving_control);
bool extension_source_should_equip_locked(int target_bag);
bool equip_extension_source_on_tab_locked(std::unique_lock<std::mutex>& lock, int target_bag,
                                          int source_bag, int source_slot);
bool route_deferred_extension_to_original_locked(std::unique_lock<std::mutex>& lock);
bool route_projected_session_drop_locked(std::unique_lock<std::mutex>& lock,
                                         int64_t x, int64_t y);
bool handle_bag_drop_release_locked(std::unique_lock<std::mutex>& lock,
                                    int64_t x, int64_t y);
bool inventory_slot_locked(int bag, int slot, void** out);
bool extension_bag_runtime_is_backpack_category(int category);
bool apply_extension_material_to_slot_locked(std::unique_lock<std::mutex>& lock,
                                             int src_bag, int src_slot, int dst_bag,
                                             int dst_slot, void* material, void* target_item);
void* extension_menu_character();
ExtensionBagEquipResult equip_extension_bag_item_locked(std::unique_lock<std::mutex>& lock,
                                                          int internal_bag, void* item);
bool equip_module_item_on_character_locked(std::unique_lock<std::mutex>& lock,
                                            void* character, void* item, int source_bag,
                                            int source_slot);
void finish_extension_equip_ui_locked(int internal_bag, void* moving_control);
int first_empty_extension_bag_locked();
bool extension_confirm_use_item(void* item);
bool extension_use_mercenary_seal();
void make_desc_equip_gate(void* ctrl, void* arg);
bool consume_extension_item_after_native_locked(int bag, int slot, void* item,
                                                int before_count, int observed_before,
                                                const char* context, void* raw_use_token);
uint64_t extension_bag_ui_isolation_now_ms_locked();
bool extension_bag_ui_equip_from_inventory(void* character, int bag, int slot, int equip_slot);
bool extension_bag_ui_add_money(int64_t amount);
bool extension_bag_ui_minus_money(int64_t amount);
void extension_bag_ui_update_character_equip();
void extension_bag_ui_description_set_off();
bool extension_bag_ui_read_item_cumulate_count(const void* item, int* out_count);
bool extension_bag_ui_read_item_sell_price(const void* item, int64_t* out_price);
bool extension_bag_ui_read_item_encoded_field(const void* item, uint32_t* out_field);
qol::contracts::QolItemCountEncoding extension_bag_ui_item_count_encoding(const void* item);
bool extension_bag_ui_read_char_can_equip(const void* character, const void* item);
bool extension_bag_ui_read_char_find_equip_slot(const void* character, const void* item,
                                                int* out_slot);
void* extension_bag_ui_read_char_equipped_item(const void* character, int slot);
bool extension_bag_ui_use_item(void* character, void* item, int arg, int* out_used);
bool extension_bag_ui_make_mercenary(void* item, void** out_mercenary);
bool extension_bag_ui_read_save_is_ok();
bool extension_bag_ui_read_has_empty_mercenary_slot();
void extension_bag_ui_play_switch_sound();
void extension_bag_ui_handle_tab_click_locked(int extension_bag);
JNIEnv* current_env();
uint64_t isolation_now_ms_locked();
bool extension_apply_stuff(void* target_item, void* material);
bool read_item_cumulate_count(const void* item, int* out_count);
bool read_save_is_ok();
bool read_has_empty_mercenary_slot();
void extension_description_set_off();
bool extension_use_item(void* character, void* item, int arg, int* out_used);
bool extension_make_mercenary(void* item, void** out_mercenary);
void extension_refresh_bag_area();
qol::contracts::QolItemCountEncoding item_count_encoding(const void* item);
uint32_t original_count_view(uint32_t field);
qol::contracts::QolItemCountEncoding category_is_equip(int category);
bool category_is_extension_backpack(int category);
void handle_extension_tab_click_locked(int extension_bag);
std::string extension_bag_status_json_locked_bridge();
std::string extension_bag_not_ready_error_locked_bridge();
bool extension_bag_ready_locked_bridge();
int extension_internal_bag_bridge(int logical_bag);
std::string extension_bag_view_result_json_bridge(bool ok, const char* error);
ExtensionBagUnequipResult unequip_extension_bag_locked_bridge(
    std::unique_lock<std::mutex>& lock, int internal_bag);
bool store_module_view_installed_locked();
void store_teardown_locked();
void store_restore_module_view_locked();
uint64_t store_event_wrapper(uint64_t event, uint64_t param, uint64_t param2);
void store_enter_wrapper();
void store_f3_wrapper();
int64_t store_sell_item_wrapper(int64_t displayed_price);
void store_make_desc_gate(void* ctrl, void* arg);
void store_draw_end_wrapper();
void store_draw_inven_bag_group_wrapper();
uint8_t store_buy_find_slot_gate(void* item, void* out_slot);
void mix_teardown_locked();
void mix_enter_wrapper();
void mix_f3_wrapper();
void mix_draw_end_wrapper();
void mix_draw_inven_bag_group_wrapper();
bool remove_item_direct_unlocked(std::unique_lock<std::mutex>& lock, int bag, int slot,
                                 void* expected_item, const char* context);
