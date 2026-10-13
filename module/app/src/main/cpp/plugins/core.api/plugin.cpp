#include "plugin.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>

#include "contracts/commands.h"
#include "contracts/host_sdk.h"
#include "contracts/http.h"
#include "json_field.h"

namespace qol::features::core_api {
namespace {

constexpr const char* kPluginId = "core.api";
constexpr const char* kPluginVersion = "0.1.0";

contracts::CapabilityProvisionRef kProvides[] = {
    {contracts::kHttpRegistryCapabilityId, contracts::kHttpRegistryVersion, nullptr},
    {contracts::kHttpDispatchCapabilityId, contracts::kHttpDispatchVersion, nullptr},
};

#define QOL_HTTP_ROUTE(method, path, owner, op_gated, lease_required) \
    {contracts::HttpMethod::method, path, owner, op_gated, lease_required},
const contracts::HttpRouteMetadataDescriptor kRouteMetadata[] = {
#include "owned_routes.inc"
};
#undef QOL_HTTP_ROUTE

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

QolResult invalid_config(const char* message) {
    return contracts::error(kQolErrInvalidArgument, message,
                            static_cast<uint32_t>(std::strlen(message)));
}

bool valid_utf8(std::string_view value) {
    size_t index = 0;
    while (index < value.size()) {
        const auto first = static_cast<uint8_t>(value[index]);
        if (first <= 0x7f) {
            ++index;
            continue;
        }
        size_t length = 0;
        uint8_t second_min = 0x80;
        uint8_t second_max = 0xbf;
        if (first >= 0xc2 && first <= 0xdf) {
            length = 2;
        } else if (first == 0xe0) {
            length = 3;
            second_min = 0xa0;
        } else if ((first >= 0xe1 && first <= 0xec) ||
                   (first >= 0xee && first <= 0xef)) {
            length = 3;
        } else if (first == 0xed) {
            length = 3;
            second_max = 0x9f;
        } else if (first == 0xf0) {
            length = 4;
            second_min = 0x90;
        } else if (first >= 0xf1 && first <= 0xf3) {
            length = 4;
        } else if (first == 0xf4) {
            length = 4;
            second_max = 0x8f;
        } else {
            return false;
        }
        if (value.size() - index < length) return false;
        const auto second = static_cast<uint8_t>(value[index + 1]);
        if (second < second_min || second > second_max) return false;
        for (size_t offset = 2; offset < length; ++offset) {
            const auto continuation = static_cast<uint8_t>(value[index + offset]);
            if (continuation < 0x80 || continuation > 0xbf) return false;
        }
        index += length;
    }
    return true;
}

int hex_digit(char value) {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

bool read_utf16_code_unit(std::string_view json, size_t* cursor, uint16_t* out) {
    if (cursor == nullptr || out == nullptr || *cursor > json.size() ||
        json.size() - *cursor < 4) return false;
    uint16_t code_unit = 0;
    for (size_t index = 0; index < 4; ++index) {
        const int digit = hex_digit(json[*cursor + index]);
        if (digit < 0) return false;
        code_unit = static_cast<uint16_t>((code_unit << 4) | static_cast<uint16_t>(digit));
    }
    *cursor += 4;
    *out = code_unit;
    return true;
}

void append_utf8(uint32_t code_point, std::string* out) {
    if (code_point <= 0x7f) {
        out->push_back(static_cast<char>(code_point));
    } else if (code_point <= 0x7ff) {
        out->push_back(static_cast<char>(0xc0 | (code_point >> 6)));
        out->push_back(static_cast<char>(0x80 | (code_point & 0x3f)));
    } else if (code_point <= 0xffff) {
        out->push_back(static_cast<char>(0xe0 | (code_point >> 12)));
        out->push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3f)));
        out->push_back(static_cast<char>(0x80 | (code_point & 0x3f)));
    } else {
        out->push_back(static_cast<char>(0xf0 | (code_point >> 18)));
        out->push_back(static_cast<char>(0x80 | ((code_point >> 12) & 0x3f)));
        out->push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3f)));
        out->push_back(static_cast<char>(0x80 | (code_point & 0x3f)));
    }
}

bool parse_json_string(std::string_view json, std::string* out) {
    if (out == nullptr) return false;
    size_t cursor = 0;
    while (cursor < json.size() &&
           (json[cursor] == ' ' || json[cursor] == '\t' || json[cursor] == '\r' ||
            json[cursor] == '\n')) ++cursor;
    if (cursor == json.size() || json[cursor++] != '"') return false;
    out->clear();
    bool closed = false;
    while (cursor < json.size()) {
        const unsigned char value = static_cast<unsigned char>(json[cursor++]);
        if (value == '"') {
            closed = true;
            break;
        }
        if (value < 0x20) return false;
        if (value != '\\') {
            out->push_back(static_cast<char>(value));
            continue;
        }
        if (cursor == json.size()) return false;
        const char escaped = json[cursor++];
        switch (escaped) {
            case '"': out->push_back('"'); break;
            case '\\': out->push_back('\\'); break;
            case '/': out->push_back('/'); break;
            case 'b': out->push_back('\b'); break;
            case 'f': out->push_back('\f'); break;
            case 'n': out->push_back('\n'); break;
            case 'r': out->push_back('\r'); break;
            case 't': out->push_back('\t'); break;
            case 'u': {
                uint16_t first = 0;
                if (!read_utf16_code_unit(json, &cursor, &first)) return false;
                uint32_t code_point = first;
                if (first >= 0xd800 && first <= 0xdbff) {
                    if (json.size() - cursor < 6 || json[cursor] != '\\' ||
                        json[cursor + 1] != 'u') return false;
                    cursor += 2;
                    uint16_t second = 0;
                    if (!read_utf16_code_unit(json, &cursor, &second) ||
                        second < 0xdc00 || second > 0xdfff) return false;
                    code_point = 0x10000u +
                        ((static_cast<uint32_t>(first) - 0xd800u) << 10) +
                        (static_cast<uint32_t>(second) - 0xdc00u);
                } else if (first >= 0xdc00 && first <= 0xdfff) {
                    return false;
                }
                append_utf8(code_point, out);
                break;
            }
            default: return false;
        }
    }
    if (!closed) return false;
    while (cursor < json.size() &&
           (json[cursor] == ' ' || json[cursor] == '\t' || json[cursor] == '\r' ||
            json[cursor] == '\n')) ++cursor;
    return cursor == json.size();
}

bool parse_listener_port(std::string_view json, uint32_t* out) {
    if (out == nullptr || json.empty()) return false;
    size_t cursor = 0;
    const auto whitespace = [&json](size_t* position) {
        while (*position < json.size() &&
               (json[*position] == ' ' || json[*position] == '\t' ||
                json[*position] == '\r' || json[*position] == '\n')) {
            ++*position;
        }
    };
    whitespace(&cursor);
    if (cursor == json.size()) return false;
    if (json[cursor] == '-') return false;
    uint32_t value = 0;
    if (json[cursor] == '0') {
        ++cursor;
        if (cursor < json.size() && json[cursor] >= '0' && json[cursor] <= '9') return false;
    } else if (json[cursor] >= '1' && json[cursor] <= '9') {
        do {
            const uint32_t digit = static_cast<uint32_t>(json[cursor] - '0');
            if (value > 6553 || (value == 6553 && digit > 5)) return false;
            value = value * 10 + digit;
            ++cursor;
        } while (cursor < json.size() && json[cursor] >= '0' && json[cursor] <= '9');
    } else {
        return false;
    }
    whitespace(&cursor);
    if (cursor != json.size() || value < 1 || value > 65535) return false;
    *out = value;
    return true;
}

QolResult validate_api_config(const char* key, const char* json, uint32_t json_size) {
    if (key == nullptr) return invalid_config("config key required");
    if (std::strcmp(key, "listen_address") == 0) {
        if (json == nullptr || json_size == 0) return invalid_config("listen_address must be a string");
        std::string address;
        if (!parse_json_string(std::string_view(json, json_size), &address)) {
            return invalid_config("listen_address must be a string");
        }
        if (address.find_first_not_of(" \t\r\n\f\v") == std::string::npos)
            return invalid_config("listen_address required");
        if (!valid_utf8(address)) return invalid_config("listen_address must be valid UTF-8");
        return contracts::ok();
    }
    if (std::strcmp(key, "listen_port") == 0) {
        uint32_t port = 0;
        if (json == nullptr ||
            !parse_listener_port(std::string_view(json, json_size), &port)) {
            return invalid_config("listen_port must be an integer from 1 to 65535");
        }
    }
    return contracts::ok();
}

int plugin_install(contracts::PluginContext& ctx) {
    if (ctx.sdk == nullptr) return 1;
    kProvides[0].iface = &ctx.sdk->http;
    kProvides[1].iface = &ctx.sdk->http_dispatch;
    g_sdk = ctx.sdk;
    return 0;
}

void plugin_deactivate(contracts::PluginContext& ctx) {
    (void)ctx;
    g_sdk = nullptr;
}

void plugin_fill_status(contracts::PluginStatusOut& out) {
    out.enabled = g_sdk != nullptr;
    out.error = nullptr;
}

// Feature callback does not replace the existing lifecycle or business controls.
constexpr contracts::FeatureDefinition kFeatures[] = {
    {"api.enabled", 1, contracts::FeatureCategory::kGeneral,
     contracts::FeatureType::kToggle, "API服务", "",
      [](contracts::PluginContext&, uint8_t) -> QolResult { return contracts::ok(); }, nullptr,
      nullptr, -5},
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
    nullptr, 0, &validate_api_config,
    kRouteMetadata, static_cast<uint32_t>(sizeof(kRouteMetadata) / sizeof(kRouteMetadata[0])),
};

}  // namespace

// Host 常驻控制端点的兼容测试辅助：body 解析 + action 映射 + 命令投递，保留响应形状验证。
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
