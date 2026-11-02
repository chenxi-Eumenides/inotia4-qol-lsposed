#pragma once

#include <cstdint>
#include <cstring>
#include <type_traits>

#include "contracts/host_sdk.h"

namespace qol::features::core_quest {

template <typename T>
class Snapshot {
    static_assert(std::is_trivially_copyable<T>::value,
                  "snapshot payload must be trivially copyable");

public:
    bool initialize(const contracts::QolHostSdk* sdk) {
        if (handle_ != nullptr) return sdk_ == sdk;
        constexpr uint32_t kApiEnd = offsetof(contracts::QolHostSdk, snapshot_release) +
                                     sizeof(contracts::QolHostSdk::snapshot_release);
        if (sdk == nullptr || sdk->abi.size < kApiEnd || sdk->abi.minor < 15 ||
            sdk->snapshot_create == nullptr || sdk->snapshot_destroy == nullptr ||
            sdk->snapshot_publish == nullptr || sdk->snapshot_acquire == nullptr ||
            sdk->snapshot_release == nullptr) return false;
        sdk_ = sdk;
        handle_ = sdk_->snapshot_create(sizeof(T));
        if (handle_ == nullptr) sdk_ = nullptr;
        return handle_ != nullptr;
    }

    void destroy() {
        if (handle_ != nullptr && sdk_ != nullptr) sdk_->snapshot_destroy(handle_);
        handle_ = nullptr;
        sdk_ = nullptr;
    }

    bool publish(const T& value) const {
        return handle_ != nullptr && sdk_ != nullptr &&
               sdk_->snapshot_publish(handle_, &value, sizeof(T));
    }

    bool read(T& out) const {
        if (handle_ == nullptr || sdk_ == nullptr) return false;
        const void* data = nullptr;
        uint32_t size = 0;
        contracts::QolSnapshotToken token = nullptr;
        if (!sdk_->snapshot_acquire(handle_, &data, &size, &token)) return false;
        const bool valid = data != nullptr && size == sizeof(T);
        if (valid) std::memcpy(&out, data, sizeof(T));
        sdk_->snapshot_release(token);
        return valid;
    }

private:
    const contracts::QolHostSdk* sdk_ = nullptr;
    contracts::QolSnapshotHandle handle_ = nullptr;
};

}  // namespace qol::features::core_quest
