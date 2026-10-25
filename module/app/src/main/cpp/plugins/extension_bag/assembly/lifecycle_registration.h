#pragma once

#include "contracts/lifecycle_participant.h"

namespace qol::features::extension_bag::detail {

inline bool register_lifecycle_participant_once(
    contracts::QolLifecycleParticipantRegisterFn register_participant,
    const contracts::QolLifecycleParticipant* participant, bool& registered) {
    if (registered) return true;
    if (register_participant == nullptr || participant == nullptr ||
        register_participant(participant) == 0) {
        return false;
    }
    registered = true;
    return true;
}

}  // namespace qol::features::extension_bag::detail
