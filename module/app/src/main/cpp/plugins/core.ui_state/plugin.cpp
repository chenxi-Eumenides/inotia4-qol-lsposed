#include "plugin.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "contracts/commands.h"
#include "contracts/game_ports/choice_write_port.h"
#include "contracts/game_ports/ui_write_port.h"
#include "contracts/http.h"

namespace qol::features::core_ui_state {
namespace {

constexpr const char* kPluginId = "core.ui_state";
constexpr const char* kPluginVersion = "0.1.0";

const contracts::QolHostSdk* g_sdk = nullptr;

const contracts::CapabilityRequirement kRequires[] = {
    {
        contracts::kHttpRegistryCapabilityId,
        {contracts::kHttpRegistryVersion.major, contracts::kHttpRegistryVersion.minor},
    },
    {
        contracts::kHttpDispatchCapabilityId,
        {contracts::kHttpDispatchVersion.major, contracts::kHttpDispatchVersion.minor},
    },
};

// 选择控件（能力 + 2 条资源 + QOL_PORT_CHOICE）已按 ADR 0001 迁入 core.custom_ui；
// 本插件只保留 UI 写面的两条端口。
const contracts::QolPortRequirement kPorts[] = {
    {contracts::QolPortId::QOL_PORT_CHOICE_WRITE,
     {contracts::kQolPortChoiceWriteVersion.major, contracts::kQolPortChoiceWriteVersion.minor}},
    {contracts::QolPortId::QOL_PORT_UI_WRITE,
     {contracts::kQolPortUiWriteVersion.major, contracts::kQolPortUiWriteVersion.minor}},
};

// 对话写（HTTP 写路由）的票据结果处理器 id（全局 typed handler id 空间；core.ui_state 前缀）。
constexpr uint32_t kChoiceWriteHandlerId = 0x4E430001;

struct ChoiceWritePayload {
    int32_t kind = 0;
    int32_t index = -1;
    char action[48] = {};
};

// 路由 ctx：需为可取址的具名常量（HttpQueryHandlerDescriptor 存 void*）。
constexpr int32_t kDialogSelectKind = contracts::kQolChoiceWriteDialogSelect;
constexpr int32_t kNpcInteractKind = contracts::kQolChoiceWriteNpcInteract;

const contracts::QolChoiceWritePort* g_choice_write = nullptr;

QolResult choice_http_error(contracts::HttpRes* response, uint16_t status, const char* message) {
    const std::string body = std::string("{\"ok\":false,\"error\":\"") + message + "\"}";
    if (response == nullptr || response->body == nullptr || response->body_capacity < body.size()) {
        return contracts::error(kQolErrInternal, "http response buffer too small", 31);
    }
    std::memcpy(response->body, body.data(), body.size());
    response->body_size = static_cast<uint32_t>(body.size());
    response->status = status;
    return contracts::error(status == 400 ? kQolErrInvalidArgument : kQolErrInternal, message,
                            static_cast<uint32_t>(std::strlen(message)));
}

QolResult choice_http_body(contracts::HttpRes* response, const std::string& body) {
    if (response == nullptr || response->body == nullptr ||
        response->body_capacity < body.size()) {
        return choice_http_error(response, 500, "http response buffer too small");
    }
    std::memcpy(response->body, body.data(), body.size());
    response->body_size = static_cast<uint32_t>(body.size());
    response->status = 200;
    return contracts::ok();
}

// body 字段提取（宽松）：body 语法拒绝留在 Kotlin 传输层（与 save/world 同口径）。
void choice_string_field(const char* body, const char* key, char* out, uint32_t capacity) {
    if (body == nullptr || key == nullptr || out == nullptr || capacity == 0) return;
    out[0] = '\0';
    const std::string needle = std::string("\"") + key + "\":\"";
    const std::string text(body);
    const size_t at = text.find(needle);
    if (at == std::string::npos) return;
    const size_t begin = at + needle.size();
    const size_t end = text.find('"', begin);
    if (end == std::string::npos) return;
    const std::string value = text.substr(begin, end - begin);
    std::snprintf(out, capacity, "%s", value.c_str());
}

void choice_int_field(const char* body, const char* key, int32_t* out) {
    if (body == nullptr || key == nullptr || out == nullptr) return;
    const std::string needle = std::string("\"") + key + "\":";
    const std::string text(body);
    const size_t at = text.find(needle);
    if (at == std::string::npos) return;
    *out = static_cast<int32_t>(std::strtol(text.c_str() + at + needle.size(), nullptr, 10));
}

// HTTP command handler（HTTP 线程）：只校验并投递票据，不直接写对话态。
QolResult choice_write_command(const contracts::HttpReq* request, contracts::HttpRes* response,
                               void* ctx) {
    if (request == nullptr || response == nullptr || ctx == nullptr) {
        return choice_http_error(response, 400, "bad request");
    }
    const int32_t kind = *static_cast<const int32_t*>(ctx);
    const std::string body_text(request->body != nullptr
                                    ? reinterpret_cast<const char*>(request->body)
                                    : "",
                                request->body_len);
    ChoiceWritePayload payload{};
    payload.kind = kind;
    if (kind == contracts::kQolChoiceWriteDialogSelect) {
        choice_string_field(body_text.c_str(), "action", payload.action,
                            static_cast<uint32_t>(sizeof(payload.action)));
        payload.index = -1;
        choice_int_field(body_text.c_str(), "index", &payload.index);
        if (payload.action[0] == '\0' && payload.index < 0) {
            return choice_http_error(response, 400, "action or index required");
        }
    }
    if (g_sdk == nullptr || g_sdk->commands.post_typed == nullptr) {
        return choice_http_error(response, 503, "command port unavailable");
    }
    contracts::QolCommandTicket ticket = 0;
    if (!contracts::succeeded(g_sdk->commands.post_typed(kChoiceWriteHandlerId, &payload,
                                                        sizeof(payload), &ticket)) ||
        ticket == 0) {
        return choice_http_error(response, 503, "command queue unavailable");
    }
    return choice_http_body(response, std::string("{\"ok\":true,\"ticket\":") +
                                          std::to_string(ticket) + "}");
}

// 结果处理器（主线程）：执行对话写并写出迁移前的原响应 JSON。
bool choice_write_result(void*, const uint8_t* payload, uint32_t size, char* out, uint32_t capacity,
                         uint32_t* out_size) {
    if (out_size != nullptr) *out_size = 0;
    if (payload == nullptr || size != sizeof(ChoiceWritePayload) || out == nullptr ||
        out_size == nullptr || g_choice_write == nullptr ||
        g_choice_write->read.write == nullptr) {
        return false;
    }
    ChoiceWritePayload p{};
    std::memcpy(&p, payload, size);
    const QolResult result = g_choice_write->read.write(p.kind, p.action, p.index, out, capacity,
                                                        out_size);
    return contracts::succeeded(result);
}

// 面板写（HTTP 写路由）使用独立的结果处理器 id：面板 kind 与对话 kind 各自从 0 起，
// 共用一个 payload/处理器会产生取值域歧义。
constexpr uint32_t kUiWriteHandlerId = 0x55490001;

struct UiWritePayload {
    int32_t kind = 0;
    char panel[64] = {};
};

constexpr int32_t kPanelOpenKind = contracts::kQolUiWritePanelOpen;
constexpr int32_t kPanelCloseKind = contracts::kQolUiWritePanelClose;

const contracts::QolUiWritePort* g_ui_write = nullptr;

// HTTP command handler（HTTP 线程）：只校验并投递票据，不直接改游戏面板栈。
QolResult ui_write_command(const contracts::HttpReq* request, contracts::HttpRes* response,
                           void* ctx) {
    if (request == nullptr || response == nullptr || ctx == nullptr) {
        return choice_http_error(response, 400, "bad request");
    }
    const int32_t kind = *static_cast<const int32_t*>(ctx);
    const std::string body_text(request->body != nullptr
                                    ? reinterpret_cast<const char*>(request->body)
                                    : "",
                                request->body_len);
    UiWritePayload payload{};
    payload.kind = kind;
    if (kind == contracts::kQolUiWritePanelOpen) {
        choice_string_field(body_text.c_str(), "panel", payload.panel,
                            static_cast<uint32_t>(sizeof(payload.panel)));
        if (payload.panel[0] == '\0') {
            return choice_http_error(response, 400, "panel required");
        }
    }
    if (g_sdk == nullptr || g_sdk->commands.post_typed == nullptr) {
        return choice_http_error(response, 503, "command port unavailable");
    }
    contracts::QolCommandTicket ticket = 0;
    if (!contracts::succeeded(g_sdk->commands.post_typed(kUiWriteHandlerId, &payload,
                                                        sizeof(payload), &ticket)) ||
        ticket == 0) {
        return choice_http_error(response, 503, "command queue unavailable");
    }
    return choice_http_body(response, std::string("{\"ok\":true,\"ticket\":") +
                                          std::to_string(ticket) + "}");
}

// 结果处理器（主线程）：执行面板写并写出迁移前的原响应 JSON（附 state=<gamestate>）。
bool ui_write_result(void*, const uint8_t* payload, uint32_t size, char* out, uint32_t capacity,
                     uint32_t* out_size) {
    if (out_size != nullptr) *out_size = 0;
    if (payload == nullptr || size != sizeof(UiWritePayload) || out == nullptr ||
        out_size == nullptr || g_ui_write == nullptr || g_ui_write->read.write == nullptr) {
        return false;
    }
    UiWritePayload p{};
    std::memcpy(&p, payload, size);
    const QolResult result = g_ui_write->read.write(p.kind, p.panel, out, capacity, out_size);
    return contracts::succeeded(result);
}

// 声明式写路由：对话写 + 面板写共用一个数组（descriptor 只有一组 http_queries）。
const contracts::HttpQueryHandlerDescriptor kWriteRoutes[] = {
    {contracts::HttpMethod::kPost, "/api/ui/dialog/select", kPluginId, &choice_write_command,
     const_cast<int32_t*>(&kDialogSelectKind)},
    {contracts::HttpMethod::kPost, "/api/ui/start_interact", kPluginId, &choice_write_command,
     const_cast<int32_t*>(&kNpcInteractKind)},
    {contracts::HttpMethod::kPost, "/api/ui/open_panel", kPluginId, &ui_write_command,
     const_cast<int32_t*>(&kPanelOpenKind)},
    {contracts::HttpMethod::kPost, "/api/ui/close_panel", kPluginId, &ui_write_command,
     const_cast<int32_t*>(&kPanelCloseKind)},
};

int plugin_install(contracts::PluginContext& ctx) {
    g_sdk = ctx.sdk;
    g_choice_write = nullptr;
    g_ui_write = nullptr;
    if (g_sdk == nullptr || ctx.ports == nullptr || ctx.port_count < 2 ||
        ctx.ports[0] == nullptr || ctx.ports[1] == nullptr) {
        g_sdk = nullptr;
        return 1;
    }
    g_choice_write = static_cast<const contracts::QolChoiceWritePort*>(ctx.ports[0]);
    g_ui_write = static_cast<const contracts::QolUiWritePort*>(ctx.ports[1]);
    if (g_choice_write->read.write == nullptr || g_ui_write->read.write == nullptr) {
        g_sdk = nullptr;
        g_choice_write = nullptr;
        g_ui_write = nullptr;
        return 1;
    }
    // 结果处理器需要 host 尾部的票据结果面（QolHostSdk 尾部追加，按 abi 协商）。
    if (g_sdk->abi.size < sizeof(contracts::QolHostSdk) || g_sdk->abi.minor < 9 ||
        g_sdk->ticket_result.register_typed_result_handler == nullptr ||
        !contracts::succeeded(g_sdk->ticket_result.register_typed_result_handler(
            kChoiceWriteHandlerId, &choice_write_result, nullptr))) {
        g_sdk = nullptr;
        g_choice_write = nullptr;
        g_ui_write = nullptr;
        return 1;
    }
    if (!contracts::succeeded(g_sdk->ticket_result.register_typed_result_handler(
            kUiWriteHandlerId, &ui_write_result, nullptr))) {
        (void)g_sdk->ticket_result.unregister_typed_result_handler(kChoiceWriteHandlerId);
        g_sdk = nullptr;
        g_choice_write = nullptr;
        g_ui_write = nullptr;
        return 1;
    }
    // 自身初始化无额外动作：资源由宿主声明式安装，backup 经 on_resource_ready 交还。
    return 0;
}

void plugin_deactivate(contracts::PluginContext& ctx) {
    (void)ctx;
    if (g_sdk != nullptr && g_sdk->ticket_result.unregister_typed_result_handler != nullptr) {
        (void)g_sdk->ticket_result.unregister_typed_result_handler(kChoiceWriteHandlerId);
        (void)g_sdk->ticket_result.unregister_typed_result_handler(kUiWriteHandlerId);
    }
    g_sdk = nullptr;
    g_choice_write = nullptr;
    g_ui_write = nullptr;
}

void plugin_on_enabled(contracts::PluginContext& ctx) {
    (void)ctx;
}

void plugin_on_disabled(contracts::PluginContext& ctx) {
    (void)ctx;
}

void plugin_fill_status(contracts::PluginStatusOut& out) {
    out.enabled = true;  // 零门禁常驻
    out.error = nullptr;
}

const contracts::PluginDescriptor kDescriptor = {
    kPluginId,
    kPluginVersion,
    2, kRequires, // requires=http.registry + http.dispatch（声明式写路由）
    0, nullptr,   // optional_requires
    0, nullptr,   // provides（ui_choice 已于 ADR 0001 迁入 core.custom_ui）
    0, nullptr,   // resources（选择控件的 2 条资源随控件迁走）
    0, nullptr,   // jni
    &plugin_install,
    &plugin_deactivate,
    &plugin_on_enabled,
    &plugin_on_disabled,
    &plugin_fill_status,
    nullptr,      // on_resource_ready（本插件已无资源）
    kPorts,
    2,
    kWriteRoutes,
    static_cast<uint32_t>(sizeof(kWriteRoutes) / sizeof(kWriteRoutes[0])),
};

}  // namespace

const contracts::PluginDescriptor& plugin_descriptor() {
    return kDescriptor;
}

const contracts::QolHostSdk* host_sdk() { return g_sdk; }

}  // namespace qol::features::core_ui_state
