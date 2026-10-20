#pragma once

#include <cstdint>

#include "host_sdk.h"
#include "owner_token.h"
#include "result.h"
#include "version.h"

namespace qol::contracts {

// 保存前通知能力（观察型）：原版保存写盘前、协调器 capture/lease 之前同步调用。
constexpr const char* kSavePreSaveCapabilityId = "save.pre_save";
constexpr QolVersion kSavePreSaveVersion{1, 0};

// 主线程同步回调；返回前消费者完成只读/文件操作。无返回值 = 观察型，不得阻断保存。
using SavePreSaveObserverFn = void (*)(void* user, int32_t slot);

struct SavePreSaveApi {
    QolAbiHeader abi;
    QolResult (*register_observer)(QolOwnerToken owner, SavePreSaveObserverFn fn, void* user,
                                   uint64_t* out_id);
    QolResult (*unregister_observer)(QolOwnerToken owner, uint64_t id);
};

}  // namespace qol::contracts
