#include "contracts/host_sdk.h"
#include "host/settings_catalog_adapter.h"
#include "infra/settings_catalog.h"
#include "infra/status.h"

#include <cassert>
#include <array>
#include <cstring>
#include <initializer_list>
#include <thread>

namespace {
using namespace qol::contracts;
using qol::infra::FeatureState;
const std::thread::id kMainThread = std::this_thread::get_id();
int g_open_calls = 0;
int g_available_calls = 0;
int32_t g_available = 1;
QolResult g_open_result = ok();
QolResult g_available_result = ok();
bool g_unregister_in_available = false;
bool g_disable_in_available = false;

QolResult open(void* user) {
    assert(user == &g_open_calls);
    ++g_open_calls;
    // Reentrant catalog operations must never deadlock.
    return g_open_result;
}
QolResult available(void* user, int32_t* out, char* reason, uint32_t capacity) {
    assert(user == &g_open_calls);
    ++g_available_calls;
    *out = g_available;
    assert(capacity >= 8);
    std::memcpy(reason, "blocked", 8);
    const CatalogView* view = nullptr;
    SettingsCatalogToken token = 0;
    const auto& api = qol::host::settings_catalog_api();
    assert(api.snapshot_acquire(&view, &token).ok == kQolOk);
    assert(api.snapshot_release(token).ok == kQolOk);
    if (g_unregister_in_available) assert(api.unregister_owner("sample.owner").ok == kQolOk);
    if (g_disable_in_available) {
        assert(qol::infra::status_update("sample.owner", FeatureState::kInstalledDisabled, false, nullptr));
    }
    return g_available_result;
}
}  // namespace

namespace qol::game {
bool game_host_is_main_thread() { return std::this_thread::get_id() == kMainThread; }
}  // namespace qol::game

int main() {
    using namespace qol::contracts;
    const auto& api = qol::host::settings_catalog_api();
    assert(api.size == sizeof(QolSettingsCatalogApi) && api.major == 1 && api.minor == 3);
    SettingsCategorySpec category{sizeof(SettingsCategorySpec), "sample", "sample.owner", "Sample", ""};
    std::array<SettingsItemSpec, 2> items{{
        {sizeof(SettingsItemSpec), "sample.open", "sample", "sample.owner", "Open", "",
         SettingsItemType::kAction, nullptr, 0, &open, &available, &g_open_calls},
        {sizeof(SettingsItemSpec), "sample.enable", "sample", "sample.owner", "Enable", "",
         SettingsItemType::kToggle, "sampleEnabled", 1, nullptr, &available, &g_open_calls}}};
    assert(api.register_category(&category, items.data(), items.size()).ok == kQolOk);
    assert(g_open_calls == 0 && g_available_calls == 0);
    const CatalogView* view = nullptr;
    SettingsCatalogToken token = 0;
    assert(api.snapshot_acquire(&view, &token).ok == kQolOk);
    const uint64_t generation = view->generation;
    assert(api.invoke_action("sample.enable", generation).code == kQolErrInvalidArgument);
    assert(api.invoke_action("missing", generation).code == kQolErrNotFound);
    assert(api.invoke_action("sample.open", generation).code == kQolErrNotReady);  // owner absent
    assert(qol::infra::status_register("sample.owner", "1"));
    for (auto state : {FeatureState::kRegistered, FeatureState::kResolving, FeatureState::kBlocked,
                       FeatureState::kUnsupported, FeatureState::kInvalid, FeatureState::kInstalledDisabled,
                       FeatureState::kDeactivating, FeatureState::kInactive, FeatureState::kFailed}) {
        assert(qol::infra::status_update("sample.owner", state, false, nullptr));
        assert(api.invoke_action("sample.open", generation).code == kQolErrNotReady);
    }
    assert(g_open_calls == 0 && g_available_calls == 0);
    const CatalogView* disabled_view = nullptr;
    SettingsCatalogToken disabled_token = 0;
    assert(api.snapshot_acquire(&disabled_view, &disabled_token).ok == kQolOk);
    assert(disabled_view->item_count == 2 && disabled_view->items[1].owner_enable == 1);
    assert(api.snapshot_release(disabled_token).ok == kQolOk);
    assert(qol::infra::status_update("sample.owner", FeatureState::kEnabled, false, nullptr));
    assert(api.invoke_action("sample.open", generation).code == kQolErrNotReady);
    assert(qol::infra::status_update("sample.owner", FeatureState::kEnabled, true, nullptr));
    SettingsItemAvailability availability{sizeof(SettingsItemAvailability)};
    assert(api.query_item_availability("sample.enable", generation, &availability).ok == kQolOk);
    assert(availability.operable == 1 && availability.owner_state ==
           static_cast<uint32_t>(SettingsOwnerState::kEnabled));
    assert(g_open_calls == 0 && g_available_calls == 1);
    assert(qol::infra::status_update("sample.owner", FeatureState::kInstalledDisabled, false, nullptr));
    assert(api.query_item_availability("sample.enable", generation, &availability).ok == kQolOk);
    assert(availability.operable == 1 && availability.owner_state ==
           static_cast<uint32_t>(SettingsOwnerState::kInstalledDisabled));
    assert(qol::infra::status_update("sample.owner", FeatureState::kInactive, false, nullptr));
    assert(api.query_item_availability("sample.enable", generation, &availability).ok == kQolOk);
    assert(availability.operable == 1 && std::strstr(availability.reason, "inactive") != nullptr);
    assert(qol::infra::status_update("sample.owner", FeatureState::kBlocked, false, "dependency missing"));
    assert(api.query_item_availability("sample.enable", generation, &availability).ok == kQolOk);
    assert(availability.operable == 0 && std::strcmp(availability.reason, "dependency missing") == 0);
    for (auto state : {FeatureState::kUnsupported, FeatureState::kFailed}) {
        assert(qol::infra::status_update("sample.owner", state, false, "owner unavailable"));
        assert(api.query_item_availability("sample.enable", generation, &availability).ok == kQolOk);
        assert(availability.operable == 0 && std::strcmp(availability.reason, "owner unavailable") == 0);
    }
    assert(qol::infra::status_update("sample.owner", FeatureState::kDeactivating, false, nullptr));
    assert(api.query_item_availability("sample.enable", generation, &availability).ok == kQolOk);
    assert(availability.operable == 0);
    assert(api.query_item_availability("sample.open", generation, &availability).ok == kQolOk);
    assert(availability.operable == 0);
    assert(qol::infra::status_update("sample.owner", FeatureState::kEnabled, true, nullptr));
    g_available = 0;
    auto unavailable = api.query_item_availability("sample.enable", generation, &availability);
    assert(unavailable.ok == kQolOk && availability.operable == 0 &&
           std::strcmp(availability.reason, "blocked") == 0);
    g_available = 1;
    g_available_result = error(kQolErrUnsupported, "probe failure", 13);
    auto probe_failure = api.query_item_availability("sample.enable", generation, &availability);
    assert(probe_failure.code == kQolErrUnsupported &&
           std::strcmp(availability.reason, "probe failure") == 0);
    g_available_result = ok();
    assert(api.query_item_availability("sample.enable", generation - 1, &availability).code == kQolErrConflict);
    assert(api.query_item_availability("missing", generation, &availability).code == kQolErrNotFound);
    SettingsItemAvailability small{sizeof(SettingsItemAvailability) - 1};
    assert(api.query_item_availability("sample.enable", generation, &small).code == kQolErrInvalidArgument);
    assert(api.query_item_availability("sample.enable", generation, nullptr).code == kQolErrInvalidArgument);
    g_disable_in_available = true;
    assert(api.query_item_availability("sample.enable", generation, &availability).code == kQolErrConflict);
    g_disable_in_available = false;
    assert(qol::infra::status_update("sample.owner", FeatureState::kEnabled, true, nullptr));
    const int calls_before_worker = g_available_calls;
    std::thread availability_worker([&] {
        SettingsItemAvailability worker_out{sizeof(SettingsItemAvailability)};
        assert(api.query_item_availability("sample.enable", generation, &worker_out).code == kQolErrNotReady);
    });
    availability_worker.join();
    assert(g_open_calls == 0 && g_available_calls == calls_before_worker);
    std::thread worker([&] {
        assert(api.invoke_action("sample.open", generation).code == kQolErrNotReady);
    });
    worker.join();
    assert(g_open_calls == 0 && g_available_calls == calls_before_worker);
    assert(api.invoke_action("sample.open", generation - 1).code == kQolErrConflict);
    const int calls_before_open = g_available_calls;
    assert(api.invoke_action("sample.open", generation).ok == kQolOk);
    assert(g_open_calls == 1 && g_available_calls == calls_before_open + 1);
    g_available = 0;
    auto blocked = api.invoke_action("sample.open", generation);
    assert(blocked.code == kQolErrBlocked);
    assert(blocked.message_len == 7 && std::memcmp(blocked.message, "blocked", 7) == 0);
    assert(g_open_calls == 1);
    g_available = 2;
    assert(api.invoke_action("sample.open", generation).code == kQolErrInvalidArgument);
    assert(g_open_calls == 1);
    g_available = 1;
    g_available_result = error(kQolErrUnsupported, "probe failure", 13);
    auto result = api.invoke_action("sample.open", generation);
    assert(result.code == kQolErrUnsupported && result.message == g_available_result.message);
    assert(g_open_calls == 1);
    g_available_result = ok();
    g_open_result = error(kQolErrInternal, "open failure", 12);
    result = api.invoke_action("sample.open", generation);
    assert(result.code == kQolErrInternal && result.message == g_open_result.message);
    assert(g_open_calls == 2);
    g_open_result = ok();
    g_disable_in_available = true;
    assert(api.invoke_action("sample.open", generation).code == kQolErrNotReady);
    assert(g_open_calls == 2);
    g_disable_in_available = false;
    assert(qol::infra::status_update("sample.owner", FeatureState::kEnabled, true, nullptr));
    g_unregister_in_available = true;
    assert(api.invoke_action("sample.open", generation).code == kQolErrConflict);
    assert(g_open_calls == 2);
    // Metadata survives explicit removal, but actions must resolve the new generation.
    assert(std::strcmp(view->items[0].id, "sample.open") == 0);
    assert(api.invoke_action("sample.open", generation).code == kQolErrConflict);
    assert(api.snapshot_release(token).ok == kQolOk);
    qol::infra::FeatureStatus status{};
    assert(qol::infra::status_get("sample.owner", &status));
    assert(status.state == FeatureState::kEnabled && status.enabled);  // no lifecycle mutation
    // Optional availability does not prevent an enabled static action from opening.
    g_unregister_in_available = false;
    items[0].available = nullptr;
    assert(api.register_category(&category, items.data(), items.size()).ok == kQolOk);
    assert(api.snapshot_acquire(&view, &token).ok == kQolOk);
    assert(api.invoke_action("sample.open", view->generation).ok == kQolOk);
    assert(g_open_calls == 3);
    assert(api.snapshot_release(token).ok == kQolOk);

    assert(api.minor == 3 && api.register_items != nullptr && api.register_category_metadata != nullptr);
    SettingsItemSpec foreign_action{sizeof(SettingsItemSpec), "foreign.action", "sample", "foreign.owner",
                                    "Foreign action", "", SettingsItemType::kAction, nullptr, 0,
                                    &open, nullptr, &g_open_calls};
    assert(api.register_items("foreign.owner", "sample", &foreign_action, 1).code == kQolErrNotFound);
    assert(qol::infra::status_register("foreign.owner", "1"));
    assert(api.register_items("foreign.owner", "sample", &foreign_action, 1).ok == kQolOk);
    assert(api.snapshot_acquire(&view, &token).ok == kQolOk);
    assert(view->item_count == 3);
    assert(std::strcmp(view->categories[0].owner, "sample.owner") == 0);
    assert(std::strcmp(view->items[2].owner, "foreign.owner") == 0);
    const auto foreign_generation = view->generation;
    assert(api.invoke_action("foreign.action", foreign_generation).code == kQolErrNotReady);
    assert(qol::infra::status_update("foreign.owner", FeatureState::kEnabled, true, nullptr));
    assert(api.invoke_action("foreign.action", foreign_generation).ok == kQolOk);
    assert(g_open_calls == 4);
    assert(api.snapshot_release(token).ok == kQolOk);
}
