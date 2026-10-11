#include "plugin.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "contracts/commands.h"
#include "contracts/host_sdk.h"
#include "contracts/http.h"
#include "json_field.h"

namespace qol::features::core_api {
namespace {

constexpr const char* kPluginId = "core.api";
constexpr const char* kPluginVersion = "0.1.0";
constexpr const char* kLifecyclePath = "/api/op/features/lifecycle";

contracts::CapabilityProvisionRef kProvides[] = {
    {contracts::kHttpRegistryCapabilityId, contracts::kHttpRegistryVersion, nullptr},
    {contracts::kHttpDispatchCapabilityId, contracts::kHttpDispatchVersion, nullptr},
};

#define QOL_HTTP_ROUTE(method, path, owner, op_gated, lease_required) \
    {contracts::HttpMethod::method, path, owner, op_gated, lease_required},
const contracts::HttpRoute kRoutes[] = {
#include "routes.inc"
};
#undef QOL_HTTP_ROUTE

const contracts::QolHttpRegistryApi* g_registry = nullptr;
const contracts::QolHostSdk* g_sdk = nullptr;

QolResult write_response(contracts::HttpRes* response, uint16_t status, const std::string& body) {
    if (response == nullptr || response->body == nullptr || response->body_capacity < body.size()) {
        return contracts::error(kQolErrInternal, "http response buffer too small", 31);
    }
    std::memcpy(response->body, body.data(), body.size());
    response->body_size = static_cast<uint32_t>(body.size());
    response->status = status;
    return contracts::ok();
}

QolResult error_response(contracts::HttpRes* response, uint16_t status, const char* message) {
    const QolResult written =
        write_response(response, status, std::string("{\"ok\":false,\"error\":\"") + message + "\"}");
    if (!contracts::succeeded(written)) return written;
    return contracts::error(kQolErrInvalidArgument, message,
                            static_cast<uint32_t>(std::strlen(message)));
}

int plugin_install(contracts::PluginContext& ctx) {
    if (ctx.sdk == nullptr || ctx.sdk->http.register_route == nullptr ||
        ctx.sdk->http_dispatch.register_command == nullptr) {
        return 1;
    }
    kProvides[0].iface = &ctx.sdk->http;
    kProvides[1].iface = &ctx.sdk->http_dispatch;
    g_registry = &ctx.sdk->http;
    for (const contracts::HttpRoute& route : kRoutes) {
        const QolResult result = g_registry->register_route(&route);
        if (!contracts::succeeded(result)) {
            (void)g_registry->unregister_owner(nullptr);
            g_registry = nullptr;
            return 1;
        }
    }
    // 路由已在上面注册，command handler 的 owner 校验据此通过；owner 即 core.api。
    const QolResult handler_result = ctx.sdk->http_dispatch.register_command(
        contracts::HttpMethod::kPost, kLifecyclePath, kPluginId, &lifecycle_command, nullptr);
    if (!contracts::succeeded(handler_result)) {
        (void)g_registry->unregister_owner(nullptr);
        g_registry = nullptr;
        return 1;
    }
    g_sdk = ctx.sdk;
    return 0;
}

void plugin_deactivate(contracts::PluginContext& ctx) {
    // 先撤 command handler（置墓碑 → 503），再冻结整个路由表。
    if (ctx.sdk != nullptr && ctx.sdk->http_dispatch.unregister_owner != nullptr) {
        (void)ctx.sdk->http_dispatch.unregister_owner(kPluginId);
    }
    const contracts::QolHttpRegistryApi* registry = g_registry;
    if (registry == nullptr && ctx.sdk != nullptr) registry = &ctx.sdk->http;
    if (registry != nullptr && registry->unregister_owner != nullptr) {
        // nullptr 表示整个 HTTP 出口：先冻结新 lease，宿主在途归零后清空路由。
        (void)registry->unregister_owner(nullptr);
    }
    g_registry = nullptr;
    g_sdk = nullptr;
}

void plugin_fill_status(contracts::PluginStatusOut& out) {
    out.enabled = g_registry != nullptr;
    out.error = nullptr;
}

// Feature callback does not replace the existing lifecycle or business controls.
constexpr contracts::FeatureDefinition kFeatures[] = {
    {"core.api.http_registry", 1, contracts::FeatureCategory::kHidden,
     contracts::FeatureType::kToggle, "HTTP 共享出口", "",
     [](contracts::PluginContext&, uint8_t) -> QolResult { return contracts::ok(); }, nullptr},
};

const contracts::PluginDescriptor kDescriptor = {
    kPluginId,
    kPluginVersion,
    0,
    nullptr,
    0,
    nullptr,
    2,
    kProvides,
    0,
    nullptr,
    0,
    nullptr,
    &plugin_install,
    &plugin_deactivate,
    nullptr,
    nullptr,
    &plugin_fill_status,
    nullptr,
    nullptr, 0, nullptr, 0,
    kFeatures, 1,
};

}  // namespace

// /api/op/features/lifecycle 的 owner 落点：body 解析 + action 映射 + 命令投递。
// 成功/未知动作/投递失败的响应串逐字节对齐迁移前 Kotlin JsonUtil 输出。
// 定义在匿名命名空间外，供 host 测试直接调用验证响应形状（生产经 http.dispatch 注册调用）。
QolResult lifecycle_command(const contracts::HttpReq* request, contracts::HttpRes* response, void*) {
    if (request == nullptr || response == nullptr) {
        return error_response(response, 400, "bad request");
    }
    std::string id;
    std::string action;
    bool has_id = false;
    bool has_action = false;
    if (!extract_string_fields(request->body, request->body_len, "id", &id, &has_id, "action",
                               &action, &has_action)) {
        return error_response(response, 400, "bad request");
    }
    if (!has_id || !has_action || id.empty() || action.empty()) {
        return error_response(response, 400, "id and action required");
    }
    int32_t action_code = 0;
    if (action == "deactivate") {
        action_code = contracts::kFeatureLifecycleDeactivate;
    } else if (action == "activate") {
        action_code = contracts::kFeatureLifecycleActivate;
    } else {
        // 未知动作是 HTTP 200 的失败信封（迁移前 service 直接返回 JsonUtil.err），非协议错误。
        return write_response(response, 200, "{\"ok\":false,\"error\":\"unknown action\"}");
    }
    contracts::QolCommandTicket ticket = 0;
    if (g_sdk != nullptr && g_sdk->commands.post != nullptr) {
        char id_buffer[64] = {};
        // 命令队列入队时按键值拷贝，命令到主线程 drain 才执行，故本地缓冲在返回后即可释放。
        std::snprintf(id_buffer, sizeof(id_buffer), "%s", id.c_str());
        const contracts::QolCommand command{contracts::kCommandFeatureLifecycle, id_buffer,
                                            action_code};
        const QolResult posted = g_sdk->commands.post(&ticket, &command);
        if (!contracts::succeeded(posted)) ticket = 0;
    }
    if (ticket == 0) {
        return write_response(response, 200, "{\"ok\":false,\"error\":\"lifecycle rejected\"}");
    }
    return write_response(response, 200,
                          "{\"ok\":true,\"ticket\":" + std::to_string(ticket) + "}");
}

const contracts::PluginDescriptor& plugin_descriptor() {
    return kDescriptor;
}

}  // namespace qol::features::core_api
