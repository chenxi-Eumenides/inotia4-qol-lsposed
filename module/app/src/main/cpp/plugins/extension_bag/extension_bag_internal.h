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

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <thread>

#include <jni.h>

using PopupEventFn = uint64_t (*)(uint64_t, uint64_t, uint64_t);
using PopupNoArgFn = void (*)();
using OriginalDrawInvenBagFn = void (*)();
using OriginalDrawInvenItemFn = void (*)();
using ExtensionDestroyOkFn = void (*)(void*);

struct ModuleUseState {
    bool active = false;
    bool consumed = false;
    bool pending_release = false;
    void* item = nullptr;
    uint64_t generation = 0;
    std::thread::id owner{};
};

struct ModuleUseToken {
    int bag = -1;
    int slot = -1;
    void* item = nullptr;
    uint64_t generation = 0;
    std::thread::id owner{};
};

struct ModuleUseReleasePlan {
    bool pending = false;
    int bag = -1;
    int slot = -1;
    void* item = nullptr;
    void* control = nullptr;
    uint32_t handle = 0;
    uint64_t generation = 0;
};

struct ExtensionDrag {
    bool active = false;
    uint8_t bag = 0;
    uint8_t slot = 0;
    int64_t press_x = 0;
    int64_t press_y = 0;
    int64_t current_x = 0;
    int64_t current_y = 0;
};

struct PhysicalInventorySnapshot {
    std::array<void*, virtual_bag::kPhysicalInventorySnapshotSlots> slots{};
    uint64_t digest = 0;
    int nonnull = 0;
    bool valid = false;
};

constexpr uint8_t kNoOriginalBagSelected = 6;
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

extern std::mutex g_virtual_bag_mtx;
extern bool g_item_state_dirty;
extern std::atomic<uint64_t> g_item_state_revision;
extern std::atomic<bool> g_extension_source_protection_requested;
extern uint64_t g_lifecycle_task;
extern const qol::contracts::QolHostSdk* g_sdk;
extern const qol::contracts::QolGameHost* g_game;
extern const qol::contracts::QolExtensionBagWritePort* g_write_port;
extern const qol::contracts::QolCharacterExtrasPort* g_character_extras;
extern const qol::contracts::QolInventoryExtrasPort* g_inventory_extras;
extern const qol::contracts::ItemFactsApi* g_item_facts;
extern const qol::contracts::CraftFactsApi* g_craft_facts;
extern int g_lifecycle_last_state;
extern std::atomic<bool> g_main_menu_cleanup_dirty;
extern std::atomic<bool> g_virtual_bag_enabled;
extern std::atomic<uint64_t> g_exit_trace_sequence;
extern jclass g_virtual_bag_bridge_class;
extern uint8_t* g_state_entry;
extern PopupEventFn g_orig_event;
extern PopupNoArgFn g_orig_f3;
extern PopupNoArgFn g_orig_enter;
extern uintptr_t g_inventory_enter;
extern PopupNoArgFn g_orig_save_enter;
extern uint8_t* g_save_state_entry;
extern bool g_save_panel_hook_installed;
extern void* g_backup_save_inventory;
extern virtual_bag::State g_virtual_bag_state;
extern int g_loaded_slot;
extern std::array<std::array<void*, virtual_bag::kSlotCount>, virtual_bag::kBagCount>
    g_module_objects;
extern std::array<std::array<ModuleUseState, virtual_bag::kSlotCount>, virtual_bag::kBagCount>
    g_module_object_use;
extern thread_local ModuleUseToken* g_active_module_use_token;
extern std::array<std::array<int, virtual_bag::kSlotCount>, virtual_bag::kBagCount>
    g_module_object_categories;
extern std::array<std::array<uint32_t, virtual_bag::kSlotCount>, virtual_bag::kBagCount>
    g_module_object_hashes;
extern uint32_t g_module_object_handles[virtual_bag::kBagCount][virtual_bag::kSlotCount];
extern std::array<void*, virtual_bag::kSlotCount> g_original_inventory;
extern uint8_t g_original_current_direct;
extern uint8_t g_original_current_got;
extern int g_original_bag_size_bag;
extern uint32_t g_original_bag_size;
extern int g_module_window_original_bag;
extern void* g_projected_item_root;
extern bool g_module_view_installed;
extern int g_module_view_index;
extern void* g_pending_jewel_detail_item;
extern int g_pending_jewel_detail_bag;
extern int g_pending_jewel_detail_slot;
extern uint8_t g_exit_display_bag;
extern bool g_inventory_frame_active;
extern bool g_extension_touch_capture;
extern std::array<void*, virtual_bag::kBagCount> g_extension_tab_buttons;
extern void* g_extension_tab_root;
extern uint64_t g_inventory_generation;
extern uint64_t g_extension_tab_generation;
extern int g_pending_extension_tab;
extern uint64_t g_pending_extension_tab_generation;
extern int g_extension_desc_unequip_bag;
extern std::array<uint64_t, 11> g_extension_desc_item_ids;
extern std::array<bool, 11> g_extension_desc_item_bound;
extern void* g_extension_desc_item;
extern int g_extension_desc_item_bag;
extern int g_extension_desc_item_slot;
extern ExtensionDestroyOkFn g_extension_destroy_original_ok;
extern ExtensionDestroyOkFn g_extension_sell_original_ok;
extern ExtensionDestroyOkFn g_extension_destroy_original_cancel;
extern ExtensionDestroyOkFn g_extension_sell_original_cancel;
extern bool g_extension_sell_apply_variant_discount;
extern ExtensionDrag g_extension_drag;
extern virtual_bag::ExtensionDragSession g_extension_drag_session;
extern std::atomic<uint64_t> g_p5_observation_sequence;
extern std::atomic<uint64_t> g_p5_last_item_query_sample_ms;
extern std::atomic<uint64_t> g_p5_last_panel_move_sample_ms;
extern thread_local bool g_p5_observation_active;
extern thread_local bool g_refresh_restore_suppress;
extern thread_local bool g_in_native_call;
extern void* g_tab_bag_items[virtual_bag::kBagCount];
extern ownership::Ledger& g_ownership_ledger;

void extension_bag_mark_dirty_locked();
uint64_t extension_bag_item_revision_locked();
const qol::contracts::QolExtensionBagWritePort* extension_bag_write_api();
bool read_item_category(const void* item, int* out_category);
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
bool route_deferred_extension_to_original_locked(std::unique_lock<std::mutex>& lock);
bool route_projected_session_drop_locked(std::unique_lock<std::mutex>& lock,
                                         int64_t x, int64_t y);
bool handle_bag_drop_release_locked(std::unique_lock<std::mutex>& lock,
                                    int64_t x, int64_t y);
bool virtual_bag_sync_projected_bag();
void delete_extension_tab_buttons_locked();
void disable_extension_tab_buttons_locked();
void install_extension_tab_buttons_locked();
bool extension_tab_buttons_complete_locked();
bool free_module_object_locked(int bag, int slot, void* use_token = nullptr);
void* materialize_module_item_locked(const virtual_bag::Item& descriptor);
void release_temporary_item_locked(void* item);
bool module_object_generation_advance_locked(int bag, int slot, const char* context);
bool inventory_slot_locked(int bag, int slot, void** out);
void log_ownership_audit_locked(const char* context);
void drain_deferred_frees_locked();
bool item_pointer_known_locked(void* item);
