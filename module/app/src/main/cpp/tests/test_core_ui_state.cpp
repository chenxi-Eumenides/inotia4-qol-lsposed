#include "plugins/core.ui_state/plugin.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "contracts/game_ports/choice_write_port.h"
#include "contracts/game_ports/ui_write_port.h"
#include "contracts/commands.h"
#include "contracts/http.h"
#include "contracts/ui.h"

namespace {

using namespace qol::contracts;

QolFrameHandler g_frame_handler = nullptr;
uint64_t g_frame_task = 0;
uint32_t g_native_reads = 0;
const char* g_native_screen = "world";
const char* g_native_panel = "";
const char* g_native_dialog = "{\"type\":\"none\",\"options\":[]}";

QolResult copy_field(char* out, uint32_t capacity, uint32_t* size, const char* value) {
    if (out == nullptr || size == nullptr || value == nullptr || capacity <= std::strlen(value)) {
        return error(kQolErrInvalidArgument, "bad test snapshot output", 24);
    }
    *size = static_cast<uint32_t>(std::strlen(value));
    std::memcpy(out, value, *size + 1);
    return ok();
}

QolResult read_snapshot(char* screen, uint32_t screen_capacity, uint32_t* screen_size,
                        char* panel, uint32_t panel_capacity, uint32_t* panel_size,
                        char* dialog, uint32_t dialog_capacity, uint32_t* dialog_size) {
    ++g_native_reads;
    const QolResult screen_result =
        copy_field(screen, screen_capacity, screen_size, g_native_screen);
    if (!succeeded(screen_result)) return screen_result;
    const QolResult panel_result = copy_field(panel, panel_capacity, panel_size, g_native_panel);
    if (!succeeded(panel_result)) return panel_result;
    return copy_field(dialog, dialog_capacity, dialog_size, g_native_dialog);
}

QolResult ui_write(int32_t, const char*, char*, uint32_t, uint32_t*) { return ok(); }
QolResult choice_write(int32_t, const char*, int32_t, char*, uint32_t, uint32_t*) { return ok(); }
int32_t is_main_thread() { return 1; }

QolResult scheduler_add(int32_t, QolFrameHandler handler, void*, int32_t interval,
                        int32_t, uint64_t* out_id) {
    if (handler == nullptr || interval != 1 || out_id == nullptr) {
        return error(kQolErrInvalidArgument, "bad scheduler request", 21);
    }
    g_frame_handler = handler;
    *out_id = ++g_frame_task;
    return ok();
}

QolResult scheduler_remove(uint64_t id) {
    if (id != g_frame_task) return error(kQolErrNotFound, "task not found", 14);
    g_frame_handler = nullptr;
    return ok();
}

QolResult register_result(uint32_t, QolTypedResultCommandHandler, void*) { return ok(); }
QolResult unregister_result(uint32_t) { return ok(); }

const HttpQueryHandlerDescriptor* route(HttpMethod method, const char* path) {
    const PluginDescriptor& descriptor = qol::features::core_ui_state::plugin_descriptor();
    assert(descriptor.requires_count == 2 && descriptor.optional_count == 0);
    for (uint32_t i = 0; i < descriptor.http_query_count; ++i) {
        const HttpQueryHandlerDescriptor& candidate = descriptor.http_queries[i];
        if (candidate.method == method && std::strcmp(candidate.path, path) == 0) {
            return &candidate;
        }
    }
    return nullptr;
}

std::string query(HttpMethod method, const char* path) {
    const HttpQueryHandlerDescriptor* handler = route(method, path);
    assert(handler != nullptr && handler->handler != nullptr);
    uint8_t output[16384]{};
    HttpReq request{};
    HttpRes response{output, sizeof(output), 0, 0};
    assert(succeeded(handler->handler(&request, &response, handler->ctx)));
    assert(response.status == 200);
    return std::string(reinterpret_cast<const char*>(output), response.body_size);
}

void test_native_fallback_and_keysets(const UiStateApi* api) {
    g_native_screen = "panel_inventory";
    g_native_panel = "panel_inventory";
    g_native_dialog = "{\"type\":\"inventory\",\"options\":[{\"id\":\"close\",\"label\":\"关闭\"}]}";
    assert(g_frame_handler != nullptr);
    g_frame_handler(1, nullptr);
    const uint32_t reads_after_snapshot = g_native_reads;
    assert(query(HttpMethod::kGet, "/api/ui/screen") == "{\"screen\":\"panel_inventory\"}");
    assert(query(HttpMethod::kGet, "/api/ui/panel") == "{\"panel\":\"panel_inventory\"}");
    assert(query(HttpMethod::kGet, "/api/ui/dialog") == g_native_dialog);
    assert(g_native_reads == reads_after_snapshot);  // HTTP readers use only the main-thread copy.

    const UiStateApi* provided = api;
    assert(provided != nullptr);
    const UiCustomPanelsSnapshot clear_panels{1, 0, nullptr};
    assert(succeeded(provided->publish_custom_panels(&clear_panels)));
    g_native_screen = "world";
    g_native_panel = "";
    g_frame_handler(2, nullptr);
    char first_panel[] = "custom.settings";
    char second_panel[] = "custom.backup";
    const char* one[] = {first_panel};
    const UiCustomPanelsSnapshot one_layer{2, 1, one};
    assert(succeeded(provided->publish_custom_panels(&one_layer)));
    assert(query(HttpMethod::kGet, "/api/ui/panel") == "{\"panel\":\"panel_custom.settings\"}");
    assert(query(HttpMethod::kGet, "/api/ui/screen") == "{\"screen\":\"world\"}");

    const char* nested[] = {first_panel, second_panel};
    const UiCustomPanelsSnapshot two_layers{3, 2, nested};
    assert(succeeded(provided->publish_custom_panels(&two_layers)));
    std::memset(first_panel, 'x', sizeof(first_panel) - 1);
    assert(query(HttpMethod::kGet, "/api/ui/panel") == "{\"panel\":\"panel_custom.backup\"}");
    const UiCustomPanelsSnapshot stale_panels{2, 1, one};
    assert(provided->publish_custom_panels(&stale_panels).code == kQolErrConflict);
    const UiCustomPanelsSnapshot pop_top{4, 1, nested};
    assert(succeeded(provided->publish_custom_panels(&pop_top)));
    assert(query(HttpMethod::kGet, "/api/ui/panel") ==
           std::string("{\"panel\":\"panel_") + std::string(15, 'x') + "\"}");
    const UiCustomPanelsSnapshot unregister_owner{5, 0, nullptr};
    assert(succeeded(provided->publish_custom_panels(&unregister_owner)));
    assert(query(HttpMethod::kGet, "/api/ui/panel") == "{\"panel\":null}");
}

void test_custom_dialog_generation_and_mapping(const UiStateApi* api) {
    char type1[] = "world_teleport.confirm";
    char title1[] = "Map title";
    char option_a[] = "North";
    char option_b[] = "Close";
    const char* labels1[] = {option_a, option_b};
    const UiCustomDialogSnapshot dialog1{1, 1, type1, title1, labels1, 2};
    assert(succeeded(api->publish_custom_dialog(&dialog1)));
    char type2[] = "save_backup.confirm";
    char title2[] = "Backup";
    char option_c[] = "Save";
    const char* labels2[] = {option_c};
    const UiCustomDialogSnapshot dialog2{2, 2, type2, title2, labels2, 1};
    assert(succeeded(api->publish_custom_dialog(&dialog2)));
    std::memset(type2, 'z', sizeof(type2) - 1);
    std::memset(title2, 'z', sizeof(title2) - 1);
    std::memset(option_c, 'z', sizeof(option_c) - 1);
    assert(query(HttpMethod::kGet, "/api/ui/dialog") ==
           "{\"type\":\"save_backup.confirm\",\"title\":\"Backup\",\"options\":[{\"id\":\"0\",\"label\":\"Save\"}],\"active\":true}");
    assert(api->clear_custom_dialog(1, 3).ok == kQolOk);  // Old generation cannot clear new.
    assert(query(HttpMethod::kGet, "/api/ui/dialog").find("save_backup.confirm") !=
           std::string::npos);
    const UiCustomDialogSnapshot late_old{4, 1, "world_teleport.confirm", "Old", labels1, 2};
    assert(api->publish_custom_dialog(&late_old).code == kQolErrConflict);
    assert(succeeded(api->clear_custom_dialog(2, 4)));
    assert(query(HttpMethod::kGet, "/api/ui/dialog") == g_native_dialog);
    assert(api->publish_custom_dialog(&dialog2).code == kQolErrConflict);
}

}  // namespace

int main() {
    QolHostSdk sdk{};
    sdk.abi = {sizeof(sdk), kHostSdkMajor, 9};
    QolUiAdapter ui{};
    ui.size = sizeof(ui);
    ui.major = kUiAdapterMajor;
    ui.minor = kUiAdapterMinor;
    ui.is_main_thread = &is_main_thread;
    sdk.ui = &ui;
    sdk.scheduler.add = &scheduler_add;
    sdk.scheduler.remove = &scheduler_remove;
    sdk.ticket_result.register_typed_result_handler = &register_result;
    sdk.ticket_result.unregister_typed_result_handler = &unregister_result;

    const QolChoiceWritePort choice_port{{1, 0}, {&choice_write}};
    const QolUiWritePort ui_write_port{{1, 0}, {&ui_write}};
    const QolUiStateReadPort ui_read_port{{1, 0}, {&read_snapshot}};
    const void* ports[] = {&choice_port, &ui_write_port, &ui_read_port};
    PluginContext context{};
    context.sdk = &sdk;
    context.ports = ports;
    context.port_count = 3;
    const PluginDescriptor& descriptor = qol::features::core_ui_state::plugin_descriptor();
    assert(descriptor.provides_count == 1);
    assert(std::strcmp(descriptor.provides[0].id, kUiStateCapabilityId) == 0);
    assert(descriptor.provides[0].version.major == 1 && descriptor.provides[0].version.minor == 0);
    assert(descriptor.http_query_count == 8);
    assert(route(HttpMethod::kGet, "/api/ui") == nullptr);
    assert(descriptor.install(context) == 0);
    const auto* api = static_cast<const UiStateApi*>(descriptor.provides[0].iface);

    test_native_fallback_and_keysets(api);
    test_custom_dialog_generation_and_mapping(api);

    descriptor.deactivate(context);
    assert(g_frame_handler == nullptr);
    std::puts("core_ui_state_tests: passed");
    return 0;
}
