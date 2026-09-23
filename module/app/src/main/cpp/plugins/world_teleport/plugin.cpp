#include "plugin.h"

#include "../../contracts/ui_choice.h"
#include "../../contracts/game_ports/choice_port.h"
#include "../../contracts/game_ports/ports.h"
#include "../../contracts/game_ports/world_write_port.h"
#include "../../contracts/http.h"
#include "feature.h"

#include <cstdlib>
#include <cstring>
#include <string>

namespace qol::features::world_teleport {
namespace {

constexpr const char* kPluginId = "world_teleport";
constexpr const char* kPluginVersion = "0.1.0";

const contracts::QolHostSdk* g_sdk = nullptr;
const contracts::QolGameHost* g_game = nullptr;
const contracts::QolChoicePort* g_choice_port = nullptr;
const contracts::QolWorldWritePort* g_world_write_port = nullptr;
const contracts::WorldFactsApi* g_world_facts = nullptr;
bool g_installed = false;
const char* g_install_error = nullptr;

// 入口为 UIPlay_CallMapName 尾部的声明式 kCallPatch。game/access/patches 负责两个版本
// 指纹与尾部回跳元数据，patch/ 负责近址 thunk、handler 调用、固定 mov x0,#1 回写和回滚。
const contracts::ResourceClaim kResources[] = {
    {
        "patch:UIPlay_CallMapName",
        "P_UI_PLAY_CALL_MAP_NAME",
        contracts::ResourceMechanism::kCallPatch,
        contracts::ResourcePolicy::kPrivate,
        contracts::ResourceRequirement::kRequired,
        contracts::ResourceFailure::kFailClosed,
        true,
        kPluginId,
        "世界态地图名入口重定向到传送选择框",
        nullptr,
        120,
        reinterpret_cast<void*>(&world_teleport_call_map_name),
    },
};

const contracts::CapabilityRequirement kOptionalRequires[] = {
    {contracts::kUiChoiceCapabilityId,
     QolVersionRange{static_cast<uint16_t>(contracts::kUiChoiceVersionMajor),
                     static_cast<uint16_t>(contracts::kUiChoiceVersionMinor)}},
};

const contracts::CapabilityRequirement kRequires[] = {
    {contracts::kWorldFactsCapabilityId,
     {contracts::kWorldFactsVersionMajor, contracts::kWorldFactsVersionMinor}},
};

const contracts::QolPortRequirement kPorts[] = {
    {contracts::QolPortId::QOL_PORT_CHOICE,
     {contracts::kQolPortChoiceVersion.major, contracts::kQolPortChoiceVersion.minor}},
    {contracts::QolPortId::QOL_PORT_WORLD_WRITE,
     {contracts::kQolPortWorldWriteVersion.major, contracts::kQolPortWorldWriteVersion.minor}},
};

// ---- HTTP 世界写端点（owner 命令）：HTTP 线程只校验并投递票据，世界写在主线程结果处理器执行 ----
constexpr uint32_t kWorldWriteHandlerId = 0x57540001;  // 全局 typed handler id 空间，WT 前缀

constexpr int32_t kRouteMoveTo = 0;
constexpr int32_t kRouteWalkDir = 1;
constexpr int32_t kRouteStopMove = 2;
constexpr int32_t kRouteInteract = 3;
constexpr int32_t kRouteTeleport = 4;

struct WorldWritePayload {
    int32_t kind = 0;
    int32_t a = -1;
    int32_t b = -1;
    int32_t c = -1;
    int32_t d = -1;
};

// 规范化 body 的整数字段提取：body 已由 Kotlin 传输层校验为合法 JSON 并归一化，
// 本处只做字段提取（语法拒绝留在传输层，见批 5-1 结论）。
bool world_int_field(const char* body, const char* key, int* out) {
    if (body == nullptr || key == nullptr || out == nullptr) return false;
    const std::string needle = std::string("\"") + key + "\":";
    const char* p = std::strstr(body, needle.c_str());
    if (p == nullptr) return false;
    p += needle.size();
    while (*p == ' ') ++p;
    char* end = nullptr;
    const long value = std::strtol(p, &end, 10);
    if (end == p) return false;
    *out = static_cast<int>(value);
    return true;
}

bool world_bool_field(const char* body, const char* key, bool* out) {
    if (body == nullptr || key == nullptr || out == nullptr) return false;
    const std::string needle = std::string("\"") + key + "\":";
    const char* p = std::strstr(body, needle.c_str());
    if (p == nullptr) return false;
    p += needle.size();
    if (std::strncmp(p, "true", 4) == 0) {
        *out = true;
        return true;
    }
    if (std::strncmp(p, "false", 5) == 0) {
        *out = false;
        return true;
    }
    return false;
}

QolResult world_write_error(contracts::HttpRes* response, uint16_t status, const char* message) {
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

QolResult world_write_body(contracts::HttpRes* response, const std::string& body) {
    if (response == nullptr || response->body == nullptr ||
        response->body_capacity < body.size()) {
        return world_write_error(response, 500, "http response buffer too small");
    }
    std::memcpy(response->body, body.data(), body.size());
    response->body_size = static_cast<uint32_t>(body.size());
    response->status = 200;
    return contracts::ok();
}

QolResult world_write_command(const contracts::HttpReq* request, contracts::HttpRes* response,
                              void* ctx) {
    if (request == nullptr || response == nullptr || ctx == nullptr) {
        return world_write_error(response, 400, "bad request");
    }
    const int32_t route = *static_cast<const int32_t*>(ctx);
    const std::string text(request->body != nullptr
                               ? reinterpret_cast<const char*>(request->body)
                               : "",
                           request->body_len);
    const char* body = text.c_str();
    WorldWritePayload payload{};
    int value = -1;
    switch (route) {
        case kRouteMoveTo: {
            int x = -1;
            int y = -1;
            if (!world_int_field(body, "x", &x)) x = -1;
            if (!world_int_field(body, "y", &y)) y = -1;
            if (x < 0 || y < 0) return world_write_error(response, 400, "x/y required");
            // API 坐标统一瓦片：native data_op_move 收像素（tile*16+8），与迁移前 Kotlin 同口径。
            payload.kind = contracts::kQolWorldWriteMoveTo;
            payload.a = x * 16 + 8;
            payload.b = y * 16 + 8;
            break;
        }
        case kRouteWalkDir:
            if (!world_int_field(body, "direction", &value)) value = -1;
            if (value < 0 || value > 3) {
                return world_write_error(response, 400, "direction 0-3 required");
            }
            payload.kind = contracts::kQolWorldWriteWalkDir;
            payload.a = value;
            break;
        case kRouteStopMove:
            payload.kind = contracts::kQolWorldWriteStopMove;
            break;
        case kRouteInteract:
            payload.kind = contracts::kQolWorldWriteInteract;
            break;
        case kRouteTeleport: {
            int map_id = -1;
            int x = -1;
            int y = -1;
            const bool has = world_int_field(body, "map_id", &map_id) &&
                             world_int_field(body, "x", &x) && world_int_field(body, "y", &y);
            if (!has || map_id < 0 || x < 0 || y < 0) {
                return world_write_error(response, 400, "map_id/x/y required");
            }
            bool force = false;
            (void)world_bool_field(body, "force", &force);
            payload.kind = contracts::kQolWorldWriteTeleport;
            payload.a = map_id;
            payload.b = x;
            payload.c = y;
            payload.d = force ? 1 : 0;
            break;
        }
        default:
            return world_write_error(response, 500, "unknown world write route");
    }
    // 游戏写只能在主线程 owner command 执行（ARCHITECTURE.md §7.1）：HTTP 线程只投递票据。
    if (g_sdk == nullptr || g_sdk->commands.post_typed == nullptr) {
        return world_write_error(response, 503, "command port unavailable");
    }
    contracts::QolCommandTicket ticket = 0;
    if (!contracts::succeeded(g_sdk->commands.post_typed(kWorldWriteHandlerId, &payload,
                                                        sizeof(payload), &ticket)) ||
        ticket == 0) {
        return world_write_error(response, 503, "command queue unavailable");
    }
    return world_write_body(response, std::string("{\"ok\":true,\"ticket\":") +
                                          std::to_string(ticket) + "}");
}

bool world_write_result(void*, const uint8_t* payload, uint32_t size, char* out, uint32_t capacity,
                        uint32_t* out_size) {
    if (out_size != nullptr) *out_size = 0;
    if (g_world_write_port == nullptr || g_world_write_port->read.write == nullptr) return false;
    if (payload == nullptr || size != sizeof(WorldWritePayload) || out == nullptr ||
        out_size == nullptr) {
        return false;
    }
    WorldWritePayload p{};
    std::memcpy(&p, payload, size);
    return contracts::succeeded(g_world_write_port->read.write(p.kind, p.a, p.b, p.c, p.d, out,
                                                              capacity, out_size));
}

const contracts::HttpQueryHandlerDescriptor kWorldWriteQueries[] = {
    {contracts::HttpMethod::kPost, "/api/world/movement/move_to", kPluginId, &world_write_command,
     const_cast<int32_t*>(&kRouteMoveTo)},
    {contracts::HttpMethod::kPost, "/api/world/movement/walk_dir", kPluginId, &world_write_command,
     const_cast<int32_t*>(&kRouteWalkDir)},
    {contracts::HttpMethod::kPost, "/api/world/movement/stop_move", kPluginId,
     &world_write_command, const_cast<int32_t*>(&kRouteStopMove)},
    {contracts::HttpMethod::kPost, "/api/world/movement/interact_with", kPluginId,
     &world_write_command, const_cast<int32_t*>(&kRouteInteract)},
    {contracts::HttpMethod::kPost, "/api/op/movement/teleport", kPluginId, &world_write_command,
     const_cast<int32_t*>(&kRouteTeleport)},
};

int plugin_install(contracts::PluginContext& ctx) {
    g_sdk = ctx.sdk;
    g_game = ctx.game;
    g_choice_port = nullptr;
    g_world_facts = nullptr;
    g_installed = false;
    g_install_error = nullptr;
    if (ctx.struct_size < sizeof(contracts::PluginContext) ||
        ctx.sdk_major != contracts::kHostSdkMajor || g_sdk == nullptr || g_game == nullptr ||
        ctx.ports == nullptr || ctx.port_count != 2 || ctx.ports[0] == nullptr ||
        ctx.ports[1] == nullptr) {
        g_sdk = nullptr;
        g_game = nullptr;
        g_choice_port = nullptr;
        g_world_write_port = nullptr;
        g_install_error = "install failed: world teleport dependencies not ready";
        return 1;
    }
    g_choice_port = static_cast<const contracts::QolChoicePort*>(ctx.ports[0]);
    g_world_write_port = static_cast<const contracts::QolWorldWritePort*>(ctx.ports[1]);
    if (!contracts::satisfies(
            g_world_write_port->abi_version,
            {contracts::kQolPortWorldWriteVersion.major, contracts::kQolPortWorldWriteVersion.minor}) ||
        g_world_write_port->read.write == nullptr) {
        g_sdk = nullptr;
        g_game = nullptr;
        g_choice_port = nullptr;
        g_world_write_port = nullptr;
        g_install_error = "install failed: world write port unavailable";
        return 1;
    }
    // 结果处理器需 host 尾部的票据结果面（QolHostSdk 尾部追加，按 abi 协商）。
    if (g_sdk->abi.size < sizeof(contracts::QolHostSdk) || g_sdk->abi.minor < 9 ||
        g_sdk->ticket_result.register_typed_result_handler == nullptr ||
        !contracts::succeeded(g_sdk->ticket_result.register_typed_result_handler(
            kWorldWriteHandlerId, &world_write_result, nullptr))) {
        g_sdk = nullptr;
        g_game = nullptr;
        g_choice_port = nullptr;
        g_world_write_port = nullptr;
        g_install_error = "install failed: world write command registration failed";
        return 1;
    }
    const void* world_facts_iface = nullptr;
    if (g_sdk->capabilities.require == nullptr ||
        !contracts::succeeded(g_sdk->capabilities.require(
            contracts::kWorldFactsCapabilityId,
            {contracts::kWorldFactsVersionMajor, contracts::kWorldFactsVersionMinor},
            &world_facts_iface)) ||
        world_facts_iface == nullptr) {
        g_sdk = nullptr;
        g_game = nullptr;
        g_choice_port = nullptr;
        g_install_error = "install failed: world facts unavailable";
        return 1;
    }
    g_world_facts = static_cast<const contracts::WorldFactsApi*>(world_facts_iface);
    return 0;
}

void plugin_deactivate(contracts::PluginContext& ctx) {
    (void)ctx;
    // 自注册的结果处理器不在 descriptor（由宿主按生命周期注册的是 http 路由），需显式注销。
    if (g_sdk != nullptr && g_sdk->ticket_result.unregister_typed_result_handler != nullptr) {
        (void)g_sdk->ticket_result.unregister_typed_result_handler(kWorldWriteHandlerId);
    }
    g_installed = false;
    g_world_facts = nullptr;
    g_game = nullptr;
    g_choice_port = nullptr;
    g_world_write_port = nullptr;
    g_sdk = nullptr;
}

void plugin_on_enabled(contracts::PluginContext& ctx) {
    (void)ctx;
    g_installed = true;
    g_install_error = nullptr;
}

void plugin_on_disabled(contracts::PluginContext& ctx) {
    (void)ctx;
    g_installed = false;
}

void plugin_fill_status(contracts::PluginStatusOut& out) {
    out.enabled = g_installed;
    out.error = g_install_error;
}

const contracts::PluginDescriptor kDescriptor = {
    kPluginId,
    kPluginVersion,
    1,
    kRequires,
    1,
    kOptionalRequires,
    0,
    nullptr,
    1,
    kResources,
    0,
    nullptr,
    &plugin_install,
    &plugin_deactivate,
    &plugin_on_enabled,
    &plugin_on_disabled,
    &plugin_fill_status,
    nullptr,
    kPorts,
    2,
    kWorldWriteQueries,
    static_cast<uint32_t>(sizeof(kWorldWriteQueries) / sizeof(kWorldWriteQueries[0])),
};

}  // namespace

const contracts::PluginDescriptor& plugin_descriptor() {
    return kDescriptor;
}

const contracts::QolHostSdk* host_sdk() {
    return g_sdk;
}

const contracts::QolGameHost* game_host() {
    return g_game;
}

const contracts::QolChoicePort* choice_port() {
    return g_choice_port;
}

const contracts::WorldFactsApi* world_facts() {
    return g_world_facts;
}

}  // namespace qol::features::world_teleport
