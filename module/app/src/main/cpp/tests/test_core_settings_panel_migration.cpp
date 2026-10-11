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
#include <type_traits>

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

// 「更多游戏」替换 tick 决策的 fake 操作。
class RecordingMoreGamesOps final : public qol::features::core_ui::MoreGamesBindOps {
public:
    qol::features::core_ui::MoreGamesBindingState binding_state_result =
        qol::features::core_ui::MoreGamesBindingState::kNeedsReplace;
    bool bind_result = true;
    bool release_result = true;
    bool btn_resolvable_result = false;
    int release_calls = 0;
    int bind_calls = 0;
    int live_calls = 0;
    int failed_calls = 0;
    int waiting_calls = 0;
    bool last_failed_resolvable = false;

    void reset() {
        release_calls = 0;
        bind_calls = 0;
        live_calls = 0;
        failed_calls = 0;
        waiting_calls = 0;
        last_failed_resolvable = false;
    }

    qol::features::core_ui::MoreGamesBindingState binding_state() override {
        return binding_state_result;
    }
    bool release() override { ++release_calls; return release_result; }
    bool bind() override { ++bind_calls; return bind_result; }
    bool btn_resolvable() override { return btn_resolvable_result; }
    void mark_live() override { ++live_calls; }
    void mark_failed(bool btn_resolvable) override {
        ++failed_calls;
        last_failed_resolvable = btn_resolvable;
    }
    void mark_waiting() override { ++waiting_calls; }
};

class RecordingMoreGamesDrawOps final : public qol::features::core_ui::MoreGamesDrawOps {
public:
    bool replacement_is_live = false;
    bool replacement_draw_succeeds = false;
    int live_checks = 0;
    int replacement_draws = 0;
    int original_draws = 0;

    bool replacement_live() override { ++live_checks; return replacement_is_live; }
    bool draw_replacement() override { ++replacement_draws; return replacement_draw_succeeds; }
    void draw_original() override { ++original_draws; }
};

// Records the show/enabled/active write order so the transactional hide/deactivate rollback and
// the current-identity-only restore can be asserted without native memory.
class RecordingOriginalStateOps final : public qol::features::core_ui::MoreGamesOriginalStateOps {
public:
    // Bit i disables the i-th write in sequence; writes past the end still succeed.
    uint32_t fail_writes = 0;
    int show_calls = 0;
    int enabled_calls = 0;
    int active_calls = 0;
    bool last_show = false;
    bool last_enabled = false;
    bool last_active = false;

    bool set_show(bool shown) override {
        ++show_calls;
        last_show = shown;
        return (fail_writes & (1u << 0)) == 0;
    }
    bool set_enabled(bool enabled) override {
        ++enabled_calls;
        last_enabled = enabled;
        return (fail_writes & (1u << 1)) == 0;
    }
    bool set_active(bool active) override {
        ++active_calls;
        last_active = active;
        return (fail_writes & (1u << 2)) == 0;
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
void settings_main_menu_more_games_draw(void*, int32_t, int32_t, void*, int32_t, int32_t, int32_t) {}
std::string data_settings_ui_inject() { return "{}"; }
std::string data_settings_ui_status_json() { return "{}"; }
std::string data_settings_ui_restore() { return "{}"; }
std::string data_settings_ui_open_panel() { return "{}"; }
std::string data_settings_ui_open_option() { return "{}"; }
}  // namespace qol::features::core_ui

int main() {
    using namespace qol;
    using MainMenuDrawProc = void (*)(void*, int32_t, int32_t, void*, int32_t, int32_t, int32_t);
    static_assert(std::is_same<decltype(&features::core_ui::settings_main_menu_more_games_draw),
                               MainMenuDrawProc>::value,
                  "MainMenu GRPX_DrawPart wrapper must preserve the full seven-argument ABI");
    const contracts::PluginDescriptor& descriptor = features::core_settings::plugin_descriptor();
    assert(descriptor.resources_count == 1 && descriptor.resources != nullptr);
    const auto& draw_claim = descriptor.resources[0];
    assert(std::strcmp(draw_claim.id, "patch:settings.main_menu.more_games.draw") == 0);
    assert(std::strcmp(draw_claim.symbol, "settings.main_menu.more_games.draw") == 0);
    assert(draw_claim.mechanism == contracts::ResourceMechanism::kCallPatch);
    assert(draw_claim.requirement == contracts::ResourceRequirement::kRequired);
    assert(draw_claim.failure == contracts::ResourceFailure::kFailClosed);
    assert(draw_claim.handler != nullptr);
    // Ordinary `settings` plugin: no legacy bool map, no config HTTP route, one real entry feature.
    assert(std::strcmp(descriptor.id, "settings") == 0);
    assert(descriptor.requires_count == 2);
    assert(descriptor.http_query_count == 0 && descriptor.http_queries == nullptr);
    assert(descriptor.feature_count == 1 && descriptor.features != nullptr);
    assert(std::strcmp(descriptor.features[0].id, "page") == 0);
    assert(descriptor.features[0].default_enabled == 1);
    assert(descriptor.features[0].type == contracts::FeatureType::kEntry);
    assert(descriptor.features[0].category == contracts::FeatureCategory::kHidden);

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

    // ---- 「更多游戏」替换每帧决策（more_games_bind_tick 纯逻辑）----
    {
        using qol::features::core_ui::more_games_bind_tick;
        RecordingMoreGamesOps ops;

        // 非 main_menu：不碰游戏控件，status 回 waiting。
        more_games_bind_tick(false, ops);
        assert(ops.release_calls == 0 && ops.bind_calls == 0 && ops.live_calls == 0 &&
               ops.failed_calls == 0 && ops.waiting_calls == 1);

        // Main-menu identity unknown: retain the component and its native callback owner.
        ops.reset();
        ops.binding_state_result = qol::features::core_ui::MoreGamesBindingState::kUnknown;
        more_games_bind_tick(true, ops);
        assert(ops.release_calls == 0 && ops.bind_calls == 0 && ops.live_calls == 0 &&
               ops.failed_calls == 0 && ops.waiting_calls == 1);

        // 主菜单控件被重建 → 下一帧释放旧组件后按新矩形替换 → live。
        ops.reset();
        ops.binding_state_result = qol::features::core_ui::MoreGamesBindingState::kNeedsReplace;
        ops.bind_result = true;
        more_games_bind_tick(true, ops);
        assert(ops.release_calls == 1 && ops.bind_calls == 1 && ops.live_calls == 1 &&
               ops.failed_calls == 0);

        // 校验一致（main_menu 常态）：仅确认 live，不重新创建。
        ops.reset();
        ops.binding_state_result = qol::features::core_ui::MoreGamesBindingState::kLive;
        more_games_bind_tick(true, ops);
        assert(ops.release_calls == 0 && ops.bind_calls == 0 && ops.live_calls == 1);

        // Repeated off-menu/on-menu frames keep unknown ownership intact, then reuse a proven live bind.
        ops.reset();
        more_games_bind_tick(false, ops);
        assert(ops.release_calls == 0 && ops.waiting_calls == 1);
        ops.reset();
        ops.binding_state_result = qol::features::core_ui::MoreGamesBindingState::kLive;
        more_games_bind_tick(true, ops);
        assert(ops.release_calls == 0 && ops.bind_calls == 0 && ops.live_calls == 1);

        // main_menu 替换失败：原控件不可解析 → waiting；原控件存在但替换创建失败 → error。
        ops.reset();
        ops.binding_state_result = qol::features::core_ui::MoreGamesBindingState::kNeedsReplace;
        ops.bind_result = false;
        ops.btn_resolvable_result = false;
        more_games_bind_tick(true, ops);
        assert(ops.release_calls == 1 && ops.bind_calls == 1 && ops.live_calls == 0 &&
               ops.failed_calls == 1 && ops.last_failed_resolvable == false);
        ops.reset();
        ops.binding_state_result = qol::features::core_ui::MoreGamesBindingState::kNeedsReplace;
        ops.btn_resolvable_result = true;
        more_games_bind_tick(true, ops);
        assert(ops.release_calls == 1 && ops.bind_calls == 1 && ops.live_calls == 0 &&
               ops.failed_calls == 1 && ops.last_failed_resolvable == true);

        // If prior component cleanup is uncertain, never install a second button.
        ops.reset();
        ops.binding_state_result = qol::features::core_ui::MoreGamesBindingState::kNeedsReplace;
        ops.release_result = false;
        more_games_bind_tick(true, ops);
        assert(ops.release_calls == 1 && ops.bind_calls == 0 && ops.live_calls == 0 &&
               ops.waiting_calls == 1);
    }

    // Explicit MainMenu draw site: native draw is suppressed only after the live custom component
    // completes successfully; all other paths replay the original draw exactly once.
    {
        using qol::features::core_ui::more_games_draw;
        RecordingMoreGamesDrawOps ops;
        ops.replacement_is_live = true;
        ops.replacement_draw_succeeds = true;
        assert(more_games_draw(true, ops));
        assert(ops.live_checks == 1 && ops.replacement_draws == 1 && ops.original_draws == 0);

        ops = RecordingMoreGamesDrawOps{};
        ops.replacement_is_live = true;
        ops.replacement_draw_succeeds = true;
        assert(!more_games_draw(false, ops));
        assert(ops.live_checks == 0 && ops.replacement_draws == 0 && ops.original_draws == 1);

        ops = RecordingMoreGamesDrawOps{};  // unbound/unknown is not live
        assert(!more_games_draw(true, ops));
        assert(ops.live_checks == 1 && ops.replacement_draws == 0 && ops.original_draws == 1);

        ops = RecordingMoreGamesDrawOps{};
        ops.replacement_is_live = true;
        ops.replacement_draw_succeeds = false;
        assert(!more_games_draw(true, ops));
        assert(ops.live_checks == 1 && ops.replacement_draws == 1 && ops.original_draws == 1);
    }

    // Transactional hide/deactivate: order is show=false, enabled=false, active=false, and a
    // failed step restores the original boolean Active before the replacement is discarded.
    {
        using qol::features::core_ui::more_games_disable_original;
        RecordingOriginalStateOps state;
        assert(more_games_disable_original(state, true));
        assert(state.show_calls == 1 && state.enabled_calls == 1 && state.active_calls == 1 &&
               !state.last_show && !state.last_enabled && !state.last_active);

        // Active setter fails mid-transaction: show/enabled roll back to true and Active is
        // restored to the originally observed boolean, so the binding must not advertise live.
        state = RecordingOriginalStateOps{};
        state.fail_writes = 1u << 2;
        assert(!more_games_disable_original(state, true));
        assert(state.show_calls == 2 && state.enabled_calls == 2 && state.active_calls == 2 &&
               state.last_show && state.last_enabled && state.last_active);

        // Original was inactive before binding: rollback must restore the exact false meaning.
        state = RecordingOriginalStateOps{};
        state.fail_writes = 1u << 1;
        assert(!more_games_disable_original(state, false));
        assert(state.last_show && state.last_enabled && !state.last_active);
    }

    // Restore only writes when the original is verified current; an unknown/non-current identity
    // performs no native writes and reports success so ownership can be released safely.
    {
        using qol::features::core_ui::more_games_restore_original;
        RecordingOriginalStateOps state;
        assert(more_games_restore_original(false, true, state));
        assert(state.show_calls == 0 && state.enabled_calls == 0 && state.active_calls == 0);

        state = RecordingOriginalStateOps{};
        assert(more_games_restore_original(true, false, state));
        assert(state.show_calls == 1 && state.enabled_calls == 1 && state.active_calls == 1 &&
               state.last_show && state.last_enabled && !state.last_active);

        state = RecordingOriginalStateOps{};
        state.fail_writes = 1u << 2;
        assert(!more_games_restore_original(true, true, state));
        assert(state.active_calls == 1);
    }
}
