// /api/op/features/lifecycle 试点迁移的响应形状 host 测试。
//
// 校验 owner command handler 的成功 / 失败 / 未知动作三类响应与迁移前 Kotlin 逐字节一致，
// 并确认 action 映射与命令键经 QolCommands.post 正确投递。
#include "plugins/core.api/plugin.h"

#include "contracts/commands.h"
#include "contracts/host_sdk.h"

#include <cstdio>
#include <cstring>
#include <string>

using qol::contracts::HttpCommandHandlerFn;
using qol::contracts::HttpMethod;
using qol::contracts::HttpReq;
using qol::contracts::HttpRes;
using qol::contracts::HttpRoute;
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

QolResult fake_register_route(const HttpRoute*) {
    return qol::contracts::ok();
}

QolResult fake_register_command(HttpMethod, const char*, const char*, HttpCommandHandlerFn, void*) {
    return qol::contracts::ok();
}

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
    sdk.http.register_route = &fake_register_route;
    sdk.http_dispatch.register_command = &fake_register_command;
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
    call("{\"id\":\"text_layer\",\"action\":\"activate\"}", storage, sizeof(storage), &response,
         &result);
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

}  // namespace

int main() {
    test_responses();
    std::printf("core_api_lifecycle: pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
