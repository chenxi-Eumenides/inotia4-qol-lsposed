#pragma once

#include "contracts/lifecycle_participant.h"

namespace qol::host {

int32_t host_lifecycle_participant_register(
    const contracts::QolLifecycleParticipant* participant);
const contracts::QolLifecycleDispatch* host_lifecycle_dispatch();

}  // namespace qol::host
