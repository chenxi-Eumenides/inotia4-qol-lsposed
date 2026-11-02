#pragma once

#include "game_ports/craft_port.h"

namespace qol::contracts {

constexpr const char* kCraftFactsCapabilityId = "craft.facts";
constexpr uint16_t kCraftFactsVersionMajor = 1;
constexpr uint16_t kCraftFactsVersionMinor = 0;
constexpr QolVersion kCraftFactsVersion{kCraftFactsVersionMajor, kCraftFactsVersionMinor};

struct CraftFactsApi {
    QolCraftReadApi read;
    QolCraftReadOnlyApi read_only;
};

}  // namespace qol::contracts
