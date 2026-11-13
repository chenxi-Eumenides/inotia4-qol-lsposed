#include "plugin.h"

#include <cstdint>
#include <cstring>
#include <string>

#include "contracts/game_ports/system_port.h"
#include "contracts/host_sdk.h"
#include "contracts/http.h"
#include "snapshot_sdk.h"

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

enum class Query : uint8_t { kSnapshot, kGame, kInfo, kFrame, kEvents };

constexpr contracts::HttpRoute kRoutes[] = {
    {contracts::HttpMethod::kGet, "/api/system/snapshot", kPluginId, 0, 1},
    {contracts::HttpMethod::kGet, "/api/system/game", kPluginId, 0, 1},
    {contracts::HttpMethod::kGet, "/api/system/info", kPluginId, 0, 1},
    {contracts::HttpMethod::kGet, "/api/system/game_frame", kPluginId, 0, 1},
    {contracts::HttpMethod::kGet, "/api/system/events", kPluginId, 0, 1},
};

Query kQueries[] = {Query::kSnapshot, Query::kGame, Query::kInfo, Query::kFrame,
                    Query::kEvents};

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
bool g_snapshot_failure_warned = false;

const contracts::QolPortRequirement kPorts[] = {
    {contracts::QolPortId::QOL_PORT_SYSTEM, {1, 1}},
};

const contracts::CapabilityRequirement kOptionalHttpRequires[] = {
    {contracts::kHttpRegistryCapabilityId, {contracts::kHttpRegistryVersion.major,
                                            contracts::kHttpRegistryVersion.minor}},
    {contracts::kHttpDispatchCapabilityId, {contracts::kHttpDispatchVersion.major,
                                             contracts::kHttpDispatchVersion.minor}},
};

void log_message(const contracts::QolHostSdk* sdk, const char* message, int level = 1) {
    if (sdk == nullptr || sdk->log.write == nullptr || message == nullptr) return;
    uint32_t length = 0;
    while (message[length] != '\0') ++length;
    (void)sdk->log.write(level, 1, kPluginId, message, length);
}

void warn_snapshot_failure() {
    if (!g_snapshot_failure_warned) {
        constexpr char kMessage[] =
            "fail op=snapshot_read kind=system reason=read_failed_or_capacity keep=previous";
        if (g_sdk != nullptr && g_sdk->log.write != nullptr) {
            (void)g_sdk->log.write(2, 1, kPluginId, kMessage, sizeof(kMessage) - 1);
        }
        g_snapshot_failure_warned = true;
    }
}

void update_snapshot() {
    if (g_port == nullptr || g_port->read.body_json == nullptr) return;
    static SystemSnapshot next{};
    std::memset(&next, 0, sizeof(next));
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
        if (!contracts::succeeded(result) || *slot.size == 0 || *slot.size >= slot.capacity) {
            *slot.size = 0;
            // 身份未下发时 info/game 暂不可用；快照与帧计数与其无关，照常发布。
            const bool identity_dependent =
                slot.kind == static_cast<int32_t>(contracts::QolSystemJsonKind::kInfo) ||
                slot.kind == static_cast<int32_t>(contracts::QolSystemJsonKind::kGame);
            if (!identity_dependent) core_ready = false;
        }
    }
    if (!core_ready) {
        warn_snapshot_failure();
        return;
    }
    const bool recovered = g_snapshot_failure_warned;
    next.state = SnapshotState::kReady;
    g_snapshot.publish(next);
    g_snapshot_failure_warned = false;
    if (recovered) log_message(g_sdk, "restore op=snapshot_read kind=system result=ready");
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
    const Query query = *static_cast<const Query*>(ctx);
    if (query == Query::kEvents) {
        if (g_port == nullptr || g_port->read.events_json == nullptr) {
            return response_error(response, 503, "snapshot not ready");
        }
        uint32_t size = 0;
        const QolResult result = g_port->read.events_json(
            reinterpret_cast<char*>(response->body), response->body_capacity, &size);
        if (!contracts::succeeded(result) || size > response->body_capacity) {
            warn_snapshot_failure();
            return response_error(response, 503, "snapshot not ready");
        }
        response->body_size = size;
        response->status = 200;
        return contracts::ok();
    }
    SystemSnapshot snapshot{};
    if (!g_snapshot.read(snapshot) || snapshot.state != SnapshotState::kReady) {
        return response_error(response, 503, "snapshot not ready");
    }
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
        case Query::kEvents:
            break;
    }
    return response_error(response, 500, "unknown system query");
}

const contracts::HttpEndpointDescriptor kHttpEndpoints[] = {
    {kRoutes[0].method, kRoutes[0].path, kPluginId, contracts::HttpHandlerKind::kQuery,
     0, 1, &query_handler, nullptr, &kQueries[0]},
    {kRoutes[1].method, kRoutes[1].path, kPluginId, contracts::HttpHandlerKind::kQuery,
     0, 1, &query_handler, nullptr, &kQueries[1]},
    {kRoutes[2].method, kRoutes[2].path, kPluginId, contracts::HttpHandlerKind::kQuery,
     0, 1, &query_handler, nullptr, &kQueries[2]},
    {kRoutes[3].method, kRoutes[3].path, kPluginId, contracts::HttpHandlerKind::kQuery,
     0, 1, &query_handler, nullptr, &kQueries[3]},
    {kRoutes[4].method, kRoutes[4].path, kPluginId, contracts::HttpHandlerKind::kQuery,
     0, 1, &query_handler, nullptr, &kQueries[4]},
};

const contracts::ResourceClaim kResourceClaims[] = {
    {"patch:MAPITEMSYSTEM_AllocateSlot+0x18",
     "P_MAPITEMSYSTEM_ALLOCATE_SLOT_CAP",
     contracts::ResourceMechanism::kCallPatch,
     contracts::ResourcePolicy::kPrivate,
     contracts::ResourceRequirement::kOptional,
     contracts::ResourceFailure::kFailSoft,
     true,
     kPluginId,
     "地面掉落池槽位上限 32→127；与池分配及切图循环界三条同组，缺一不可",
     nullptr,
     120,
     nullptr},
    {"patch:MAPITEMSYSTEM_Create+0x4",
     "P_MAPITEMSYSTEM_CREATE_POOL_SIZE",
     contracts::ResourceMechanism::kCallPatch,
     contracts::ResourcePolicy::kPrivate,
     contracts::ResourceRequirement::kOptional,
     contracts::ResourceFailure::kFailSoft,
     true,
     kPluginId,
     "地面掉落池分配大小 0x400→0xfe0；与槽位上限及切图循环界三条同组，缺一不可",
     nullptr,
     120,
     nullptr},
    {"patch:MAPITEMSYSTEM_Initialize+0x34",
     "P_MAPITEMSYSTEM_INITIALIZE_POOL_LIMIT",
     contracts::ResourceMechanism::kCallPatch,
     contracts::ResourcePolicy::kPrivate,
     contracts::ResourceRequirement::kOptional,
     contracts::ResourceFailure::kFailSoft,
     true,
     kPluginId,
     "切图初始化循环界扩至 0xfe0；与容量比较及池分配三条同组，缺一不可",
     nullptr,
     120,
     nullptr},
    {"patch:MAPITEMSYSTEM_Unload+0x28",
     "P_MAPITEMSYSTEM_UNLOAD_POOL_LIMIT",
     contracts::ResourceMechanism::kCallPatch,
     contracts::ResourcePolicy::kPrivate,
     contracts::ResourceRequirement::kOptional,
     contracts::ResourceFailure::kFailSoft,
     true,
     kPluginId,
     "切图卸载循环界扩至 0xfe0；与容量比较及池分配三条同组，缺一不可",
     nullptr,
     120,
     nullptr},
    {"patch:ITEMPOOL_Create+0x4",
     "P_ITEMPOOL_CREATE_SIZE",
     contracts::ResourceMechanism::kCallPatch,
     contracts::ResourcePolicy::kPrivate,
     contracts::ResourceRequirement::kOptional,
     contracts::ResourceFailure::kFailSoft,
     true,
     kPluginId,
     "物品对象池分配大小 0x2ee0→0xfff0（300→1638 槽）；与初始化及分配界三条同组，缺一不可",
     nullptr,
     120,
     nullptr},
    {"patch:ITEMPOOL_Initialize+0x18",
     "P_ITEMPOOL_INITIALIZE_LIMIT",
     contracts::ResourceMechanism::kCallPatch,
     contracts::ResourcePolicy::kPrivate,
     contracts::ResourceRequirement::kOptional,
     contracts::ResourceFailure::kFailSoft,
     true,
     kPluginId,
     "物品对象池初始化循环界扩至 0xfff0；与池分配及分配界三条同组，缺一不可",
     nullptr,
     120,
     nullptr},
    {"patch:ITEMPOOL_Allocate+0x18",
     "P_ITEMPOOL_ALLOCATE_BOUND_HI",
     contracts::ResourceMechanism::kCallPatch,
     contracts::ResourcePolicy::kPrivate,
     contracts::ResourceRequirement::kOptional,
     contracts::ResourceFailure::kFailSoft,
     true,
     kPluginId,
     "物品对象池扫描上界高 12 位 0x2→0xf；与分配大小及低 12 位两条同组",
     nullptr,
     120,
     nullptr},
    {"patch:ITEMPOOL_Allocate+0x1c",
     "P_ITEMPOOL_ALLOCATE_BOUND_LO",
     contracts::ResourceMechanism::kCallPatch,
     contracts::ResourcePolicy::kPrivate,
     contracts::ResourceRequirement::kOptional,
     contracts::ResourceFailure::kFailSoft,
     true,
     kPluginId,
     "物品对象池扫描上界低 12 位 0xee0→0xff0；与分配大小及高 12 位两条同组",
     nullptr,
     120,
     nullptr},
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
    if (ctx.struct_size < contracts::PLUGIN_CONTEXT_PORTS_STRUCT_SIZE ||
        ctx.sdk_major != contracts::kHostSdkMajor || g_sdk == nullptr || ctx.ports == nullptr ||
        ctx.port_count != 1 || ctx.ports[0] == nullptr) {
        g_sdk = nullptr;
        return 1;
    }
    g_port = static_cast<const contracts::QolSystemPort*>(ctx.ports[0]);
    if (!contracts::satisfies(g_port->abi_version, {1, 1}) || g_port->read.body_json == nullptr ||
        g_port->read.events_json == nullptr) {
        g_port = nullptr;
        g_sdk = nullptr;
        return 1;
    }
    if (!g_snapshot.initialize(g_sdk)) {
        g_port = nullptr;
        g_sdk = nullptr;
        return 1;
    }
    SystemSnapshot empty_snapshot{};
    g_snapshot.publish(empty_snapshot);
    g_snapshot_failure_warned = false;
    log_message(g_sdk, "op=install provider=core.system result=ok");
    return 0;
}

void plugin_deactivate(contracts::PluginContext& ctx) {
    stop_frame_task(ctx.sdk);
    g_snapshot.destroy();
    g_port = nullptr;
    g_sdk = nullptr;
}

void plugin_on_enabled(contracts::PluginContext& ctx) {
    (void)ctx;
    if (!g_snapshot.initialize(g_sdk)) {
        return;
    }
    SystemSnapshot empty_snapshot{};
    g_snapshot.publish(empty_snapshot);
    g_snapshot_failure_warned = false;
    if (g_frame_task == 0 && g_sdk != nullptr && g_sdk->scheduler.add != nullptr) {
        uint64_t task = 0;
        if (contracts::succeeded(g_sdk->scheduler.add(
                1, &frame_tick, nullptr, kSnapshotIntervalFrames, 0, &task))) {
            g_frame_task = task;
        } else {
        }
    }
}

void plugin_on_disabled(contracts::PluginContext& ctx) {
    stop_frame_task(ctx.sdk);
    g_snapshot.destroy();
}

void plugin_fill_status(contracts::PluginStatusOut& out) {
    out.enabled = g_port != nullptr;
    out.error = nullptr;
}

// Feature callback does not replace the existing lifecycle or business controls.
constexpr contracts::FeatureDefinition kFeatures[] = {
    {"core.system.snapshots", 1, contracts::FeatureCategory::kHidden,
     contracts::FeatureType::kToggle, "系统状态服务", "",
     [](contracts::PluginContext&, uint8_t) -> QolResult { return contracts::ok(); }, nullptr},
};

const contracts::PluginDescriptor kDescriptor = {
    kPluginId,
    kPluginVersion,
    0,
    nullptr,     // requires
    2,
    kOptionalHttpRequires,
    0,
    nullptr,     // provides
    kResourceCount,
    kResourceClaims,
    0,
    nullptr,     // jni
    &plugin_install,
    &plugin_deactivate,
    &plugin_on_enabled,
    &plugin_on_disabled,
    &plugin_fill_status,
    nullptr,
    kPorts,
    1,
    nullptr,
    0,
    kFeatures, 1,
    kHttpEndpoints,
    static_cast<uint32_t>(sizeof(kHttpEndpoints) / sizeof(kHttpEndpoints[0])),
    nullptr,
    nullptr, 0,
};

}  // namespace

const contracts::PluginDescriptor& plugin_descriptor() {
    return kDescriptor;
}

}  // namespace qol::features::core_system
