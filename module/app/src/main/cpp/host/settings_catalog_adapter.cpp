#include "host/settings_catalog_adapter.h"

#include <array>
#include <cstring>

#include "game/adapter_ports/game_host_port.h"
#include "infra/settings_catalog.h"
#include "infra/status.h"

namespace qol::host {
namespace {
QolResult fail(int32_t code, const char* text) {
    return contracts::error(code, text, static_cast<uint32_t>(std::strlen(text)));
}

bool owner_enabled(const char* owner) {
    infra::FeatureStatus status{};
    return infra::status_get(owner, &status) && status.state == infra::FeatureState::kEnabled &&
           status.enabled;
}

contracts::SettingsOwnerState owner_state(infra::FeatureState state) {
    using contracts::SettingsOwnerState;
    switch (state) {
        case infra::FeatureState::kRegistered: return SettingsOwnerState::kRegistered;
        case infra::FeatureState::kResolving: return SettingsOwnerState::kResolving;
        case infra::FeatureState::kBlocked: return SettingsOwnerState::kBlocked;
        case infra::FeatureState::kUnsupported: return SettingsOwnerState::kUnsupported;
        case infra::FeatureState::kInvalid: return SettingsOwnerState::kInvalid;
        case infra::FeatureState::kInstalledDisabled: return SettingsOwnerState::kInstalledDisabled;
        case infra::FeatureState::kEnabled: return SettingsOwnerState::kEnabled;
        case infra::FeatureState::kDeactivating: return SettingsOwnerState::kDeactivating;
        case infra::FeatureState::kInactive: return SettingsOwnerState::kInactive;
        case infra::FeatureState::kFailed: return SettingsOwnerState::kFailed;
    }
    return SettingsOwnerState::kUnknown;
}

void set_reason(contracts::SettingsItemAvailability* out, const char* text) {
    if (text == nullptr) text = "";
    const std::size_t length = strnlen(text, sizeof(out->reason) - 1);
    std::memcpy(out->reason, text, length);
    out->reason[length] = '\0';
}

QolResult query_item_availability(const char* item_id, uint64_t generation,
                                  contracts::SettingsItemAvailability* out) {
    if (!game::game_host_is_main_thread()) return fail(kQolErrNotReady, "settings availability requires main thread");
    if (out == nullptr) return fail(kQolErrInvalidArgument, "null settings availability output");
    if (out->size < sizeof(*out)) return fail(kQolErrInvalidArgument, "settings availability output too small");
    const uint32_t output_size = out->size;
    *out = {};
    out->size = output_size;

    infra::SettingsActionRecord record{};
    auto result = infra::settings_catalog_resolve_item(item_id, generation, &record);
    if (!contracts::succeeded(result)) return result;

    infra::FeatureStatus before{};
    if (!infra::status_get(record.metadata.owner, &before)) {
        out->owner_state = static_cast<uint32_t>(contracts::SettingsOwnerState::kUnknown);
        set_reason(out, "settings item owner status unavailable");
        return contracts::ok();
    }
    out->owner_state = static_cast<uint32_t>(owner_state(before.state));
    bool operable = false;
    if (record.metadata.owner_enable != 0) {
        switch (before.state) {
            case infra::FeatureState::kInstalledDisabled:
                operable = true;
                break;
            case infra::FeatureState::kEnabled:
                operable = before.enabled;
                break;
            case infra::FeatureState::kInactive:
                operable = true;
                set_reason(out, "owner plugin is inactive; configuration can be changed without activation");
                break;
            case infra::FeatureState::kBlocked:
            case infra::FeatureState::kUnsupported:
            case infra::FeatureState::kFailed:
            case infra::FeatureState::kDeactivating:
            case infra::FeatureState::kRegistered:
            case infra::FeatureState::kResolving:
            case infra::FeatureState::kInvalid:
                break;
        }
    } else {
        operable = before.state == infra::FeatureState::kEnabled && before.enabled;
    }
    if (!operable && out->reason[0] == '\0') {
        set_reason(out, before.error != nullptr ? before.error : "owner does not permit this setting");
    }

    int32_t available = -1;
    std::array<char, contracts::kSettingsDescriptionCapacity> probe_reason{};
    QolResult probe_result = contracts::ok();
    if (record.available != nullptr) {
        probe_result = record.available(record.user, &available, probe_reason.data(), probe_reason.size());
        probe_reason.back() = '\0';
        if (contracts::succeeded(probe_result) && available != 0 && available != 1) {
            probe_result = fail(kQolErrInvalidArgument, "invalid settings availability");
        }
    }

    infra::SettingsActionRecord current{};
    result = infra::settings_catalog_resolve_item(record.metadata.id, generation, &current);
    if (!contracts::succeeded(result)) return result;
    infra::FeatureStatus after{};
    if (std::strcmp(current.metadata.owner, record.metadata.owner) != 0 ||
        !infra::status_get(record.metadata.owner, &after) || before.state != after.state ||
        before.enabled != after.enabled) {
        return fail(kQolErrConflict, "settings owner changed during availability query");
    }
    if (!contracts::succeeded(probe_result)) {
        set_reason(out, probe_result.message);
        return probe_result;
    }
    if (record.available != nullptr) {
        operable = operable && available == 1;
        if (available == 0) set_reason(out, probe_reason.data());
    }
    out->operable = operable ? 1 : 0;
    return contracts::ok();
}

QolResult invoke_action(const char* item_id, uint64_t generation) {
    // The same adapter fact used by the Host SDK/UI adapter guards unsynchronized owner state.
    if (!game::game_host_is_main_thread()) return fail(kQolErrNotReady, "settings action requires main thread");
    infra::SettingsActionRecord record{};
    auto result = infra::settings_catalog_resolve_action(item_id, generation, &record);
    if (!contracts::succeeded(result)) return result;
    if (!owner_enabled(record.metadata.owner)) return fail(kQolErrNotReady, "settings action owner not enabled");
    if (record.available != nullptr) {
        int32_t is_available = 0;
        std::array<char, contracts::kSettingsDescriptionCapacity> reason{};
        result = record.available(record.user, &is_available, reason.data(), reason.size());
        if (!contracts::succeeded(result)) return result;
        if (is_available != 0 && is_available != 1) return fail(kQolErrInvalidArgument, "invalid settings availability");
        if (is_available == 0) {
            // Result text must outlive this stack frame. Copy bounded, even if callback omitted NUL.
            thread_local std::array<char, contracts::kSettingsDescriptionCapacity> rejected_reason{};
            rejected_reason = reason;
            rejected_reason.back() = '\0';
            return fail(kQolErrBlocked, rejected_reason[0] != '\0' ? rejected_reason.data() :
                         "settings action unavailable");
        }
    }
    // available may explicitly remove contributions or disable its owner. Resolve again
    // with the copied id (not a snapshot pointer), without holding any catalog lock over callbacks.
    infra::SettingsActionRecord current{};
    result = infra::settings_catalog_resolve_action(record.metadata.id, generation, &current);
    if (!contracts::succeeded(result)) return result;
    if (!owner_enabled(current.metadata.owner)) return fail(kQolErrNotReady, "settings action owner not enabled");
    return current.open(current.user);
}

QolResult register_shared_items(const char* owner, const char* category_id,
                                const contracts::SettingsItemSpec* items, uint32_t count) {
    infra::FeatureStatus status{};
    if (owner == nullptr || !infra::status_get(owner, &status)) {
        return fail(kQolErrNotFound, "settings item owner is not registered");
    }
    return infra::settings_catalog_register_items(owner, category_id, items, count);
}
}  // namespace

const contracts::QolSettingsCatalogApi& settings_catalog_api() {
    static const contracts::QolSettingsCatalogApi api{
        sizeof(contracts::QolSettingsCatalogApi), contracts::kSettingsCatalogMajor,
        contracts::kSettingsCatalogMinor, &infra::settings_catalog_register_category,
        &infra::settings_catalog_unregister_owner, &infra::settings_catalog_snapshot_acquire,
        &infra::settings_catalog_snapshot_release, &invoke_action, &query_item_availability,
        &register_shared_items, &infra::settings_catalog_register_category_metadata};
    return api;
}
}  // namespace qol::host
