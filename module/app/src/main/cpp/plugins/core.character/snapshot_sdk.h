#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "contracts/host_sdk.h"

namespace qol::features::core_character {

template <typename T>
class Snapshot {
    static_assert(std::is_trivially_copyable<T>::value,
                  "snapshot payload must be trivially copyable");
    // 宿主快照存储是 std::vector<uint8_t> 持有的字节副本（默认 new 对齐 ≥ alignof(max_align_t)），
    // ReadView 直接把该缓冲当 T 读取，不做整块拷贝；超出该对齐的 T 不能这样读。
    static_assert(alignof(T) <= alignof(std::max_align_t),
                  "snapshot payload alignment must not exceed default new alignment");

public:
    class ReadView {
    public:
        ReadView() = default;
        ~ReadView() { release(); }

        ReadView(const ReadView&) = delete;
        ReadView& operator=(const ReadView&) = delete;

        // 无锁视图：返回宿主快照缓冲的首地址，生命周期由本守卫持有到析构。
        const T* get() const { return data_; }

    private:
        friend class Snapshot<T>;

        void release() {
            if (active_ && sdk_ != nullptr) sdk_->snapshot_release(token_);
            sdk_ = nullptr;
            data_ = nullptr;
            token_ = nullptr;
            active_ = false;
        }

        const contracts::QolHostSdk* sdk_ = nullptr;
        const T* data_ = nullptr;
        contracts::QolSnapshotToken token_ = nullptr;
        bool active_ = false;
    };

    bool initialize(const contracts::QolHostSdk* sdk) {
        if (handle_ != nullptr) return sdk_ == sdk;
        constexpr uint32_t kApiEnd =
            offsetof(contracts::QolHostSdk, snapshot_release) +
            sizeof(contracts::QolHostSdk::snapshot_release);
        if (sdk == nullptr || sdk->abi.size < kApiEnd || sdk->abi.minor < 15 ||
            sdk->snapshot_create == nullptr || sdk->snapshot_destroy == nullptr ||
            sdk->snapshot_publish == nullptr || sdk->snapshot_acquire == nullptr ||
            sdk->snapshot_release == nullptr) {
            return false;
        }
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

    bool acquire(ReadView& out) const {
        if (handle_ == nullptr || sdk_ == nullptr || out.active_) return false;
        const void* data = nullptr;
        uint32_t size = 0;
        contracts::QolSnapshotToken token = nullptr;
        if (!sdk_->snapshot_acquire(handle_, &data, &size, &token)) return false;
        if (data == nullptr || size != sizeof(T)) {
            sdk_->snapshot_release(token);
            return false;
        }
        out.sdk_ = sdk_;
        out.data_ = static_cast<const T*>(data);
        out.token_ = token;
        out.active_ = true;
        return true;
    }

private:
    const contracts::QolHostSdk* sdk_ = nullptr;
    contracts::QolSnapshotHandle handle_ = nullptr;
};

}  // namespace qol::features::core_character
