#include "plugin.h"

#include <cstdint>
#include <cstring>
#include <string>

#include "contracts/game_ports/system_port.h"
#include "contracts/host_sdk.h"
#include "contracts/http.h"
#include "snapshot.h"

namespace qol::features::core_system {
namespace {

constexpr const char* kPluginId = "core.system";
constexpr const char* kPluginVersion = "0.1.0";
// 单个响应体上限：party 三人 + 装备聚合远小于此值，超出即放弃本轮发布。
constexpr uint32_t kSnapshotCapacity = 32768;
constexpr uint32_t kGameCapacity = 32768;
constexpr uint32_t kInfoCapacity = 8192;
constexpr uint32_t kFrameCapacity = 64;
// 主线程快照节拍：快照/info 聚合 JSON 构造不宜每帧执行；15 帧（≈4Hz）对只读查询足够。
constexpr int32_t kSnapshotIntervalFrames = 15;

enum class Query : uint8_t { kSnapshot, kGame, kInfo, kFrame };

constexpr contracts::HttpRoute kRoutes[] = {
    {contracts::HttpMethod::kGet, "/api/system/snapshot", kPluginId, 0, 1},
    {contracts::HttpMethod::kGet, "/api/system/game", kPluginId, 0, 1},
    {contracts::HttpMethod::kGet, "/api/system/info", kPluginId, 0, 1},
    {contracts::HttpMethod::kGet, "/api/system/game_frame", kPluginId, 0, 1},
};

constexpr Query kQueries[] = {Query::kSnapshot, Query::kGame, Query::kInfo, Query::kFrame};

enum class SnapshotState : uint8_t { kUnavailable, kReady };

struct SystemSnapshot {
    SnapshotState state = SnapshotState::kUnavailable;
    uint32_t snapshot_size = 0;
    char snapshot[kSnapshotCapacity]{};
    uint32_t game_size = 0;
    char game[kGameCapacity]{};
    uint32_t info_size = 0;
    char info[kInfoCapacity]{};
    uint32_t frame_size = 0;
    char frame[kFrameCapacity]{};
};

const contracts::QolHostSdk* g_sdk = nullptr;
const contracts::QolSystemPort* g_port = nullptr;
uint64_t g_frame_task = 0;
Snapshot<SystemSnapshot> g_snapshot;

const contracts::QolPortRequirement kPorts[] = {
    {contracts::QolPortId::QOL_PORT_SYSTEM, {1, 0}},
};

const contracts::CapabilityRequirement kRequires[] = {
    {contracts::kHttpRegistryCapabilityId, {contracts::kHttpRegistryVersion.major,
                                            contracts::kHttpRegistryVersion.minor}},
    {contracts::kHttpDispatchCapabilityId, {contracts::kHttpDispatchVersion.major,
                                             contracts::kHttpDispatchVersion.minor}},
};

void log_message(const contracts::QolHostSdk* sdk, const char* message) {
    if (sdk == nullptr || sdk->log.write == nullptr || message == nullptr) return;
    uint32_t length = 0;
    while (message[length] != '\0') ++length;
    (void)sdk->log.write(1, 1, kPluginId, message, length);
}

void update_snapshot() {
    if (g_port == nullptr || g_port->read.body_json == nullptr) return;
    SystemSnapshot next{};
    struct Slot {
        int32_t kind;
        char* out;
        uint32_t capacity;
        uint32_t* size;
    };
    const Slot slots[] = {
        {static_cast<int32_t>(contracts::QolSystemJsonKind::kSnapshot), next.snapshot,
         kSnapshotCapacity, &next.snapshot_size},
        {static_cast<int32_t>(contracts::QolSystemJsonKind::kGame), next.game, kGameCapacity,
         &next.game_size},
        {static_cast<int32_t>(contracts::QolSystemJsonKind::kInfo), next.info, kInfoCapacity,
         &next.info_size},
        {static_cast<int32_t>(contracts::QolSystemJsonKind::kFrame), next.frame, kFrameCapacity,
         &next.frame_size},
    };
    bool core_ready = true;
    for (const Slot& slot : slots) {
        const QolResult result =
            g_port->read.body_json(slot.kind, slot.out, slot.capacity, slot.size);
        if (!contracts::succeeded(result) || *slot.size == 0) {
            *slot.size = 0;
            // 身份未下发时 info/game 暂不可用；快照与帧计数与其无关，照常发布。
            const bool identity_dependent =
                slot.kind == static_cast<int32_t>(contracts::QolSystemJsonKind::kInfo) ||
                slot.kind == static_cast<int32_t>(contracts::QolSystemJsonKind::kGame);
            if (!identity_dependent) core_ready = false;
        }
    }
    if (!core_ready) return;
    next.state = SnapshotState::kReady;
    g_snapshot.publish(next);
}

int32_t frame_tick(int64_t, void*) {
    update_snapshot();
    return 1;
}

QolResult response_error(contracts::HttpRes* response, uint16_t status, const char* message) {
    const std::string body = std::string("{\"ok\":false,\"error\":\"") + message + "\"}";
    if (response == nullptr || response->body == nullptr || response->body_capacity < body.size()) {
        return contracts::error(kQolErrInternal, "http response buffer too small", 31);
    }
    std::memcpy(response->body, body.data(), body.size());
    response->body_size = static_cast<uint32_t>(body.size());
    response->status = status;
    return contracts::error(status == 503 ? kQolErrNotReady : kQolErrInvalidArgument, message,
                            static_cast<uint32_t>(std::strlen(message)));
}

QolResult response_body(contracts::HttpRes* response, const char* body, uint32_t size) {
    if (response == nullptr || response->body == nullptr || body == nullptr ||
        response->body_capacity < size) {
        return response_error(response, 500, "http response buffer too small");
    }
    std::memcpy(response->body, body, size);
    response->body_size = size;
    response->status = 200;
    return contracts::ok();
}

QolResult query_handler(const contracts::HttpReq* request, contracts::HttpRes* response, void* ctx) {
    if (request == nullptr || response == nullptr || ctx == nullptr) {
        return response_error(response, 400, "bad request");
    }
    SystemSnapshot snapshot{};
    if (!g_snapshot.read(snapshot) || snapshot.state != SnapshotState::kReady) {
        return response_error(response, 503, "snapshot not ready");
    }
    const Query query = *static_cast<const Query*>(ctx);
    switch (query) {
        case Query::kSnapshot:
            if (snapshot.snapshot_size == 0) return response_error(response, 503, "snapshot not ready");
            return response_body(response, snapshot.snapshot, snapshot.snapshot_size);
        case Query::kGame:
            if (snapshot.game_size == 0) return response_error(response, 503, "snapshot not ready");
            return response_body(response, snapshot.game, snapshot.game_size);
        case Query::kInfo:
            if (snapshot.info_size == 0) return response_error(response, 503, "snapshot not ready");
            return response_body(response, snapshot.info, snapshot.info_size);
        case Query::kFrame:
            if (snapshot.frame_size == 0) return response_error(response, 503, "snapshot not ready");
            return response_body(response, snapshot.frame, snapshot.frame_size);
    }
    return response_error(response, 500, "unknown system query");
}

const contracts::HttpQueryHandlerDescriptor kHttpQueries[] = {
    {kRoutes[0].method, kRoutes[0].path, kPluginId, &query_handler, const_cast<Query*>(&kQueries[0])},
    {kRoutes[1].method, kRoutes[1].path, kPluginId, &query_handler, const_cast<Query*>(&kQueries[1])},
    {kRoutes[2].method, kRoutes[2].path, kPluginId, &query_handler, const_cast<Query*>(&kQueries[2])},
    {kRoutes[3].method, kRoutes[3].path, kPluginId, &query_handler, const_cast<Query*>(&kQueries[3])},
};

void stop_frame_task(const contracts::QolHostSdk* sdk) {
    if (g_frame_task != 0 && sdk != nullptr && sdk->scheduler.remove != nullptr) {
        (void)sdk->scheduler.remove(g_frame_task);
    }
    g_frame_task = 0;
}

int plugin_install(contracts::PluginContext& ctx) {
    g_sdk = ctx.sdk;
    g_port = nullptr;
    g_frame_task = 0;
    SystemSnapshot empty_snapshot{};
    g_snapshot.publish(empty_snapshot);
    if (ctx.struct_size < contracts::PLUGIN_CONTEXT_PORTS_STRUCT_SIZE ||
        ctx.sdk_major != contracts::kHostSdkMajor || g_sdk == nullptr || ctx.ports == nullptr ||
        ctx.port_count != 1 || ctx.ports[0] == nullptr) {
        g_sdk = nullptr;
        return 1;
    }
    g_port = static_cast<const contracts::QolSystemPort*>(ctx.ports[0]);
    if (!contracts::satisfies(g_port->abi_version, {1, 0}) || g_port->read.body_json == nullptr) {
        g_port = nullptr;
        g_sdk = nullptr;
        return 1;
    }
    log_message(g_sdk, "[core.system] provider install");
    return 0;
}

void plugin_deactivate(contracts::PluginContext& ctx) {
    stop_frame_task(ctx.sdk);
    log_message(ctx.sdk, "[core.system] provider deactivate");
    g_port = nullptr;
    g_sdk = nullptr;
}

void plugin_on_enabled(contracts::PluginContext& ctx) {
    SystemSnapshot empty_snapshot{};
    g_snapshot.publish(empty_snapshot);
    if (g_frame_task == 0 && g_sdk != nullptr && g_sdk->scheduler.add != nullptr) {
        uint64_t task = 0;
        if (contracts::succeeded(g_sdk->scheduler.add(
                1, &frame_tick, nullptr, kSnapshotIntervalFrames, 0, &task))) {
            g_frame_task = task;
        } else {
            log_message(ctx.sdk, "[core.system] snapshot scheduler unavailable");
        }
    }
    log_message(ctx.sdk, "[core.system] provider enabled: http queries");
}

void plugin_on_disabled(contracts::PluginContext& ctx) {
    stop_frame_task(ctx.sdk);
    SystemSnapshot empty_snapshot{};
    g_snapshot.publish(empty_snapshot);
    log_message(ctx.sdk, "[core.system] provider disabled: http queries withdrawn");
}

void plugin_fill_status(contracts::PluginStatusOut& out) {
    out.enabled = g_port != nullptr;
    out.error = nullptr;
}

const contracts::PluginDescriptor kDescriptor = {
    kPluginId,
    kPluginVersion,
    2,
    kRequires,
    0,
    nullptr,
    0,
    nullptr,
    0,
    nullptr,
    0,
    nullptr,
    &plugin_install,
    &plugin_deactivate,
    &plugin_on_enabled,
    &plugin_on_disabled,
    &plugin_fill_status,
    nullptr,
    kPorts,
    1,
    kHttpQueries,
    static_cast<uint32_t>(sizeof(kHttpQueries) / sizeof(kHttpQueries[0])),
};

}  // namespace

const contracts::PluginDescriptor& plugin_descriptor() {
    return kDescriptor;
}

}  // namespace qol::features::core_system
