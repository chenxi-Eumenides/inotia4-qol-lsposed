#pragma once

#include <cstdint>

#include "result.h"
#include "version.h"

namespace qol::contracts {

constexpr const char* kModuleHealthCapabilityId = "module.health";
constexpr QolVersion kModuleHealthCapabilityVersion{1, 0};

enum class QolModuleHealthBit : uint32_t {
    kNative = 1U << 0,
    kFrameHooks = 1U << 1,
    kModuleInitialization = 1U << 2,
    kConfigStorage = 1U << 3,
};

constexpr uint32_t kQolModuleHealthAllMask =
    static_cast<uint32_t>(QolModuleHealthBit::kNative) |
    static_cast<uint32_t>(QolModuleHealthBit::kFrameHooks) |
    static_cast<uint32_t>(QolModuleHealthBit::kModuleInitialization) |
    static_cast<uint32_t>(QolModuleHealthBit::kConfigStorage);

struct QolModuleHealthSnapshot {
    uint32_t struct_size;
    uint64_t revision;
    uint32_t known_mask;
    uint32_t failed_mask;
    uint8_t main_menu_seen;
};

struct QolHostModuleHealthFactsApi {
    uint32_t struct_size;
    void* context;
    QolResult (*read)(void*, QolModuleHealthSnapshot*);
};

}  // namespace qol::contracts
