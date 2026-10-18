#include "contracts/host_sdk.h"
#include "plugins/extension_bag/http/routes.h"
#include "plugins/extension_bag/model/virtual_bag_state.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>

namespace {

int g_pass = 0;
int g_fail = 0;
int g_post_count = 0;
std::array<uint8_t, qol::contracts::kTypedCommandPayloadCapacity> g_payload{};
uint32_t g_payload_size = 0;
std::string g_exit_result = "{\"ok\":false,\"error\":\"not in extension view\"}";
qol::contracts::QolHostSdk g_sdk{};

#define CHECK(condition)                                                                          \
    do {                                                                                          \
        if (condition) {                                                                          \
            ++g_pass;                                                                             \
        } else {                                                                                  \
            ++g_fail;                                                                             \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);                    \
        }                                                                                         \
    } while (false)

QolResult fake_post_typed(uint32_t handler_id, const void* payload, uint32_t size,
                          qol::contracts::QolCommandTicket* out_ticket) {
    if (handler_id != qol::features::extension_bag::kEbResultHandlerId || payload == nullptr ||
        size > g_payload.size() || out_ticket == nullptr) {
        return qol::contracts::error(kQolErrInvalidArgument, "invalid fake command", 21);
    }
    std::memcpy(g_payload.data(), payload, size);
    g_payload_size = size;
    *out_ticket = static_cast<qol::contracts::QolCommandTicket>(++g_post_count);
    return qol::contracts::ok();
}

const qol::contracts::HttpEndpointDescriptor* endpoint(std::string_view path) {
    uint32_t count = 0;
    const auto* endpoints = qol::features::extension_bag::extension_bag_http_endpoints(&count);
    for (uint32_t i = 0; i < count; ++i) {
        if (path == endpoints[i].path) return &endpoints[i];
    }
    return nullptr;
}

qol::contracts::HttpRes invoke_command(std::string_view path, std::string_view body,
                                       std::array<uint8_t, 256>* output) {
    const auto* route = endpoint(path);
    CHECK(route != nullptr);
    if (route == nullptr || route->command_handler == nullptr || output == nullptr) {
        return qol::contracts::HttpRes{nullptr, 0, 0, 0};
    }
    const qol::contracts::HttpReq request{
        qol::contracts::HttpMethod::kPost,
        route->path,
        static_cast<uint32_t>(std::strlen(route->path)),
        body.empty() ? nullptr : reinterpret_cast<const uint8_t*>(body.data()),
        static_cast<uint32_t>(body.size()),
        nullptr,
        0,
        nullptr,
        0,
    };
    qol::contracts::HttpRes response{output->data(), static_cast<uint32_t>(output->size()), 0, 0};
    (void)route->command_handler(&request, &response, route->ctx);
    return response;
}

void expect_bad_request(std::string_view path, std::string_view body) {
    const int posts_before = g_post_count;
    std::array<uint8_t, 256> output{};
    const auto response = invoke_command(path, body, &output);
    CHECK(response.status == 400);
    CHECK(response.body_size > 0);
    CHECK(g_post_count == posts_before);
}

void expect_queued(std::string_view path, std::string_view body) {
    std::array<uint8_t, 256> output{};
    const auto response = invoke_command(path, body, &output);
    CHECK(response.status == 200);
    CHECK(std::string_view(reinterpret_cast<const char*>(output.data()), response.body_size) ==
          std::string("{\"ok\":true,\"ticket\":") + std::to_string(g_post_count) + "}");
}

void test_endpoint_catalog_and_validation() {
    using qol::contracts::HttpHandlerKind;
    using qol::contracts::HttpMethod;
    uint32_t count = 0;
    const auto* endpoints = qol::features::extension_bag::extension_bag_http_endpoints(&count);
    CHECK(endpoints != nullptr);
    CHECK(count == 8);
    int queries = 0;
    int commands = 0;
    for (uint32_t i = 0; i < count; ++i) {
        if (endpoints[i].kind == HttpHandlerKind::kQuery) {
            ++queries;
            CHECK(endpoints[i].method == HttpMethod::kGet);
            CHECK(endpoints[i].query_handler != nullptr);
            CHECK(endpoints[i].command_handler == nullptr);
        } else {
            ++commands;
            CHECK(endpoints[i].method == HttpMethod::kPost);
            CHECK(endpoints[i].query_handler == nullptr);
            CHECK(endpoints[i].command_handler != nullptr);
        }
    }
    CHECK(queries == 1);
    CHECK(commands == 7);

    const auto* status = endpoint("/api/debug/extension_bag/status");
    CHECK(status != nullptr);
    if (status != nullptr && status->query_handler != nullptr) {
        std::array<uint8_t, 128> status_output{};
        const qol::contracts::HttpReq status_request{
            HttpMethod::kGet, status->path, static_cast<uint32_t>(std::strlen(status->path)),
            nullptr, 0, nullptr, 0, nullptr, 0};
        qol::contracts::HttpRes status_response{
            status_output.data(), static_cast<uint32_t>(status_output.size()), 0, 0};
        (void)status->query_handler(&status_request, &status_response, status->ctx);
        CHECK(status_response.status == 200);
        CHECK(std::string_view(reinterpret_cast<const char*>(status_output.data()),
                               status_response.body_size) == "{\"enabled\":true}");
    }

    expect_bad_request("/api/debug/extension_bag/enter_view", "{}");
    expect_bad_request("/api/debug/extension_bag/enter_view", "{\"bag\":11}");
    expect_bad_request("/api/debug/extension_bag/unequip", "{}");
    expect_bad_request("/api/debug/extension_bag/unequip", "{\"bag\":5}");
    expect_bad_request("/api/debug/extension_bag/select_bag", "{}");
    expect_bad_request("/api/debug/extension_bag/select_bag", "{\"bag\":6.5}");
    expect_bad_request("/api/debug/extension_bag/click_item", "{\"bag\":6}");
    expect_bad_request("/api/debug/extension_bag/click_item", "{\"bag\":6,\"slot\":16}");
    expect_bad_request("/api/debug/extension_bag/equip", "{\"bagType\":1}");
    expect_bad_request("/api/debug/extension_bag/equip", "{\"index\":5,\"bagType\":1}");
    expect_bad_request("/api/debug/extension_bag/equip", "{\"index\":0,\"bagType\":0}");
    expect_bad_request("/api/debug/extension_bag/item", "{\"slot\":0,\"category\":1,\"count\":1}");
    expect_bad_request("/api/debug/extension_bag/item", "{\"index\":0,\"category\":1,\"count\":1}");
    expect_bad_request("/api/debug/extension_bag/item", "{\"index\":0,\"slot\":0,\"count\":1}");
    expect_bad_request("/api/debug/extension_bag/item", "{\"index\":0,\"slot\":0,\"category\":1}");
    expect_bad_request("/api/debug/extension_bag/item",
                       "{\"index\":5,\"slot\":0,\"category\":1,\"count\":1}");
    expect_bad_request("/api/debug/extension_bag/item",
                       "{\"index\":0,\"slot\":16,\"category\":1,\"count\":1}");
    expect_bad_request("/api/debug/extension_bag/item",
                       "{\"index\":0,\"slot\":0,\"category\":-1,\"count\":1}");
    expect_bad_request("/api/debug/extension_bag/item",
                       "{\"index\":0,\"slot\":0,\"category\":1,\"count\":-1}");

    expect_queued("/api/debug/extension_bag/enter_view", "{\"bag\":6}");
    expect_queued("/api/debug/extension_bag/exit_view", "");
    expect_queued("/api/debug/extension_bag/select_bag", "{\"bag\":10}");
    expect_queued("/api/debug/extension_bag/unequip", "{\"bag\":6}");
    expect_queued("/api/debug/extension_bag/click_item", "{\"bag\":6,\"slot\":15}");
    expect_queued("/api/debug/extension_bag/equip", "{\"index\":4,\"bagType\":4}");
    expect_queued("/api/debug/extension_bag/item",
                  "{\"index\":0,\"slot\":0,\"category\":0,\"count\":0}");
}

void test_business_error_is_returned_by_ticket_result() {
    std::array<uint8_t, 256> response_body{};
    const auto response = invoke_command("/api/debug/extension_bag/exit_view", "", &response_body);
    CHECK(response.status == 200);
    CHECK(g_payload_size > 0);

    std::array<uint8_t, 256> result_body{};
    uint32_t result_size = 0;
    const bool succeeded = qol::features::extension_bag::eb_result_handler(
        nullptr, g_payload.data(), g_payload_size,
        reinterpret_cast<char*>(result_body.data()), static_cast<uint32_t>(result_body.size()),
        &result_size);
    CHECK(!succeeded);
    CHECK(std::string_view(reinterpret_cast<const char*>(result_body.data()), result_size) ==
          g_exit_result);

    g_exit_result = "{\"ok\":true}";
    result_size = 0;
    const bool success = qol::features::extension_bag::eb_result_handler(
        nullptr, g_payload.data(), g_payload_size,
        reinterpret_cast<char*>(result_body.data()), static_cast<uint32_t>(result_body.size()),
        &result_size);
    CHECK(success);
    CHECK(std::string_view(reinterpret_cast<const char*>(result_body.data()), result_size) ==
          g_exit_result);
}

void test_pure_exit_view_state_transition() {
    using virtual_bag::ExitViewResult;
    using virtual_bag::Mode;
    using virtual_bag::State;

    State state{};
    state.mode = Mode::kModule;
    state.selected = 2;
    state.inspected = 4;
    state.info_bag = 2;
    CHECK(virtual_bag::exit_module_view(&state, 5) == ExitViewResult::kExited);
    CHECK(state.mode == Mode::kOriginal);
    CHECK(state.original_selected == 5);
    CHECK(state.selected == -1);
    CHECK(state.inspected == -1);
    CHECK(state.info_bag == -1);

    state.mode = Mode::kExitingModule;
    state.selected = 1;
    state.inspected = 3;
    state.info_bag = 1;
    CHECK(virtual_bag::exit_module_view(&state, 4) == ExitViewResult::kExitInProgress);
    CHECK(state.mode == Mode::kExitingModule);
    CHECK(state.selected == 1);
    CHECK(state.inspected == 3);
    CHECK(state.info_bag == 1);

    state.mode = Mode::kOriginal;
    state.selected = -1;
    CHECK(virtual_bag::exit_module_view(&state, 0) == ExitViewResult::kNotInModule);
    CHECK(virtual_bag::exit_module_view(nullptr, 0) == ExitViewResult::kNotInModule);
}

}  // namespace

const qol::contracts::QolHostSdk* qol::features::extension_bag::extension_bag_http_host_sdk() {
    return &g_sdk;
}

std::string extension_bag_api_status_json_impl() { return "{\"enabled\":true}"; }
std::string data_op_extension_bag_enter_view(int) { return "{\"ok\":true}"; }
std::string data_op_extension_bag_exit_view() { return g_exit_result; }
std::string data_op_extension_bag_unequip(int) { return "{\"ok\":true}"; }
std::string data_op_extension_bag_select_bag(int) { return "{\"ok\":true}"; }
std::string data_op_extension_bag_click_item(int, int) { return "{\"ok\":true}"; }
std::string data_virtual_bag_test_equip(int, int) { return "{\"ok\":true}"; }
std::string data_virtual_bag_test_item(int, int, int, int) { return "{\"ok\":true}"; }

int main() {
    g_sdk.commands.post_typed = &fake_post_typed;
    test_endpoint_catalog_and_validation();
    test_business_error_is_returned_by_ticket_result();
    test_pure_exit_view_state_transition();
    std::printf("extension_bag_debug_api_tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
