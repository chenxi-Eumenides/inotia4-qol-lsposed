#pragma once

#include <cstdint>

namespace qol::contracts {

// Apply-state taxonomy shared by the core.settings config service and the settings panel model.
// Values are frozen: they persist in apply receipts and panel status text.
enum class SettingsConfigApplyState : uint32_t {
    kNotSubmitted = 0,
    kPendingFrame = 1,  // setter accepted; neither command execution nor resources confirmed
    kApplied = 2,       // synchronous platform setter completed; not a gameplay guarantee
    kRejected = 3,
    kPlatformPending = 4,  // HTTP listener start/stop scheduled; readiness not confirmed
    kUnchanged = 5,
};

}  // namespace qol::contracts
