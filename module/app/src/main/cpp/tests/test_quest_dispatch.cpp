// quest HTTP dispatch 接缝 host 测试：
// 覆盖 4 条 /api/quest* GET query handler 的注册、快照发布前的 not-ready、发布后的富化响应
// （静态片段按迁移前注入顺序拼接）、非 world 错误体、owner 注销墓碑。
//
// 真实 registry/dispatch（host/http_registry.cpp、host/http_dispatch.cpp）参与链接，
// 游戏侧只以 fake QolQuestPort 与 fake WorldFactsApi 注入，避免依赖真机内存。

#include "contracts/game_ports/ports.h"
#include "contracts/game_ports/quest_port.h"
#include "contracts/host_sdk.h"
#include "contracts/http.h"
#include "contracts/result.h"
#include "contracts/world_facts.h"
#include "host/http_dispatch.h"
#include "host/http_registry.h"
#include "infra/commands.h"
#include "plugins/quest/plugin.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond) do { \
    if (cond) ++g_pass; \
    else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

namespace {

using qol::contracts::QolQuestPort;
using qol::contracts::QolQuestReadOnlyApi;

bool g_in_world = true;
int32_t g_static_ready = 1;

// 活动槽：slot0 → quest 5（state=2 可交付）；状态表：5=2、2=3、7=3，其余 0。
uint32_t g_active_ids[] = {5};
int32_t g_active_states[] = {2};
constexpr uint32_t kTotal = 10;

std::unordered_map<uint32_t, std::string>& fragments(int32_t endpoint) {
    static std::unordered_map<uint32_t, std::string> tables[3];
    return tables[endpoint];
}

QolResult fake_state(int32_t* out) {
    *out = g_in_world ? 5 : 4;
    return qol::contracts::ok();
}

QolResult fake_active_slot_count(uint32_t* out) {
    *out = 1;
    return qol::contracts::ok();
}

QolResult fake_active_slot_id(uint32_t index, uint32_t* out) {
    if (index != 0) return qol::contracts::error(kQolErrNotFound, "no slot", 7);
    *out = g_active_ids[index];
    return qol::contracts::ok();
}

QolResult fake_total_count(uint32_t* out) {
    *out = kTotal;
    return qol::contracts::ok();
}

QolResult fake_quest_state(uint32_t quest_id, int32_t* out) {
    if (quest_id == 5) *out = g_active_states[0];
    else if (quest_id == 2 || quest_id == 7) *out = 3;
    else *out = 0;
    return qol::contracts::ok();
}

QolResult fake_is_complete(uint32_t, int32_t* out) {
    *out = 0;
    return qol::contracts::ok();
}

QolResult fake_fragment(int32_t endpoint, uint32_t quest_id, const char** out) {
    if (endpoint < 0 || endpoint >= 3 || out == nullptr) {
        return qol::contracts::error(kQolErrInvalidArgument, "bad", 3);
    }
    const auto& table = fragments(endpoint);
    const auto it = table.find(quest_id);
    if (it == table.end()) return qol::contracts::error(kQolErrNotFound, "missing", 7);
    *out = it->second.c_str();
    return qol::contracts::ok();
}

QolResult fake_static_ready(int32_t* out) {
    *out = g_static_ready;
    return qol::contracts::ok();
}

QolResult fake_set_state(uint32_t, int32_t) { return qol::contracts::ok(); }

QolResult fake_quit_result(int32_t, char* out, uint32_t capacity, uint32_t* out_size) {
    static const char kJson[] = "{\"ok\":true,\"state\":{\"money\":0}}";
    const uint32_t len = sizeof(kJson) - 1;
    if (out == nullptr || out_size == nullptr || capacity < len) {
        return qol::contracts::error(1, "small", 5);
    }
    std::memcpy(out, kJson, len);
    *out_size = len;
    return qol::contracts::ok();
}

const QolQuestPort kFakePort{
    qol::contracts::kQolPortQuestVersion,
    {&fake_state, &fake_active_slot_count, &fake_active_slot_id, &fake_total_count,
     &fake_quest_state, &fake_is_complete, &fake_fragment, &fake_static_ready},
    {&fake_set_state, &fake_quit_result},
};

const void* const kPortArray[] = {&kFakePort};

const qol::contracts::WorldFactsApi kWorldFactsApi{
    &fake_state, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
};

QolResult fake_capability_require(const char* id, QolVersionRange, const void** out_iface) {
    if (id == nullptr || out_iface == nullptr ||
        std::strcmp(id, qol::contracts::kWorldFactsCapabilityId) != 0) {
        return qol::contracts::error(kQolErrNotFound, "missing capability", 18);
    }
    *out_iface = &kWorldFactsApi;
    return qol::contracts::ok();
}

QolResult fake_register_typed_handler(uint32_t, qol::contracts::QolTypedCommandHandler, void*) {
    return qol::contracts::ok();
}
QolResult fake_unregister_typed_handler(uint32_t) { return qol::contracts::ok(); }

// 票据结果面：本测试不校验结果通道（由 infra_config_commands_tests 覆盖），只保证 install 守卫通过。
QolResult fake_register_typed_result_handler(uint32_t,
                                             qol::contracts::QolTypedResultCommandHandler, void*) {
    return qol::contracts::ok();
}
QolResult fake_unregister_typed_result_handler(uint32_t) { return qol::contracts::ok(); }
QolResult fake_read_result(qol::contracts::QolCommandTicket, char*, uint32_t, uint32_t*) {
    return qol::contracts::error(kQolErrNotFound, "no result", 9);
}
int32_t fake_ticket_state(qol::contracts::QolCommandTicket) { return 0; }

struct FakeScheduler {
    qol::contracts::QolFrameHandler handler = nullptr;
    void* ctx = nullptr;
    int32_t interval = 0;
};

FakeScheduler g_scheduler;

QolResult fake_log_write(int32_t, int32_t, const char*, const char*, uint32_t) {
    return qol::contracts::ok();
}

QolResult fake_scheduler_add(int32_t, qol::contracts::QolFrameHandler handler, void* ctx,
                             int32_t interval, int32_t, uint64_t* out_id) {
    g_scheduler.handler = handler;
    g_scheduler.ctx = ctx;
    g_scheduler.interval = interval;
    *out_id = 1;
    return qol::contracts::ok();
}

QolResult fake_scheduler_remove(uint64_t) {
    g_scheduler.handler = nullptr;
    g_scheduler.ctx = nullptr;
    return qol::contracts::ok();
}

qol::contracts::QolHostSdk make_sdk() {
    qol::contracts::QolHostSdk sdk{};
    sdk.abi = {sizeof(qol::contracts::QolHostSdk), qol::contracts::kHostSdkMajor,
               qol::contracts::kHostSdkMinor};
    sdk.log = {&fake_log_write, nullptr};
    sdk.scheduler = {&fake_scheduler_add, &fake_scheduler_remove};
    sdk.capabilities.require = &fake_capability_require;
    sdk.commands.register_typed_handler = &fake_register_typed_handler;
    sdk.commands.unregister_typed_handler = &fake_unregister_typed_handler;
    sdk.http = qol::host::http_registry_api();
    sdk.http_dispatch = qol::host::http_dispatch_api();
    sdk.ticket_result = {&fake_register_typed_result_handler,
                         &fake_unregister_typed_result_handler, &fake_read_result,
                         &fake_ticket_state};
    return sdk;
}

qol::contracts::QolGameHost fake_game() {
    qol::contracts::QolGameHost game{};
    game.abi = {sizeof(qol::contracts::QolGameHost), qol::contracts::kGameHostMajor,
                qol::contracts::kGameHostMinor};
    return game;
}

const qol::contracts::HttpRoute kRoutes[] = {
    {qol::contracts::HttpMethod::kGet, "/api/quest", "quest", 0, 1},
    {qol::contracts::HttpMethod::kGet, "/api/quest/active", "quest", 0, 1},
    {qol::contracts::HttpMethod::kGet, "/api/quest/details", "quest", 0, 1},
    {qol::contracts::HttpMethod::kGet, "/api/quest/completed", "quest", 0, 1},
    {qol::contracts::HttpMethod::kPost, "/api/quest/quit_quest", "quest", 0, 1},
};

void register_frozen_routes() {
    for (const auto& route : kRoutes) {
        CHECK(qol::host::http_registry_api().register_route(&route).ok == kQolOk);
    }
}

std::string dispatch_get(const char* path, uint16_t* status = nullptr) {
    const std::string path_value(path);
    char storage[8192]{};
    qol::contracts::HttpReq request{
        qol::contracts::HttpMethod::kGet,
        path_value.c_str(),
        static_cast<uint32_t>(path_value.size()),
        nullptr,
        0,
        nullptr,
        0,
        nullptr,
        0,
    };
    qol::contracts::HttpRes response{reinterpret_cast<uint8_t*>(storage), sizeof(storage), 0, 0};
    (void)qol::host::http_dispatch_api().dispatch(&request, &response);
    if (status != nullptr) *status = response.status;
    return std::string(storage, response.body_size);
}

void test_lifecycle_and_responses() {
    qol::host::http_dispatch_reset_for_test();
    qol::host::http_registry_reset_for_test();
    g_scheduler = FakeScheduler{};
    g_in_world = true;
    g_static_ready = 1;
    fragments(qol::contracts::kQuestFragmentActive)[5] = "\"id_name\":\"Q5\",\"group_id\":4";
    fragments(qol::contracts::kQuestFragmentDetails)[5] =
        "\"group_id\":4,\"name\":\"Q5\"";
    fragments(qol::contracts::kQuestFragmentCompleted)[2] = "\"name\":\"Q2\"";
    fragments(qol::contracts::kQuestFragmentCompleted)[7] = "\"name\":\"Q7\"";
    register_frozen_routes();

    const qol::contracts::QolHostSdk sdk = make_sdk();
    qol::contracts::QolGameHost game = fake_game();
    qol::contracts::PluginContext ctx{sizeof(qol::contracts::PluginContext),
                                      qol::contracts::kHostSdkMajor,
                                      qol::contracts::kHostSdkMinor, &sdk, &game, "quest",
                                      kPortArray, 1};
    const auto& descriptor = qol::features::quest::plugin_descriptor();

    CHECK(descriptor.install(ctx) == 0);
    CHECK(g_scheduler.handler != nullptr);
    descriptor.on_enabled(ctx);
    CHECK(qol::host::http_dispatch_register_queries(descriptor.http_queries,
                                                     descriptor.http_query_count).ok == kQolOk);
    CHECK(g_scheduler.interval > 1);

    uint16_t status = 0;
    (void)dispatch_get("/api/quest/active", &status);
    CHECK(status == 503);

    CHECK(g_scheduler.handler(100, g_scheduler.ctx) != 0);

    CHECK(dispatch_get("/api/quest/active", &status) ==
          "{\"quests\":[{\"slot\":0,\"quest_id\":5,\"deliverable\":true,"
          "\"id_name\":\"Q5\",\"group_id\":4}]}");
    CHECK(status == 200);
    CHECK(dispatch_get("/api/quest/details") ==
          "{\"quests\":[{\"slot\":0,\"quest_id\":5,\"deliverable\":true,"
          "\"group_id\":4,\"name\":\"Q5\"}]}");
    CHECK(dispatch_get("/api/quest/completed") ==
          "{\"quests\":[{\"quest_id\":2,\"name\":\"Q2\"},{\"quest_id\":7,\"name\":\"Q7\"}]}");
    CHECK(dispatch_get("/api/quest") ==
          "{\"active\":[{\"slot\":0,\"quest_id\":5,\"deliverable\":true,"
          "\"id_name\":\"Q5\",\"group_id\":4}],"
          "\"details\":[{\"slot\":0,\"quest_id\":5,\"deliverable\":true,"
          "\"group_id\":4,\"name\":\"Q5\"}],"
          "\"completed\":[{\"quest_id\":2,\"name\":\"Q2\"},{\"quest_id\":7,\"name\":\"Q7\"}]}");

    // 非 world：发布与迁移前一致的 not in game 错误体（HTTP 200）。
    g_in_world = false;
    CHECK(g_scheduler.handler(115, g_scheduler.ctx) != 0);
    CHECK(dispatch_get("/api/quest", &status) == "{\"error\":\"not in game\"}");
    CHECK(status == 200);
    g_in_world = true;
    CHECK(g_scheduler.handler(130, g_scheduler.ctx) != 0);
    CHECK(dispatch_get("/api/quest/active").find("\"quest_id\":5") != std::string::npos);

    // owner 停用：handler 墓碑，请求返回明确停用错误。
    CHECK(qol::host::http_dispatch_api().unregister_owner("quest").ok == kQolOk);
    descriptor.on_disabled(ctx);
    CHECK(g_scheduler.handler == nullptr);
    (void)dispatch_get("/api/quest/active", &status);
    CHECK(status == 503);

    descriptor.deactivate(ctx);
}

void test_static_not_ready_holds_snapshot() {
    qol::host::http_dispatch_reset_for_test();
    qol::host::http_registry_reset_for_test();
    g_scheduler = FakeScheduler{};
    g_in_world = true;
    g_static_ready = 0;
    register_frozen_routes();

    const qol::contracts::QolHostSdk sdk = make_sdk();
    qol::contracts::QolGameHost game = fake_game();
    qol::contracts::PluginContext ctx{sizeof(qol::contracts::PluginContext),
                                      qol::contracts::kHostSdkMajor,
                                      qol::contracts::kHostSdkMinor, &sdk, &game, "quest",
                                      kPortArray, 1};
    const auto& descriptor = qol::features::quest::plugin_descriptor();
    CHECK(descriptor.install(ctx) == 0);
    descriptor.on_enabled(ctx);
    CHECK(qol::host::http_dispatch_register_queries(descriptor.http_queries,
                                                     descriptor.http_query_count).ok == kQolOk);
    CHECK(g_scheduler.handler(1, g_scheduler.ctx) != 0);
    uint16_t status = 0;
    (void)dispatch_get("/api/quest/active", &status);
    CHECK(status == 503);
    g_static_ready = 1;
    CHECK(g_scheduler.handler(20, g_scheduler.ctx) != 0);
    CHECK(dispatch_get("/api/quest/active", &status).find("\"quest_id\":5") != std::string::npos);
    CHECK(status == 200);
    descriptor.deactivate(ctx);
}

}  // namespace

int main() {
    test_lifecycle_and_responses();
    test_static_not_ready_holds_snapshot();
    std::printf("quest_dispatch: pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
