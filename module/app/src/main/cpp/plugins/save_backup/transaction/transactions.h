#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "contracts/slot_operation.h"

// save_backup L3：导入/删除/回滚/恢复事务。
// 只暴露 usecase 需要的窄接口。

namespace save_backup {
namespace usecases {
namespace transaction {

struct SlotOperationGuard {
    qol::contracts::QolSlotOperationLease lease{};
    const qol::contracts::QolSlotOperationApi* api = nullptr;
    bool held = false;
};

enum class ContainerStatus : uint8_t {
    kOk,
    kNotFound,
    kCorrupt,
    kOther,
};

bool slot_operation_acquire(int slot, SlotOperationGuard* guard);
void slot_operation_release(SlotOperationGuard* guard);
ContainerStatus store_read_container(int slot, std::vector<uint8_t>& out);
std::string import_bundle_file(const std::string& path, int slot);
std::string delete_json(const char* checksum);
std::string delete_slot_json(int slot);
void recover_deletes();
void slot_deleted_sync(int32_t slot);

}  // namespace transaction
}  // namespace usecases
}  // namespace save_backup
