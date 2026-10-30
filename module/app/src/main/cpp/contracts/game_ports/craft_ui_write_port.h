#pragma once

#include <cstdint>
#include <type_traits>

#include "../result.h"
#include "../version.h"
#include "../item_handle.h"

namespace qol::contracts {

using QolCraftPopupCallback = void (*)();

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
    QolResult (*show_yesno_from_text)(uint32_t text_id, QolCraftPopupCallback ok,
                                      QolCraftPopupCallback cancel);
    QolResult (*set_target_slot_item)(QolItemHandle item);
    QolResult (*set_cost_zero)();
    // v1.2：三同类宝石三合一委托原版 UIMix_StartMix（内部先按原版口径重算费用再调原版函数）。
    // 未进入原版 StartMix（符号缺失/配方非法）返回非 Ok，此时材料未扣。
    QolResult (*delegate_start_mix)(uint32_t mix_type);
};

static_assert(std::is_standard_layout<QolCraftUiWritePort>::value,
              "QolCraftUiWritePort must remain standard layout");
static_assert(std::is_trivially_copyable<QolCraftUiWritePort>::value,
              "QolCraftUiWritePort must remain trivially copyable");

}  // namespace qol::contracts
