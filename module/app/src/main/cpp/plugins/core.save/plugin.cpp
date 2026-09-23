#include "plugin.h"

#include <cstdlib>
#include <cstring>
#include <string>

#include "contracts/commands.h"
#include "contracts/game_ports/save_port.h"
#include "contracts/game_ports/save_write_port.h"
#include "contracts/http.h"
#include "contracts/save_facts.h"

namespace qol::features::core_save {
namespace {

constexpr const char* kPluginId = "core.save";
constexpr const char* kPluginVersion = "0.1.0";
[[gnu::used]] constexpr char kSaveFactsBuildMarker[] = "save.facts:v1";

const contracts::QolSavePort* g_port = nullptr;

QolResult not_ready() {
    return contracts::error(kQolErrNotReady, "save facts not ready", 20);
}

QolResult slot_state(int32_t slot, int32_t* out_state) {
    if (g_port == nullptr || g_port->read.slot_state == nullptr) return not_ready();
    return g_port->read.slot_state(slot, out_state);
}
QolResult slot_exists(int32_t slot, int32_t* out_exists) {
    if (g_port == nullptr || g_port->read.slot_exists == nullptr) return not_ready();
    return g_port->read.slot_exists(slot, out_exists);
}
QolResult slot_hero_index(int32_t slot, int32_t* out_index) {
    if (g_port == nullptr || g_port->read.slot_hero_index == nullptr) return not_ready();
    return g_port->read.slot_hero_index(slot, out_index);
}
QolResult hero_level(int32_t slot, int32_t* out_level) {
    if (g_port == nullptr || g_port->read.hero_level == nullptr) return not_ready();
    return g_port->read.hero_level(slot, out_level);
}
QolResult hero_class(int32_t slot, int32_t* out_class) {
    if (g_port == nullptr || g_port->read.hero_class == nullptr) return not_ready();
    return g_port->read.hero_class(slot, out_class);
}
QolResult load_data(int32_t slot, void** out_data, int32_t* out_size) {
    if (g_port == nullptr || g_port->read.load_data == nullptr) return not_ready();
    return g_port->read.load_data(slot, out_data, out_size);
}
QolResult hub_key(const char** out_key) {
    if (g_port == nullptr || g_port->read.hub_key == nullptr) return not_ready();
    return g_port->read.hub_key(out_key);
}
QolResult map_id(int32_t slot, int32_t* out_map_id) {
    if (g_port == nullptr || g_port->read.map_id == nullptr) return not_ready();
    return g_port->read.map_id(slot, out_map_id);
}
QolResult current_slot(int32_t* out_slot) {
    if (g_port == nullptr || g_port->read.current_slot == nullptr) return not_ready();
    return g_port->read.current_slot(out_slot);
}
QolResult is_ok(int32_t* out_ok) {
    if (g_port == nullptr || g_port->read.is_ok == nullptr) return not_ready();
    return g_port->read.is_ok(out_ok);
}

constexpr contracts::SaveFactsApi kSaveFactsApi{
    &slot_state, &slot_exists, &slot_hero_index, &hero_level, &hero_class,
    &load_data, &hub_key, &map_id, &current_slot, &is_ok,
};

const contracts::CapabilityProvisionRef kProvides[] = {
    {contracts::kSaveFactsCapabilityId, contracts::kSaveFactsVersion, &kSaveFactsApi},
};

const contracts::QolPortRequirement kPorts[] = {
    {contracts::QolPortId::QOL_PORT_SAVE,
     {contracts::kQolPortSaveVersion.major, contracts::kQolPortSaveVersion.minor}},
    {contracts::QolPortId::QOL_PORT_SAVE_WRITE,
     {contracts::kQolPortSaveWriteVersion.major, contracts::kQolPortSaveWriteVersion.minor}},
};

void log_message(const contracts::QolHostSdk* sdk, const char* message) {
    if (sdk == nullptr || sdk->log.write == nullptr || message == nullptr) return;
    uint32_t length = 0;
    while (message[length] != '\0') ++length;
    (void)sdk->log.write(1, 1, kPluginId, message, length);
}

// ---- 存档域写面（阶段 5 票据契约）----
// HTTP 线程只校验并投递票据；游戏写在主线程结果处理器执行（含模块 sidecar 生命周期与
// 转场后的世界态等待），结果经票据回取。owner 与读面同属 core.save（一个领域一个 owner）。
enum class SaveWriteRoute : uint8_t { kSave, kEnterSlot, kCreateSlot };

// typed handler id 为全局空间：core.save 沿用原 save 写 id（随代码从 save_backup 迁入）。
constexpr uint32_t kSaveWriteHandlerId = 0x53420001;

struct SaveWritePayload {
    int32_t kind = 0;  // contracts::kQolSaveWrite*
    int32_t slot = -1;
    int32_t class_idx = -1;
};

const contracts::QolSaveWritePort* g_save_write = nullptr;
const contracts::QolHostSdk* g_sdk = nullptr;

QolResult cs_http_error(contracts::HttpRes* response, uint16_t status, const char* message) {
    const std::string body = std::string("{\"ok\":false,\"error\":\"") + message + "\"}";
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

QolResult cs_http_body(contracts::HttpRes* response, const std::string& body) {
    if (response == nullptr || response->body == nullptr ||
        response->body_capacity < body.size()) {
        return cs_http_error(response, 500, "http response buffer too small");
    }
    std::memcpy(response->body, body.data(), body.size());
    response->body_size = static_cast<uint32_t>(body.size());
    response->status = 200;
    return contracts::ok();
}

// HttpReq.body 无 null 结尾契约；调用方已拷贝为 std::string，此处只做字段提取。
bool cs_extract_int(const char* body, const char* key, int* out) {
    const std::string pattern = std::string("\"") + key + "\":";
    const char* p = std::strstr(body, pattern.c_str());
    if (p == nullptr) return false;
    p += pattern.size();
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') ++p;
    char* end = nullptr;
    const long value = std::strtol(p, &end, 10);
    if (end == p) return false;
    *out = static_cast<int>(value);
    return true;
}

// 主线程命令体：执行存档写（含模块 sidecar 生命周期），把迁移前结果 JSON 写入票据缓冲。
bool save_write_result_handler(void*, const uint8_t* payload, uint32_t size, char* out,
                               uint32_t capacity, uint32_t* out_size) {
    if (out_size != nullptr) *out_size = 0;
    if (payload == nullptr || size != sizeof(SaveWritePayload) || out == nullptr ||
        out_size == nullptr || g_save_write == nullptr || g_save_write->read.write == nullptr) {
        return false;
    }
    SaveWritePayload p{};
    std::memcpy(&p, payload, size);
    // 命令执行次数（首次 0，延迟重试递增）：端口据此区分「首次发起转换」与「等待世界态就绪」。
    const uint32_t attempt =
        (g_sdk != nullptr && g_sdk->ticket_result.current_attempt != nullptr)
            ? g_sdk->ticket_result.current_attempt()
            : 0;
    const QolResult result =
        g_save_write->read.write(p.kind, p.slot, p.class_idx, attempt, out, capacity, out_size);
    if (result.code == kQolErrNotReady) {
        // 转场后世界态未提交：保持票据 pending，下一帧重试（不提前报告 done）。
        if (g_sdk != nullptr && g_sdk->ticket_result.defer_current_result != nullptr) {
            (void)g_sdk->ticket_result.defer_current_result();
            return true;
        }
        return false;
    }
    // 业务失败时端口返回错误，但 out 已携带原错误体（与迁移前响应一致），票据置 failed。
    return contracts::succeeded(result) && *out_size != 0;
}

// 写命令投递：校验与迁移前 Kotlin 一致（slot ∈ [0,2] 否则 400 slot required (0-2)；
// create_slot 另需 class_idx 非负），通过后投递票据并立即返回 {"ok":true,"ticket":N}。
QolResult cs_save_write_command(contracts::HttpRes* response, SaveWriteRoute kind, int32_t slot,
                                int32_t class_idx) {
    if (kind == SaveWriteRoute::kEnterSlot || kind == SaveWriteRoute::kCreateSlot) {
        if (slot < 0 || slot > 2) return cs_http_error(response, 400, "slot required (0-2)");
    }
    if (kind == SaveWriteRoute::kCreateSlot && class_idx < 0) {
        return cs_http_error(response, 400, "class_idx required (0-5)");
    }
    if (g_sdk == nullptr || g_sdk->commands.post_typed == nullptr) {
        return cs_http_error(response, 503, "command port unavailable");
    }
    SaveWritePayload payload{};
    payload.kind = kind == SaveWriteRoute::kSave ? contracts::kQolSaveWriteSave
                   : kind == SaveWriteRoute::kEnterSlot ? contracts::kQolSaveWriteEnterSlot
                                                        : contracts::kQolSaveWriteCreateSlot;
    payload.slot = slot;
    payload.class_idx = class_idx;
    contracts::QolCommandTicket ticket = 0;
    if (!contracts::succeeded(g_sdk->commands.post_typed(kSaveWriteHandlerId, &payload,
                                                        sizeof(payload), &ticket)) ||
        ticket == 0) {
        return cs_http_error(response, 503, "command queue unavailable");
    }
    return cs_http_body(response, std::string("{\"ok\":true,\"ticket\":") +
                                        std::to_string(ticket) + "}");
}

QolResult cs_http_handler(const contracts::HttpReq* request, contracts::HttpRes* response,
                          void* ctx) {
    if (request == nullptr || response == nullptr || ctx == nullptr) {
        return cs_http_error(response, 400, "bad request");
    }
    const SaveWriteRoute kind = *static_cast<const SaveWriteRoute*>(ctx);
    const std::string body_text(
        request->body != nullptr ? reinterpret_cast<const char*>(request->body) : "",
        request->body_len);
    const char* body = body_text.c_str();
    int slot = -1;
    int class_idx = -1;
    switch (kind) {
        case SaveWriteRoute::kSave:
            return cs_save_write_command(response, kind, -1, -1);
        case SaveWriteRoute::kEnterSlot:
            (void)cs_extract_int(body, "slot", &slot);
            return cs_save_write_command(response, kind, slot, -1);
        case SaveWriteRoute::kCreateSlot:
            (void)cs_extract_int(body, "slot", &slot);
            (void)cs_extract_int(body, "class_idx", &class_idx);
            return cs_save_write_command(response, kind, slot, class_idx);
    }
    return cs_http_error(response, 400, "bad request");
}

const SaveWriteRoute kSaveWriteRouteKinds[] = {
    SaveWriteRoute::kSave,
    SaveWriteRoute::kEnterSlot,
    SaveWriteRoute::kCreateSlot,
};

const contracts::HttpQueryHandlerDescriptor kSaveWriteRoutes[] = {
    {contracts::HttpMethod::kPost, "/api/system/save", kPluginId, &cs_http_handler,
     const_cast<SaveWriteRoute*>(&kSaveWriteRouteKinds[0])},
    {contracts::HttpMethod::kPost, "/api/system/enter_slot", kPluginId, &cs_http_handler,
     const_cast<SaveWriteRoute*>(&kSaveWriteRouteKinds[1])},
    {contracts::HttpMethod::kPost, "/api/system/create_slot", kPluginId, &cs_http_handler,
     const_cast<SaveWriteRoute*>(&kSaveWriteRouteKinds[2])},
};

int plugin_install(contracts::PluginContext& ctx) {
    g_port = nullptr;
    g_save_write = nullptr;
    g_sdk = nullptr;
    if (ctx.struct_size < contracts::PLUGIN_CONTEXT_PORTS_STRUCT_SIZE ||
        ctx.sdk_major != contracts::kHostSdkMajor || ctx.sdk == nullptr || ctx.ports == nullptr ||
        ctx.port_count != 2 || ctx.ports[0] == nullptr || ctx.ports[1] == nullptr) {
        return 1;
    }
    g_port = static_cast<const contracts::QolSavePort*>(ctx.ports[0]);
    if (!contracts::satisfies(g_port->abi_version,
                              {contracts::kQolPortSaveVersion.major,
                               contracts::kQolPortSaveVersion.minor}) ||
        g_port->read.slot_state == nullptr || g_port->read.load_data == nullptr ||
        g_port->read.is_ok == nullptr) {
        g_port = nullptr;
        return 1;
    }
    const auto* save_write = static_cast<const contracts::QolSaveWritePort*>(ctx.ports[1]);
    if (!contracts::satisfies(save_write->abi_version,
                              {contracts::kQolPortSaveWriteVersion.major,
                               contracts::kQolPortSaveWriteVersion.minor}) ||
        save_write->read.write == nullptr || ctx.sdk->abi.size < sizeof(contracts::QolHostSdk) ||
        ctx.sdk->abi.minor < 9 || ctx.sdk->ticket_result.register_typed_result_handler == nullptr) {
        g_port = nullptr;
        return 1;
    }
    g_save_write = save_write;
    g_sdk = ctx.sdk;
    if (!contracts::succeeded(ctx.sdk->ticket_result.register_typed_result_handler(
            kSaveWriteHandlerId, &save_write_result_handler, nullptr))) {
        g_save_write = nullptr;
        g_sdk = nullptr;
        g_port = nullptr;
        return 1;
    }
    log_message(ctx.sdk, "[core.save] provider install: facts + save writes");
    return 0;
}

void plugin_deactivate(contracts::PluginContext& ctx) {
    // 结果处理器为自注册（不在 descriptor），必须显式注销。
    if (g_sdk != nullptr && g_sdk->ticket_result.unregister_typed_result_handler != nullptr) {
        (void)g_sdk->ticket_result.unregister_typed_result_handler(kSaveWriteHandlerId);
    }
    log_message(ctx.sdk, "[core.save] provider deactivate");
    g_save_write = nullptr;
    g_sdk = nullptr;
    g_port = nullptr;
}

void plugin_on_enabled(contracts::PluginContext& ctx) {
    log_message(ctx.sdk, "[core.save] provider enabled: capability published");
}

void plugin_on_disabled(contracts::PluginContext& ctx) {
    log_message(ctx.sdk, "[core.save] provider disabled: capability withdrawn");
}

void plugin_fill_status(contracts::PluginStatusOut& out) {
    out.enabled = g_port != nullptr;
    out.error = nullptr;
}

const contracts::PluginDescriptor kDescriptor = {
    kPluginId, kPluginVersion,
    0, nullptr,
    0, nullptr,
    1, kProvides,
    0, nullptr,
    0, nullptr,
    &plugin_install, &plugin_deactivate, &plugin_on_enabled, &plugin_on_disabled,
    &plugin_fill_status, nullptr,
    kPorts, 2,
    kSaveWriteRoutes, 3,
};

}  // namespace

const contracts::PluginDescriptor& plugin_descriptor() { return kDescriptor; }

}  // namespace qol::features::core_save
