// system_facts HTTP dispatch 接缝 host 测试：
// 覆盖 5 条 /api/system/{snapshot,game,info,game_frame,events} query handler 的注册、快照发布前的
// not-ready、发布后的响应、身份未下发时 info/game 的独立降级、owner 注销后的 handler 归零，
// 以及主线程快照节拍（由 fake scheduler 承载）。
//
// 真实 registry/dispatch（host/http_registry.cpp、host/http_dispatch.cpp）参与链接，
// 游戏侧只以 fake QolSystemPort 注入，避免依赖真机内存。响应体内容由 system_port 的
// 原生拼装决定，本测试只验证快照/透传接缝。

#include "contracts/game_ports/ports.h"
#include "contracts/game_ports/system_port.h"
#include "contracts/host_sdk.h"
#include "contracts/http.h"
#include "contracts/result.h"
#include "host/http_dispatch.h"
#include "host/http_registry.h"
#include "http_endpoint_test_support.h"
#include "plugins/core.system/plugin.h"
#include "test_snapshot_sdk.h"

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

using qol::contracts::QolSystemPort;
using qol::contracts::QolSystemPortReadApi;

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

bool g_identity_ready = true;
bool g_fail_snapshot_read = false;
bool g_snapshot_too_large = false;
bool g_events_too_large = false;

QolResult fake_body_json(int32_t kind, char* out, uint32_t capacity, uint32_t* out_size) {
    switch (static_cast<qol::contracts::QolSystemJsonKind>(kind)) {
        case qol::contracts::QolSystemJsonKind::kSnapshot:
            if (g_snapshot_too_large) {
                *out_size = capacity + 1;
                return qol::contracts::ok();
            }
            if (g_fail_snapshot_read) {
                return qol::contracts::error(kQolErrBlocked, "snapshot read failed", 20);
            }
            return write_json("{\"frame\":9,\"screen\":\"world\"}", out, capacity, out_size);
        case qol::contracts::QolSystemJsonKind::kGame:
            if (!g_identity_ready) {
                return qol::contracts::error(kQolErrNotReady, "system identity not ready", 28);
            }
            return write_json("{\"snapshot\":{},\"info\":{}}", out, capacity, out_size);
        case qol::contracts::QolSystemJsonKind::kInfo:
            if (!g_identity_ready) {
                return qol::contracts::error(kQolErrNotReady, "system identity not ready", 28);
            }
            return write_json("{\"version\":\"0.7.13\"}", out, capacity, out_size);
        case qol::contracts::QolSystemJsonKind::kFrame:
            return write_json("{\"frame\":9}", out, capacity, out_size);
    }
    return qol::contracts::error(kQolErrInvalidArgument, "bad kind", 8);
}

QolResult fake_events_json(char* out, uint32_t capacity, uint32_t* out_size) {
    if (g_events_too_large) {
        *out_size = capacity + 1;
        return qol::contracts::ok();
    }
    return write_json("{\"events\":[{\"type\":\"money\",\"old\":1,\"new\":2}]}", out,
                      capacity, out_size);
}

const QolSystemPortReadApi kFakeRead{&fake_body_json, &fake_events_json};
const QolSystemPort kFakePort{qol::contracts::kQolPortSystemVersion, kFakeRead};

// 插件描述符声明 1 个端口；这里提供与宿主装配同形的静态端口数组。
const void* const kPortArray[] = {&kFakePort};

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
    test_snapshot_sdk::attach(&sdk);
    return sdk;
}

qol::contracts::PluginContext make_context(const qol::contracts::QolHostSdk* sdk) {
    qol::contracts::PluginContext ctx{};
    ctx.struct_size = sizeof(qol::contracts::PluginContext);
    ctx.sdk_major = qol::contracts::kHostSdkMajor;
    ctx.sdk_minor = qol::contracts::kHostSdkMinor;
    ctx.sdk = sdk;
    ctx.game = nullptr;
    ctx.plugin_id = "core.system";
    ctx.ports = kPortArray;
    ctx.port_count = 1;
    return ctx;
}

std::string dispatch_get(const char* path, uint16_t* status = nullptr) {
    std::string path_value(path);
    char storage[4096]{};
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
    g_identity_ready = true;
    g_fail_snapshot_read = false;
    g_snapshot_too_large = false;
    g_events_too_large = false;
    const qol::contracts::QolHostSdk sdk = make_sdk();
    qol::contracts::PluginContext ctx = make_context(&sdk);
    const auto& descriptor = qol::features::core_system::plugin_descriptor();

    CHECK(descriptor.requires_count == 0 && descriptor.optional_count == 2);
    CHECK(descriptor.http_query_count == 0 && descriptor.http_queries == nullptr);
    CHECK(descriptor.http_endpoint_count == 5 && descriptor.http_endpoints != nullptr);
    CHECK(descriptor.http_route_metadata_count == 0 && descriptor.http_route_metadata == nullptr);
    CHECK(std::strcmp(descriptor.optional_requires[0].id,
                      qol::contracts::kHttpRegistryCapabilityId) == 0);
    CHECK(std::strcmp(descriptor.optional_requires[1].id,
                      qol::contracts::kHttpDispatchCapabilityId) == 0);
    for (uint32_t i = 0; i < 2; ++i) {
        CHECK(descriptor.optional_requires[i].range.major == 1);
        CHECK(descriptor.optional_requires[i].range.min_minor == 1);
    }
    for (uint32_t i = 0; i < descriptor.http_endpoint_count; ++i) {
        const auto& endpoint = descriptor.http_endpoints[i];
        CHECK(endpoint.method == qol::contracts::HttpMethod::kGet);
        CHECK(endpoint.kind == qol::contracts::HttpHandlerKind::kQuery);
        CHECK(endpoint.op_required == 0 && endpoint.lease_required == 1);
        CHECK(endpoint.query_handler != nullptr && endpoint.command_handler == nullptr);
        CHECK(std::strcmp(endpoint.route_owner, "core.system") == 0);
    }
    CHECK(descriptor.install(ctx) == 0);
    descriptor.on_enabled(ctx);
    CHECK(qol::contracts::succeeded(qol::tests::register_plugin_http(descriptor)));
    CHECK(g_scheduler.handler != nullptr);
    CHECK(g_scheduler.interval > 1);  // 快照节流，不逐帧重建

    // 快照尚未发布：handler 已注册但返回 not ready。
    uint16_t status = 0;
    (void)dispatch_get("/api/system/snapshot", &status);
    CHECK(status == 503);

    // 主线程帧回调发布快照（fake scheduler 手动驱动）。
    CHECK(g_scheduler.handler(100, g_scheduler.ctx) != 0);

    CHECK(dispatch_get("/api/system/snapshot", &status) == "{\"frame\":9,\"screen\":\"world\"}");
    CHECK(status == 200);
    CHECK(dispatch_get("/api/system/game") == "{\"snapshot\":{},\"info\":{}}");
    CHECK(dispatch_get("/api/system/info") == "{\"version\":\"0.7.13\"}");
    CHECK(dispatch_get("/api/system/game_frame") == "{\"frame\":9}");
    CHECK(dispatch_get("/api/system/events") ==
          "{\"events\":[{\"type\":\"money\",\"old\":1,\"new\":2}]}");
    g_events_too_large = true;
    CHECK(dispatch_get("/api/system/events", &status) ==
          "{\"ok\":false,\"error\":\"snapshot not ready\"}");
    CHECK(status == 503);
    g_events_too_large = false;

    g_fail_snapshot_read = true;
    CHECK(g_scheduler.handler(115, g_scheduler.ctx) != 0);
    CHECK(dispatch_get("/api/system/snapshot", &status) ==
          "{\"frame\":9,\"screen\":\"world\"}");
    CHECK(status == 200);
    g_fail_snapshot_read = false;
    g_snapshot_too_large = true;
    CHECK(g_scheduler.handler(130, g_scheduler.ctx) != 0);
    CHECK(dispatch_get("/api/system/snapshot", &status) ==
          "{\"frame\":9,\"screen\":\"world\"}");
    CHECK(status == 200);
    g_snapshot_too_large = false;

    // 身份未下发：info/game 独立 503，snapshot/frame 不受影响。
    g_identity_ready = false;
    CHECK(g_scheduler.handler(115, g_scheduler.ctx) != 0);
    (void)dispatch_get("/api/system/info", &status);
    CHECK(status == 503);
    (void)dispatch_get("/api/system/game", &status);
    CHECK(status == 503);
    CHECK(dispatch_get("/api/system/snapshot") == "{\"frame\":9,\"screen\":\"world\"}");
    CHECK(dispatch_get("/api/system/game_frame") == "{\"frame\":9}");
    g_identity_ready = true;
    CHECK(g_scheduler.handler(145, g_scheduler.ctx) != 0);
    CHECK(dispatch_get("/api/system/info") == "{\"version\":\"0.7.13\"}");
    CHECK(dispatch_get("/api/system/events") ==
          "{\"events\":[{\"type\":\"money\",\"old\":1,\"new\":2}]}");

    // 注销 owner：handler 进入墓碑态，调度器任务撤销，请求返回明确停用错误。
    CHECK(qol::host::http_dispatch_api().unregister_owner("core.system").ok == kQolOk);
    descriptor.on_disabled(ctx);
    CHECK(g_scheduler.handler == nullptr);
    (void)dispatch_get("/api/system/snapshot", &status);
    CHECK(status == 503);
    (void)dispatch_get("/api/system/events", &status);
    CHECK(status == 503);

    // core.api 的路由元数据保留：owner 恢复后 handler 可以重新绑定，首轮快照仍 not ready。
    descriptor.on_enabled(ctx);
    CHECK(qol::contracts::succeeded(qol::tests::register_plugin_http(descriptor)));
    CHECK(g_scheduler.handler != nullptr);
    (void)dispatch_get("/api/system/snapshot", &status);
    CHECK(status == 503);
    CHECK(g_scheduler.handler(160, g_scheduler.ctx) != 0);
    CHECK(dispatch_get("/api/system/snapshot") == "{\"frame\":9,\"screen\":\"world\"}");

    descriptor.deactivate(ctx);
}

void test_missing_http_provider_keeps_business_installed() {
    qol::host::http_dispatch_reset_for_test();
    qol::host::http_registry_reset_for_test();
    g_scheduler = FakeScheduler{};
    g_identity_ready = true;

    // HTTP provider 缺失时，插件业务保持可安装，只省略 native endpoint binding。
    auto sdk = make_sdk();
    sdk.http = {};
    sdk.http_dispatch = {};
    qol::contracts::PluginContext ctx = make_context(&sdk);
    const auto& descriptor = qol::features::core_system::plugin_descriptor();
    CHECK(descriptor.install(ctx) == 0);
    descriptor.on_enabled(ctx);
    CHECK(g_scheduler.handler != nullptr);
    descriptor.on_disabled(ctx);
    CHECK(g_scheduler.handler == nullptr);
    descriptor.deactivate(ctx);
}

}  // namespace

int main() {
    test_lifecycle_and_responses();
    test_missing_http_provider_keeps_business_installed();
    std::printf("system_facts_dispatch: pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
