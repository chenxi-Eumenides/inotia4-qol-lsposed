#pragma once

#include <cstdint>
#include "owner_token.h"

namespace qol::contracts {
struct QolStoreSection;
enum class QolOwnerStoreState : uint32_t {
    kAbsent = 0, kPrimaryValid = 1, kFallbackUsed = 2, kCorrupt = 3, kIoFailed = 4,
};
struct QolOwnerStoreReservation {
    uint32_t size;
    int32_t slot;
    uint64_t id;
    uint64_t generation;
    QolOwnerToken owner;
    uint64_t encoded_bytes; // 所有候选编码长度的最大值，不是 payload 求和。
    uint32_t retain;        // 非零：在途/恢复仍依赖该租约，owner 停用时不得释放。
    uint32_t reserved;
    // 磁盘 generation 不变的引用/归属变更也使旧凭据失效，不以 retain 副本作为权限。
    uint64_t revision;
};
struct QolOwnerStoreSlotState {
    uint32_t size;
    uint32_t state; // QolOwnerStoreState
    uint64_t generation;
    uint64_t encoded_bytes;
    QolOwnerStoreReservation reservation; // 无预约时 id=0；授权恢复者可取得迁移后的凭据。
};
struct QolOwnerStoreCandidate {
    uint32_t size;
    uint32_t replacement_count;
    const QolStoreSection* replacements;
    uint32_t removal_count;
    const char* const* removals;
    const QolStoreSection* journal; // 可为空；只允许 module.save.journal，追加于替换集之后。
};
}  // namespace qol::contracts
