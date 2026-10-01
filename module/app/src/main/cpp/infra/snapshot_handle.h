#pragma once

#include "contracts/host_sdk.h"

namespace qol::infra::snapshot_handle {

contracts::QolSnapshotHandle create(uint32_t byte_size) noexcept;
void destroy(contracts::QolSnapshotHandle handle) noexcept;
bool publish(contracts::QolSnapshotHandle handle, const void* data,
             uint32_t byte_size) noexcept;
bool acquire(contracts::QolSnapshotHandle handle, const void** out_data, uint32_t* out_size,
             contracts::QolSnapshotToken* out_token) noexcept;
void release(contracts::QolSnapshotToken token) noexcept;

}  // namespace qol::infra::snapshot_handle
