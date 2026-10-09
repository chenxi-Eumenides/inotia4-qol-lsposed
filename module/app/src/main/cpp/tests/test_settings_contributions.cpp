#include "contracts/host_sdk.h"
#include "contracts/ui.h"
#include "host/settings_catalog_adapter.h"
#include "infra/settings_catalog.h"
#include "infra/status.h"
#include "plugins/autosell/settings_contribution.h"
#include "plugins/core.settings/settings_contribution.h"
#include "plugins/core.custom_ui/settings_contribution.h"
#include "plugins/core.custom_ui/panels/stack.h"
#include "plugins/custom_recipe/settings_contribution.h"
#include "plugins/extension_bag/settings_contribution.h"
#include "plugins/move_merge/settings_contribution.h"
#include "plugins/push_all/settings_contribution.h"
#include "plugins/save_backup/settings_contribution.h"
#include "plugins/simple_mode/settings_contribution.h"
#include "plugins/stack_limit/settings_contribution.h"

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <thread>

namespace {
using namespace qol::contracts;
using qol::infra::FeatureState;
using RegisterFn = QolResult (*)(const QolHostSdk*);
constexpr RegisterFn kContributions[]{
    &qol::features::save_backup::register_settings,
    &qol::features::core_settings::register_settings,
    &qol::features::extension_bag::register_settings,
    &qol::features::autosell::register_settings,
    &qol::features::custom_recipe::register_settings,
    &qol::features::stack_limit::register_settings,
    &qol::features::push_all::register_settings,
    &qol::features::move_merge::register_settings,
    &qol::features::simple_mode::register_settings,
    &qol::features::core_ui::register_settings,
};
const auto kMainThread = std::this_thread::get_id();
int g_register_calls = 0;
int g_find_calls = 0;
int g_require_calls = 0;
int g_open_calls = 0;
bool g_provider_present = true;
bool g_debug_panel_present = true;
bool g_capability_present = true;
bool g_open_succeeds = true;
bool g_null_interface = false;
const char* g_reason = nullptr;
const char* g_after_open_reason = nullptr;
const char* g_last_panel = nullptr;
bool g_debug_active = false;
bool g_debug_open_fails = false;
int g_registry_open_calls = 0;
const QolHostSdk* g_test_sdk = nullptr;

QolResult forbidden_get(const char*, int32_t, int32_t*) {
    assert(false && "static declaration/action must not read config");
    return error(kQolErrInternal, "forbidden config read", 21);
}
QolResult forbidden_set(const char*, int32_t) {
    assert(false && "contribution must never enable features or write config");
    return error(kQolErrInternal, "forbidden config write", 22);
}
QolResult forbidden_open(const char*) {
    assert(false && "must open through ui_panels capability, not SDK provider shortcut");
    return error(kQolErrInternal, "forbidden panel shortcut", 24);
}
bool forbidden_provider_open() {
    assert(false && "provider open must be owned by the shared navigation stack");
    return false;
}
const char* block_reason() { return g_reason; }
const PanelProviderSpec kAutosellProvider{
    "autosell", "autosell", &forbidden_provider_open, nullptr, nullptr, &block_reason, nullptr};
const PanelProviderSpec kBackupProvider{
    "save_backup", "save_backup", &forbidden_provider_open, nullptr, nullptr, &block_reason, nullptr};
bool debug_provider_open() {
    if (g_debug_open_fails) return false;
    g_debug_active = true;
    return true;
}
void debug_provider_close() { g_debug_active = false; }
bool debug_provider_active() { return g_debug_active; }
QolResult find(const char* id, const PanelProviderSpec** out) {
    ++g_find_calls;
    *out = nullptr;
    if (!g_provider_present) return error(kQolErrNotFound, "provider missing", 16);
    if (std::strcmp(id, "autosell") == 0) *out = &kAutosellProvider;
    if (std::strcmp(id, "save_backup") == 0) *out = &kBackupProvider;
    static const PanelProviderSpec debug_provider{
        "debug_components", "core.custom_ui", &debug_provider_open, &debug_provider_close,
        &debug_provider_active, nullptr, nullptr};
    if (std::strcmp(id, "debug_components") == 0 && g_debug_panel_present) *out = &debug_provider;
    return *out != nullptr ? ok() : error(kQolErrNotFound, "unknown provider", 16);
}
QolResult registry_panel_open(const char* id) {
    ++g_registry_open_calls;
    g_last_panel = id;
    return ok();
}
bool stack_open(const char* id) {
    ++g_open_calls;
    g_last_panel = id;
    g_reason = g_after_open_reason;
    return g_open_succeeds;
}
UiPanelsApi g_panels{&stack_open, nullptr, nullptr, nullptr, nullptr, nullptr};
QolResult require(const char* id, QolVersionRange range, const void** out) {
    ++g_require_calls;
    if (std::strcmp(id, kUiStateCapabilityId) == 0) {
        *out = nullptr;
        return error(kQolErrNotReady, "ui.state unavailable", 20);
    }
    assert(std::strcmp(id, kUiPanelsCapabilityId) == 0);
    assert(range.major == kUiPanelsVersionMajor && range.min_minor == kUiPanelsVersionMinor);
    *out = nullptr;
    if (!g_capability_present) return error(kQolErrNotReady, "ui_panels unavailable", 21);
    if (!g_null_interface) *out = &g_panels;
    return ok();
}
QolResult register_category(const SettingsCategorySpec* category,
                            const SettingsItemSpec* items, uint32_t count) {
    ++g_register_calls;
    return qol::infra::settings_catalog_register_category(category, items, count);
}
QolHostSdk make_sdk() {
    QolHostSdk sdk{};
    sdk.abi = {sizeof(sdk), kHostSdkMajor, kHostSdkMinor};
    static QolSettingsCatalogApi api = qol::host::settings_catalog_api();
    api.register_category = &register_category;
    sdk.settings_catalog = &api;
    sdk.config.get_bool = &forbidden_get;
    sdk.config.set_bool = &forbidden_set;
    sdk.panels.find = &find;
    sdk.panels.open = &registry_panel_open;
    sdk.capabilities.require = &require;
    return sdk;
}

struct View {
    const CatalogView* data = nullptr;
    SettingsCatalogToken token = 0;
    View() { assert(qol::infra::settings_catalog_snapshot_acquire(&data, &token).ok == kQolOk); }
    ~View() { assert(qol::infra::settings_catalog_snapshot_release(token).ok == kQolOk); }
};

void test_optional_sdk_tail() {
    alignas(QolHostSdk) std::array<unsigned char, offsetof(QolHostSdk, settings_catalog)> prefix{};
    // 真正短于新 SDK 的存储；不得在检查 size 之前读取尾指针。
    QolAbiHeader old{static_cast<uint32_t>(prefix.size()), kHostSdkMajor, 15};
    std::memcpy(prefix.data(), &old, sizeof(old));
    const auto* old_sdk = reinterpret_cast<const QolHostSdk*>(prefix.data());
    for (auto contribution : kContributions) {
        assert(contribution(nullptr).code == kQolErrUnsupported);
        assert(contribution(old_sdk).code == kQolErrUnsupported);
    }
    auto sdk = make_sdk();
    sdk.abi.size = prefix.size();
    for (auto contribution : kContributions) assert(contribution(&sdk).code == kQolErrUnsupported);
    sdk = make_sdk();
    sdk.settings_catalog = nullptr;
    for (auto contribution : kContributions) assert(contribution(&sdk).code == kQolErrUnsupported);
    sdk = make_sdk();
    auto bad_api = *sdk.settings_catalog;
    bad_api.register_category = nullptr;
    sdk.settings_catalog = &bad_api;
    for (auto contribution : kContributions) assert(contribution(&sdk).code == kQolErrUnsupported);
    assert(g_register_calls == 0 && View{}.data->item_count == 0);

    sdk = make_sdk();
    auto old_catalog = *sdk.settings_catalog;
    old_catalog.size = offsetof(QolSettingsCatalogApi, register_category_metadata);
    old_catalog.minor = 2;
    sdk.settings_catalog = &old_catalog;
    assert(settings_catalog_metadata_if_available(&sdk) == nullptr);
    assert(qol::features::core_ui::register_settings_category(&sdk).code == kQolErrUnsupported);
    assert(qol::features::core_settings::register_categories(&sdk).code == kQolErrUnsupported);
    assert(qol::features::core_settings::register_settings(&sdk).code == kQolErrNotFound);
    assert(View{}.data->category_count == 0 && View{}.data->item_count == 0);
}

void test_static_declarations_and_public_keys(const QolHostSdk& sdk) {
    constexpr const char* owners[]{"core.settings", "core.custom_ui", "save_backup", "extension_bag", "autosell",
                                   "custom_recipe", "stack_limit", "push_all", "move_merge", "simple_mode"};
    for (const char* owner : owners) {
        assert(qol::infra::status_register(owner, "test"));
        assert(qol::infra::status_update(owner, FeatureState::kInstalledDisabled, false, nullptr));
    }
    assert(qol::features::core_settings::register_categories(&sdk).ok == kQolOk);
    assert(qol::features::core_ui::register_settings_category(&sdk).ok == kQolOk);
    View empty_bootstrap;
    assert(empty_bootstrap.data->category_count == 3 && empty_bootstrap.data->item_count == 0);
    assert(std::strcmp(empty_bootstrap.data->categories[0].title, "基础设置") == 0);
    assert(std::strcmp(empty_bootstrap.data->categories[1].title, "优化功能") == 0);
    assert(std::strcmp(empty_bootstrap.data->categories[2].id, "debug") == 0);
    assert(std::strcmp(empty_bootstrap.data->categories[2].title, "调试") == 0);
    for (auto contribution : kContributions) {
        const auto result = contribution(&sdk);
        if (result.ok != kQolOk) std::fprintf(stderr, "settings contribution failed: %.*s\n",
                                              static_cast<int>(result.message_len), result.message);
        assert(result.ok == kQolOk);
    }
    assert(g_register_calls == 0);
    assert(g_find_calls == 0 && g_require_calls == 0 && g_open_calls == 0);
    View view;
    assert(view.data->category_count == 3 && view.data->item_count == 12);
    for (uint32_t i = 0; i < view.data->category_count; ++i) {
        const auto& category = view.data->categories[i];
        assert(std::strcmp(category.id, i == 0 ? kSettingsBasicCategoryId :
                            i == 1 ? kSettingsOptimizationCategoryId : "debug") == 0);
        assert(category.title[0] != '\0' && category.description[0] != '\0');
    }
    struct Expected {
        const char* id;
        const char* owner;
        const char* label;
        const char* key;
        int32_t owner_enable;
    };
    constexpr Expected expected[]{
        {"save_backup.open_manager", "save_backup", "存档备份导出", nullptr, 0},
        {"core.settings.apiEnabled", "core.settings", "API 服务", "apiEnabled", 1},
        {"core.settings.opEnabled", "core.settings", "API 服务 OP 操作", "opEnabled", 1},
        {"core.settings.debugLogEnabled", "core.settings", "调试日志", "debugLogEnabled", 1},
        {"extension_bag.extensionBagEnabled", "extension_bag", "扩展背包", "extensionBagEnabled", 1},
        {"autosell.autoSellEnabled", "autosell", "自动出售", "autoSellEnabled", 1},
        {"custom_recipe.customRecipeEnabled", "custom_recipe", "自定义合成", "customRecipeEnabled", 1},
        {"stack_limit.stackLimitIncrease", "stack_limit", "堆叠上限", "stackLimitIncrease", 1},
        {"push_all.pushAllEnabled", "push_all", "推怪", "pushAllEnabled", 1},
        {"move_merge.moveMergeEnabled", "move_merge", "拖拽合并", "moveMergeEnabled", 1},
        {"simple_mode.simpleModeEnabled", "simple_mode", "简单模式", "simpleModeEnabled", 1},
        {"core.custom_ui.open_debug_components", "core.custom_ui", "打开调试面板", nullptr, 0},
    };
    for (uint32_t i = 0; i < view.data->item_count; ++i) {
        const auto& item = view.data->items[i];
        const auto& want = expected[i];
        assert(std::strcmp(item.id, want.id) == 0 && std::strcmp(item.owner, want.owner) == 0);
        assert(std::strcmp(item.category_id, i < 4 ? kSettingsBasicCategoryId :
                            i < 11 ? kSettingsOptimizationCategoryId : "debug") == 0 &&
               item.owner_enable == want.owner_enable);
        assert(std::strcmp(item.label, want.label) == 0 && item.description[0] != '\0');
        if (want.key != nullptr) {
            assert(item.type == SettingsItemType::kToggle && std::strcmp(item.config_key, want.key) == 0);
        } else {
            assert(item.type == SettingsItemType::kAction && item.config_key[0] == '\0');
            assert(std::strcmp(item.id, i == 11 ? "core.custom_ui.open_debug_components" :
                                "save_backup.open_manager") == 0);
        }
    }
    // 功能关态也保留声明和自身 enable；重复装配贡献不改变 generation。
    const auto generation = view.data->generation;
    for (std::size_t i = 1; i < std::size(kContributions); ++i) {
        if (i == 2) continue;
        const auto contribution = kContributions[i];
        const auto result = contribution(&sdk);
        if (result.ok != kQolOk) std::fprintf(stderr, "repeat settings contribution failed: %.*s\n",
                                              static_cast<int>(result.message_len), result.message);
        assert(result.ok == kQolOk);
    }
    View again;
    assert(again.data->generation == generation && again.data->item_count == 12);
    assert(g_find_calls == 0 && g_require_calls == 0 && g_open_calls == 0);
}

void test_action_admission_and_capability_navigation() {
    const auto& api = qol::host::settings_catalog_api();
    View view;
    const auto generation = view.data->generation;
    for (const auto* action : {"save_backup.open_manager"}) {
        assert(api.invoke_action(action, generation).code == kQolErrNotReady);
    }
    assert(g_find_calls == 0 && g_require_calls == 0 && g_open_calls == 0);
    for (const auto* owner : {"save_backup"}) {
        assert(qol::infra::status_update(owner, FeatureState::kEnabled, true, nullptr));
    }
    constexpr const char* actions[]{"save_backup.open_manager"};
    constexpr const char* panels[]{"save_backup"};
    for (uint32_t i = 0; i < 1; ++i) {
        const int before = g_open_calls;
        g_provider_present = false;
        assert(api.invoke_action(actions[i], generation).code == kQolErrNotFound);
        g_provider_present = true;
        g_reason = "provider rejects";
        const auto rejected = api.invoke_action(actions[i], generation);
        assert(rejected.code == kQolErrBlocked && rejected.message_len == 16);
        assert(std::memcmp(rejected.message, "provider rejects", 16) == 0);
        g_reason = nullptr;
        g_capability_present = false;
        assert(api.invoke_action(actions[i], generation).code == kQolErrNotReady);
        g_capability_present = true;
        g_null_interface = true;
        assert(api.invoke_action(actions[i], generation).code == kQolErrNotReady);
        g_null_interface = false;
        g_panels.open = nullptr;
        assert(api.invoke_action(actions[i], generation).code == kQolErrUnsupported);
        g_panels.open = &stack_open;
        assert(g_open_calls == before);
        g_open_succeeds = false;
        auto failed = api.invoke_action(actions[i], generation);
        assert(failed.code == kQolErrBlocked && failed.message != nullptr && failed.message_len > 0);
        g_after_open_reason = "provider became blocked";
        failed = api.invoke_action(actions[i], generation);
        assert(failed.code == kQolErrBlocked && failed.message_len == 23);
        assert(std::memcmp(failed.message, "provider became blocked", 23) == 0);
        g_after_open_reason = g_reason = nullptr;
        g_open_succeeds = true;
        assert(api.invoke_action(actions[i], generation).ok == kQolOk);
        assert(std::strcmp(g_last_panel, panels[i]) == 0);
        assert(g_open_calls == before + 3);
    }
    // 停用不注销静态内容；owner gate 拒绝在 provider 查询之前完成。
    assert(qol::infra::status_update("save_backup", FeatureState::kInactive, false, nullptr));
    const int queries = g_find_calls;
    assert(api.invoke_action(actions[0], generation).code == kQolErrNotReady);
    assert(g_find_calls == queries && View{}.data->item_count == 12);
    assert(api.invoke_action(actions[0], generation - 1).code == kQolErrConflict);
    std::thread worker([&] { assert(api.invoke_action(actions[0], generation).code == kQolErrNotReady); });
    worker.join();
    qol::infra::FeatureStatus extension{};
    assert(qol::infra::status_get("extension_bag", &extension));
    assert(!extension.enabled && extension.state == FeatureState::kInstalledDisabled);
}

void test_debug_panel_action_and_owner_removal() {
    const auto& api = qol::host::settings_catalog_api();
    View view;
    const uint64_t generation = view.data->generation;
    assert(api.invoke_action("core.custom_ui.open_debug_components", generation).code ==
           kQolErrNotReady);
    assert(qol::infra::status_update("core.custom_ui", FeatureState::kEnabled, true, nullptr));
    g_debug_panel_present = false;
    const int before = g_registry_open_calls;
    const auto unavailable = api.invoke_action("core.custom_ui.open_debug_components", generation);
    assert(unavailable.code == kQolErrBlocked);
    assert(g_registry_open_calls == before);
    g_debug_panel_present = true;
    qol::features::core_ui::ui_panels_bind_sdk(g_test_sdk);
    const int depth_before = qol::features::core_ui::ui_panels_depth_for_test();
    assert(api.invoke_action("core.custom_ui.open_debug_components", generation).ok == kQolOk);
    assert(g_registry_open_calls == before);
    assert(g_debug_active && qol::features::core_ui::ui_panels_depth_for_test() == depth_before + 1);
    assert(qol::features::core_ui::ui_panels_active("debug_components"));
    qol::features::core_ui::ui_panels_frame_pump(1, nullptr);
    qol::features::core_ui::ui_panels_request_back();
    qol::features::core_ui::ui_panels_frame_pump(2, nullptr);
    assert(!g_debug_active && qol::features::core_ui::ui_panels_depth_for_test() == depth_before);
    // 导航失败必须如实返回受阻，不得伪报成功；注册表 open 亦不得被当作导航证据。
    g_debug_open_fails = true;
    const auto nav_failed = api.invoke_action("core.custom_ui.open_debug_components", generation);
    assert(nav_failed.code == kQolErrBlocked);
    assert(g_registry_open_calls == before);
    assert(!g_debug_active && qol::features::core_ui::ui_panels_depth_for_test() == depth_before);
    g_debug_open_fails = false;

    assert(api.unregister_owner("core.custom_ui").ok == kQolOk);
    View remaining;
    assert(remaining.data->category_count == 2 && remaining.data->item_count == 11);
    for (uint32_t i = 0; i < remaining.data->item_count; ++i) {
        assert(std::strcmp(remaining.data->items[i].owner, "core.custom_ui") != 0);
    }
}
}  // namespace

namespace qol::game {
bool game_host_is_main_thread() { return std::this_thread::get_id() == kMainThread; }
}  // namespace qol::game

int main() {
    qol::infra::settings_catalog_reset_for_test();
    qol::infra::status_reset_for_test();
    test_optional_sdk_tail();
    const auto sdk = make_sdk();
    test_static_declarations_and_public_keys(sdk);
    test_action_admission_and_capability_navigation();
    g_test_sdk = &sdk;
    test_debug_panel_action_and_owner_removal();
    std::puts("settings_contributions: PASS (3 categories, 12 items, debug panel action, optional tail, owner gate, owner removal)");
}
