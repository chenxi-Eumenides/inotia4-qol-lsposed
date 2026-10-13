// /api/op/features/lifecycle 响应兼容测试辅助。
//
// 校验 body parser 的成功 / 失败 / 未知动作三类响应与迁移前 Kotlin 逐字节一致，
// 并确认 action 映射与命令键经 QolCommands.post 正确投递。
#include "plugins/core.api/plugin.h"

#include "contracts/commands.h"
#include "contracts/host_sdk.h"

#include <cstdio>
#include <cstring>
#include <string>

using qol::contracts::HttpMethod;
using qol::contracts::HttpReq;
using qol::contracts::HttpRes;
using qol::contracts::QolCommand;
using qol::contracts::QolCommandTicket;

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond) do { \
    if (cond) ++g_pass; \
    else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

namespace {

bool g_post_ok = true;
QolCommandTicket g_next_ticket = 0;
uint32_t g_last_kind = 0;
int32_t g_last_action = 0;
std::string g_last_key;

QolResult fake_post(QolCommandTicket* out, const QolCommand* command) {
    g_last_kind = command->kind;
    g_last_action = command->int_value;
    g_last_key = command->key;
    if (!g_post_ok) return qol::contracts::error(kQolErrBlocked, "blocked", 7);
    if (out != nullptr) *out = g_next_ticket;
    return qol::contracts::ok();
}

std::string body_of(const HttpRes& response) {
    return std::string(reinterpret_cast<const char*>(response.body), response.body_size);
}

constexpr const char* kLifecyclePath = "/api/op/features/lifecycle";

void call(const char* body, uint8_t* storage, uint32_t capacity, HttpRes* response,
          QolResult* result) {
    const HttpReq request{HttpMethod::kPost,
                          kLifecyclePath,
                          static_cast<uint32_t>(std::strlen(kLifecyclePath)),
                          reinterpret_cast<const uint8_t*>(body),
                          static_cast<uint32_t>(std::strlen(body)),
                          nullptr,
                          0,
                          nullptr,
                          0};
    *response = HttpRes{storage, capacity, 0, 0};
    *result = qol::features::core_api::lifecycle_command(&request, response, nullptr);
}

void test_responses() {
    qol::contracts::QolHostSdk sdk{};
    sdk.commands.post = &fake_post;
    qol::contracts::PluginContext ctx{sizeof(qol::contracts::PluginContext),
                                      qol::contracts::kHostSdkMajor,
                                      qol::contracts::kHostSdkMinor,
                                      &sdk,
                                      nullptr,
                                      "core.api",
                                      nullptr,
                                      0};
    const qol::contracts::PluginDescriptor& descriptor =
        qol::features::core_api::plugin_descriptor();
    CHECK(descriptor.install(ctx) == 0);

    uint8_t storage[256]{};
    HttpRes response{};
    QolResult result{};

    // 非法 body → 400 bad request（与 Kotlin JsonUtil.parseBody 失败一致）。
    call("not-json", storage, sizeof(storage), &response, &result);
    CHECK(response.status == 400);
    CHECK(body_of(response) == "{\"ok\":false,\"error\":\"bad request\"}");

    // 缺 action → 400 id and action required。
    call("{\"id\":\"simple_mode\"}", storage, sizeof(storage), &response, &result);
    CHECK(response.status == 400);
    CHECK(body_of(response) == "{\"ok\":false,\"error\":\"id and action required\"}");

    // 未知动作 → HTTP 200 失败信封。
    call("{\"id\":\"simple_mode\",\"action\":\"explode\"}", storage, sizeof(storage), &response,
         &result);
    CHECK(response.status == 200);
    CHECK(body_of(response) == "{\"ok\":false,\"error\":\"unknown action\"}");

    // deactivate → ticket 响应，映射 3。
    g_post_ok = true;
    g_next_ticket = 7;
    call("{\"id\":\"simple_mode\",\"action\":\"deactivate\"}", storage, sizeof(storage), &response,
         &result);
    CHECK(response.status == 200);
    CHECK(body_of(response) == "{\"ok\":true,\"ticket\":7}");
    CHECK(g_last_kind == qol::contracts::kCommandFeatureLifecycle);
    CHECK(g_last_action == qol::contracts::kFeatureLifecycleDeactivate);
    CHECK(g_last_key == "simple_mode");

    // activate → 映射 4。
    g_next_ticket = 8;
    call("{\"id\":\"character_panel_i18n\",\"action\":\"activate\"}", storage, sizeof(storage),
         &response, &result);
    CHECK(body_of(response) == "{\"ok\":true,\"ticket\":8}");
    CHECK(g_last_action == qol::contracts::kFeatureLifecycleActivate);

    // 投递失败 → HTTP 200 lifecycle rejected。
    g_post_ok = false;
    call("{\"id\":\"simple_mode\",\"action\":\"deactivate\"}", storage, sizeof(storage), &response,
         &result);
    CHECK(response.status == 200);
    CHECK(body_of(response) == "{\"ok\":false,\"error\":\"lifecycle rejected\"}");

    descriptor.deactivate(ctx);
}

QolResult validate_config(const char* key, const std::string& json) {
    const auto& descriptor = qol::features::core_api::plugin_descriptor();
    CHECK(descriptor.validate_config != nullptr);
    return descriptor.validate_config(
        key, json.data(), static_cast<uint32_t>(json.size()));
}

void test_api_feature_and_listener_config() {
    const auto& descriptor = qol::features::core_api::plugin_descriptor();
    CHECK(std::strcmp(descriptor.id, "core.api") == 0);
    CHECK(descriptor.feature_count == 1);
    CHECK(descriptor.features[0].default_enabled == 1);
    CHECK(descriptor.features[0].category == qol::contracts::FeatureCategory::kHidden);
    CHECK(descriptor.features[0].type == qol::contracts::FeatureType::kToggle);
    CHECK(std::strcmp(descriptor.features[0].id, "api.enabled") == 0);
    CHECK(std::strcmp(descriptor.features[0].description, "") == 0);
    CHECK(descriptor.validate_config != nullptr);
    CHECK(descriptor.http_endpoint_count == 0);
    CHECK(descriptor.http_query_count == 0);
    CHECK(descriptor.http_route_metadata_count == 5);
    CHECK(descriptor.http_route_metadata != nullptr);
    const char* const expected_routes[] = {
        "/api/health", "/api/system/features", "/api/system/resources",
        "/api/system/tickets/{id}", "/api/system/routes",
    };
    for (uint32_t index = 0; index < descriptor.http_route_metadata_count; ++index) {
        const auto& route = descriptor.http_route_metadata[index];
        CHECK(route.method == qol::contracts::HttpMethod::kGet);
        CHECK(std::strcmp(route.path, expected_routes[index]) == 0);
        CHECK(std::strcmp(route.route_owner, "core.api") == 0);
        CHECK(route.op_required == 0);
        CHECK(route.lease_required == (index == 0 || index == 4 ? 0 : 1));
    }
    if (descriptor.feature_count == 1) {
        const auto& feature = descriptor.features[0];
        CHECK(std::strcmp(feature.id, "api.enabled") == 0);
        CHECK(feature.default_enabled == 1);
        CHECK(feature.category == qol::contracts::FeatureCategory::kHidden);
        CHECK(feature.type == qol::contracts::FeatureType::kToggle);
        CHECK(std::strcmp(feature.name, "API服务") == 0);
        CHECK(std::strcmp(feature.description, "") == 0);
    }

    CHECK(qol::contracts::succeeded(validate_config("listen_address", "\"0.0.0.0\"")));
    CHECK(qol::contracts::succeeded(validate_config("listen_address", "\"192.0.2.1\"")));
    CHECK(!qol::contracts::succeeded(validate_config("listen_address", "123")));
    CHECK(!qol::contracts::succeeded(validate_config("listen_address", "\" \\t \"")));
    CHECK(!qol::contracts::succeeded(validate_config("listen_address", "\"\\u0020\"")));
    CHECK(!qol::contracts::succeeded(validate_config("listen_address", "\"\\uD800\"")));
    CHECK(qol::contracts::succeeded(validate_config(
        "listen_address", "\"\\uD83D\\uDE00\"")));
    std::string invalid_utf8{"\""};
    invalid_utf8.push_back(static_cast<char>(0xc0));
    invalid_utf8.push_back(static_cast<char>(0x80));
    invalid_utf8.push_back('"');
    CHECK(!qol::contracts::succeeded(validate_config("listen_address", invalid_utf8)));

    CHECK(qol::contracts::succeeded(validate_config("listen_port", "1")));
    CHECK(qol::contracts::succeeded(validate_config("listen_port", "8088")));
    CHECK(qol::contracts::succeeded(validate_config("listen_port", "65535")));
    CHECK(!qol::contracts::succeeded(validate_config("listen_port", "0")));
    CHECK(!qol::contracts::succeeded(validate_config("listen_port", "65536")));
    CHECK(!qol::contracts::succeeded(validate_config("listen_port", "8088.0")));
    CHECK(!qol::contracts::succeeded(validate_config("listen_port", "\"8088\"")));

    // Unknown keys stay generic JSON config; this validator only owns listener keys.
    CHECK(qol::contracts::succeeded(validate_config("custom_key", "{\"any\":true}")));
}

}  // namespace

int main() {
    test_api_feature_and_listener_config();
    test_responses();
    std::printf("core_api_lifecycle: pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
