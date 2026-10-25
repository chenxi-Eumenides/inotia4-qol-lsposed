#pragma once

#include "contracts/ui.h"
#include "plugins/extension_bag/model/ownership_ledger.h"
#include "plugins/extension_bag/runtime/ownership_port.h"
#include "plugins/extension_bag/runtime/state.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <thread>

using PopupEventFn = uint64_t (*)(uint64_t, uint64_t, uint64_t);
using PopupNoArgFn = void (*)();
using OriginalDrawInvenBagFn = void (*)();
using OriginalDrawInvenItemFn = void (*)();
using ExtensionDestroyOkFn = void (*)(void*);
using StoreSceneEventFn = uint64_t (*)(uint64_t, uint64_t, uint64_t);
using StoreSceneNoArgFn = void (*)();
using MixSceneNoArgFn = void (*)();
using MixSceneEventFn = uint64_t (*)(uint64_t, uint64_t, uint64_t);

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

struct ExtensionBagObjectUiState {
    std::array<std::array<void*, virtual_bag::kSlotCount>, virtual_bag::kMaxExtensionBags>
        module_objects{};
    std::array<std::array<ModuleUseState, virtual_bag::kSlotCount>, virtual_bag::kMaxExtensionBags>
        module_object_use{};
    std::array<std::array<int, virtual_bag::kSlotCount>, virtual_bag::kMaxExtensionBags>
        module_object_categories{};
    std::array<std::array<uint32_t, virtual_bag::kSlotCount>, virtual_bag::kMaxExtensionBags>
        module_object_hashes{};
    uint32_t module_object_handles[virtual_bag::kMaxExtensionBags][virtual_bag::kSlotCount]{};
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
    int exit_display_bag = -1;
    bool inventory_frame_active = false;
    bool extension_touch_capture = false;
    std::array<void*, virtual_bag::kMaxExtensionBags> extension_tab_buttons{};
    std::array<qol::contracts::QolUiAnchoredControl, virtual_bag::kMaxExtensionBags> extension_tab_anchors{};
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
    void* tab_bag_items[virtual_bag::kMaxExtensionBags]{};
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
    std::array<void*, virtual_bag::kMaxExtensionBags> tab_buttons{};
    std::array<qol::contracts::QolUiAnchoredControl, virtual_bag::kMaxExtensionBags> tab_anchors{};
    std::array<void*, virtual_bag::kMaxExtensionBags> tab_bag_items{};
    void* tab_root = nullptr;
    uint64_t tab_generation = 0;
    int pending_tab = -1;
    uint64_t pending_tab_generation = 0;
    bool frame_active = false;
    uint64_t view_generation = 0;
    uint64_t refresh_generation = 0;
    bool refresh_pending = false;
};

struct ExtensionBagMixState {
    bool view_installed = false;
    int view_bag = -1;
    int window_original_bag = -1;
    uint32_t original_size_word = 0;
    uint32_t original_bag_size = 0;
    void* projected_item_root = nullptr;
    std::array<void*, virtual_bag::kMaxExtensionBags> tab_buttons{};
    std::array<qol::contracts::QolUiAnchoredControl, virtual_bag::kMaxExtensionBags> tab_anchors{};
    std::array<void*, virtual_bag::kMaxExtensionBags> tab_bag_items{};
    void* tab_root = nullptr;
    uint64_t tab_generation = 0;
    int pending_tab = -1;
    uint64_t pending_tab_generation = 0;
    bool frame_active = false;
    uint64_t view_generation = 0;
    uint64_t refresh_generation = 0;
    bool refresh_pending = false;
};

struct ExtensionBagPersistenceUiState {
    bool save_panel_hook_installed = false;
    void* backup_save_inventory = nullptr;
    PopupEventFn orig_event = nullptr;
    PopupNoArgFn orig_f3 = nullptr;
    PopupNoArgFn orig_enter = nullptr;
    uintptr_t inventory_enter = 0;
    PopupNoArgFn orig_save_enter = nullptr;
};

struct ExtensionBagRuntimeAuxState {
    std::atomic<bool> source_protection_requested{false};
    uint64_t lifecycle_task = 0;
    int lifecycle_last_state = -1;
    std::atomic<bool> main_menu_cleanup_dirty{false};
    std::atomic<uint64_t> exit_trace_sequence{0};
    std::atomic<uint64_t> p5_observation_sequence{0};
    std::atomic<uint64_t> p5_last_item_query_sample_ms{0};
    std::atomic<uint64_t> p5_last_panel_move_sample_ms{0};
};

struct UiState {
    ExtensionBagObjectUiState objects{};
    ExtensionBagUiState ui{};
    ExtensionBagStoreState store{};
    ExtensionBagMixState mix{};
    ExtensionBagPersistenceUiState persistence_hooks{};
    ExtensionBagRuntimeAuxState aux{};
};

namespace qol::extension_bag {

UiState& ui_state();

}  // namespace qol::extension_bag
