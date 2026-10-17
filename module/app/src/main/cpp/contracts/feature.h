#pragma once

#include <cstdint>
#include "result.h"
#include "owner_token.h"

namespace qol::contracts {
struct PluginContext;
enum class FeatureCategory : uint8_t {
    kGeneral,
    kGameplay,
    kTools,
    kHidden,
    kBasicOptimization,
    kGameInfo = 5,  // settings page read-only information category; no feature uses it
};
enum class FeatureType : uint8_t { kToggle, kEntry };
constexpr int8_t kMinFeatureDisplayPriority = -10;
constexpr int8_t kMaxFeatureDisplayPriority = 10;
using FeatureToggleCallback = QolResult (*)(PluginContext&, uint8_t enabled);
using FeatureEntryCallback = QolResult (*)(PluginContext&);

// Metadata has static lifetime and is never serialized. Both kinds have a boolean state.
struct FeatureDefinition {
    const char* id;
    uint8_t default_enabled;
    FeatureCategory category;
    FeatureType type;
    const char* name;
    const char* description;
    FeatureToggleCallback on_toggle;
    FeatureEntryCallback on_entry;
    const char* action_label = nullptr;
    int8_t display_priority = 0;
};

// Category labels and ordering are Host-owned registration metadata, not UI literals.
struct FeatureCategoryDefinition {
    FeatureCategory category;
    const char* name;
    int8_t display_priority = 0;
};

enum class FeatureAdmission : uint8_t { kRejected, kAdmitted };
enum class FeaturePersistence : uint8_t { kPending, kPersisted, kFailed };
enum class FeatureActual : uint8_t { kNotSubmitted, kPending };
struct FeatureReceipt {
    uint64_t request_id;
    uint64_t revision;
    FeatureAdmission admission;
    FeaturePersistence persistence;
    FeatureActual actual;
    int32_t error_code;
};

// Host injects context and administrative authorization; no caller-supplied identity.
struct QolFeatureApi {
    uint32_t struct_size;
    void* context;
    QolResult (*read_namespace)(void*, QolOwnerToken, const char* plugin_id,
                                char*, uint32_t, uint32_t*);
    QolResult (*read_feature)(void*, QolOwnerToken, const char*, const char*, uint8_t*);
    QolResult (*request_set_feature)(void*, QolOwnerToken, const char*, const char*, uint8_t,
                                    FeatureReceipt*);
    QolResult (*read_config)(void*, QolOwnerToken, const char*, const char*,
                             char*, uint32_t, uint32_t*);
    QolResult (*request_set_config)(void*, QolOwnerToken, const char*, const char*,
                                    const char*, uint32_t, FeatureReceipt*);
    QolResult (*receipt)(void*, QolOwnerToken, uint64_t, FeatureReceipt*);
    QolResult (*read_catalog)(void*, QolOwnerToken, char*, uint32_t, uint32_t*);
    QolResult (*read_desired)(void*, QolOwnerToken, const char*, uint8_t*, uint8_t*);
    QolResult (*request_desired)(void*, QolOwnerToken, const char*, uint8_t, FeatureReceipt*);
    QolResult (*invoke_entry)(void*, QolOwnerToken, const char*, const char*);
};

}  // namespace qol::contracts
