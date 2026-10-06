#pragma once

#include <cstdint>
#include <type_traits>

#include "item_handle.h"
#include "result.h"

namespace qol::contracts {

constexpr const char* kInventoryMergeProjectionCapabilityId = "inventory.merge_projection";
constexpr uint16_t kInventoryMergeProjectionVersionMajor = 1;
constexpr uint16_t kInventoryMergeProjectionVersionMinor = 0;
constexpr const char* kMoveMergeControlCapabilityId = "move_merge.control";
constexpr uint16_t kMoveMergeControlVersionMajor = 1;
constexpr uint16_t kMoveMergeControlVersionMinor = 0;
constexpr const char* kMoveMergePluginId = "move_merge";
constexpr uint16_t kMoveMergePluginVersionMajor = 1;
constexpr uint16_t kMoveMergePluginVersionMinor = 0;

enum class MoveMergeProjectionDropResult : uint8_t {
    kNotHandled = 0,
    kHandled = 1,
    kRejected = 2,
};

// Projection callbacks consume opaque UI pointers only during their current callback invocation.
// A consumer must reacquire this capability per callback and must not retain the table or pointers.
struct InventoryMergeProjectionApi {
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
};

struct MoveMergeControlApi {
    uint32_t struct_size;
    uint16_t version_major;
    uint16_t version_minor;
    bool (*enabled)();
};

static_assert(std::is_standard_layout<InventoryMergeProjectionApi>::value,
              "merge projection API must be standard layout");
static_assert(std::is_trivially_copyable<InventoryMergeProjectionApi>::value,
              "merge projection API must be trivially copyable");
static_assert(std::is_standard_layout<MoveMergeControlApi>::value,
              "move merge control API must be standard layout");
static_assert(std::is_trivially_copyable<MoveMergeControlApi>::value,
              "move merge control API must be trivially copyable");

inline bool inventory_merge_projection_api_valid(const InventoryMergeProjectionApi* api) {
    return api != nullptr && api->struct_size >= sizeof(InventoryMergeProjectionApi) &&
           api->version_major == kInventoryMergeProjectionVersionMajor &&
           api->version_minor >= kInventoryMergeProjectionVersionMinor;
}

inline bool move_merge_control_api_valid(const MoveMergeControlApi* api) {
    return api != nullptr && api->struct_size >= sizeof(MoveMergeControlApi) &&
           api->version_major == kMoveMergeControlVersionMajor &&
           api->version_minor >= kMoveMergeControlVersionMinor;
}

}  // namespace qol::contracts
