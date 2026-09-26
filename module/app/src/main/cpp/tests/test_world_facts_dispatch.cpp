// world_facts HTTP dispatch 接缝 host 测试：
// 覆盖 7 条 /api/world/map/* query handler 的注册、快照发布前的 not-ready、发布后的响应、
// owner 注销后的 handler 归零，以及主线程快照节拍（由 fake scheduler 承载）。
//
// 真实 registry/dispatch（host/http_registry.cpp、host/http_dispatch.cpp）参与链接，
// 游戏侧只以 fake QolWorldPort 注入，避免依赖真机内存。

#include "contracts/game_ports/ports.h"
#include "contracts/game_ports/world_port.h"
#include "contracts/game_ports/world_write_port.h"
#include "contracts/host_sdk.h"
#include "contracts/http.h"
#include "contracts/result.h"
#include "host/http_dispatch.h"
#include "host/http_registry.h"
#include "plugins/core.world/plugin.h"

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

using qol::contracts::QolWorldNavigation;
using qol::contracts::QolWorldPort;
using qol::contracts::QolWorldPortReadApi;

constexpr int32_t kMapId = 7;
QolWorldNavigation g_navigation;

QolResult write_json(const char* text, char* out, uint32_t capacity, uint32_t* out_size) {
    const uint32_t size = static_cast<uint32_t>(std::strlen(text));
    if (out == nullptr || out_size == nullptr || size >= capacity) {
        return qol::contracts::error(kQolErrInvalidArgument, "tiny buffer", 11);
    }
    std::memcpy(out, text, size);
    out[size] = '\0';
    *out_size = size;
    return qol::contracts::ok();
}

QolResult fake_state(int32_t* out) { *out = 1; return qol::contracts::ok(); }
QolResult fake_gate(int32_t* out) { *out = 1; return qol::contracts::ok(); }
QolResult fake_record_count(int32_t* out) { *out = 32; return qol::contracts::ok(); }
QolResult fake_map_id(int32_t* out) { *out = kMapId; return qol::contracts::ok(); }
QolResult fake_map_name(int32_t map_id, const char** out) {
    *out = map_id == kMapId ? "TestMap" : nullptr;
    return *out == nullptr ? qol::contracts::error(kQolErrNotFound, "no name", 7)
                           : qol::contracts::ok();
}
QolResult fake_direction(int32_t* out) { *out = 2; return qol::contracts::ok(); }
QolResult fake_money(int64_t* out) { *out = 1000; return qol::contracts::ok(); }
QolResult fake_tutorial(int32_t* out) { *out = 0; return qol::contracts::ok(); }

QolResult fake_exits(char* out, uint32_t capacity, uint32_t* out_size) {
    return write_json("{\"exits\":[{\"x\":3,\"y\":4}]}", out, capacity, out_size);
}
QolResult fake_units(char* out, uint32_t capacity, uint32_t* out_size) {
    return write_json("{\"units\":[{\"slot\":1,\"type\":0}]}", out, capacity, out_size);
}
QolResult fake_enemies(char* out, uint32_t capacity, uint32_t* out_size) {
    return write_json("{\"units\":[{\"slot\":5,\"type\":1}]}", out, capacity, out_size);
}
QolResult fake_interactives(char* out, uint32_t capacity, uint32_t* out_size) {
    return write_json("{\"units\":[{\"slot\":6,\"type\":2}]}", out, capacity, out_size);
}
QolResult fake_drops(char* out, uint32_t capacity, uint32_t* out_size) {
    return write_json("{\"drops\":[{\"item\":9}]}", out, capacity, out_size);
}

QolResult fake_navigation(QolWorldNavigation* out) {
    if (out == nullptr) return qol::contracts::error(kQolErrInvalidArgument, "null", 4);
    *out = g_navigation;
    return qol::contracts::ok();
}

bool g_in_world = true;

QolResult fake_in_world(int32_t* out) {
    *out = g_in_world ? 1 : 0;
    return qol::contracts::ok();
}

const QolWorldPortReadApi kFakeRead{
    &fake_state,          &fake_gate,        &fake_record_count,  &fake_map_id,
    &fake_map_name,       &fake_direction,   &fake_money,         &fake_tutorial,
    &fake_exits,          &fake_units,       &fake_enemies,       &fake_interactives,
    &fake_drops,          &fake_navigation,  &fake_in_world,
};

const QolWorldPort kFakePort{qol::contracts::kQolPortWorldVersion, kFakeRead};

// 世界写端口假实现：仅满足插件 install 的端口校验；本测试不驱动写路径。
QolResult fake_world_write(int32_t, int32_t, int32_t, int32_t, int32_t, char*, uint32_t,
                           uint32_t*) {
    return qol::contracts::ok();
}
const qol::contracts::QolWorldWritePortReadApi kFakeWriteRead{&fake_world_write};
const qol::contracts::QolWorldWritePort kFakeWritePort{
    qol::contracts::kQolPortWorldWriteVersion, kFakeWriteRead};

// 插件描述符声明 2 个端口（世界读 + 世界写）；这里提供与宿主装配同形的静态端口数组。
const void* const kPortArray[] = {&kFakePort, &kFakeWritePort};

struct FakeScheduler {
    qol::contracts::QolFrameHandler handler = nullptr;
    void* ctx = nullptr;
    int32_t interval = 0;
};

FakeScheduler g_scheduler;

struct CapturedWorldPayload {
    int32_t kind = -1;
    int32_t a = -1;
    int32_t b = -1;
    int32_t c = -1;
    int32_t d = -1;
};

CapturedWorldPayload g_last_payload;
int g_post_count = 0;
qol::contracts::QolTypedResultCommandHandler g_result_handler = nullptr;

QolResult fake_post_typed(uint32_t, const void* payload, uint32_t size,
                          qol::contracts::QolCommandTicket* out_ticket) {
    if (payload == nullptr || size != sizeof(g_last_payload) || out_ticket == nullptr) {
        return qol::contracts::error(kQolErrInvalidArgument, "bad payload", 11);
    }
    std::memcpy(&g_last_payload, payload, size);
    ++g_post_count;
    *out_ticket = static_cast<qol::contracts::QolCommandTicket>(100 + g_post_count);
    return qol::contracts::ok();
}

QolResult fake_register_result(uint32_t, qol::contracts::QolTypedResultCommandHandler handler,
                               void*) {
    g_result_handler = handler;
    return qol::contracts::ok();
}

QolResult fake_unregister_result(uint32_t) {
    g_result_handler = nullptr;
    return qol::contracts::ok();
}

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
    sdk.commands.post_typed = &fake_post_typed;
    sdk.http = qol::host::http_registry_api();
    sdk.http_dispatch = qol::host::http_dispatch_api();
    sdk.ticket_result = qol::contracts::QolTicketResultApi{
        &fake_register_result, &fake_unregister_result, nullptr, nullptr};
    return sdk;
}

const qol::contracts::HttpRoute kRoutes[] = {
    {qol::contracts::HttpMethod::kGet, "/api/world/map/id", "core.world", 0, 1},
    {qol::contracts::HttpMethod::kGet, "/api/world/map/exits", "core.world", 0, 1},
    {qol::contracts::HttpMethod::kGet, "/api/world/map/units", "core.world", 0, 1},
    {qol::contracts::HttpMethod::kGet, "/api/world/map/enemies", "core.world", 0, 1},
    {qol::contracts::HttpMethod::kGet, "/api/world/map/interactives", "core.world", 0, 1},
    {qol::contracts::HttpMethod::kGet, "/api/world/map/drops", "core.world", 0, 1},
    {qol::contracts::HttpMethod::kGet, "/api/world/map/distance", "core.world", 0, 1},
    {qol::contracts::HttpMethod::kPost, "/api/world/movement/move_to", "core.world", 0, 1},
    {qol::contracts::HttpMethod::kPost, "/api/world/movement/walk_dir", "core.world", 0, 1},
    {qol::contracts::HttpMethod::kPost, "/api/world/movement/stop_move", "core.world", 0, 1},
    {qol::contracts::HttpMethod::kPost, "/api/world/movement/interact_with", "core.world", 0, 1},
    {qol::contracts::HttpMethod::kPost, "/api/op/movement/teleport", "core.world", 1, 1},
};

qol::contracts::PluginContext make_context(const qol::contracts::QolHostSdk* sdk) {
    qol::contracts::PluginContext ctx{};
    ctx.struct_size = sizeof(qol::contracts::PluginContext);
    ctx.sdk_major = qol::contracts::kHostSdkMajor;
    ctx.sdk_minor = qol::contracts::kHostSdkMinor;
    ctx.sdk = sdk;
    ctx.game = nullptr;
    ctx.plugin_id = "core.world";
    ctx.ports = kPortArray;
    ctx.port_count = 2;
    return ctx;
}

void register_frozen_routes() {
    for (const auto& route : kRoutes) {
        CHECK(qol::host::http_registry_api().register_route(&route).ok == kQolOk);
    }
}

std::string dispatch_get(const char* path, const char* query = nullptr, uint16_t* status = nullptr) {
    std::string path_value(path);
    std::string query_value(query == nullptr ? "" : query);
    char storage[4096]{};
    qol::contracts::HttpReq request{
        qol::contracts::HttpMethod::kGet,
        path_value.c_str(),
        static_cast<uint32_t>(path_value.size()),
        nullptr,
        0,
        query_value.c_str(),
        static_cast<uint32_t>(query_value.size()),
        nullptr,
        0,
    };
    qol::contracts::HttpRes response{reinterpret_cast<uint8_t*>(storage), sizeof(storage), 0, 0};
    (void)qol::host::http_dispatch_api().dispatch(&request, &response);
    if (status != nullptr) *status = response.status;
    return std::string(storage, response.body_size);
}

std::string dispatch_post(const char* path, const char* body, uint16_t* status = nullptr) {
    std::string path_value(path);
    std::string body_value(body == nullptr ? "" : body);
    char storage[4096]{};
    qol::contracts::HttpReq request{
        qol::contracts::HttpMethod::kPost,
        path_value.c_str(),
        static_cast<uint32_t>(path_value.size()),
        reinterpret_cast<const uint8_t*>(body_value.data()),
        static_cast<uint32_t>(body_value.size()),
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
    g_post_count = 0;
    g_result_handler = nullptr;
    g_in_world = true;
    std::memset(&g_navigation, 0, sizeof(g_navigation));
    g_navigation.player_x = 16;
    g_navigation.player_y = 16;
    g_navigation.valid = 1;
    register_frozen_routes();

    const qol::contracts::QolHostSdk sdk = make_sdk();
    qol::contracts::PluginContext ctx = make_context(&sdk);
    const auto& descriptor = qol::features::core_world::plugin_descriptor();

    CHECK(descriptor.install(ctx) == 0);
    descriptor.on_enabled(ctx);
    CHECK(qol::host::http_dispatch_register_queries(descriptor.http_queries,
                                                     descriptor.http_query_count).ok == kQolOk);
    CHECK(g_scheduler.handler != nullptr);
    CHECK(g_scheduler.interval > 1);  // 快照节流，不逐帧重建

    // 快照尚未发布：handler 已注册但返回 not ready。
    uint16_t status = 0;
    (void)dispatch_get("/api/world/map/units", nullptr, &status);
    CHECK(status == 503);

    // 主线程帧回调发布快照（fake scheduler 手动驱动）。
    CHECK(g_scheduler.handler(100, g_scheduler.ctx) != 0);

    CHECK(dispatch_get("/api/world/map/id", nullptr, &status) ==
          "{\"map_id\":7,\"id_name\":\"TestMap\"}");
    CHECK(status == 200);

    // 距离接口按冻结键序输出，阻挡目标提供 nearest 瓦片中心像素坐标。
    g_navigation.blocked[2 * qol::contracts::kWorldNavWidth + 2] = 1;
    CHECK(g_scheduler.handler(101, g_scheduler.ctx) != 0);
    CHECK(dispatch_get("/api/world/map/distance", "tx=32&ty=32", &status) ==
          "{\"target\":{\"x\":32,\"y\":32},\"start\":{\"x\":16,\"y\":16},\"found\":false,"
          "\"distance\":1,\"nearest\":{\"x\":24,\"y\":40,\"distance\":1}}");
    CHECK(status == 200);

    // 写路由只投递定型 payload，HTTP 成功体保持票据 JSON。
    qol::host::http_dispatch_set_op_enabled(true);
    CHECK(dispatch_post("/api/world/movement/move_to", "{\"x\":10,\"y\":20}", &status) ==
          "{\"ok\":true,\"ticket\":101}");
    CHECK(status == 200 && g_last_payload.kind == qol::contracts::kQolWorldWriteMoveTo &&
          g_last_payload.a == 168 && g_last_payload.b == 328);
    const int posts_before_invalid = g_post_count;
    CHECK(dispatch_post("/api/world/movement/move_to", "{\"x\":4294967296,\"y\":1}",
                        &status) == "{\"ok\":false,\"error\":\"x/y required\"}");
    CHECK(status == 400 && g_post_count == posts_before_invalid);
    CHECK(dispatch_post("/api/world/movement/move_to", "{\"x\":-1,\"y\":1}", &status) ==
          "{\"ok\":false,\"error\":\"x/y required\"}");
    CHECK(status == 400 && g_post_count == posts_before_invalid);
    CHECK(dispatch_post("/api/world/movement/move_to", "{\"x\":134217728,\"y\":1}",
                        &status) == "{\"ok\":false,\"error\":\"x/y required\"}");
    CHECK(status == 400 && g_post_count == posts_before_invalid);
    CHECK(dispatch_post("/api/world/movement/walk_dir", "{\"direction\":4}", &status) ==
          "{\"ok\":false,\"error\":\"direction 0-3 required\"}");
    CHECK(status == 400 && g_post_count == posts_before_invalid);
    CHECK(dispatch_post("/api/world/movement/stop_move", "{}", &status) ==
          "{\"ok\":true,\"ticket\":102}");
    CHECK(g_last_payload.kind == qol::contracts::kQolWorldWriteStopMove);
    CHECK(dispatch_post("/api/world/movement/interact_with", "{}", &status) ==
          "{\"ok\":true,\"ticket\":103}");
    CHECK(g_last_payload.kind == qol::contracts::kQolWorldWriteInteract);
    CHECK(dispatch_post("/api/op/movement/teleport",
                        "{\"map_id\":7,\"x\":12,\"y\":13,\"force\":true}", &status) ==
          "{\"ok\":true,\"ticket\":104}");
    CHECK(status == 200 && g_last_payload.kind == qol::contracts::kQolWorldWriteTeleport &&
          g_last_payload.a == 7 && g_last_payload.b == 12 && g_last_payload.c == 13 &&
          g_last_payload.d == 1);
    const int posts_before_teleport_invalid = g_post_count;
    CHECK(dispatch_post("/api/op/movement/teleport", "{\"map_id\":-1,\"x\":1,\"y\":2}",
                        &status) == "{\"ok\":false,\"error\":\"map_id/x/y required\"}");
    CHECK(status == 400 && g_post_count == posts_before_teleport_invalid);
    CHECK(g_result_handler != nullptr);
    uint8_t malformed_payload[sizeof(CapturedWorldPayload) - 1]{};
    char result_body[32]{};
    uint32_t result_size = 0;
    CHECK(!g_result_handler(nullptr, malformed_payload, sizeof(malformed_payload), result_body,
                             sizeof(result_body), &result_size));
    CHECK(dispatch_get("/api/world/map/distance", "tx=1024&ty=0", &status) ==
          "{\"target\":{\"x\":1024,\"y\":0},\"start\":{\"x\":16,\"y\":16},"
          "\"found\":false,\"distance\":-1,\"nearest\":null}");
    CHECK(status == 200);
    CHECK(dispatch_get("/api/world/map/exits") == "{\"exits\":[{\"x\":3,\"y\":4}]}");
    CHECK(dispatch_get("/api/world/map/units") == "{\"units\":[{\"slot\":1,\"type\":0}]}");
    CHECK(dispatch_get("/api/world/map/enemies") == "{\"units\":[{\"slot\":5,\"type\":1}]}");
    CHECK(dispatch_get("/api/world/map/interactives") == "{\"units\":[{\"slot\":6,\"type\":2}]}");
    CHECK(dispatch_get("/api/world/map/drops") == "{\"drops\":[{\"item\":9}]}");
    g_navigation.blocked[2 * qol::contracts::kWorldNavWidth + 2] = 0;
    CHECK(g_scheduler.handler(102, g_scheduler.ctx) != 0);
    CHECK(dispatch_get("/api/world/map/distance", "tx=32&ty=32", &status) ==
          "{\"target\":{\"x\":32,\"y\":32},\"start\":{\"x\":16,\"y\":16},\"found\":true,"
          "\"distance\":2,\"nearest\":{\"x\":40,\"y\":40,\"distance\":2}}");
    CHECK(status == 200);

    // 缺参/非法参数：400。
    (void)dispatch_get("/api/world/map/distance", "tx=32", &status);
    CHECK(status == 400);

    // 非 world 态：发布 legacy 一致的 not in game 错误体（HTTP 200，与迁移前口径相同）。
    g_in_world = false;
    CHECK(g_scheduler.handler(115, g_scheduler.ctx) != 0);
    CHECK(dispatch_get("/api/world/map/id", nullptr, &status) == "{\"error\":\"not in game\"}");
    CHECK(status == 200);
    CHECK(dispatch_get("/api/world/map/units") == "{\"error\":\"not in game\"}");
    g_in_world = true;
    CHECK(g_scheduler.handler(130, g_scheduler.ctx) != 0);
    CHECK(dispatch_get("/api/world/map/id") == "{\"map_id\":7,\"id_name\":\"TestMap\"}");

    // 注销 owner：handler 进入墓碑态，调度器任务撤销，请求返回明确停用错误。
    CHECK(qol::host::http_dispatch_api().unregister_owner("core.world").ok == kQolOk);
    descriptor.on_disabled(ctx);
    CHECK(g_scheduler.handler == nullptr);
    (void)dispatch_get("/api/world/map/units", nullptr, &status);
    CHECK(status == 503);
    const int posts_before_inactive = g_post_count;
    CHECK(dispatch_post("/api/world/movement/stop_move", "{}", &status) ==
          "{\"ok\":false,\"error\":\"http owner inactive\"}");
    CHECK(status == 503 && g_post_count == posts_before_inactive);

    // core.api 的路由元数据保留：owner 恢复后 handler 可以重新绑定，首轮快照仍 not ready。
    descriptor.on_enabled(ctx);
    CHECK(qol::host::http_dispatch_register_queries(descriptor.http_queries,
                                                     descriptor.http_query_count).ok == kQolOk);
    CHECK(g_scheduler.handler != nullptr);
    (void)dispatch_get("/api/world/map/units", nullptr, &status);
    CHECK(status == 503);
    CHECK(g_scheduler.handler(145, g_scheduler.ctx) != 0);
    CHECK(dispatch_get("/api/world/map/id") == "{\"map_id\":7,\"id_name\":\"TestMap\"}");

    descriptor.deactivate(ctx);
}

void test_missing_route_rejected() {
    qol::host::http_dispatch_reset_for_test();
    qol::host::http_registry_reset_for_test();
    g_scheduler = FakeScheduler{};
    g_in_world = true;

    // 冻结路由表缺失：宿主声明注册失败，不得留下 handler。
    const qol::contracts::QolHostSdk sdk = make_sdk();
    qol::contracts::PluginContext ctx = make_context(&sdk);
    const auto& descriptor = qol::features::core_world::plugin_descriptor();
    CHECK(descriptor.install(ctx) == 0);
    descriptor.on_enabled(ctx);
    CHECK(qol::host::http_dispatch_register_queries(descriptor.http_queries,
                                                     descriptor.http_query_count).code ==
          kQolErrNotFound);
    CHECK(qol::host::http_dispatch_api().dispatch != nullptr);
    descriptor.on_disabled(ctx);
    CHECK(g_scheduler.handler == nullptr);
    descriptor.deactivate(ctx);
}

}  // namespace

int main() {
    test_lifecycle_and_responses();
    test_missing_route_rejected();
    std::printf("world_facts_dispatch: pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
