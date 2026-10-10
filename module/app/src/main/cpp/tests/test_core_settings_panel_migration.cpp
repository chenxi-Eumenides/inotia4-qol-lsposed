#include "contracts/host_sdk.h"
#include "contracts/plugin.h"
#include "contracts/ui.h"
#include "plugins/core.custom_ui/panels/stack.h"
#include "plugins/core.settings/plugin.h"
#include "plugins/core.settings/panels/settings.h"
#include "plugins/core.settings/panels/settings_binding.h"

#include <cassert>
#include <cstdint>
#include <cstring>
#include <string>

namespace {

using qol::contracts::PanelCallbacks;
using qol::contracts::PanelProviderSpec;
using qol::contracts::QolPopupStateEntry;
using qol::contracts::QolPopupStateHooks;

const PanelProviderSpec* g_provider = nullptr;
const QolPopupStateHooks* g_popup_hooks = nullptr;
bool g_panel_active = false;
int g_enter_calls = 0;
int g_process_calls = 0;
int g_f3_calls = 0;
int g_f4_calls = 0;
int g_event_calls = 0;
int g_acquires = 0;
int g_restores = 0;
uint64_t g_next_task_id = 1;
int g_scheduler_interval = -1;
QolResult scheduler_add(int32_t, qol::contracts::QolFrameHandler, void*, int32_t interval, int32_t,
                        uint64_t* out_id) {
    if (out_id == nullptr) return qol::contracts::error(kQolErrInvalidArgument, "bad", 3);
    g_scheduler_interval = interval;
    *out_id = g_next_task_id++;
    return qol::contracts::ok();
}
QolResult scheduler_remove(uint64_t) { return qol::contracts::ok(); }
int g_pushes = 0;
int g_pops = 0;
uint32_t g_popup_count = 0;
uintptr_t g_popup_top = 0;
uint32_t g_slots[5] = {};

void settings_enter() { ++g_enter_calls; g_panel_active = true; ++g_slots[0]; }
void settings_process() { ++g_process_calls; ++g_slots[1]; }
void settings_f3() { ++g_f3_calls; g_panel_active = false; ++g_slots[2]; }
void settings_f4() { ++g_f4_calls; ++g_slots[3]; }
uint32_t settings_event(uint64_t, uint64_t, uint64_t) { ++g_event_calls; ++g_slots[4]; return 73; }
const PanelCallbacks kSettingsCallbacks{
    settings_enter, settings_process, settings_f3, settings_f4, settings_event};

QolResult register_provider(const PanelProviderSpec* provider) {
    g_provider = provider;
    return qol::contracts::ok();
}
QolResult register_item(const qol::contracts::PanelItemSpec*) { return qol::contracts::ok(); }
QolResult find_provider(const char* panel_id, const PanelProviderSpec** out) {
    if (panel_id == nullptr || out == nullptr || g_provider == nullptr ||
        std::strcmp(panel_id, g_provider->panel_id) != 0) {
        return qol::contracts::error(kQolErrNotFound, "missing", 7);
    }
    *out = g_provider;
    return qol::contracts::ok();
}
QolResult register_command(qol::contracts::HttpMethod, const char*, const char*,
                           qol::contracts::HttpCommandHandlerFn, void*) {
    return qol::contracts::ok();
}
QolResult resolve_popup(const char* symbol, QolPopupStateEntry* out) {
    assert(symbol != nullptr && std::strcmp(symbol, "F_PANEL_UNK1_ENTER") == 0);
    static uint8_t slot[0x40]{};
    out->slot = slot;
    out->state_id = 17;
    return qol::contracts::ok();
}
QolResult acquire_popup(QolPopupStateEntry*, const QolPopupStateHooks* hooks) {
    ++g_acquires;
    g_popup_hooks = hooks;
    return qol::contracts::ok();
}
QolResult restore_popup(QolPopupStateEntry* entry) {
    ++g_restores;
    entry->slot = nullptr;
    return qol::contracts::ok();
}
QolResult set_popup_process_info(int32_t process, int32_t state_id) {
    if (process == 1) {
        ++g_pushes;
        ++g_popup_count;
        g_popup_top = 0x9999;
        assert(state_id == 17);
        assert(g_popup_hooks != nullptr);
        g_popup_hooks->enter();
    } else {
        ++g_pops;
        assert(process == 3 && state_id == 0);
        if (g_popup_count > 0) --g_popup_count;
        g_popup_top = 0;
    }
    return qol::contracts::ok();
}

QolResult popup_stack_snapshot(uint32_t* count, uintptr_t* top_enter_vma) {
    if (count == nullptr || top_enter_vma == nullptr) {
        return qol::contracts::error(kQolErrInvalidArgument, "invalid", 7);
    }
    *count = g_popup_count;
    *top_enter_vma = g_popup_top;
    return qol::contracts::ok();
}

// The settings plugin now installs only when the Host-selected management SDK exposes the
// resident feature-control channel; this stub satisfies the structural availability check.
QolResult control_read_catalog(void*, const qol::contracts::QolFeatureCatalogPlugin**, uint32_t*) {
    return qol::contracts::ok();
}
QolResult control_read_desired(void*, const char*, uint8_t*, uint8_t*) {
    return qol::contracts::ok();
}
QolResult control_read_feature(void*, const char*, const char*, uint8_t*) {
    return qol::contracts::ok();
}
QolResult control_request_set_feature(void*, const char*, const char*, uint8_t,
                                      qol::contracts::FeatureReceipt*) {
    return qol::contracts::ok();
}
QolResult control_request_desired(void*, const char*, uint8_t, qol::contracts::FeatureReceipt*) {
    return qol::contracts::ok();
}
QolResult control_receipt(void*, uint64_t, qol::contracts::FeatureReceipt*) {
    return qol::contracts::ok();
}
QolResult control_invoke_entry(void*, const char*, const char*) { return qol::contracts::ok(); }
const qol::contracts::QolFeatureControlApi kFeatureControl{
    sizeof(qol::contracts::QolFeatureControlApi), nullptr, &control_read_catalog,
    &control_read_desired, &control_read_feature, &control_request_set_feature,
    &control_request_desired, &control_receipt, &control_invoke_entry};

// 「更多游戏」绑定 tick 决策的记录用 fake 操作（settings_binding 纯逻辑的注入面）。
class RecordingMoreGamesOps : public qol::features::core_ui::MoreGamesBindOps {
public:
    bool proc_matches_result = false;
    bool bind_result = true;
    bool btn_resolvable_result = false;
    int release_calls = 0;
    int bind_calls = 0;
    int live_calls = 0;
    int failed_calls = 0;
    bool last_failed_resolvable = false;

    void reset() {
        release_calls = 0;
        bind_calls = 0;
        live_calls = 0;
        failed_calls = 0;
        last_failed_resolvable = false;
    }

    bool proc_matches() override { return proc_matches_result; }
    void release() override { ++release_calls; }
    bool bind() override { ++bind_calls; return bind_result; }
    bool btn_resolvable() override { return btn_resolvable_result; }
    void mark_live() override { ++live_calls; }
    void mark_failed(bool btn_resolvable) override {
        ++failed_calls;
        last_failed_resolvable = btn_resolvable;
    }
};

}  // namespace

namespace qol::features::core_ui {
void settings_bind_sdk(const contracts::QolHostSdk*, contracts::QolOwnerToken) {}
void settings_ui_close_panel() {}
const contracts::PanelCallbacks* settings_panel_callbacks() { return &kSettingsCallbacks; }
bool settings_provider_open() { return true; }
void settings_provider_close() { g_panel_active = false; }
bool settings_provider_active() { return g_panel_active; }
const char* settings_provider_block_reason() { return nullptr; }
void settings_more_games_tick() {}
void settings_release_more_games() {}
const char* settings_more_games_bind_state() { return "waiting"; }
std::string data_settings_ui_inject() { return "{}"; }
std::string data_settings_ui_status_json() { return "{}"; }
std::string data_settings_ui_restore() { return "{}"; }
std::string data_settings_ui_open_panel() { return "{}"; }
std::string data_settings_ui_open_option() { return "{}"; }
}  // namespace qol::features::core_ui

int main() {
    using namespace qol;
    const contracts::PluginDescriptor& descriptor = features::core_settings::plugin_descriptor();
    assert(descriptor.resources_count == 0 && descriptor.resources == nullptr);
    // Ordinary `settings` plugin: no legacy bool map, no config HTTP route, one real entry feature.
    assert(std::strcmp(descriptor.id, "settings") == 0);
    assert(descriptor.requires_count == 2);
    assert(descriptor.http_query_count == 0 && descriptor.http_queries == nullptr);
    assert(descriptor.feature_count == 1 && descriptor.features != nullptr);
    assert(std::strcmp(descriptor.features[0].id, "page") == 0);
    assert(descriptor.features[0].default_enabled == 1);
    assert(descriptor.features[0].type == contracts::FeatureType::kEntry);
    assert(descriptor.features[0].category == contracts::FeatureCategory::kTools);

    contracts::QolUiAdapter ui{};
    ui.set_popup_process_info = &set_popup_process_info;
    ui.popup_stack_snapshot = &popup_stack_snapshot;
    contracts::QolHostSdk sdk{};
    sdk.abi = {sizeof(contracts::QolHostSdk), contracts::kHostSdkMajor,
               contracts::kHostSdkMinor};
    sdk.panels.register_provider = &register_provider;
    sdk.panels.register_item = &register_item;
    sdk.panels.find = &find_provider;
    sdk.hooks.popup_resolve = &resolve_popup;
    sdk.hooks.popup_acquire = &acquire_popup;
    sdk.hooks.popup_restore = &restore_popup;
    sdk.ui = &ui;
    sdk.http_dispatch.register_command = &register_command;
    sdk.scheduler = {&scheduler_add, &scheduler_remove};
    sdk.feature_control = &kFeatureControl;

    features::core_ui::ui_panels_bind_sdk(&sdk);
    contracts::PluginContext context{};
    context.sdk = &sdk;
    context.owner_token = 73;
    assert(descriptor.install != nullptr && descriptor.install(context) == 0);
    assert(g_provider != nullptr && std::strcmp(g_provider->panel_id, "settings") == 0);
    assert(g_provider->callbacks != nullptr);
    assert(g_provider->callbacks->enter != nullptr && g_provider->callbacks->process != nullptr &&
           g_provider->callbacks->f3 != nullptr && g_provider->callbacks->f4 != nullptr &&
           g_provider->callbacks->event != nullptr);
    // 「更多游戏」重绑帧任务必须每帧运行（interval=1）：游戏 STATE_EnterMainMenu 会把
    // ExecuteProc 槽重填回原版 GotoShowMoreGames，间隔更大时存在点击落回原版 gemshop
    // 的窗口；每帧校验把窗口收窄到 1 帧。
    assert(g_scheduler_interval == 1);

    assert(features::core_ui::ui_panels_open("settings"));
    assert(g_acquires == 1 && g_pushes == 1 && g_enter_calls == 1);
    g_popup_hooks->process();
    g_popup_hooks->f4();
    assert(g_popup_hooks->event(1, 2, 3) == 73);
    g_popup_hooks->f3();
    assert(g_process_calls == 1 && g_f3_calls == 1 && g_f4_calls == 1 && g_event_calls == 1);
    assert(g_pops == 0 && g_restores == 0);
    assert(features::core_ui::ui_panels_frame_pump(1, nullptr) == 1);
    features::core_ui::ui_panels_frame_pump(2, nullptr);
    features::core_ui::ui_panels_frame_pump(3, nullptr);
    assert(g_pops == 1 && g_restores == 1);

    features::core_ui::ui_panels_close("settings");
    assert(g_pops == 1 && g_restores == 1);
    assert(g_slots[0] == 1 && g_slots[1] == 1 && g_slots[2] == 1 &&
           g_slots[3] == 1 && g_slots[4] == 1);
    features::core_ui::ui_panels_bind_sdk(nullptr);
    descriptor.deactivate(context);

    // ---- 「更多游戏」绑定每帧决策（more_games_bind_tick 纯逻辑）----
    {
        using qol::features::core_ui::more_games_bind_tick;
        RecordingMoreGamesOps ops;

        // 非 main_menu：只释放、不标记 live——status 回 waiting，不再展示过时的 live 快照。
        more_games_bind_tick(false, ops);
        assert(ops.release_calls == 1 && ops.bind_calls == 0 && ops.live_calls == 0 &&
               ops.failed_calls == 0);

        // 槽被游戏改写（STATE_EnterMainMenu 重填）→ 下一帧 tick：释放既有记录后重绑 → live。
        ops.reset();
        ops.proc_matches_result = false;
        ops.bind_result = true;
        more_games_bind_tick(true, ops);
        assert(ops.release_calls == 1 && ops.bind_calls == 1 && ops.live_calls == 1 &&
               ops.failed_calls == 0);

        // 校验一致（main_menu 常态）：仅确认 live，无释放/重装写操作。
        ops.reset();
        ops.proc_matches_result = true;
        more_games_bind_tick(true, ops);
        assert(ops.release_calls == 0 && ops.bind_calls == 0 && ops.live_calls == 1);

        // main_menu 绑定失败：控件不可解析 → waiting（resolvable=false）；
        // 控件可解析但写槽失败 → error（resolvable=true）。
        ops.reset();
        ops.proc_matches_result = false;
        ops.bind_result = false;
        ops.btn_resolvable_result = false;
        more_games_bind_tick(true, ops);
        assert(ops.release_calls == 1 && ops.bind_calls == 1 && ops.live_calls == 0 &&
               ops.failed_calls == 1 && ops.last_failed_resolvable == false);
        ops.reset();
        ops.proc_matches_result = false;
        ops.btn_resolvable_result = true;
        more_games_bind_tick(true, ops);
        assert(ops.release_calls == 1 && ops.bind_calls == 1 && ops.live_calls == 0 &&
               ops.failed_calls == 1 && ops.last_failed_resolvable == true);
    }
}
