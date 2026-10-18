#include "plugins/extension_bag/http/routes.h"

#include "plugins/extension_bag/game_ui_virtbag.h"
#include "plugins/extension_bag/http/query.h"
#include "plugins/extension_bag/plugin.h"
#include "plugins/extension_bag/model/virtual_bag_state.h"

#include <cstring>
#include <string>
#include <string_view>

namespace qol::features::extension_bag {
namespace {

constexpr const char* kPluginId = "extension_bag";

// ---- HTTP command handlers（/api/debug/extension_bag/*，owner=extension_bag）----
// Host 从 descriptor 绑定及撤销 HTTP handlers；票据结果处理器仍由插件生命周期管理。
// body 校验逐字对齐迁移前 Kotlin DebugController 的 ApiException 文本与状态码。

enum class EbCommand : uint8_t {
    kEnterView,
    kExitView,
    kUnequip,
    kSelectBag,
    kClickItem,
    kEquip,
    kItem,
};

struct EbCommandRoute {
    const char* path;
    EbCommand kind;
};

EbCommandRoute kEbCommandRoutes[] = {
    {"/api/debug/extension_bag/enter_view", EbCommand::kEnterView},
    {"/api/debug/extension_bag/exit_view", EbCommand::kExitView},
    {"/api/debug/extension_bag/unequip", EbCommand::kUnequip},
    {"/api/debug/extension_bag/select_bag", EbCommand::kSelectBag},
    {"/api/debug/extension_bag/click_item", EbCommand::kClickItem},
    {"/api/debug/extension_bag/equip", EbCommand::kEquip},
    {"/api/debug/extension_bag/item", EbCommand::kItem},
};

QolResult eb_command_handler(const contracts::HttpReq* request, contracts::HttpRes* response,
                             void* ctx);

}  // namespace

const qol::contracts::HttpEndpointDescriptor kHttpEndpoints[kHttpEndpointCount] = {
    {qol::contracts::HttpMethod::kGet, "/api/debug/extension_bag/status", kPluginId,
     qol::contracts::HttpHandlerKind::kQuery, 0, 1, &status_query_handler, nullptr, nullptr},
    {qol::contracts::HttpMethod::kPost, "/api/debug/extension_bag/enter_view", kPluginId,
     qol::contracts::HttpHandlerKind::kCommand, 0, 1, nullptr, &eb_command_handler,
     &kEbCommandRoutes[0].kind},
    {qol::contracts::HttpMethod::kPost, "/api/debug/extension_bag/exit_view", kPluginId,
     qol::contracts::HttpHandlerKind::kCommand, 0, 1, nullptr, &eb_command_handler,
     &kEbCommandRoutes[1].kind},
    {qol::contracts::HttpMethod::kPost, "/api/debug/extension_bag/unequip", kPluginId,
     qol::contracts::HttpHandlerKind::kCommand, 0, 1, nullptr, &eb_command_handler,
     &kEbCommandRoutes[2].kind},
    {qol::contracts::HttpMethod::kPost, "/api/debug/extension_bag/select_bag", kPluginId,
     qol::contracts::HttpHandlerKind::kCommand, 0, 1, nullptr, &eb_command_handler,
     &kEbCommandRoutes[3].kind},
    {qol::contracts::HttpMethod::kPost, "/api/debug/extension_bag/click_item", kPluginId,
     qol::contracts::HttpHandlerKind::kCommand, 0, 1, nullptr, &eb_command_handler,
     &kEbCommandRoutes[4].kind},
    {qol::contracts::HttpMethod::kPost, "/api/debug/extension_bag/equip", kPluginId,
     qol::contracts::HttpHandlerKind::kCommand, 0, 1, nullptr, &eb_command_handler,
     &kEbCommandRoutes[5].kind},
    {qol::contracts::HttpMethod::kPost, "/api/debug/extension_bag/item", kPluginId,
     qol::contracts::HttpHandlerKind::kCommand, 0, 1, nullptr, &eb_command_handler,
     &kEbCommandRoutes[6].kind},
};

namespace {

// 仅接受顶层 JSON 对象中的 int32 整数字段；小数、指数、尾随字符和溢出均拒绝。
bool extract_int_field(std::string_view body, const char* key, int32_t* out) {
    return key != nullptr && detail::parse_json_int32_field(body, key, out);
}

QolResult eb_command_error(contracts::HttpRes* response, uint16_t status, const char* message) {
    const std::string body =
        std::string("{\"ok\":false,\"error\":\"") + message + "\"}";
    if (response == nullptr || response->body == nullptr ||
        response->body_capacity < body.size()) {
        return contracts::error(kQolErrInternal, "http response buffer too small", 31);
    }
    std::memcpy(response->body, body.data(), body.size());
    response->body_size = static_cast<uint32_t>(body.size());
    response->status = status;
    return contracts::error(status == 400 ? kQolErrInvalidArgument : kQolErrInternal, message,
                            static_cast<uint32_t>(std::strlen(message)));
}

QolResult eb_command_body(contracts::HttpRes* response, const std::string& body) {
    if (response == nullptr || response->body == nullptr ||
        response->body_capacity < body.size()) {
        return eb_command_error(response, 500, "http response buffer too small");
    }
    std::memcpy(response->body, body.data(), body.size());
    response->body_size = static_cast<uint32_t>(body.size());
    response->status = 200;
    return contracts::ok();
}

// bag 合法域 [6,10] 与迁移前 Kotlin DebugController 一致；未提供同样拒绝。
bool eb_bag_valid(std::string_view body, int32_t* out_bag, contracts::HttpRes* response) {
    int32_t bag = -1;
    if (!extract_int_field(body, "bag", &bag)) bag = -1;
    if (bag < 6 || bag > 10) {
        eb_command_error(response, 400, "bag required (6-10)");
        return false;
    }
    *out_bag = bag;
    return true;
}

// 游戏写路由的命令化：HTTP 线程只校验并投递票据，DB 写在主线程结果处理器中执行
// （ARCHITECTURE.md §7.1/§8.1）。结果处理器把迁移前的原响应 JSON 写入票据缓冲。
struct EbCommandPayload {
    int32_t kind = 0;
    int32_t a = -1;
    int32_t b = -1;
    int32_t c = -1;
    int32_t d = -1;
};

}  // namespace

bool eb_result_handler(void*, const uint8_t* payload, uint32_t size, char* out, uint32_t capacity,
                       uint32_t* out_size) {
    if (out_size != nullptr) *out_size = 0;
    if (payload == nullptr || size != sizeof(EbCommandPayload) || out == nullptr ||
        out_size == nullptr) {
        return false;
    }
    EbCommandPayload p{};
    std::memcpy(&p, payload, size);
    std::string body;
    switch (static_cast<EbCommand>(p.kind)) {
        case EbCommand::kEnterView:
            body = data_op_extension_bag_enter_view(p.a);
            break;
        case EbCommand::kExitView:
            body = data_op_extension_bag_exit_view();
            break;
        case EbCommand::kUnequip:
            body = data_op_extension_bag_unequip(p.a);
            break;
        case EbCommand::kSelectBag:
            body = data_op_extension_bag_select_bag(p.a);
            break;
        case EbCommand::kClickItem:
            body = data_op_extension_bag_click_item(p.a, p.b);
            break;
        case EbCommand::kEquip:
            body = data_virtual_bag_test_equip(p.a, p.b);
            break;
        case EbCommand::kItem:
            body = data_virtual_bag_test_item(p.a, p.b, p.c, p.d);
            break;
        default:
            return false;
    }
    if (body.size() > capacity) return false;
    std::memcpy(out, body.data(), body.size());
    *out_size = static_cast<uint32_t>(body.size());
    // 票据状态只由顶层 JSON 布尔 ok 决定；原始响应体仍完整写入结果缓冲。
    return detail::parse_json_top_level_boolean(body, "ok") == detail::JsonBooleanValue::kTrue;
}

namespace {

QolResult eb_command_handler(const contracts::HttpReq* request, contracts::HttpRes* response,
                             void* ctx) {
    if (request == nullptr || response == nullptr || ctx == nullptr) {
        return eb_command_error(response, 400, "bad request");
    }
    const EbCommand kind = *static_cast<const EbCommand*>(ctx);
    // HttpReq.body 无 null 结尾契约：拷贝到 std::string（自带结尾）再做字段提取。
    if (request->body == nullptr && request->body_len != 0) {
        return eb_command_error(response, 400, "bad request");
    }
    const std::string body_text(request->body != nullptr
                                    ? reinterpret_cast<const char*>(request->body)
                                    : "",
                                request->body_len);
    const std::string_view body(body_text);
    EbCommandPayload payload{};
    payload.kind = static_cast<int32_t>(kind);
    int32_t bag = -1;
    int32_t slot = -1;
    switch (kind) {
        case EbCommand::kEnterView:
            if (!eb_bag_valid(body, &bag, response)) return contracts::ok();
            payload.a = bag;
            break;
        case EbCommand::kExitView:
            break;
        case EbCommand::kUnequip:
            if (!eb_bag_valid(body, &bag, response)) return contracts::ok();
            payload.a = bag;
            break;
        case EbCommand::kSelectBag:
            if (!eb_bag_valid(body, &bag, response)) return contracts::ok();
            payload.a = bag;
            break;
        case EbCommand::kClickItem: {
            if (!eb_bag_valid(body, &bag, response)) return contracts::ok();
            if (!extract_int_field(body, "slot", &slot)) slot = -1;
            if (slot < 0 || slot > 15) {
                return eb_command_error(response, 400, "slot required (0-15)");
            }
            payload.a = bag;
            payload.b = slot;
            break;
        }
        case EbCommand::kEquip: {
            int32_t index = -1;
            int32_t bag_type = -1;
            if (!extract_int_field(body, "index", &index)) {
                return eb_command_error(response, 400, "index required");
            }
            if (!extract_int_field(body, "bagType", &bag_type)) {
                return eb_command_error(response, 400, "bagType required");
            }
            if (index < 0 || index >= virtual_bag::kBagCount || bag_type <= 0 ||
                bag_type > static_cast<int32_t>(virtual_bag::BagType::kLargeBackpack)) {
                return eb_command_error(response, 400, "bad virtual bag index or bag type");
            }
            payload.a = index;
            payload.b = bag_type;
            break;
        }
        case EbCommand::kItem: {
            int32_t index = -1;
            int32_t item_slot = -1;
            int32_t category = -1;
            int32_t count = -1;
            if (!extract_int_field(body, "index", &index)) {
                return eb_command_error(response, 400, "index required");
            }
            if (!extract_int_field(body, "slot", &item_slot)) {
                return eb_command_error(response, 400, "slot required (0-15)");
            }
            if (!extract_int_field(body, "category", &category)) {
                return eb_command_error(response, 400, "category required");
            }
            if (!extract_int_field(body, "count", &count)) {
                return eb_command_error(response, 400, "count required");
            }
            if (index < 0 || index >= virtual_bag::kBagCount || item_slot < 0 ||
                item_slot >= virtual_bag::kSlotCount || category < 0 || count < 0) {
                return eb_command_error(response, 400, "bad virtual bag item");
            }
            payload.a = index;
            payload.b = item_slot;
            payload.c = category;
            payload.d = count;
            break;
        }
        default:
            return eb_command_error(response, 500, "unknown extension bag command");
    }
    // 游戏写只能在主线程 owner command 执行（ARCHITECTURE.md §7.1）：HTTP 线程只投递票据。
    if (extension_bag_http_host_sdk() == nullptr ||
        extension_bag_http_host_sdk()->commands.post_typed == nullptr) {
        return eb_command_error(response, 503, "command port unavailable");
    }
    contracts::QolCommandTicket ticket = 0;
    if (!contracts::succeeded(extension_bag_http_host_sdk()->commands.post_typed(
            kEbResultHandlerId, &payload, sizeof(payload), &ticket)) ||
        ticket == 0) {
        return eb_command_error(response, 503, "command queue unavailable");
    }
    return eb_command_body(response, std::string("{\"ok\":true,\"ticket\":") +
                                        std::to_string(ticket) + "}");
}

}  // namespace

const qol::contracts::HttpEndpointDescriptor* extension_bag_http_endpoints(uint32_t* out_count) {
    if (out_count != nullptr) *out_count = kHttpEndpointCount;
    return kHttpEndpoints;
}

}  // namespace qol::features::extension_bag
