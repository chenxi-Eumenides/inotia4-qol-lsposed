#pragma once

#include "game_host.h"
#include "version.h"

namespace qol::contracts {

constexpr const char* kStackLimitItemCountCapabilityId = "stack_limit.item_count";
constexpr QolVersion kStackLimitItemCountVersion{1, 0};

struct StackLimitItemCountCapability {
    const QolItemCountApi* api;
    int32_t (*enabled)();
};

}  // namespace qol::contracts
