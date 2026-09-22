#pragma once

#include <cstdint>
#include <type_traits>

#include "../result.h"
#include "../version.h"

namespace qol::contracts {

struct QolCraftUiWritePort {
    QolVersion abi_version;
    QolResult (*init_mixing_state)();
    QolResult (*reset_stuff_item_control)();
    QolResult (*refresh_inventory_items)();
    QolResult (*select_first_empty_stuff_slot)(int32_t* out_index);
    QolResult (*set_ui_type)(int64_t type);
    QolResult (*set_selected_recipe)(int32_t type, int64_t recipe);
    QolResult (*play_sound)(int32_t sound_id);
    QolResult (*show_text)(uint32_t text_id);
    QolResult (*show_yesno_from_text)(uint32_t text_id, void* ok, void* cancel);
    QolResult (*set_target_slot_item)(void* item);
    QolResult (*set_cost_zero)();
};

static_assert(std::is_standard_layout<QolCraftUiWritePort>::value,
              "QolCraftUiWritePort must remain standard layout");
static_assert(std::is_trivially_copyable<QolCraftUiWritePort>::value,
              "QolCraftUiWritePort must remain trivially copyable");

}  // namespace qol::contracts
