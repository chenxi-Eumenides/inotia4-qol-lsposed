#pragma once

#include <cstdint>
#include <type_traits>

#include "../result.h"
#include "../version.h"
#include "../item_handle.h"
#include "ports.h"

namespace qol::contracts {

struct QolCraftReadApi {
    QolResult (*ui_type)(uint32_t* out_type);
    QolResult (*panel_state)(int32_t* out_state);
    QolResult (*mix_type)(uint32_t* out_type);
    QolResult (*selected_stuff_index)(int64_t* out_index);
    QolResult (*selected_stuff_item)(QolItemHandle* out_item);
    QolResult (*stuff_item)(uint32_t index, QolItemHandle* out_item);
    QolResult (*category_held_count)(int32_t category, int32_t* out_count);
    QolResult (*character_level)(int32_t* out_level);
};

struct QolCraftReadOnlyApi {
    QolResult (*ui_type)(uint32_t* out_type);
    QolResult (*get_ui_type)(uint32_t* out_type);
    QolResult (*panel_state)(int32_t* out_state);
    QolResult (*mix_type)(uint32_t* out_type);
    QolResult (*selected_stuff_index)(int64_t* out_index);
    QolResult (*selected_stuff_item)(QolItemHandle* out_item);
    QolResult (*stuff_item)(uint32_t index, QolItemHandle* out_item);
    QolResult (*category_held_count)(int32_t category, int32_t* out_count);
    QolResult (*character_level)(int32_t* out_level);
    QolResult (*stuff_slot_available)(uint32_t index, int32_t* out_available);
    QolResult (*target_slot_item)(QolItemHandle* out_item);
    QolResult (*selected_inventory_item)(QolItemHandle* out_item);
    QolResult (*selected_recipe)(int32_t type, int64_t* out_recipe);
    QolResult (*stuff_slot_count)(uint32_t* out_count);
};

struct QolCraftPort {
    QolVersion abi_version;
    QolCraftReadApi read;
    QolCraftReadOnlyApi read_only;
};

static_assert(std::is_standard_layout<QolCraftPort>::value,
              "QolCraftPort must remain standard layout");
static_assert(std::is_trivially_copyable<QolCraftPort>::value,
              "QolCraftPort must remain trivially copyable");

}  // namespace qol::contracts
