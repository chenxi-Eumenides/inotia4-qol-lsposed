#pragma once

#include "contracts/craft_facts.h"
#include "contracts/game_host.h"
#include "contracts/game_ports/character_extras_port.h"
#include "contracts/game_ports/extension_bag_write_port.h"
#include "contracts/game_ports/inventory_extras_port.h"
#include "contracts/host_sdk.h"
#include "contracts/item_facts.h"
#include "contracts/ownership.h"
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
using StoreSceneEventFn = uint64_t (*)(uint64_t, uint64_t, uint64_t);
using StoreSceneNoArgFn = void (*)();
using MixSceneNoArgFn = void (*)();

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

struct ExtensionBagHostPorts {
    const qol::contracts::QolHostSdk* sdk = nullptr;
    const qol::contracts::QolGameHost* game = nullptr;
    const qol::contracts::QolExtensionBagWritePort* write_port = nullptr;
    const qol::contracts::QolCharacterExtrasPort* character_extras = nullptr;
    const qol::contracts::QolInventoryExtrasPort* inventory_extras = nullptr;
    const qol::contracts::ItemFactsApi* item_facts = nullptr;
    const qol::contracts::CraftFactsApi* craft_facts = nullptr;
};

struct ExtensionBagPersistenceState {
    bool dirty = false;
    std::atomic<uint64_t> revision{0};
    int loaded_slot = -2;
    uint8_t* state_entry = nullptr;
    uint8_t* save_state_entry = nullptr;
    bool save_panel_hook_installed = false;
    void* backup_save_inventory = nullptr;
    PopupEventFn orig_event = nullptr;
    PopupNoArgFn orig_f3 = nullptr;
    PopupNoArgFn orig_enter = nullptr;
    uintptr_t inventory_enter = 0;
    PopupNoArgFn orig_save_enter = nullptr;
    jclass bridge_class = nullptr;
};

struct ExtensionBagObjectState {
    virtual_bag::State bag{};
    std::array<std::array<void*, virtual_bag::kSlotCount>, virtual_bag::kBagCount>
        module_objects{};
    std::array<std::array<ModuleUseState, virtual_bag::kSlotCount>, virtual_bag::kBagCount>
        module_object_use{};
    std::array<std::array<int, virtual_bag::kSlotCount>, virtual_bag::kBagCount>
        module_object_categories{};
    std::array<std::array<uint32_t, virtual_bag::kSlotCount>, virtual_bag::kBagCount>
        module_object_hashes{};
    uint32_t module_object_handles[virtual_bag::kBagCount][virtual_bag::kSlotCount]{};
    std::array<void*, virtual_bag::kSlotCount> original_inventory{};
    uint8_t original_current_direct = 0;
    uint8_t original_current_got = 0;
    int original_bag_size_bag = -1;
    uint32_t original_bag_size = 0;
    int module_window_original_bag = -1;
    void* projected_item_root = nullptr;
    bool module_view_installed = false;
    int module_view_index = -1;
    ownership::Ledger ownership_ledger{};
};

struct ExtensionBagUiState {
    void* pending_jewel_detail_item = nullptr;
    int pending_jewel_detail_bag = -1;
    int pending_jewel_detail_slot = -1;
    uint8_t exit_display_bag = kNoOriginalBagSelected;
    bool inventory_frame_active = false;
    bool extension_touch_capture = false;
    std::array<void*, virtual_bag::kBagCount> extension_tab_buttons{};
    void* extension_tab_root = nullptr;
    uint64_t inventory_generation = 0;
    uint64_t extension_tab_generation = 0;
    int pending_extension_tab = -1;
    uint64_t pending_extension_tab_generation = 0;
    int extension_desc_unequip_bag = -1;
    std::array<uint64_t, 11> extension_desc_item_ids{};
    std::array<bool, 11> extension_desc_item_bound{};
    void* extension_desc_item = nullptr;
    int extension_desc_item_bag = -1;
    int extension_desc_item_slot = -1;
    ExtensionDestroyOkFn extension_destroy_original_ok = nullptr;
    ExtensionDestroyOkFn extension_sell_original_ok = nullptr;
    ExtensionDestroyOkFn extension_destroy_original_cancel = nullptr;
    ExtensionDestroyOkFn extension_sell_original_cancel = nullptr;
    bool extension_sell_apply_variant_discount = true;
    ExtensionDrag extension_drag{};
    virtual_bag::ExtensionDragSession extension_drag_session{};
    void* tab_bag_items[virtual_bag::kBagCount]{};
    // UIStore_SellItem hook backup and PopupState wrapper originals stay shared across TUs.
    void* backup_store_sell_item = nullptr;
    StoreSceneEventFn store_original_event = nullptr;
    StoreSceneNoArgFn store_original_f3 = nullptr;
    StoreSceneNoArgFn store_original_enter = nullptr;
    uint8_t* store_state_entry = nullptr;
    MixSceneNoArgFn mix_original_enter = nullptr;
    MixSceneNoArgFn mix_original_f3 = nullptr;
    uint8_t* mix_state_entry = nullptr;
};

struct ExtensionBagRuntimeState {
    std::mutex mtx;
    ExtensionBagHostPorts host{};
    ExtensionBagPersistenceState persistence{};
    ExtensionBagObjectState objects{};
    ExtensionBagUiState ui{};
    std::atomic<bool> source_protection_requested{false};
    uint64_t lifecycle_task = 0;
    int lifecycle_last_state = -1;
    std::atomic<bool> main_menu_cleanup_dirty{false};
    std::atomic<bool> enabled{false};
    std::atomic<uint64_t> exit_trace_sequence{0};
    std::atomic<uint64_t> p5_observation_sequence{0};
    std::atomic<uint64_t> p5_last_item_query_sample_ms{0};
    std::atomic<uint64_t> p5_last_panel_move_sample_ms{0};
};

inline ExtensionBagRuntimeState& runtime_state() {
    static ExtensionBagRuntimeState state{};
    return state;
}

// M2a 状态收口临时别名，后续按模块改为 runtime_state() 显式访问。
inline std::mutex& g_virtual_bag_mtx = runtime_state().mtx;
inline bool& g_item_state_dirty = runtime_state().persistence.dirty;
inline std::atomic<uint64_t>& g_item_state_revision = runtime_state().persistence.revision;
inline std::atomic<bool>& g_extension_source_protection_requested =
    runtime_state().source_protection_requested;
inline uint64_t& g_lifecycle_task = runtime_state().lifecycle_task;
inline const qol::contracts::QolHostSdk*& g_sdk = runtime_state().host.sdk;
inline const qol::contracts::QolGameHost*& g_game = runtime_state().host.game;
inline const qol::contracts::QolExtensionBagWritePort*& g_write_port =
    runtime_state().host.write_port;
inline const qol::contracts::QolCharacterExtrasPort*& g_character_extras =
    runtime_state().host.character_extras;
inline const qol::contracts::QolInventoryExtrasPort*& g_inventory_extras =
    runtime_state().host.inventory_extras;
inline const qol::contracts::ItemFactsApi*& g_item_facts = runtime_state().host.item_facts;
inline const qol::contracts::CraftFactsApi*& g_craft_facts = runtime_state().host.craft_facts;
inline int& g_lifecycle_last_state = runtime_state().lifecycle_last_state;
inline std::atomic<bool>& g_main_menu_cleanup_dirty = runtime_state().main_menu_cleanup_dirty;
inline std::atomic<bool>& g_virtual_bag_enabled = runtime_state().enabled;
inline std::atomic<uint64_t>& g_exit_trace_sequence = runtime_state().exit_trace_sequence;
inline jclass& g_virtual_bag_bridge_class = runtime_state().persistence.bridge_class;
inline uint8_t*& g_state_entry = runtime_state().persistence.state_entry;
inline PopupEventFn& g_orig_event = runtime_state().persistence.orig_event;
inline PopupNoArgFn& g_orig_f3 = runtime_state().persistence.orig_f3;
inline PopupNoArgFn& g_orig_enter = runtime_state().persistence.orig_enter;
inline uintptr_t& g_inventory_enter = runtime_state().persistence.inventory_enter;
inline PopupNoArgFn& g_orig_save_enter = runtime_state().persistence.orig_save_enter;
inline uint8_t*& g_save_state_entry = runtime_state().persistence.save_state_entry;
inline bool& g_save_panel_hook_installed = runtime_state().persistence.save_panel_hook_installed;
inline void*& g_backup_save_inventory = runtime_state().persistence.backup_save_inventory;
inline virtual_bag::State& g_virtual_bag_state = runtime_state().objects.bag;
inline int& g_loaded_slot = runtime_state().persistence.loaded_slot;
inline auto& g_module_objects = runtime_state().objects.module_objects;
inline auto& g_module_object_use = runtime_state().objects.module_object_use;
extern thread_local ModuleUseToken* g_active_module_use_token;
inline auto& g_module_object_categories = runtime_state().objects.module_object_categories;
inline auto& g_module_object_hashes = runtime_state().objects.module_object_hashes;
inline auto& g_module_object_handles = runtime_state().objects.module_object_handles;
inline auto& g_original_inventory = runtime_state().objects.original_inventory;
inline uint8_t& g_original_current_direct = runtime_state().objects.original_current_direct;
inline uint8_t& g_original_current_got = runtime_state().objects.original_current_got;
inline int& g_original_bag_size_bag = runtime_state().objects.original_bag_size_bag;
inline uint32_t& g_original_bag_size = runtime_state().objects.original_bag_size;
inline int& g_module_window_original_bag = runtime_state().objects.module_window_original_bag;
inline void*& g_projected_item_root = runtime_state().objects.projected_item_root;
inline bool& g_module_view_installed = runtime_state().objects.module_view_installed;
inline int& g_module_view_index = runtime_state().objects.module_view_index;
inline void*& g_pending_jewel_detail_item = runtime_state().ui.pending_jewel_detail_item;
inline int& g_pending_jewel_detail_bag = runtime_state().ui.pending_jewel_detail_bag;
inline int& g_pending_jewel_detail_slot = runtime_state().ui.pending_jewel_detail_slot;
inline uint8_t& g_exit_display_bag = runtime_state().ui.exit_display_bag;
inline bool& g_inventory_frame_active = runtime_state().ui.inventory_frame_active;
inline bool& g_extension_touch_capture = runtime_state().ui.extension_touch_capture;
inline auto& g_extension_tab_buttons = runtime_state().ui.extension_tab_buttons;
inline void*& g_extension_tab_root = runtime_state().ui.extension_tab_root;
inline uint64_t& g_inventory_generation = runtime_state().ui.inventory_generation;
inline uint64_t& g_extension_tab_generation = runtime_state().ui.extension_tab_generation;
inline int& g_pending_extension_tab = runtime_state().ui.pending_extension_tab;
inline uint64_t& g_pending_extension_tab_generation =
    runtime_state().ui.pending_extension_tab_generation;
inline int& g_extension_desc_unequip_bag = runtime_state().ui.extension_desc_unequip_bag;
inline auto& g_extension_desc_item_ids = runtime_state().ui.extension_desc_item_ids;
inline auto& g_extension_desc_item_bound = runtime_state().ui.extension_desc_item_bound;
inline void*& g_extension_desc_item = runtime_state().ui.extension_desc_item;
inline int& g_extension_desc_item_bag = runtime_state().ui.extension_desc_item_bag;
inline int& g_extension_desc_item_slot = runtime_state().ui.extension_desc_item_slot;
inline ExtensionDestroyOkFn& g_extension_destroy_original_ok =
    runtime_state().ui.extension_destroy_original_ok;
inline ExtensionDestroyOkFn& g_extension_sell_original_ok =
    runtime_state().ui.extension_sell_original_ok;
inline ExtensionDestroyOkFn& g_extension_destroy_original_cancel =
    runtime_state().ui.extension_destroy_original_cancel;
inline ExtensionDestroyOkFn& g_extension_sell_original_cancel =
    runtime_state().ui.extension_sell_original_cancel;
inline bool& g_extension_sell_apply_variant_discount =
    runtime_state().ui.extension_sell_apply_variant_discount;
inline ExtensionDrag& g_extension_drag = runtime_state().ui.extension_drag;
inline virtual_bag::ExtensionDragSession& g_extension_drag_session =
    runtime_state().ui.extension_drag_session;
inline std::atomic<uint64_t>& g_p5_observation_sequence = runtime_state().p5_observation_sequence;
inline std::atomic<uint64_t>& g_p5_last_item_query_sample_ms =
    runtime_state().p5_last_item_query_sample_ms;
inline std::atomic<uint64_t>& g_p5_last_panel_move_sample_ms =
    runtime_state().p5_last_panel_move_sample_ms;
extern thread_local bool g_p5_observation_active;
extern thread_local bool g_refresh_restore_suppress;
extern thread_local bool g_in_native_call;
inline auto& g_tab_bag_items = runtime_state().ui.tab_bag_items;
inline ownership::Ledger& g_ownership_ledger = runtime_state().objects.ownership_ledger;
inline void*& g_backup_store_sell_item = runtime_state().ui.backup_store_sell_item;
inline StoreSceneEventFn& store_original_event = runtime_state().ui.store_original_event;
inline StoreSceneNoArgFn& store_original_f3 = runtime_state().ui.store_original_f3;
inline StoreSceneNoArgFn& store_original_enter = runtime_state().ui.store_original_enter;
inline uint8_t*& store_state_entry = runtime_state().ui.store_state_entry;
inline MixSceneNoArgFn& mix_original_enter = runtime_state().ui.mix_original_enter;
inline MixSceneNoArgFn& mix_original_f3 = runtime_state().ui.mix_original_f3;
inline uint8_t*& mix_state_entry = runtime_state().ui.mix_state_entry;
