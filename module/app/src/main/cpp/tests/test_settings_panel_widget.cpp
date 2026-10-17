// Focused UI test for the production settings page after the feature-control rewiring.
//
// The page no longer owns any config store/schema or legacy bool map. It renders the
// Host-owned typed feature catalog through QolFeatureControlApi and drives management
// writes with its own active callbacks. This file includes the production body so the
// real rendering/toggle/entry paths are exercised, not a copy.
#include "../plugins/settings/panels/settings.cpp"

#include <algorithm>
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
     "在主菜单打开模块设置页。", "打开设置"},
    {"console", 0, FeatureCategory::kHidden, FeatureType::kToggle, "调试控制台",
     "隐藏功能。"},
};
const contracts::QolFeatureCatalogItem kSellFeatures[]{
    {"sell", 1, FeatureCategory::kGameplay, FeatureType::kToggle, "自动出售",
     "控制自动出售扫描。"},
    {"diag", 0, FeatureCategory::kHidden, FeatureType::kToggle, "诊断日志",
     "隐藏诊断。"},
};
const contracts::QolFeatureCatalogItem kBasicOptimizationFeatures[]{
    {"ranges", 1, FeatureCategory::kBasicOptimization, FeatureType::kToggle,
     "属性范围显示", "显示属性作用范围。"},
    {"translation", 1, FeatureCategory::kBasicOptimization, FeatureType::kToggle,
     "角色属性汉化", "角色属性中文文案。"},
    {"map_jump", 1, FeatureCategory::kBasicOptimization, FeatureType::kToggle,
     "更好的地图传送对话框", "改善地图传送选择。"},
    {"drag_merge", 1, FeatureCategory::kBasicOptimization, FeatureType::kToggle,
     "拖拽合并", "拖拽同类物品时自动合并。"},
};
const contracts::QolFeatureCatalogPlugin kPlugins[]{
    {"settings", kSettingsFeatures, 2},
    {"autosell", kSellFeatures, 2},
    {"basic", kBasicOptimizationFeatures, 4},
};
const contracts::FeatureCategoryDefinition kCategories[]{
    {FeatureCategory::kGeneral, "基础设置", -8},
    {FeatureCategory::kBasicOptimization, "基础优化设置", -7},
    {FeatureCategory::kGameplay, "更多优化功能", -5},
    {FeatureCategory::kTools, "更多工具", -3},
    {FeatureCategory::kHidden, "调试开关", -10},
    {FeatureCategory::kGameInfo, "游戏信息", -9},
};

struct CallLog {
    std::string plugin;
    std::string feature;
    int set_feature_calls = 0;
    int desired_calls = 0;
    int read_desired_calls = 0;
    int value = -1;
    int entry_calls = 0;
    uint64_t next_request = 1;
    uint64_t last_request = 0;
    FeatureReceipt receipt{};
    int receipt_calls = 0;
    int read_actual_calls = 0;
};

CallLog g_log;
bool g_request_fails = false;
bool g_read_feature_fails = false;
bool g_read_actual_fails = false;
int g_actual_not_ready_remaining = 0;
uint8_t g_actual_enabled = 1;
uint8_t g_feature_values[8]{1, 0, 1, 0, 1, 1, 1, 1};
uint8_t g_desired_values[2]{1, 1};

int plugin_index(const char* id) { return std::strcmp(id, "settings") == 0 ? 0 : 1; }
int feature_index(const char* plugin, const char* feature) {
    if (std::strcmp(plugin, "settings") == 0) return std::strcmp(feature, "page") == 0 ? 0 : 1;
    if (std::strcmp(plugin, "basic") == 0) {
        if (std::strcmp(feature, "ranges") == 0) return 4;
        if (std::strcmp(feature, "translation") == 0) return 5;
        if (std::strcmp(feature, "map_jump") == 0) return 6;
        return 7;
    }
    return std::strcmp(feature, "sell") == 0 ? 2 : 3;
}

void reset_log() {
    g_log = {};
    g_request_fails = false;
    g_feature_values[0] = 1; g_feature_values[1] = 0;
    g_feature_values[2] = 1; g_feature_values[3] = 0;
    g_feature_values[4] = g_feature_values[5] = g_feature_values[6] = g_feature_values[7] = 1;
    g_desired_values[0] = 1; g_desired_values[1] = 1;
}

QolResult fake_read_catalog(void*, const contracts::QolFeatureCatalogPlugin** out, uint32_t* count) {
    *out = kPlugins;
    *count = 3;
    return contracts::ok();
}
QolResult fake_read_categories(void*, const contracts::FeatureCategoryDefinition** out,
                                uint32_t* count) {
    *out = kCategories;
    *count = 6;
    return contracts::ok();
}
QolResult fake_read_feature(void*, const char* plugin, const char* feature, uint8_t* out) {
    if (g_read_feature_fails) return contracts::error(kQolErrNotReady, "feature unavailable", 19);
    *out = g_feature_values[feature_index(plugin, feature)];
    return contracts::ok();
}
QolResult fake_read_desired(void*, const char* plugin, uint8_t* enable, uint8_t* any) {
    ++g_log.read_desired_calls;
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
    ++g_log.receipt_calls;
    *out = g_log.receipt;
    return contracts::ok();
}
QolResult fake_read_actual(void*, const char*, int32_t* state, uint8_t* enabled) {
    ++g_log.read_actual_calls;
    if (g_actual_not_ready_remaining > 0) {
        --g_actual_not_ready_remaining;
        return contracts::error(kQolErrNotReady, "actual not ready", 16);
    }
    if (g_read_actual_fails)
        return contracts::error(kQolErrInternal, "actual failed", 13);
    *state = 0x6f6f;  // The settings page must use only the explicit enabled result.
    *enabled = g_actual_enabled;
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
int g_button_enabled_calls = 0;
uint8_t g_button_enabled = 0;
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
contracts::QolGameHost g_game_host{};

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
QolResult fake_button_set_enabled(contracts::QolUiHandle, uint8_t enabled) {
    ++g_button_enabled_calls;
    g_button_enabled = enabled;
    return contracts::ok();
}
// 菜单工厂：测试只关心调用面与入参，替换控件由 fake 返回句柄。
QolResult fake_menu_button_create(const contracts::UiMainMenuDecoratedButtonDesc* desc,
                                  contracts::QolUiHandle* out_original,
                                  contracts::QolUiHandle* out_replacement) {
    if (desc == nullptr || out_original == nullptr || out_replacement == nullptr) {
        return contracts::error(kQolErrInvalidArgument, "invalid", 7);
    }
    *out_original = g_fake_root;
    *out_replacement = g_button_handle;
    return contracts::ok();
}
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
    g_fake_control.read_actual = &fake_read_actual;
    g_fake_control.read_categories = &fake_read_categories;

    g_components_api = {};
    g_components_api.size = sizeof(g_components_api);
    g_components_api.major = contracts::kUiContractMajor;
    g_components_api.minor = contracts::kUiContractMinor;
    g_components_api.capsule_text_button_create = &fake_button_create;
    g_components_api.main_menu_decorated_button_create = &fake_menu_button_create;
    g_components_api.destroy = &fake_components_destroy;
    g_components_api.scroll_list_create = &fake_list_create;
    g_components_api.scroll_list_get_row = &fake_list_get_row;
    g_components_api.scroll_list_set_selection_enabled = &fake_list_selection;
    g_components_api.native_toggle_create = &fake_toggle_create;
    g_components_api.native_toggle_set_value = &fake_toggle_set_value;
    g_components_api.native_toggle_set_enabled = &fake_toggle_set_enabled;
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
    g_read_feature_fails = false;
    g_read_actual_fails = false;
    g_actual_not_ready_remaining = 0;
    g_actual_enabled = 1;
    g_widget_creates = g_widget_destroys = g_button_creates = g_button_destroys = 0;
    g_list_destroys = 0;
    g_button_enabled_calls = 0;
    g_button_enabled = 0;
    settings_bind_game(nullptr);
    bind_production();
}

QolResult fake_read_game_identity(contracts::QolGameIdentity* out) {
    if (out == nullptr)
        return contracts::error(kQolErrInvalidArgument, "invalid argument", 16);
    *out = {"monster", "v29", "1.3.2", "com.example.inotia4", 1};
    return contracts::ok();
}

int find_feature(const char* plugin, const char* feature) {
    for (uint32_t i = 0; i < g_items.size(); ++i)
        if (g_items[i].feature_id != nullptr &&
            std::strcmp(g_items[i].plugin_id, plugin) == 0 && std::strcmp(g_items[i].feature_id, feature) == 0)
            return static_cast<int>(i);
    return -1;
}

int prepare_entry_page() {
    assert(load_catalog());
    refresh_all_values();
    const int page = find_feature("settings", "page");
    assert(page >= 0 && g_items[page].type == FeatureType::kEntry);
    const auto category = std::find_if(g_categories.begin(), g_categories.end(), [](const auto& c) {
        return c.category == FeatureCategory::kTools;
    });
    assert(category != g_categories.end());
    assert(build_right_list(static_cast<int>(category - g_categories.begin())));
    assert(g_button_creates == 1 && g_last_button.on_click != nullptr);
    g_panel_active = true;
    return page;
}

void click_entry_button() {
    assert(g_last_button.on_click != nullptr);
    g_last_button.on_click(g_last_button.user);
}

// ---- pure model checks (small UI model test) ----

void test_feature_model_filters_and_orders() {
    std::vector<feature_model::Item> items;
    assert(feature_model::build(kPlugins, 3, false, items));
    int hidden = 0, entry = 0;
    for (const auto& item : items) {
        if (item.category == FeatureCategory::kHidden) ++hidden;
        if (item.type == FeatureType::kEntry) ++entry;
    }
    assert(hidden == 0 && entry == 1 && items.size() == 6);
    std::vector<contracts::FeatureCategoryDefinition> categories;
    assert(feature_model::build_categories(kCategories, 6, false, items, categories));
    assert(categories.size() == 4);
    assert(categories[0].category == FeatureCategory::kGameInfo);
    assert(categories[1].category == FeatureCategory::kBasicOptimization);
    assert(categories[2].category == FeatureCategory::kGameplay);
    assert(categories[3].category == FeatureCategory::kTools);
    assert(std::strcmp(categories[0].name, "游戏信息") == 0);
    const char* expected_basic_labels[]{"属性范围显示", "角色属性汉化",
        "更好的地图传送对话框", "拖拽合并"};
    uint32_t basic_index = 0;
    for (const auto& item : items) {
        if (item.category != FeatureCategory::kBasicOptimization) continue;
        assert(basic_index < 4 && std::strcmp(item.label, expected_basic_labels[basic_index]) == 0);
        ++basic_index;
    }
    assert(basic_index == 4);
    const auto entry_item = std::find_if(items.begin(), items.end(), [](const auto& item) {
        return item.type == FeatureType::kEntry;
    });
    assert(entry_item != items.end() && std::strcmp(entry_item->action_label, "打开设置") == 0);
    assert(kRightRowH == 96);
    const auto no_description = settings_model::row_text_layout(false, kRightRowH);
    assert(no_description.title_y == (kRightRowH - settings_model::kTextGlyphHeight) / 2);

    assert(feature_model::build(kPlugins, 3, true, items));
    hidden = 0;
    for (const auto& item : items) if (item.category == FeatureCategory::kHidden) ++hidden;
    assert(hidden == 2 && items.size() == 8);  // feature rows only; no plugin-level rows
    assert(feature_model::build_categories(kCategories, 6, true, items, categories));
    assert(categories.size() == 5);
    assert(categories[0].category == FeatureCategory::kHidden);
    assert(categories[1].category == FeatureCategory::kGameInfo);
    assert(categories[2].category == FeatureCategory::kBasicOptimization);
    assert(categories[3].category == FeatureCategory::kGameplay);
    assert(categories[4].category == FeatureCategory::kTools);
    assert(!feature_model::build(nullptr, 1, true, items));  // malformed catalog rejected
}

// ---- production page behavior ----

void test_catalog_abi_rejected_before_read() {
    setup();
    g_sdk.abi.minor = 25;
    settings_bind_sdk(&g_sdk, contracts::kQolOwnerTokenInvalid);
    assert(g_control == nullptr && !load_catalog());
    g_sdk.abi.minor = contracts::kHostSdkMinor;
    g_fake_control.struct_size = offsetof(contracts::QolFeatureControlApi, read_categories);
    settings_bind_sdk(&g_sdk, contracts::kQolOwnerTokenInvalid);
    assert(g_control == nullptr && !load_catalog());
    g_fake_control.struct_size = sizeof(g_fake_control);
    g_fake_control.read_categories = nullptr;
    settings_bind_sdk(&g_sdk, contracts::kQolOwnerTokenInvalid);
    assert(g_control == nullptr && !load_catalog());
    bind_production();
    assert(load_catalog());
}

void test_catalog_categories_and_debug_visibility() {
    setup();
    assert(load_catalog());
    assert(g_categories.size() == 5);
    assert(g_categories[0].category == FeatureCategory::kHidden);
    assert(g_categories[1].category == FeatureCategory::kGameInfo);
    assert(g_categories[2].category == FeatureCategory::kBasicOptimization);
    assert(g_categories[3].category == FeatureCategory::kGameplay);
    assert(g_categories[4].category == FeatureCategory::kTools);
    assert(g_items.size() == 8);
    assert(find_feature("autosell", "sell") >= 0);
}

void test_game_info_category_builds_read_only_composition() {
    setup();
    g_game_host = {};
    g_game_host.abi = {sizeof(g_game_host), contracts::kGameHostMajor, 4};
    g_game_host.game_identity.read = &fake_read_game_identity;
    settings_bind_game(&g_game_host);
    std::snprintf(g_module_version, sizeof(g_module_version), "9.8.7");
    assert(load_catalog());
    const auto category = std::find_if(g_categories.begin(), g_categories.end(), [](const auto& c) {
        return c.category == FeatureCategory::kGameInfo;
    });
    assert(category != g_categories.end());
    assert(build_right_list(static_cast<int>(category - g_categories.begin())));
    assert(g_selected_is_game_info && g_right == nullptr && g_right_indices.empty());
    assert(g_game_info.game == "艾诺迪亚4 v1.3.2");
    assert(g_game_info.variant == "Monster版 v29");
    assert(g_game_info.module == "QoL模块 v9.8.7");
    assert(g_widget_creates == 0 && g_button_creates == 0);
    destroy_right_list();

    // Without a Game Host the same category still builds and shows explicit fallbacks.
    settings_bind_game(nullptr);
    assert(build_right_list(static_cast<int>(category - g_categories.begin())));
    assert(g_selected_is_game_info && g_right == nullptr);
    assert(g_game_info.game == "艾诺迪亚4");
    assert(g_game_info.variant == "未知版本");
    assert(g_game_info.module == "QoL模块 v9.8.7");
    destroy_right_list();
}

void test_page_consumes_host_priorities_and_titles() {
    setup();
    static const contracts::QolFeatureCatalogItem first[]{
        {"late", 1, FeatureCategory::kGeneral, FeatureType::kToggle, "Late", "", nullptr, 9},
        {"same.first", 1, FeatureCategory::kGeneral, FeatureType::kToggle, "First", "", nullptr, -2},
        {"tools", 1, FeatureCategory::kTools, FeatureType::kToggle, "Tools", ""},
    };
    static const contracts::QolFeatureCatalogItem second[]{
        {"early", 1, FeatureCategory::kGeneral, FeatureType::kToggle, "Early", "", nullptr, -3},
        {"same.second", 1, FeatureCategory::kGeneral, FeatureType::kToggle, "Second", "", nullptr, -2},
    };
    static const contracts::QolFeatureCatalogPlugin plugins[]{
        {"first", first, 3}, {"second", second, 2},
    };
    static const contracts::FeatureCategoryDefinition categories[]{
        {FeatureCategory::kGeneral, "Host renamed general", 8},
        {FeatureCategory::kTools, "Host tools first", -8},
        {FeatureCategory::kGameplay, "Empty category", -10},
    };
    g_fake_control.read_catalog = [](void*, const contracts::QolFeatureCatalogPlugin** out,
                                     uint32_t* count) {
        *out = plugins; *count = 2; return contracts::ok();
    };
    g_fake_control.read_categories = [](void*, const contracts::FeatureCategoryDefinition** out,
                                        uint32_t* count) {
        *out = categories; *count = 3; return contracts::ok();
    };
    assert(load_catalog());
    assert(g_categories.size() == 2);
    assert(g_categories[0].category == FeatureCategory::kTools);
    assert(std::strcmp(g_categories[1].name, "Host renamed general") == 0);
    assert(build_right_list(1));
    const char* expected[]{"early", "same.first", "same.second", "late"};
    assert(g_right_indices.size() == 4);
    for (size_t i = 0; i < g_right_indices.size(); ++i)
        assert(std::strcmp(g_items[g_right_indices[i]].feature_id, expected[i]) == 0);
    destroy_page();
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

void test_feature_toggle_writes_when_plugin_desired_disabled() {
    setup();
    assert(load_catalog());
    g_desired_values[1] = 0;
    refresh_all_values();
    const int index = find_feature("autosell", "sell");
    assert(index >= 0 && g_states[index].valid);
    assert(toggle_enabled(static_cast<uint32_t>(index)));

    g_states[index].gesture = true;
    g_states[index].desired = 0;
    process_pending_toggles();
    assert(g_log.set_feature_calls == 1 && g_log.desired_calls == 0 &&
           g_log.read_desired_calls == 0 && g_log.plugin == "autosell" &&
           g_log.feature == "sell" && g_log.value == 0);
}

void test_entry_button_respects_validity_and_direct_invocation() {
    setup();
    const int page = prepare_entry_page();
    assert(g_widget_creates == 0);
    assert(std::strcmp(g_last_button.text, "打开设置") == 0);
    g_feature_values[0] = 0;
    refresh_item_value(static_cast<uint32_t>(page));
    assert(action_enabled(static_cast<uint32_t>(page)));
    g_feature_values[0] = 1;
    refresh_item_value(static_cast<uint32_t>(page));
    assert(action_enabled(static_cast<uint32_t>(page)));

    g_actual_enabled = 0;
    click_entry_button();
    process_pending_action();
    assert(g_log.entry_calls == 0 && g_log.set_feature_calls == 0 &&
           g_log.read_actual_calls == 1 && g_status[0] != '\0');

    g_actual_enabled = 1;
    g_actual_not_ready_remaining = 1;
    click_entry_button();
    process_pending_action();
    assert(g_log.entry_calls == 0 && g_log.set_feature_calls == 0 &&
           g_log.read_actual_calls == 2 && !g_entry_continuation.active &&
           g_status[0] != '\0');

    click_entry_button();
    process_pending_action();
    assert(g_log.entry_calls == 1 && g_log.plugin == "settings" && g_log.feature == "page");
    assert(g_log.set_feature_calls == 0 && g_log.desired_calls == 0 &&
           g_log.read_actual_calls == 3 && g_status[0] == '\0');
}

void test_entry_false_enables_and_waits_for_persistence_and_actual() {
    setup();
    const int page = prepare_entry_page();
    g_feature_values[0] = 0;
    refresh_item_value(static_cast<uint32_t>(page));
    assert(action_enabled(static_cast<uint32_t>(page)));

    click_entry_button();
    click_entry_button();  // A queued click cannot enqueue a second write.
    assert(g_pending_action_item == page && g_log.set_feature_calls == 0);
    process_pending_action();
    assert(g_log.set_feature_calls == 1 && g_log.value == 1 &&
           g_log.plugin == "settings" && g_log.feature == "page");
    assert(g_log.entry_calls == 0 && g_feature_values[0] == 0);
    assert(g_entry_continuation.active && g_entry_continuation.request_id == g_log.last_request);
    assert(!action_enabled(static_cast<uint32_t>(page)) && g_button_enabled == 0);
    click_entry_button();  // The admitted continuation also blocks duplicate clicks.
    assert(g_pending_action_item == -1 && g_log.set_feature_calls == 1);

    const int receipts_before = g_log.receipt_calls;
    poll_entry_continuation();
    assert(g_entry_continuation.active && g_log.receipt_calls == receipts_before + 1 &&
           g_log.entry_calls == 0);
    poll_entry_continuation();
    assert(g_entry_continuation.active && g_log.receipt_calls == receipts_before + 2 &&
           g_log.entry_calls == 0);

    g_log.receipt.persistence = FeaturePersistence::kPersisted;
    g_feature_values[0] = 1;
    g_actual_not_ready_remaining = 1;
    g_actual_enabled = 0;
    poll_entry_continuation();
    assert(g_entry_continuation.active && g_entry_continuation.persisted &&
           g_log.receipt_calls == receipts_before + 3 && g_log.entry_calls == 0);

    const int persisted_receipts = g_log.receipt_calls;
    poll_entry_continuation();  // Host actual state is readable but not enabled yet.
    assert(g_entry_continuation.active && g_log.receipt_calls == persisted_receipts &&
           g_log.entry_calls == 0);
    g_actual_enabled = 1;
    poll_entry_continuation();
    assert(!g_entry_continuation.active && g_log.receipt_calls == persisted_receipts &&
           g_log.entry_calls == 1 && g_log.set_feature_calls == 1);
    poll_entry_continuation();
    assert(g_log.entry_calls == 1);
}

void test_entry_persistence_failure_stops_without_invoking() {
    setup();
    const int page = prepare_entry_page();
    g_feature_values[0] = 0;
    refresh_item_value(static_cast<uint32_t>(page));
    click_entry_button();
    process_pending_action();
    g_log.receipt.persistence = FeaturePersistence::kFailed;
    g_log.receipt.error_code = kQolErrInternal;
    poll_entry_continuation();
    assert(!g_entry_continuation.active && g_log.entry_calls == 0 &&
           g_log.set_feature_calls == 1 && g_feature_values[0] == 0 && g_status[0] != '\0');
}

void test_entry_read_error_stops_without_invoking() {
    setup();
    const int page = prepare_entry_page();
    g_feature_values[0] = 0;
    refresh_item_value(static_cast<uint32_t>(page));
    click_entry_button();
    process_pending_action();
    g_log.receipt.persistence = FeaturePersistence::kPersisted;
    g_feature_values[0] = 1;
    g_read_feature_fails = true;
    poll_entry_continuation();
    assert(!g_entry_continuation.active && g_log.entry_calls == 0 && g_status[0] != '\0');
}

void test_entry_wait_timeout_stops_without_rollback_or_invoke() {
    setup();
    const int page = prepare_entry_page();
    g_feature_values[0] = 0;
    refresh_item_value(static_cast<uint32_t>(page));
    click_entry_button();
    process_pending_action();
    g_log.receipt.persistence = FeaturePersistence::kPersisted;
    g_feature_values[0] = 1;
    g_actual_enabled = 0;
    for (uint32_t tick = 0; tick < kEntryWaitMaxProcessTicks; ++tick)
        poll_entry_continuation();
    assert(!g_entry_continuation.active && g_entry_continuation.process_ticks == 0 &&
           g_log.entry_calls == 0 && g_log.set_feature_calls == 1 &&
           g_feature_values[0] == 1 && g_status[0] != '\0');
    assert(g_log.receipt_calls == 1 &&
           g_log.read_actual_calls == static_cast<int>(kEntryWaitMaxProcessTicks));
}

void test_entry_close_cancels_continuation_without_rollback() {
    setup();
    const int page = prepare_entry_page();
    g_feature_values[0] = 0;
    refresh_item_value(static_cast<uint32_t>(page));
    click_entry_button();
    process_pending_action();
    g_log.receipt.persistence = FeaturePersistence::kPersisted;
    g_feature_values[0] = 1;
    g_actual_enabled = 0;
    poll_entry_continuation();
    assert(g_entry_continuation.active && g_log.entry_calls == 0);

    destroy_page();
    g_actual_enabled = 1;
    poll_entry_continuation();
    assert(!g_entry_continuation.active && g_log.entry_calls == 0 &&
           g_log.set_feature_calls == 1 && g_feature_values[0] == 1);
}

void test_entry_requires_read_actual_tail_before_enabling_write() {
    setup();
    const int page = prepare_entry_page();
    g_feature_values[0] = 0;
    refresh_item_value(static_cast<uint32_t>(page));
    g_fake_control.struct_size = static_cast<uint32_t>(
        offsetof(contracts::QolFeatureControlApi, read_actual) +
        sizeof(g_fake_control.read_actual) - 1);
    click_entry_button();
    process_pending_action();
    assert(g_log.set_feature_calls == 0 && g_log.read_actual_calls == 0 &&
           g_log.entry_calls == 0 && !g_entry_continuation.active && g_status[0] != '\0');
}

void test_entry_requires_read_actual_function_before_enabling_write() {
    setup();
    const int page = prepare_entry_page();
    g_feature_values[0] = 0;
    refresh_item_value(static_cast<uint32_t>(page));
    g_fake_control.read_actual = nullptr;
    click_entry_button();
    process_pending_action();
    assert(g_log.set_feature_calls == 0 && g_log.read_actual_calls == 0 &&
           g_log.entry_calls == 0 && !g_entry_continuation.active && g_status[0] != '\0');
}

void test_true_entry_requires_read_actual_abi_before_invoking() {
    setup();
    prepare_entry_page();
    g_fake_control.struct_size = static_cast<uint32_t>(
        offsetof(contracts::QolFeatureControlApi, read_actual) +
        sizeof(g_fake_control.read_actual) - 1);
    click_entry_button();
    process_pending_action();
    assert(g_log.read_actual_calls == 0 && g_log.entry_calls == 0 &&
           g_log.set_feature_calls == 0 && g_status[0] != '\0');

    setup();
    prepare_entry_page();
    g_fake_control.read_actual = nullptr;
    click_entry_button();
    process_pending_action();
    assert(g_log.read_actual_calls == 0 && g_log.entry_calls == 0 &&
           g_log.set_feature_calls == 0 && g_status[0] != '\0');
}

void test_deactivate_tears_down_page_and_reinstate_rebuilds() {
    setup();
    assert(load_catalog());
    assert(build_right_list(2));  // Basic optimization creates four toggle children
    assert(g_widget_creates == 4);
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
    assert(g_items.size() == 8);
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
bool more_games_disable_original(MoreGamesOriginalStateOps&, bool) { return false; }
bool more_games_restore_original(bool, bool, MoreGamesOriginalStateOps&) { return false; }
bool more_games_draw(bool, MoreGamesDrawOps&) { return false; }
}  // namespace qol::features::core_ui

int main() {
    test_catalog_abi_rejected_before_read();
    test_page_consumes_host_priorities_and_titles();
    test_feature_model_filters_and_orders();
    test_catalog_categories_and_debug_visibility();
    test_game_info_category_builds_read_only_composition();
    test_toggle_routes_to_host_request_set_feature();
    test_feature_toggle_writes_when_plugin_desired_disabled();
    test_entry_button_respects_validity_and_direct_invocation();
    test_entry_false_enables_and_waits_for_persistence_and_actual();
    test_entry_persistence_failure_stops_without_invoking();
    test_entry_read_error_stops_without_invoking();
    test_entry_wait_timeout_stops_without_rollback_or_invoke();
    test_entry_close_cancels_continuation_without_rollback();
    test_entry_requires_read_actual_tail_before_enabling_write();
    test_entry_requires_read_actual_function_before_enabling_write();
    test_true_entry_requires_read_actual_abi_before_invoking();
    test_deactivate_tears_down_page_and_reinstate_rebuilds();
    std::puts("settings_panel_widget_tests: feature-control UI path passed");
    return 0;
}
