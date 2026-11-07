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
    // 存档 enter/exit 发起点 call patch（装配末段首装 + 逻辑帧补装）。
    // 与 kModuleInitialization 分开：首装失败属可恢复的待补装状态，补装成功不应留下失败位，
    // 补装窗口耗尽才上报失败。
    kSaveEntryPatches = 1U << 4,
};

constexpr uint32_t kQolModuleHealthAllMask =
    static_cast<uint32_t>(QolModuleHealthBit::kNative) |
    static_cast<uint32_t>(QolModuleHealthBit::kFrameHooks) |
    static_cast<uint32_t>(QolModuleHealthBit::kModuleInitialization) |
    static_cast<uint32_t>(QolModuleHealthBit::kConfigStorage) |
    static_cast<uint32_t>(QolModuleHealthBit::kSaveEntryPatches);

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
