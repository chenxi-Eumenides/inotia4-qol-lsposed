#include "plugins/core.ui_state/plugin.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

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
bool g_native_read_fails = false;
const char* g_native_screen = "world";
const char* g_native_panel = "";
const char* g_native_dialog = "{\"type\":\"none\",\"options\":[]}";
QolTypedResultCommandHandler g_ui_write_result = nullptr;
void* g_ui_write_result_ctx = nullptr;
uint8_t g_posted_ui_payload[kTypedCommandPayloadCapacity]{};
uint32_t g_posted_ui_payload_size = 0;
QolCommandTicket g_next_ticket = 100;
uint32_t g_result_attempt = 0;
uint32_t g_defer_calls = 0;
uint32_t g_ui_step_calls = 0;
uint64_t g_step_operation_id = 0;
uint32_t g_step_attempt = 0;
int32_t g_step_kind = -1;
std::string g_step_panel;
uint64_t g_cancel_operation_id = UINT64_MAX;
uint32_t g_cancel_order = 0;
uint32_t g_ui_unregister_order = 0;
uint32_t g_order = 0;
bool g_step_pending_first_attempt = true;

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
    if (g_native_read_fails) return error(kQolErrNotReady, "simulated snapshot failure", 26);
    const QolResult screen_result =
        copy_field(screen, screen_capacity, screen_size, g_native_screen);
    if (!succeeded(screen_result)) return screen_result;
    const QolResult panel_result = copy_field(panel, panel_capacity, panel_size, g_native_panel);
    if (!succeeded(panel_result)) return panel_result;
    return copy_field(dialog, dialog_capacity, dialog_size, g_native_dialog);
}

QolResult ui_write(int32_t, const char*, char*, uint32_t, uint32_t*) { return ok(); }
QolResult ui_write_step(uint64_t operation_id, uint32_t attempt, int32_t kind,
                        const char* panel, char* out, uint32_t capacity, uint32_t* out_size) {
    ++g_ui_step_calls;
    g_step_operation_id = operation_id;
    g_step_attempt = attempt;
    g_step_kind = kind;
    g_step_panel = panel != nullptr ? panel : "";
    if (g_step_pending_first_attempt && attempt == 0) {
        if (out_size != nullptr) *out_size = 0;
        return error(kQolErrNotReady, "panel close pending", 19);
    }
    constexpr char kResult[] = "{\"ok\":true,\"state\":{}}";
    if (out == nullptr || out_size == nullptr || sizeof(kResult) - 1 > capacity) {
        return error(kQolErrInvalidArgument, "bad fake result output", 22);
    }
    std::memcpy(out, kResult, sizeof(kResult) - 1);
    *out_size = static_cast<uint32_t>(sizeof(kResult) - 1);
    return ok();
}
QolResult ui_write_cancel_pending(uint64_t operation_id) {
    g_cancel_operation_id = operation_id;
    g_cancel_order = ++g_order;
    return ok();
}
QolResult choice_write(int32_t, const char*, int32_t, char*, uint32_t, uint32_t*) { return ok(); }
int32_t is_main_thread() { return 1; }
QolResult defer_current_result() {
    ++g_defer_calls;
    return ok();
}
uint32_t current_attempt() { return g_result_attempt; }

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

QolResult register_result(uint32_t id, QolTypedResultCommandHandler handler, void* ctx) {
    if (id == 0x55490001) {
        g_ui_write_result = handler;
        g_ui_write_result_ctx = ctx;
    }
    return ok();
}
QolResult unregister_result(uint32_t id) {
    if (id == 0x55490001) g_ui_unregister_order = ++g_order;
    return ok();
}
QolResult post_typed(uint32_t handler_id, const void* payload, uint32_t size,
                     QolCommandTicket* out_ticket) {
    if (handler_id != 0x55490001 || payload == nullptr || out_ticket == nullptr ||
        size > sizeof(g_posted_ui_payload)) {
        return error(kQolErrInvalidArgument, "invalid UI command", 20);
    }
    std::memcpy(g_posted_ui_payload, payload, size);
    g_posted_ui_payload_size = size;
    *out_ticket = ++g_next_ticket;
    return ok();
}

const HttpEndpointDescriptor* route(HttpMethod method, const char* path) {
    const PluginDescriptor& descriptor = qol::features::core_ui_state::plugin_descriptor();
    assert(descriptor.requires_count == 0 && descriptor.optional_count == 2);
    for (uint32_t i = 0; i < descriptor.http_endpoint_count; ++i) {
        const HttpEndpointDescriptor& candidate = descriptor.http_endpoints[i];
        if (candidate.method == method && std::strcmp(candidate.path, path) == 0) {
            return &candidate;
        }
    }
    return nullptr;
}

std::string query(HttpMethod method, const char* path, uint16_t* out_status = nullptr) {
    const HttpEndpointDescriptor* endpoint = route(method, path);
    assert(endpoint != nullptr && endpoint->kind == HttpHandlerKind::kQuery &&
           endpoint->query_handler != nullptr && endpoint->command_handler == nullptr);
    uint8_t output[16384]{};
    HttpReq request{};
    HttpRes response{output, sizeof(output), 0, 0};
    const QolResult result = endpoint->query_handler(&request, &response, endpoint->ctx);
    if (out_status != nullptr) *out_status = response.status;
    if (response.status == 200) assert(succeeded(result));
    else assert(response.status == 503 && !succeeded(result));
    return std::string(reinterpret_cast<const char*>(output), response.body_size);
}

void test_ui_write_ticket_retry() {
    const HttpEndpointDescriptor* open_endpoint = route(HttpMethod::kPost, "/api/ui/open_panel");
    const HttpEndpointDescriptor* close_endpoint = route(HttpMethod::kPost, "/api/ui/close_panel");
    assert(open_endpoint != nullptr && open_endpoint->kind == HttpHandlerKind::kCommand &&
           open_endpoint->command_handler != nullptr);
    assert(close_endpoint != nullptr && close_endpoint->kind == HttpHandlerKind::kCommand &&
           close_endpoint->command_handler != nullptr);

    const std::string open_body = "{\"panel\":\"inventory\"}";
    uint8_t response_bytes[1024]{};
    HttpReq request{};
    request.body = reinterpret_cast<const uint8_t*>(open_body.data());
    request.body_len = static_cast<uint32_t>(open_body.size());
    HttpRes response{response_bytes, sizeof(response_bytes), 0, 0};
    assert(succeeded(open_endpoint->command_handler(&request, &response, open_endpoint->ctx)));
    assert(response.status == 200 &&
           std::string(reinterpret_cast<const char*>(response_bytes), response.body_size) ==
               "{\"ok\":true,\"ticket\":101}");
    const uint32_t first_payload_size = g_posted_ui_payload_size;
    uint8_t first_payload[kTypedCommandPayloadCapacity]{};
    std::memcpy(first_payload, g_posted_ui_payload, first_payload_size);

    request.body = nullptr;
    request.body_len = 0;
    response = HttpRes{response_bytes, sizeof(response_bytes), 0, 0};
    assert(succeeded(close_endpoint->command_handler(&request, &response, close_endpoint->ctx)));
    assert(g_posted_ui_payload_size == first_payload_size && first_payload_size <=
               kTypedCommandPayloadCapacity);
    struct UiWritePayloadFixture {
        uint64_t operation_id;
        int32_t kind;
        char panel[64];
    } first{}, second{};
    static_assert(sizeof(UiWritePayloadFixture) <= kTypedCommandPayloadCapacity,
                  "test UI payload must fit typed command");
    std::memcpy(&first, first_payload, first_payload_size);
    std::memcpy(&second, g_posted_ui_payload, g_posted_ui_payload_size);
    assert(first.operation_id != 0 && second.operation_id != 0 &&
           first.operation_id != second.operation_id);
    assert(first.kind == kQolUiWritePanelOpen && std::strcmp(first.panel, "inventory") == 0);
    assert(second.kind == kQolUiWritePanelClose && second.panel[0] == '\0');

    assert(g_ui_write_result != nullptr);
    char result_bytes[256]{};
    uint32_t result_size = 0;
    g_step_pending_first_attempt = true;
    g_result_attempt = 0;
    assert(!g_ui_write_result(g_ui_write_result_ctx, first_payload, first_payload_size,
                              result_bytes, sizeof(result_bytes), &result_size));
    assert(result_size == 0 && g_defer_calls == 1 && g_step_operation_id == first.operation_id &&
           g_step_attempt == 0 && g_step_kind == kQolUiWritePanelOpen &&
           g_step_panel == "inventory");

    g_result_attempt = 1;
    assert(g_ui_write_result(g_ui_write_result_ctx, first_payload, first_payload_size,
                             result_bytes, sizeof(result_bytes), &result_size));
    assert(g_step_attempt == 1 &&
           std::string(result_bytes, result_size) == "{\"ok\":true,\"state\":{}}");
}

void test_native_fallback_and_keysets(const UiStateApi* api) {
    g_native_screen = "panel_inventory";
    g_native_panel = "panel_inventory";
    g_native_dialog =
        "{\"type\":\"npc\",\"text\":\"say \\\"type\\\" and \\\\ path\",\"speaker\":\"merchant\",\"displayed\":true,\"options\":[{\"id\":\"0\",\"label\":\"Open\",\"meta\":{\"type\":\"nested\"}}]}";
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
           "{\"type\":\"save_backup.confirm\",\"text\":\"say \\\"type\\\" and \\\\ path\",\"speaker\":\"merchant\",\"displayed\":true,\"options\":[{\"id\":\"0\",\"label\":\"Open\",\"meta\":{\"type\":\"nested\"}}]}");
    assert(api->clear_custom_dialog(1, 3).ok == kQolOk);  // Old generation cannot clear new.
    assert(query(HttpMethod::kGet, "/api/ui/dialog").find("save_backup.confirm") !=
           std::string::npos);
    const UiCustomDialogSnapshot late_old{4, 1, "world_teleport.confirm", "Old", labels1, 2};
    assert(api->publish_custom_dialog(&late_old).code == kQolErrConflict);

    g_native_dialog = "{\"t\\u0079pe\":\"native\",\"text\":\"escaped value\\\" here\"}";
    g_frame_handler(3, nullptr);
    assert(query(HttpMethod::kGet, "/api/ui/dialog") ==
           "{\"t\\u0079pe\":\"save_backup.confirm\",\"text\":\"escaped value\\\" here\"}");

    g_native_dialog = "{\"text\":\"no top-level type\"}";
    g_frame_handler(4, nullptr);
    uint16_t status = 0;
    assert(query(HttpMethod::kGet, "/api/ui/dialog", &status) ==
           "{\"ok\":false,\"error\":\"snapshot not ready\"}");
    assert(status == 503);

    const char* malformed_native = g_native_dialog;
    assert(succeeded(api->clear_custom_dialog(2, 5)));
    assert(query(HttpMethod::kGet, "/api/ui/dialog") == malformed_native);
    assert(api->publish_custom_dialog(&dialog2).code == kQolErrConflict);
}

struct LayerCapture {
    std::vector<std::string> ids;
    std::vector<uint32_t> types;
};

void capture_layer(const UiLayerInfo* layer, void* user) {
    auto* out = static_cast<LayerCapture*>(user);
    assert(layer != nullptr && layer->id != nullptr);
    out->ids.emplace_back(layer->id);
    out->types.push_back(layer->type);
}

void test_read_layers(const UiStateApi* api) {
    assert(api != nullptr && api->read_layers != nullptr);
    const UiCustomPanelsSnapshot clear{100, 0, nullptr};
    assert(succeeded(api->publish_custom_panels(&clear)));
    char panel_a[] = "custom.a";
    char panel_b[] = "custom.b";
    const char* ids[] = {panel_a, panel_b};
    const UiCustomPanelsSnapshot panels{101, 2, ids};
    assert(succeeded(api->publish_custom_panels(&panels)));
    char type[] = "world_teleport.confirm";
    char title[] = "t";
    char option[] = "ok";
    const char* labels[] = {option};
    const UiCustomDialogSnapshot dialog{102, 7, type, title, labels, 1};
    assert(succeeded(api->publish_custom_dialog(&dialog)));

    g_native_screen = "panel_inventory";
    g_native_panel = "panel_inventory";
    g_native_dialog = "{\"type\":\"npc\",\"options\":[]}";
    g_frame_handler(3, nullptr);

    LayerCapture layers{};
    assert(succeeded(api->read_layers(&capture_layer, &layers)));
    assert(layers.ids.size() == 6);
    assert(layers.ids[0] == "world_teleport.confirm" &&
           layers.types[0] == static_cast<uint32_t>(UiLayerType::kDialog));
    assert(layers.ids[1] == "custom.b" &&
           layers.types[1] == static_cast<uint32_t>(UiLayerType::kPanel));
    assert(layers.ids[2] == "custom.a" &&
           layers.types[2] == static_cast<uint32_t>(UiLayerType::kPanel));
    assert(layers.ids[3] == "npc" &&
           layers.types[3] == static_cast<uint32_t>(UiLayerType::kDialog));
    assert(layers.ids[4] == "panel_inventory" &&
           layers.types[4] == static_cast<uint32_t>(UiLayerType::kPanel));
    assert(layers.ids[5] == "panel_inventory" &&
           layers.types[5] == static_cast<uint32_t>(UiLayerType::kOther));

    const UiCustomPanelsSnapshot clear_panels{103, 0, nullptr};
    assert(succeeded(api->publish_custom_panels(&clear_panels)));
    assert(succeeded(api->clear_custom_dialog(7, 104)));
    LayerCapture native{};
    assert(succeeded(api->read_layers(&capture_layer, &native)));
    assert(native.ids.size() == 3);
    assert(native.ids[0] == "npc");
    assert(native.ids[1] == "panel_inventory");
    assert(native.ids[2] == "panel_inventory");
}

void test_native_snapshot_failure_invalidates_all_queries(const UiStateApi* api) {
    g_native_screen = "world";
    g_native_panel = "";
    g_native_dialog = "{\"type\":\"native\",\"text\":\"fresh\"}";
    g_native_read_fails = false;
    g_frame_handler(5, nullptr);
    assert(query(HttpMethod::kGet, "/api/ui/screen") == "{\"screen\":\"world\"}");

    const char* panel_ids[] = {"pending.panel"};
    const UiCustomPanelsSnapshot panels{6, 1, panel_ids};
    assert(succeeded(api->publish_custom_panels(&panels)));
    const char* dialog_labels[] = {"Choose"};
    const UiCustomDialogSnapshot dialog{6, 3, "custom.confirm", "Title", dialog_labels, 1};
    assert(succeeded(api->publish_custom_dialog(&dialog)));

    g_native_read_fails = true;
    g_frame_handler(6, nullptr);
    for (const char* path : {"/api/ui/screen", "/api/ui/panel", "/api/ui/dialog"}) {
        uint16_t status = 0;
        assert(query(HttpMethod::kGet, path, &status) ==
               "{\"ok\":false,\"error\":\"snapshot not ready\"}");
        assert(status == 503);
    }

    g_native_read_fails = false;
    g_native_screen = "main_menu";
    g_native_panel = "";
    g_native_dialog = "{\"type\":\"fresh\",\"text\":\"new sample\"}";
    g_frame_handler(7, nullptr);
    assert(query(HttpMethod::kGet, "/api/ui/screen") == "{\"screen\":\"main_menu\"}");
    assert(query(HttpMethod::kGet, "/api/ui/panel") == "{\"panel\":\"panel_pending.panel\"}");
    assert(query(HttpMethod::kGet, "/api/ui/dialog") ==
           "{\"type\":\"custom.confirm\",\"text\":\"new sample\"}");
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
    sdk.ticket_result.defer_current_result = &defer_current_result;
    sdk.ticket_result.current_attempt = &current_attempt;
    sdk.commands.post_typed = &post_typed;

    const QolChoiceWritePort choice_port{{1, 0}, {&choice_write}};
    const QolUiWritePort ui_write_port{{1, 1},
                                       {&ui_write, &ui_write_step, &ui_write_cancel_pending}};
    const QolUiStateReadPort ui_read_port{{1, 0}, {&read_snapshot}};
    const void* ports[] = {&choice_port, &ui_write_port, &ui_read_port};
    PluginContext context{};
    context.sdk = &sdk;
    context.ports = ports;
    context.port_count = 3;
    const PluginDescriptor& descriptor = qol::features::core_ui_state::plugin_descriptor();
    assert(descriptor.provides_count == 1);
    assert(std::strcmp(descriptor.provides[0].id, kUiStateCapabilityId) == 0);
    assert(descriptor.provides[0].version.major == 1 && descriptor.provides[0].version.minor == 1);
    assert(std::strcmp(descriptor.optional_requires[0].id, kHttpRegistryCapabilityId) == 0);
    assert(std::strcmp(descriptor.optional_requires[1].id, kHttpDispatchCapabilityId) == 0);
    assert(descriptor.optional_requires[0].range.major == 1 &&
           descriptor.optional_requires[0].range.min_minor == 1);
    assert(descriptor.optional_requires[1].range.major == 1 &&
           descriptor.optional_requires[1].range.min_minor == 1);
    assert(descriptor.http_query_count == 0 && descriptor.http_queries == nullptr);
    assert(descriptor.http_endpoint_count == 8 && descriptor.http_endpoints != nullptr);
    assert(descriptor.http_route_metadata_count == 1 &&
           descriptor.http_route_metadata != nullptr);
    assert(descriptor.http_route_metadata[0].method == HttpMethod::kGet);
    assert(std::strcmp(descriptor.http_route_metadata[0].path, "/api/ui") == 0);
    assert(std::strcmp(descriptor.http_route_metadata[0].route_owner, "core.ui_state") == 0);
    assert(descriptor.http_route_metadata[0].op_required == 0 &&
           descriptor.http_route_metadata[0].lease_required == 1);
    assert(descriptor.ports[1].id == QolPortId::QOL_PORT_UI_WRITE &&
           descriptor.ports[1].range.major == 1 && descriptor.ports[1].range.min_minor == 1);
    assert(route(HttpMethod::kGet, "/api/ui") == nullptr);
    assert(descriptor.install(context) == 0);
    const auto* api = static_cast<const UiStateApi*>(descriptor.provides[0].iface);

    test_native_fallback_and_keysets(api);
    test_custom_dialog_generation_and_mapping(api);
    test_native_snapshot_failure_invalidates_all_queries(api);
    test_ui_write_ticket_retry();
    test_read_layers(api);

    descriptor.deactivate(context);
    assert(g_frame_handler == nullptr);
    assert(g_cancel_operation_id == 0 && g_cancel_order != 0 &&
           g_ui_unregister_order > g_cancel_order);
    std::puts("core_ui_state_tests: passed");
    return 0;
}
