#pragma once

#include <cstdint>

namespace extension_bag_retry {

enum class FailureKind : uint8_t {
    kNone,
    kSourceProtectionPending,
    kInventoryStateCallbacksMismatch,
    kItemDrawThunkFailed,
    kItemDrawTargetOutOfRange,
    kItemDrawMprotectFailed,
    kItemDrawPatchMismatch,
    kBagDrawThunkFailed,
    kBagDrawTargetOutOfRange,
    kBagDrawMprotectFailed,
    kBagDrawPatchMismatch,
    kDrawWrapperThunkFailed,
    kDrawBranchOutOfRange,
    kDrawMprotectFailed,
    kDrawPatchMismatch,
    kNativeHookUnavailable,
    kSaveHookFailed,
    kDropGateThunkFailed,
    kDropGateOutOfRange,
    kDropGateMprotectFailed,
    kDropGatePatchMismatch,
    kDrawGateThunkFailed,
    kDrawGateOutOfRange,
    kDrawGateMprotectFailed,
    kDrawGatePatchMismatch,
    kDescOpenThunkFailed,
    kDescOpenOutOfRange,
    kDescOpenMprotectFailed,
    kDescOpenPatchMismatch,
    kSavePanelUnavailable,
    kSaveCallsiteFailed,
};

struct FailureKey {
    FailureKind kind = FailureKind::kNone;
    uintptr_t address = 0;
    uintptr_t detail = 0;
    int32_t result = 0;
    int32_t error = 0;
    uint32_t word = 0;
};

constexpr bool failure_key_equal(const FailureKey& lhs, const FailureKey& rhs) {
    return lhs.kind == rhs.kind && lhs.address == rhs.address && lhs.detail == rhs.detail &&
           lhs.result == rhs.result && lhs.error == rhs.error && lhs.word == rhs.word;
}

constexpr bool retry_log_should_emit(bool has_previous, const FailureKey& previous,
                                     const FailureKey& current) {
    return !has_previous || !failure_key_equal(previous, current);
}

struct RetryLogState {
    FailureKey previous{};
    bool has_previous = false;

    constexpr bool observe_failure(const FailureKey& current) {
        if (!retry_log_should_emit(has_previous, previous, current)) return false;
        previous = current;
        has_previous = true;
        return true;
    }

    constexpr void reset() {
        previous = {};
        has_previous = false;
    }
};

struct InjectionInstallState {
    bool inventory = false;
    bool save_panel = false;
    bool store = false;
    bool mix = false;
};

constexpr bool injection_complete(const InjectionInstallState& state) {
    return state.inventory && state.save_panel && state.store && state.mix;
}

constexpr int64_t debug_fingerprint(const FailureKey& key) {
    uint64_t value = static_cast<uint64_t>(key.kind);
    value = (value * 1315423911u) ^ static_cast<uint64_t>(key.address);
    value = (value * 1315423911u) ^ static_cast<uint64_t>(key.detail);
    value = (value * 1315423911u) ^ static_cast<uint32_t>(key.result);
    value = (value * 1315423911u) ^ static_cast<uint32_t>(key.error);
    value = (value * 1315423911u) ^ key.word;
    return static_cast<int64_t>(value);
}

}  // namespace extension_bag_retry
