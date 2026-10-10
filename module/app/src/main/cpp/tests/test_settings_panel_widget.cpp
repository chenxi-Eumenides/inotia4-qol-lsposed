// Focused UI test for the production settings page after the feature-control rewiring.
//
// The page no longer owns any config store/schema or legacy bool map. It renders the
// Host-owned typed feature catalog through QolFeatureControlApi and drives management
// writes with its own active callbacks. This file includes the production body so the
// real rendering/toggle/entry paths are exercised, not a copy.
#include "../plugins/core.settings/panels/settings.cpp"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

using namespace qol;
using namespace qol::features::core_ui;
using contracts::FeatureAdmission;
using contracts::FeatureCategory;
using contracts::FeaturePersistence;
using contracts::FeatureReceipt;
using contracts::FeatureType;
using ::QolResult;

// ---- fake feature catalog (static metadata, Host-owned in production) ----

const contracts::QolFeatureCatalogItem kSettingsFeatures[]{
    {"page", 1, FeatureCategory::kTools, FeatureType::kEntry, "模块设置",
     "在主菜单打开模块设置页。"},
    {"console", 0, FeatureCategory::kHidden, FeatureType::kToggle, "调试控制台",
     "隐藏功能。"},
};
const contracts::QolFeatureCatalogItem kSellFeatures[]{
    {"sell", 1, FeatureCategory::kGameplay, FeatureType::kToggle, "自动出售",
     "控制自动出售扫描。"},
    {"diag", 0, FeatureCategory::kHidden, FeatureType::kToggle, "诊断日志",
     "隐藏诊断。"},
};
const contracts::QolFeatureCatalogPlugin kPlugins[]{
    {"settings", kSettingsFeatures, 2},
    {"autosell", kSellFeatures, 2},
};

struct CallLog {
    std::string plugin;
    std::string feature;
    int set_feature_calls = 0;
    int desired_calls = 0;
    int value = -1;
    int entry_calls = 0;
    uint64_t next_request = 1;
    uint64_t last_request = 0;
    FeatureReceipt receipt{};
};

CallLog g_log;
bool g_read_feature_fails = false;
bool g_request_fails = false;
uint8_t g_feature_values[4]{1, 0, 1, 0};
uint8_t g_desired_values[2]{1, 1};

int plugin_index(const char* id) { return std::strcmp(id, "settings") == 0 ? 0 : 1; }
int feature_index(const char* plugin, const char* feature) {
    if (std::strcmp(plugin, "settings") == 0) return std::strcmp(feature, "page") == 0 ? 0 : 1;
    return std::strcmp(feature, "sell") == 0 ? 2 : 3;
}

void reset_log() {
    g_log = {};
    g_read_feature_fails = false;
    g_request_fails = false;
    g_feature_values[0] = 1; g_feature_values[1] = 0;
    g_feature_values[2] = 1; g_feature_values[3] = 0;
    g_desired_values[0] = 1; g_desired_values[1] = 1;
}

QolResult fake_read_catalog(void*, const contracts::QolFeatureCatalogPlugin** out, uint32_t* count) {
    *out = kPlugins;
    *count = 2;
    return contracts::ok();
}
QolResult fake_read_feature(void*, const char* plugin, const char* feature, uint8_t* out) {
    if (g_read_feature_fails && std::strcmp(plugin, "autosell") == 0)
        return contracts::error(kQolErrNotReady, "plugin inactive", 16);
    *out = g_feature_values[feature_index(plugin, feature)];
    return contracts::ok();
}
QolResult fake_read_desired(void*, const char* plugin, uint8_t* enable, uint8_t* any) {
    const int i = plugin_index(plugin);
    *enable = g_desired_values[i];
    *any = g_desired_values[i];
    return contracts::ok();
}
QolResult fake_request_set_feature(void*, const char* plugin, const char* feature, uint8_t value,
                                   FeatureReceipt* receipt) {
    ++g_log.set_feature_calls;
    g_log.plugin = plugin;
    g_log.feature = feature;
    g_log.value = value;
    if (g_request_fails) return contracts::error(kQolErrBlocked, "rejected", 8);
    g_log.last_request = g_log.next_request++;
    receipt->request_id = g_log.last_request;
    receipt->admission = FeatureAdmission::kAdmitted;
    receipt->persistence = FeaturePersistence::kPending;
    g_log.receipt = *receipt;
    return contracts::ok();
}
QolResult fake_request_desired(void*, const char* plugin, uint8_t value, FeatureReceipt* receipt) {
    ++g_log.desired_calls;
    g_log.plugin = plugin;
    g_log.feature.clear();
    g_log.value = value;
    if (g_request_fails) return contracts::error(kQolErrBlocked, "rejected", 8);
    g_log.last_request = g_log.next_request++;
    receipt->request_id = g_log.last_request;
    receipt->admission = FeatureAdmission::kAdmitted;
    receipt->persistence = FeaturePersistence::kPending;
    g_log.receipt = *receipt;
    return contracts::ok();
}
QolResult fake_receipt(void*, uint64_t, FeatureReceipt* out) {
    *out = g_log.receipt;
    return contracts::ok();
}
QolResult fake_invoke_entry(void*, const char* plugin, const char* feature) {
    ++g_log.entry_calls;
    g_log.plugin = plugin;
    g_log.feature = feature;
    return contracts::ok();
}

contracts::QolFeatureControlApi g_fake_control{};

// ---- fake UI adapter/components (only members the exercised paths touch) ----

int g_widget_creates = 0;
int g_widget_destroys = 0;
int g_button_creates = 0;
int g_button_destroys = 0;
int g_list_destroys = 0;
contracts::QolUiHandle g_fake_root = reinterpret_cast<void*>(0x1000);
contracts::QolUiHandle g_list_handle = reinterpret_cast<void*>(0x2000);
contracts::QolUiHandle g_row_handle = reinterpret_cast<void*>(0x3000);
contracts::QolUiHandle g_widget_handle = reinterpret_cast<void*>(0x4000);
contracts::QolUiHandle g_button_handle = reinterpret_cast<void*>(0x5000);
contracts::UiNativeToggleDesc g_last_toggle{};
contracts::UiButtonDesc g_last_button{};
contracts::QolUiComponents g_components_api{};
contracts::QolUiAdapter g_ui_api{};
contracts::QolHostSdk g_sdk{};

QolResult fake_components_destroy(contracts::QolUiHandle h) {
    if (h == g_widget_handle) ++g_widget_destroys;
    if (h == g_button_handle) ++g_button_destroys;
    if (h == g_list_handle) ++g_list_destroys;
    return contracts::ok();
}
QolResult fake_delete_control(contracts::QolUiHandle) { return contracts::ok(); }
QolResult fake_list_create(const contracts::UiScrollListDesc*, contracts::QolUiHandle,
                           contracts::QolUiHandle* out) {
    *out = g_list_handle;
    return contracts::ok();
}
QolResult fake_list_get_row(contracts::QolUiHandle, uint32_t, contracts::QolUiHandle* out) {
    *out = g_row_handle;
    return contracts::ok();
}
QolResult fake_list_selection(contracts::QolUiHandle, uint8_t) { return contracts::ok(); }
QolResult fake_toggle_create(const contracts::UiNativeToggleDesc* desc, contracts::QolUiHandle,
                             contracts::QolUiHandle* out) {
    g_last_toggle = *desc;
    ++g_widget_creates;
    *out = g_widget_handle;
    return contracts::ok();
}
QolResult fake_toggle_set_value(contracts::QolUiHandle, int32_t) { return contracts::ok(); }
QolResult fake_toggle_set_enabled(contracts::QolUiHandle, uint8_t) { return contracts::ok(); }
QolResult fake_button_create(const contracts::UiButtonDesc* desc, contracts::QolUiHandle,
                             contracts::QolUiHandle* out) {
    g_last_button = *desc;
    ++g_button_creates;
    *out = g_button_handle;
    return contracts::ok();
}
QolResult fake_button_set_enabled(contracts::QolUiHandle, uint8_t) { return contracts::ok(); }
QolResult fake_require(const char* id, QolVersionRange, const void** out) {
    if (std::strcmp(id, contracts::kUiComponentsCapabilityId) != 0)
        return contracts::error(kQolErrNotFound, "no such capability", 18);
    *out = &g_components_api;
    return contracts::ok();
}

// Installs the production binding so g_ui/g_components/g_control resolve exactly as in
// production. Feature values are left untouched so callers can preset them.
void bind_production() {
    g_fake_control = {};
    g_fake_control.struct_size = sizeof(g_fake_control);
    g_fake_control.context = &g_log;
    g_fake_control.read_catalog = &fake_read_catalog;
    g_fake_control.read_desired = &fake_read_desired;
    g_fake_control.read_feature = &fake_read_feature;
    g_fake_control.request_set_feature = &fake_request_set_feature;
    g_fake_control.request_desired = &fake_request_desired;
    g_fake_control.receipt = &fake_receipt;
    g_fake_control.invoke_entry = &fake_invoke_entry;

    g_components_api = {};
    g_components_api.size = sizeof(g_components_api);
    g_components_api.destroy = &fake_components_destroy;
    g_components_api.scroll_list_create = &fake_list_create;
    g_components_api.scroll_list_get_row = &fake_list_get_row;
    g_components_api.scroll_list_set_selection_enabled = &fake_list_selection;
    g_components_api.native_toggle_create = &fake_toggle_create;
    g_components_api.native_toggle_set_value = &fake_toggle_set_value;
    g_components_api.native_toggle_set_enabled = &fake_toggle_set_enabled;
    g_components_api.text_button_create = &fake_button_create;
    g_components_api.text_button_set_enabled = &fake_button_set_enabled;

    g_ui_api = {};
    g_ui_api.delete_control = &fake_delete_control;

    g_sdk = {};
    g_sdk.abi.major = contracts::kHostSdkMajor;
    g_sdk.abi.minor = contracts::kHostSdkMinor;
    g_sdk.abi.size = sizeof(contracts::QolHostSdk);
    g_sdk.ui = &g_ui_api;
    g_sdk.capabilities.require = &fake_require;
    g_sdk.feature_control = &g_fake_control;

    settings_bind_sdk(&g_sdk, contracts::kQolOwnerTokenInvalid);
    g_root = g_fake_root;
    g_left = reinterpret_cast<void*>(0x6000);
    g_back = reinterpret_cast<void*>(0x7000);
    g_panel_active = false;
}

void setup() {
    reset_log();
    g_widget_creates = g_widget_destroys = g_button_creates = g_button_destroys = 0;
    g_list_destroys = 0;
    bind_production();
}

int find_desired(const char* plugin) {
    for (uint32_t i = 0; i < g_items.size(); ++i)
        if (g_items[i].desired && std::strcmp(g_items[i].plugin_id, plugin) == 0)
            return static_cast<int>(i);
    return -1;
}
int find_feature(const char* plugin, const char* feature) {
    for (uint32_t i = 0; i < g_items.size(); ++i)
        if (!g_items[i].desired && g_items[i].feature_id != nullptr &&
            std::strcmp(g_items[i].plugin_id, plugin) == 0 && std::strcmp(g_items[i].feature_id, feature) == 0)
            return static_cast<int>(i);
    return -1;
}

// ---- pure model checks (small UI model test) ----

void test_feature_model_filters_and_orders() {
    std::vector<feature_model::Item> items;
    assert(feature_model::build(kPlugins, 2, false, items));
    int hidden = 0, entry = 0, desired = 0;
    for (const auto& item : items) {
        if (item.category == FeatureCategory::kHidden) ++hidden;
        if (item.type == FeatureType::kEntry) ++entry;
        if (item.desired) ++desired;
    }
    assert(hidden == 0 && entry == 1 && desired == 2);  // hidden excluded when not debug

    assert(feature_model::build(kPlugins, 2, true, items));
    hidden = 0;
    for (const auto& item : items) if (item.category == FeatureCategory::kHidden) ++hidden;
    assert(hidden == 2);
    assert(!feature_model::build(nullptr, 1, true, items));  // malformed catalog rejected
}

// ---- production page behavior ----

void test_catalog_categories_and_debug_visibility() {
    setup();
    assert(load_catalog());
    assert(g_categories.size() == 4);
    assert(g_categories[0] == FeatureCategory::kGeneral);
    assert(g_categories[1] == FeatureCategory::kGameplay);
    assert(g_categories[2] == FeatureCategory::kTools);
    assert(g_categories[3] == FeatureCategory::kHidden);
    assert(g_items.size() == 6);
    assert(find_feature("autosell", "sell") >= 0);
}

void test_toggle_routes_to_host_request_set_feature() {
    setup();
    assert(load_catalog());
    refresh_all_values();
    const int index = find_feature("autosell", "sell");
    assert(index >= 0 && toggle_enabled(static_cast<uint32_t>(index)));

    g_states[index].gesture = true;
    g_states[index].desired = 0;
    process_pending_toggles();
    assert(g_log.set_feature_calls == 1 && g_log.plugin == "autosell" && g_log.feature == "sell");
    assert(g_log.value == 0 && g_states[index].request_id != 0);
    assert(!toggle_enabled(static_cast<uint32_t>(index)));

    g_log.receipt.persistence = FeaturePersistence::kPersisted;
    poll_requests();
    assert(g_states[index].request_id == 0);
    assert(toggle_enabled(static_cast<uint32_t>(index)));
}

void test_desired_toggle_writes_even_when_plugin_inactive() {
    setup();
    assert(load_catalog());
    g_read_feature_fails = true;
    refresh_all_values();
    const int index = find_desired("autosell");
    assert(index >= 0 && g_states[index].valid);
    assert(toggle_enabled(static_cast<uint32_t>(index)));

    g_states[index].gesture = true;
    g_states[index].desired = 0;
    process_pending_toggles();
    assert(g_log.desired_calls == 1 && g_log.plugin == "autosell" && g_log.value == 0);
}

void test_entry_button_requires_own_toggle_and_invokes_entry() {
    setup();
    assert(load_catalog());
    refresh_all_values();
    const int page = find_feature("settings", "page");
    assert(page >= 0);
    assert(g_items[page].type == FeatureType::kEntry);

    assert(build_right_list(2));  // Tools holds only the entry item
    assert(g_button_creates == 1);
    assert(std::strcmp(g_last_button.text, "打开") == 0 && g_last_button.on_click != nullptr);

    g_feature_values[0] = 0;
    refresh_item_value(static_cast<uint32_t>(page));
    assert(!action_enabled(static_cast<uint32_t>(page)));

    g_feature_values[0] = 1;
    refresh_item_value(static_cast<uint32_t>(page));
    assert(action_enabled(static_cast<uint32_t>(page)));

    g_pending_action_item = page;
    process_pending_action();
    assert(g_log.entry_calls == 1 && g_log.plugin == "settings" && g_log.feature == "page");
}

void test_deactivate_tears_down_page_and_reinstate_rebuilds() {
    setup();
    assert(load_catalog());
    assert(build_right_list(1));  // Gameplay creates a toggle child
    assert(g_widget_creates == 1);
    g_panel_active = true;
    const uint64_t generation_before = g_generation;

    // Lifecycle teardown: page destroyed, private controls/callbacks cleared, no residual UI.
    settings_bind_sdk(nullptr, contracts::kQolOwnerTokenInvalid);
    assert(g_root == nullptr && g_left == nullptr && g_back == nullptr);
    assert(!settings_provider_active());
    assert(g_items.empty() && g_categories.empty());
    assert(g_widget_destroys >= 1 || g_list_destroys >= 1);  // toggle child released with its list
    assert(!load_catalog());  // control channel released: no catalog without a live SDK

    // New lifecycle: fresh SDK + fresh catalog replays metadata/state into a new page.
    g_feature_values[0] = 0; g_feature_values[1] = 1;
    g_feature_values[2] = 0; g_feature_values[3] = 1;
    g_widget_creates = 0;
    bind_production();
    assert(load_catalog());
    assert(g_items.size() == 6);
    assert(g_generation != generation_before);  // new instance generation
    refresh_all_values();
    assert(g_states[static_cast<uint32_t>(find_feature("settings", "console"))].confirmed == 1);
}

}  // namespace

// Production feature.cpp owns these symbols; the UI test links only settings.cpp.
namespace qol::features::core_ui {
bool plugin_enabled() { return true; }
bool settings_panels_open(const char*) { return true; }
void settings_panels_close(const char*) {}
void more_games_bind_tick(bool, MoreGamesBindOps&) {}
}  // namespace qol::features::core_ui

int main() {
    test_feature_model_filters_and_orders();
    test_catalog_categories_and_debug_visibility();
    test_toggle_routes_to_host_request_set_feature();
    test_desired_toggle_writes_even_when_plugin_inactive();
    test_entry_button_requires_own_toggle_and_invokes_entry();
    test_deactivate_tears_down_page_and_reinstate_rebuilds();
    std::puts("settings_panel_widget_tests: feature-control UI path passed");
    return 0;
}
