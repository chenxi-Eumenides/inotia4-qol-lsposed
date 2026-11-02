#pragma once

#include "contracts/lifecycle_participant.h"

namespace qol::game {

bool lifecycle_participant_bind_host(const contracts::QolLifecycleDispatch* dispatch);
bool lifecycle_participant_prepare_save_slot_load();
bool lifecycle_participant_is_inventory_enter(uintptr_t enter);
bool lifecycle_participant_prepare_main_menu();
int32_t lifecycle_participant_inventory_state_id();

}  // namespace qol::game
