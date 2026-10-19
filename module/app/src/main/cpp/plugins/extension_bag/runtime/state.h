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
using MixSceneEventFn = uint64_t (*)(uint64_t, uint64_t, uint64_t);

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
    // UIStore hook backups and PopupState wrapper originals stay shared across TUs.
    void* backup_store_sell_item = nullptr;
    void* backup_store_button_sell_exe = nullptr;
    void* backup_store_draw_inven_bag_group = nullptr;
    StoreSceneEventFn store_original_event = nullptr;
    StoreSceneNoArgFn store_original_f3 = nullptr;
    StoreSceneNoArgFn store_original_enter = nullptr;
    uint8_t* store_state_entry = nullptr;
    MixSceneNoArgFn mix_original_enter = nullptr;
    MixSceneNoArgFn mix_original_f3 = nullptr;
    MixSceneEventFn mix_original_event = nullptr;
    uint8_t* mix_state_entry = nullptr;
};

struct ExtensionBagStoreState {
    bool view_installed = false;
    int view_bag = -1;
    int window_original_bag = -1;
    uint32_t original_size_word = 0;
    uint32_t original_bag_size = 0;
    void* projected_item_root = nullptr;
    void* hidden_equip_bag_root = nullptr;
    void* hidden_original_bag_control = nullptr;
    int original_current_bag = -1;
    std::array<void*, virtual_bag::kBagCount> tab_buttons{};
    std::array<void*, virtual_bag::kBagCount> tab_bag_items{};
    void* tab_root = nullptr;
    uint64_t tab_generation = 0;
    int pending_tab = -1;
    uint64_t pending_tab_generation = 0;
    bool frame_active = false;
    uint64_t view_generation = 0;
    uint64_t refresh_generation = 0;
    bool refresh_pending = false;
};

struct ExtensionBagRuntimeState {
    std::mutex mtx;
    ExtensionBagHostPorts host{};
    ExtensionBagPersistenceState persistence{};
    ExtensionBagObjectState objects{};
    ExtensionBagUiState ui{};
    ExtensionBagStoreState store{};
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

extern thread_local ModuleUseToken* g_active_module_use_token;
extern thread_local bool g_p5_observation_active;
extern thread_local bool g_refresh_restore_suppress;
extern thread_local bool g_in_native_call;
