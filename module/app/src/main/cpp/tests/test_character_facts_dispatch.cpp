// character_facts HTTP dispatch 接缝 host 测试：
// 覆盖 15 条 /api/character/* GET query handler 的注册、快照发布前的 not-ready、发布后的
// 逐路由响应（含 leader/status 的多源组合与 404 语义）、非 world 态的统一 error 体、
// owner 注销后的 handler 停用。
//
// 真实 registry/dispatch（host/http_registry.cpp、host/http_dispatch.cpp）参与链接，
// 游戏侧只以 fake QolCharacterHttpPort/QolCharacterPort 注入；富化拼装在 legacy
// game_system.cpp，本测试只验证快照/切片/组合接缝。

#include "contracts/game_ports/character_http_port.h"
#include "contracts/game_ports/character_port.h"
#include "contracts/game_ports/ports.h"
#include "contracts/host_sdk.h"
#include "contracts/http.h"
#include "contracts/result.h"
#include "host/http_dispatch.h"
#include "host/http_registry.h"
#include "plugins/character_facts/plugin.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond) do { \
    if (cond) ++g_pass; \
    else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

namespace {

using qol::contracts::QolCharacterHttpJsonKind;
using qol::contracts::QolCharacterHttpPort;
using qol::contracts::QolCharacterHttpPortReadApi;

QolResult write_json(const char* text, char* out, uint32_t capacity, uint32_t* out_size) {
    if (out_size != nullptr) *out_size = 0;
    const uint32_t size = static_cast<uint32_t>(std::strlen(text));
    if (out == nullptr || out_size == nullptr || size >= capacity) {
        return qol::contracts::error(kQolErrBlocked, "tiny buffer", 11);
    }
    std::memcpy(out, text, size);
    out[size] = '\0';
    *out_size = size;
    return qol::contracts::ok();
}

constexpr char kPlayer[] = "{\"money\":100,\"leader_slot\":1,\"party_count\":3}";
constexpr char kParty[] =
    "[{\"name\":\"凯恩\",\"class_name\":\"黑暗骑士\",\"level\":10,\"hp\":50,\"max_hp\":60,"
    "\"mp\":5,\"max_mp\":6,\"exp\":1,\"exp_next\":2,\"status_point\":3,"
    "\"equipment\":[null,{\"slot\":1,\"name\":\"钢铁 剑\"}],\"type\":0,\"class_idx\":0,"
    "\"name_id\":7,\"stats\":{\"0\":1}},null]";
constexpr char kSkills[] =
    "[{\"role\":0,\"skills\":[{\"action_id\":10,\"level\":1,\"skill_name\":\"火球\"}],"
    "\"unlock_bitmap\":0,\"active_skill_id\":-1,\"skill_points\":4},null]";
constexpr char kMercenaries[] =
    "[{\"slot\":0,\"type\":1,\"name\":\"A\"},{\"slot\":2,\"type\":1,\"name\":\"B\"}]";
constexpr char kMember0[] =
    "{\"name\":\"凯恩\",\"class_name\":\"黑暗骑士\",\"level\":10,\"hp\":50,\"max_hp\":60,"
    "\"mp\":5,\"max_mp\":6,\"exp\":1,\"exp_next\":2,\"status_point\":3,"
    "\"equipment\":[null,{\"slot\":1,\"name\":\"钢铁 剑\"}],\"type\":0,\"class_idx\":0,"
    "\"name_id\":7,\"stats\":{\"0\":1}}";
constexpr char kNotInGame[] = "{\"error\":\"not in game\"}";

bool g_not_in_game = false;

QolResult fake_character_json(int32_t kind, char* out, uint32_t capacity, uint32_t* out_size) {
    if (g_not_in_game) return write_json(kNotInGame, out, capacity, out_size);
    switch (static_cast<QolCharacterHttpJsonKind>(kind)) {
        case QolCharacterHttpJsonKind::kPlayer:
            return write_json(kPlayer, out, capacity, out_size);
        case QolCharacterHttpJsonKind::kParty:
            return write_json(kParty, out, capacity, out_size);
        case QolCharacterHttpJsonKind::kSkills:
            return write_json(kSkills, out, capacity, out_size);
        case QolCharacterHttpJsonKind::kMercenaries:
            return write_json(kMercenaries, out, capacity, out_size);
    }
    return qol::contracts::error(kQolErrInvalidArgument, "bad kind", 8);
}

QolResult fake_type(const void*, int32_t* out_type) {
    if (out_type == nullptr) return qol::contracts::error(kQolErrInvalidArgument, "bad", 3);
    *out_type = 0;
    return qol::contracts::ok();
}

QolResult fake_active(const void*, int32_t* out_active) {
    if (out_active == nullptr) return qol::contracts::error(kQolErrInvalidArgument, "bad", 3);
    *out_active = 1;
    return qol::contracts::ok();
}

const QolCharacterHttpPortReadApi kFakeHttpRead{&fake_character_json};
const QolCharacterHttpPort kFakeHttpPort{qol::contracts::kQolPortCharacterHttpVersion,
                                         kFakeHttpRead};
const qol::contracts::QolCharacterPort kFakeCharacterPort{
    qol::contracts::kQolPortCharacterVersion, {&fake_type, &fake_active}};

const void* const kPortArray[] = {&kFakeCharacterPort, &kFakeHttpPort};

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
    sdk.http = qol::host::http_registry_api();
    sdk.http_dispatch = qol::host::http_dispatch_api();
    return sdk;
}

const qol::contracts::HttpRoute kRoutes[] = {
    {qol::contracts::HttpMethod::kGet, "/api/character/mercenary", "character_facts", 0, 1},
    {qol::contracts::HttpMethod::kGet, "/api/character/mercenary/list", "character_facts", 0, 1},
    {qol::contracts::HttpMethod::kGet, "/api/character/mercenary/{slot}", "character_facts", 0, 1},
    {qol::contracts::HttpMethod::kGet, "/api/character/party", "character_facts", 0, 1},
    {qol::contracts::HttpMethod::kGet, "/api/character/party/count", "character_facts", 0, 1},
    {qol::contracts::HttpMethod::kGet, "/api/character/leader", "character_facts", 0, 1},
    {qol::contracts::HttpMethod::kGet, "/api/character/party/{slot}", "character_facts", 0, 1},
    {qol::contracts::HttpMethod::kGet, "/api/character/party/{slot}/id", "character_facts", 0, 1},
    {qol::contracts::HttpMethod::kGet, "/api/character/party/{slot}/name", "character_facts", 0, 1},
    {qol::contracts::HttpMethod::kGet, "/api/character/party/{slot}/level", "character_facts", 0, 1},
    {qol::contracts::HttpMethod::kGet, "/api/character/party/{slot}/status", "character_facts", 0, 1},
    {qol::contracts::HttpMethod::kGet, "/api/character/party/{slot}/stats", "character_facts", 0, 1},
    {qol::contracts::HttpMethod::kGet, "/api/character/party/{slot}/equipment", "character_facts", 0,
     1},
    {qol::contracts::HttpMethod::kGet, "/api/character/party/{slot}/equipment/{equip_slot}",
     "character_facts", 0, 1},
    {qol::contracts::HttpMethod::kGet, "/api/character/party/{slot}/skills", "character_facts", 0,
     1},
};

qol::contracts::PluginContext make_context(const qol::contracts::QolHostSdk* sdk) {
    qol::contracts::PluginContext ctx{};
    ctx.struct_size = sizeof(qol::contracts::PluginContext);
    ctx.sdk_major = qol::contracts::kHostSdkMajor;
    ctx.sdk_minor = qol::contracts::kHostSdkMinor;
    ctx.sdk = sdk;
    ctx.game = nullptr;
    ctx.plugin_id = "character_facts";
    ctx.ports = kPortArray;
    ctx.port_count = 2;
    return ctx;
}

void register_frozen_routes() {
    for (const auto& route : kRoutes) {
        CHECK(qol::host::http_registry_api().register_route(&route).ok == kQolOk);
    }
}

std::string dispatch_get(const char* path, uint16_t* status = nullptr) {
    std::string path_value(path);
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
    g_not_in_game = false;
    register_frozen_routes();

    const qol::contracts::QolHostSdk sdk = make_sdk();
    qol::contracts::PluginContext ctx = make_context(&sdk);
    const auto& descriptor = qol::features::character_facts::plugin_descriptor();

    CHECK(descriptor.install(ctx) == 0);
    descriptor.on_enabled(ctx);
    CHECK(qol::host::http_dispatch_register_queries(descriptor.http_queries,
                                                     descriptor.http_query_count).ok == kQolOk);
    CHECK(g_scheduler.handler != nullptr);
    CHECK(g_scheduler.interval > 1);

    uint16_t status = 0;
    (void)dispatch_get("/api/character/party", &status);
    CHECK(status == 503);

    CHECK(g_scheduler.handler(100, g_scheduler.ctx) != 0);

    CHECK(dispatch_get("/api/character/party") == kParty);
    CHECK(dispatch_get("/api/character/party/count") == "{\"count\":3}");
    CHECK(dispatch_get("/api/character/party/0") == kMember0);
    CHECK(dispatch_get("/api/character/party/0/id") == "{\"id\":0,\"id_name\":\"黑暗骑士\"}");
    CHECK(dispatch_get("/api/character/party/0/name") == "{\"name\":\"凯恩\"}");
    CHECK(dispatch_get("/api/character/party/0/level") == "{\"level\":10}");
    CHECK(dispatch_get("/api/character/party/0/status") ==
          "{\"status\":{\"hp\":50,\"max_hp\":60,\"mp\":5,\"max_mp\":6,\"exp\":1,\"exp_next\":2,"
          "\"attribute_points\":3,\"skill_points\":4}}");
    CHECK(dispatch_get("/api/character/party/0/stats") == "{\"stats\":{\"0\":1}}");
    CHECK(dispatch_get("/api/character/party/0/equipment") ==
          "{\"equipment\":[null,{\"slot\":1,\"name\":\"钢铁 剑\"}]}");
    CHECK(dispatch_get("/api/character/party/0/equipment/1") == "{\"slot\":1,\"name\":\"钢铁 剑\"}");
    CHECK(dispatch_get("/api/character/party/0/skills") ==
          "{\"role\":0,\"skills\":[{\"action_id\":10,\"level\":1,\"skill_name\":\"火球\"}],"
          "\"unlock_bitmap\":0,\"active_skill_id\":-1,\"skill_points\":4}");

    CHECK(dispatch_get("/api/character/mercenary") == kMercenaries);
    CHECK(dispatch_get("/api/character/mercenary/list") == "{\"slots\":[0,2]}");
    CHECK(dispatch_get("/api/character/mercenary/2") == "{\"slot\":2,\"type\":1,\"name\":\"B\"}");

    // 404 语义：null 成员、越界成员、equipment 空槽、leader 指向 null、无匹配佣兵槽。
    (void)dispatch_get("/api/character/party/1", &status);
    CHECK(status == 404);
    CHECK(dispatch_get("/api/character/party/1/stats") == "{\"stats\":null}");
    (void)dispatch_get("/api/character/party/9", &status);
    CHECK(status == 404);
    (void)dispatch_get("/api/character/party/0/equipment/0", &status);
    CHECK(status == 404);
    (void)dispatch_get("/api/character/leader", &status);
    CHECK(status == 404);
    (void)dispatch_get("/api/character/mercenary/5", &status);
    CHECK(status == 404);

    // 非 world 态：15 条统一 200 + {"error":"not in game"}。
    g_not_in_game = true;
    CHECK(g_scheduler.handler(115, g_scheduler.ctx) != 0);
    CHECK(dispatch_get("/api/character/party", &status) == kNotInGame);
    CHECK(status == 200);
    CHECK(dispatch_get("/api/character/mercenary", &status) == kNotInGame);
    CHECK(status == 200);
    CHECK(dispatch_get("/api/character/party/count", &status) == kNotInGame);
    CHECK(status == 200);

    // 注销 owner：handler 进入墓碑态，调度器任务撤销，请求返回明确停用错误。
    CHECK(qol::host::http_dispatch_api().unregister_owner("character_facts").ok == kQolOk);
    descriptor.on_disabled(ctx);
    CHECK(g_scheduler.handler == nullptr);
    (void)dispatch_get("/api/character/party", &status);
    CHECK(status == 503);

    // 恢复：路由元数据保留，重新绑定后需等待首轮快照。
    g_not_in_game = false;
    descriptor.on_enabled(ctx);
    CHECK(qol::host::http_dispatch_register_queries(descriptor.http_queries,
                                                     descriptor.http_query_count).ok == kQolOk);
    CHECK(g_scheduler.handler != nullptr);
    (void)dispatch_get("/api/character/party", &status);
    CHECK(status == 503);
    CHECK(g_scheduler.handler(130, g_scheduler.ctx) != 0);
    CHECK(dispatch_get("/api/character/party") == kParty);

    descriptor.deactivate(ctx);
}

void test_missing_route_rejected() {
    qol::host::http_dispatch_reset_for_test();
    qol::host::http_registry_reset_for_test();
    g_scheduler = FakeScheduler{};
    g_not_in_game = false;

    const qol::contracts::QolHostSdk sdk = make_sdk();
    qol::contracts::PluginContext ctx = make_context(&sdk);
    const auto& descriptor = qol::features::character_facts::plugin_descriptor();
    CHECK(descriptor.install(ctx) == 0);
    descriptor.on_enabled(ctx);
    CHECK(qol::host::http_dispatch_register_queries(descriptor.http_queries,
                                                     descriptor.http_query_count).code ==
          kQolErrNotFound);
    descriptor.on_disabled(ctx);
    CHECK(g_scheduler.handler == nullptr);
    descriptor.deactivate(ctx);
}

}  // namespace

int main() {
    test_lifecycle_and_responses();
    test_missing_route_rejected();
    std::printf("character_facts_dispatch: pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
