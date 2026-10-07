#pragma once

#include <mutex>
#include "contracts/slot_operation.h"

namespace qol::infra::slot_operation {
// gate/store 共用同一锁。*_locked 只能在持此锁时调用，准入检查与 IO 不拆开。
std::mutex& mutex();
bool busy_locked(int32_t slot);
QolResult validate_locked(uint64_t owner, const contracts::QolSlotOperationLease*, int32_t slot);
bool reservation_admitted_locked(uint64_t owner, uint64_t operation, int32_t slot);
bool referenced_locked(uint64_t operation);
void owner_stopped_locked(uint64_t owner);
void owner_rebound_locked(uint64_t old_owner, uint64_t new_owner);

QolResult acquire(uint64_t owner, uint32_t mask, contracts::QolSlotOperationLease*);
QolResult set_referenced(uint64_t owner, contracts::QolSlotOperationLease*, uint32_t);
QolResult refresh(uint64_t owner, uint64_t id, contracts::QolSlotOperationLease*);
QolResult release(uint64_t owner, const contracts::QolSlotOperationLease*);
}  // namespace qol::infra::slot_operation
