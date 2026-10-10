#pragma once

#include <cstdint>
#include "result.h"
#include "owner_token.h"

namespace qol::contracts {

constexpr uint16_t kSettingsConfigMajor = 1;
constexpr uint16_t kSettingsConfigMinor = 1;
constexpr uint16_t kSettingsConfigLegacyMinor = 0;
constexpr uint32_t kSettingsConfigBoolCount = 10;
constexpr uint32_t kSettingsConfigRequestCapacity = 128;
constexpr uint32_t kSettingsConfigMessageCapacity = 256;

enum class SettingsConfigRequestState : uint32_t { kQueued = 0, kSucceeded = 1, kFailed = 2 };
enum class SettingsConfigApplyState : uint32_t {
    kNotSubmitted = 0,
    kPendingFrame = 1,  // setter accepted; neither command execution nor resources confirmed
    kApplied = 2,       // synchronous platform setter completed; not a gameplay guarantee
    kRejected = 3,
    kPlatformPending = 4,  // HTTP listener start/stop scheduled; readiness not confirmed
    kUnchanged = 5,
};

struct SettingsConfigRequestStatus {
    uint32_t size;  // caller initializes to sizeof(SettingsConfigRequestStatus)
    SettingsConfigRequestState state;
    uint64_t request_id;
    int32_t desired_value;
    int32_t persisted;  // 0/1; failure after save can still have persisted=1
    int32_t configured_value;  // confirmed configuration at completion, not effective gate
    int32_t error_code;
    SettingsConfigApplyState apply_state;
    char error[kSettingsConfigMessageCapacity];
    char apply_message[kSettingsConfigMessageCapacity];
};

struct QolSettingsConfigApi {
    uint32_t size;
    uint16_t major;
    uint16_t minor;
    // Only the ten public ModuleConfig bool keys. No defaults, gate keys or HTTP dependency.
    QolResult (*read_bool)(const char* key, int32_t* out_value);
    // Nonblocking queue admission only. desired must be exactly 0/1; poll the receipt.
    QolResult (*request_set_bool)(const char* key, int32_t desired, uint64_t* out_request);
    // Last 128 receipts, never evict queued requests. Unknown=NotFound; evicted=Cancelled.
    // succeeded confirms persistence + setter acceptance, NOT gameplay/listener readiness.
    QolResult (*query_request)(uint64_t id, SettingsConfigRequestStatus* out_status);
    // v1.1 owner-token UI adapter. Writes still route through config_namespace.update_value.
    QolResult (*read_bool_for_owner)(QolOwnerToken owner, const char* key, int32_t* out_value);
    QolResult (*request_set_bool_for_owner)(QolOwnerToken owner, const char* key, int32_t desired,
                                            uint64_t* out_request);
    QolResult (*query_request_for_owner)(QolOwnerToken owner, uint64_t id,
                                         SettingsConfigRequestStatus* out_status);
};

}  // namespace qol::contracts
