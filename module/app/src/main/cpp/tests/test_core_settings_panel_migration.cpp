#include "contracts/host_sdk.h"
#include "contracts/plugin.h"
#include "contracts/ui.h"
#include "plugins/core.custom_ui/panels/stack.h"
#include "plugins/core.settings/plugin.h"
#include "plugins/core.settings/panels/settings.h"
#include "plugins/core.settings/panels/settings_ui_local.h"

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
QolResult scheduler_add(int32_t, qol::contracts::QolFrameHandler, void*, int32_t, int32_t,
                        uint64_t* out_id) {
    if (out_id == nullptr) return qol::contracts::error(kQolErrInvalidArgument, "bad", 3);
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

}  // namespace

namespace qol::features::core_ui {
void settings_bind_sdk(const contracts::QolHostSdk*) {}
const contracts::PanelCallbacks* settings_panel_callbacks() { return &kSettingsCallbacks; }
bool settings_provider_open() { return true; }
void settings_provider_close() { g_panel_active = false; }
bool settings_provider_active() { return g_panel_active; }
const char* settings_provider_block_reason() { return nullptr; }
void settings_more_games_tick() {}
void settings_release_more_games() {}
const char* settings_more_games_bind_state() { return "waiting"; }
void settings_ui_bind(const contracts::QolUiAdapter*, const contracts::QolUiComponents*) {}
void settings_ui_unbind() {}
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

    features::core_ui::ui_panels_bind_sdk(&sdk);
    contracts::PluginContext context{};
    context.sdk = &sdk;
    assert(descriptor.install != nullptr && descriptor.install(context) == 0);
    assert(g_provider != nullptr && std::strcmp(g_provider->panel_id, "settings") == 0);
    assert(g_provider->callbacks != nullptr);
    assert(g_provider->callbacks->enter != nullptr && g_provider->callbacks->process != nullptr &&
           g_provider->callbacks->f3 != nullptr && g_provider->callbacks->f4 != nullptr &&
           g_provider->callbacks->event != nullptr);

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
}
