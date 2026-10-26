#pragma once

#include <cstdint>
#include <type_traits>

#include "item_handle.h"
#include "result.h"

namespace qol::contracts {

constexpr const char* kInventoryProjectionCapabilityId = "inventory.projection";
constexpr uint16_t kInventoryProjectionVersionMajor = 1;
constexpr uint16_t kInventoryProjectionVersionMinor = 2;

enum class MoveMergeProjectionDropResult : uint8_t {
    kNotHandled = 0,
    kHandled = 1,
    kRejected = 2,
};

// Projection callbacks consume opaque UI pointers only during their current callback invocation.
// The registrant owns the table and every function pointer; they remain valid until unregister.
struct InventoryProjectionCallbacks {
    uint32_t struct_size;
    uint16_t version_major;
    uint16_t version_minor;
    MoveMergeProjectionDropResult (*projection_drop)(void* destination_control,
                                                      void* source_control);
    bool (*original_item_input_blocked)();
    bool (*allow_original_tab_drop)(void* control, void* source_control,
                                    uint64_t caller_offset);
    QolResult (*identify_extension_item)(QolItemHandle item, int32_t* out_bag,
                                         int32_t* out_slot);
    uint64_t (*observe_item_proc_pre)(void* control, uint64_t event, void* x2, void* param);
    void (*observe_item_proc_source)(uint64_t observation_token, void* control, uint64_t event,
                                     void* x2, void* param, void* source_control);
    void (*observe_item_proc_post)(uint64_t observation_token, void* control, uint64_t event,
                                   void* x2, void* param, uint64_t result);
    bool (*module_view_installed)();
    bool (*sync_projected_slot)(int32_t display_bag, int32_t slot);
    bool (*sync_projected_bag)();
    bool (*handle_original_bag_unequip)();
    bool (*handle_use_mercenary_seal)();
    bool (*handle_backpack_equip)();
};

struct InventoryProjectionRegistryApi {
    uint32_t struct_size;
    QolResult (*register_callbacks)(const InventoryProjectionCallbacks* callbacks);
    QolResult (*unregister_callbacks)();
};

static_assert(std::is_standard_layout<InventoryProjectionCallbacks>::value,
              "inventory projection callbacks must be standard layout");
static_assert(std::is_trivially_copyable<InventoryProjectionCallbacks>::value,
              "inventory projection callbacks must be trivially copyable");
static_assert(std::is_standard_layout<InventoryProjectionRegistryApi>::value,
              "inventory projection registry API must be standard layout");
static_assert(std::is_trivially_copyable<InventoryProjectionRegistryApi>::value,
              "inventory projection registry API must be trivially copyable");

inline bool inventory_projection_callbacks_valid(const InventoryProjectionCallbacks* api) {
    return api != nullptr && api->struct_size >= sizeof(InventoryProjectionCallbacks) &&
           api->version_major == kInventoryProjectionVersionMajor &&
           api->version_minor >= kInventoryProjectionVersionMinor;
}

inline bool inventory_projection_registry_api_valid(const InventoryProjectionRegistryApi* api) {
    return api != nullptr && api->struct_size >= sizeof(InventoryProjectionRegistryApi) &&
           api->register_callbacks != nullptr && api->unregister_callbacks != nullptr;
}

}  // namespace qol::contracts
