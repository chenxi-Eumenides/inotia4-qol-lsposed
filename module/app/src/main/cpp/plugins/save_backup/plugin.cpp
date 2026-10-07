#include "plugin.h"

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "../../contracts/http.h"
#include "../../contracts/ownership.h"
#include "../../contracts/panels.h"
#include "../../contracts/save_facts.h"
#include "../../contracts/game_ports/save_call_port.h"
#include "../../contracts/game_ports/save_write_port.h"
#include "../../contracts/commands.h"
#include "bundle.h"
#include "feature.h"
#include "log.h"
#include "ui/panel.h"

namespace qol::features::save_backup {
namespace {

#define SB_LOG(...) SAVE_BACKUP_LOG_INFO(__VA_ARGS__)

constexpr const char* kPluginId = "save_backup";
constexpr const char* kPluginVersion = "0.1.0";

// 指针槽资源登记名：与 game/access/ptr_slots 的 kSlots 条目一一对应。
constexpr const char* kSlotDeleteResourceId = "ptrslot:SAVESLOT_DELETE_GOT";

const contracts::CapabilityRequirement kRequires[] = {
    {
        contracts::kUiPanelsCapabilityId,
        {contracts::kUiPanelsVersionMajor, contracts::kUiPanelsVersionMinor},
    },
    {
        contracts::kSaveFactsCapabilityId,
        {contracts::kSaveFactsVersionMajor, contracts::kSaveFactsVersionMinor},
    },
    {
        contracts::kHttpRegistryCapabilityId,
        {contracts::kHttpRegistryVersion.major, contracts::kHttpRegistryVersion.minor},
    },
    {
        contracts::kHttpDispatchCapabilityId,
        {contracts::kHttpDispatchVersion.major, contracts::kHttpDispatchVersion.minor},
    },
};

const contracts::QolPortRequirement kPorts[] = {
    {contracts::QolPortId::QOL_PORT_SAVE_CALL,
     {contracts::kQolPortSaveCallVersion.major, contracts::kQolPortSaveCallVersion.minor}},
};

// 游戏内备份面板 provider：由 core.ui 容器（设置页「存档备份」动作项）经 ui_panels 打开。
const contracts::PanelProviderSpec kPanelProvider = {
    "save_backup",
    kPluginId,
    &savebackup_provider_open,
    &savebackup_provider_close,
    &savebackup_provider_active,
    &savebackup_provider_block_reason,
};

// own:popup.savebackup：PopupState 死条目 F_PANEL_UNK2_ENTER 的 kOwnership 占用。
// kFailSoft（工具类面板，不因死条目缺失而终态化插件）；acquire/release 走 hooks/popup_state。
const contracts::OwnershipOps kPopupOwnershipOps{
    &savebackup_popup_acquire,
    &savebackup_popup_release,
};

// 常开标记：无配置门禁；仅用于 fill_status 上报（HTTP/UI 业务不受其门控）。
std::atomic<bool> g_active{true};

// 删档原函数入口：宿主安装 kPtrSlot 资源成功后经 on_resource_ready 交还（§3.3 backup 交还）。
// wrapper 先回调原函数（删 save{n}.dat + 重载三槽），再调业务同步体删该槽 sidecar。
std::atomic<void*> g_slot_delete_orig{nullptr};

// 执行上下文：游戏主线程（UIPopupMsg_ButtonOKExe → blr），x0 = slot(int32_t)。
// 槽值由宿主按声明写入本 wrapper；原函数入口经 g_slot_delete_orig 取回。
void slot_delete_hook_wrapper(int32_t slot) {
    void* orig = g_slot_delete_orig.load(std::memory_order_acquire);
    if (orig != nullptr) {
        reinterpret_cast<void (*)(int32_t)>(orig)(slot);
    }
    save_backup_slot_deleted_sync(slot);
}

// 声明式资源：own:popup.savebackup（PopupState 死条目占用，工具面板） + 删档回调 GOT 槽。
// 槽址/期望值由 game/access/ptr_slots 解析；handler 为本 wrapper；常驻（gate=nullptr）、
// 必需、fail-closed、可物理还原（归还时写回期望值）。
const contracts::ResourceClaim kResources[] = {
    {
        kSlotDeleteResourceId,   // id
        kSlotDeleteResourceId,   // symbol（与登记名同串，经 game/access 指针槽登记解析）
        contracts::ResourceMechanism::kPtrSlot,
        contracts::ResourcePolicy::kPrivate,
        contracts::ResourceRequirement::kRequired,
        contracts::ResourceFailure::kFailClosed,
        true,                    // resumable
        kPluginId,               // owner
        "内存档面板删档后游戏立即重载槽区，模块必须同步清掉该槽 sidecar",  // reason
        nullptr,                 // gate（常驻）
        120,                     // step_budget
        reinterpret_cast<void*>(&slot_delete_hook_wrapper),  // handler
    },
    {
        "own:popup.savebackup",  // id
        "own:popup.savebackup",  // symbol（kOwnership 不使用站点解析，保留 id 作诊断标识）
        contracts::ResourceMechanism::kOwnership,
        contracts::ResourcePolicy::kPrivate,
        contracts::ResourceRequirement::kRequired,
        contracts::ResourceFailure::kFailSoft,
        true,                    // resumable
        kPluginId,               // owner
        "备份面板以 PopupState 死条目承载，需整段占用 5 回调并承诺写回快照",  // reason
        nullptr,                 // gate（常驻）
        120,                     // step_budget
        reinterpret_cast<void*>(const_cast<contracts::OwnershipOps*>(&kPopupOwnershipOps)),  // handler
    },
};

// ---- HTTP 面：备份四端点（owner=save_backup）----
// 声明式注册：descriptor 的 http 列表由宿主在插件 enabled 时注册、停用时置路由墓碑摘除，
// 复用 dispatch 的 query（GET）/command（POST）双槽语义；插件不自注册，故无需显式注销。
enum class SbHttpRoute : uint8_t { kList, kExport, kImport, kDelete };

bool sb_extract_int(const char* body, const char* key, int* out) {
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

// checksum 恒为 ^[0-9a-f]{12}$（无转义字符），取到下一个双引号即可。
bool sb_extract_string(const char* body, const char* key, std::string* out) {
    const std::string pattern = std::string("\"") + key + "\":\"";
    const char* p = std::strstr(body, pattern.c_str());
    if (p == nullptr) return false;
    p += pattern.size();
    const char* end = std::strchr(p, '"');
    if (end == nullptr) return false;
    out->assign(p, static_cast<size_t>(end - p));
    return true;
}

QolResult sb_http_error(contracts::HttpRes* response, uint16_t status, const char* message) {
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

QolResult sb_http_body(contracts::HttpRes* response, const std::string& body) {
    if (response == nullptr || response->body == nullptr ||
        response->body_capacity < body.size()) {
        return sb_http_error(response, 500, "http response buffer too small");
    }
    std::memcpy(response->body, body.data(), body.size());
    response->body_size = static_cast<uint32_t>(body.size());
    response->status = 200;
    return contracts::ok();
}

QolResult sb_http_handler(const contracts::HttpReq* request, contracts::HttpRes* response,
                          void* ctx) {
    if (request == nullptr || response == nullptr || ctx == nullptr) {
        return sb_http_error(response, 400, "bad request");
    }
    const SbHttpRoute kind = *static_cast<const SbHttpRoute*>(ctx);
    // HttpReq.body 无 null 结尾契约：拷贝到 std::string（自带结尾）再做字段提取。
    const std::string body_text(
        request->body != nullptr ? reinterpret_cast<const char*>(request->body) : "",
        request->body_len);
    const char* body = body_text.c_str();
    int slot = -1;
    switch (kind) {
        case SbHttpRoute::kList:
            return sb_http_body(response, save_backup_list_json());
        case SbHttpRoute::kExport:
            if (!sb_extract_int(body, "slot", &slot) || slot < 0 || slot > 2) {
                return sb_http_error(response, 400, "bad slot");
            }
            return sb_http_body(response, save_backup_export_json(slot));
        case SbHttpRoute::kImport: {
            std::string checksum;
            if (!sb_extract_string(body, "checksum", &checksum) ||
                !::save_backup::valid_checksum(checksum)) {
                return sb_http_error(response, 400, "bad checksum");
            }
            if (!sb_extract_int(body, "slot", &slot) || slot < 0 || slot > 2) {
                return sb_http_error(response, 400, "bad slot");
            }
            return sb_http_body(response, save_backup_import_json(checksum.c_str(), slot));
        }
        case SbHttpRoute::kDelete: {
            std::string checksum;
            if (!sb_extract_string(body, "checksum", &checksum) ||
                !::save_backup::valid_checksum(checksum)) {
                return sb_http_error(response, 400, "bad checksum");
            }
            return sb_http_body(response, save_backup_delete_json(checksum.c_str()));
        }
    }
    return sb_http_error(response, 400, "bad request");
}

const SbHttpRoute kSbHttpRouteKinds[] = {
    SbHttpRoute::kList,
    SbHttpRoute::kExport,
    SbHttpRoute::kImport,
    SbHttpRoute::kDelete,
};

const contracts::HttpQueryHandlerDescriptor kSbHttpRoutes[] = {
    {contracts::HttpMethod::kGet, "/api/system/backup/list", kPluginId, &sb_http_handler,
     const_cast<SbHttpRoute*>(&kSbHttpRouteKinds[0])},
    {contracts::HttpMethod::kPost, "/api/system/backup/export", kPluginId, &sb_http_handler,
     const_cast<SbHttpRoute*>(&kSbHttpRouteKinds[1])},
    {contracts::HttpMethod::kPost, "/api/system/backup/import", kPluginId, &sb_http_handler,
     const_cast<SbHttpRoute*>(&kSbHttpRouteKinds[2])},
    {contracts::HttpMethod::kPost, "/api/system/backup/delete", kPluginId, &sb_http_handler,
     const_cast<SbHttpRoute*>(&kSbHttpRouteKinds[3])},
};

int plugin_install(contracts::PluginContext& ctx) {
    const void* save_facts_iface = nullptr;
    if (ctx.sdk == nullptr || ctx.game == nullptr || ctx.sdk->capabilities.require == nullptr ||
        !contracts::succeeded(ctx.sdk->capabilities.require(
            contracts::kSaveFactsCapabilityId,
            {contracts::kSaveFactsVersionMajor, contracts::kSaveFactsVersionMinor},
            &save_facts_iface)) ||
        save_facts_iface == nullptr || ctx.ports == nullptr || ctx.port_count != 1 ||
        ctx.ports[0] == nullptr) {
        save_backup_bind_context(nullptr, nullptr, nullptr, nullptr, 0);
        return 1;
    }
    const auto* save_call = static_cast<const contracts::QolSaveCallPort*>(ctx.ports[0]);
    const auto* save_facts = static_cast<const contracts::SaveFactsApi*>(save_facts_iface);
    if (!contracts::satisfies(save_call->abi_version,
                              {contracts::kQolPortSaveCallVersion.major,
                               contracts::kQolPortSaveCallVersion.minor}) ||
        save_call->write.free_data == nullptr || save_call->write.encrypt == nullptr ||
        save_call->write.mark_stale == nullptr || save_facts->load_data == nullptr ||
        save_facts->slot_exists == nullptr || save_facts->hub_key == nullptr) {
        save_backup_bind_context(nullptr, nullptr, nullptr, nullptr, 0);
        return 1;
    }
    save_backup_bind_context(ctx.sdk, save_facts, save_call, ctx.game, ctx.owner_token);
    g_active.store(true, std::memory_order_release);
    // 自身初始化：资源由宿主声明式安装；本面板注册 provider 供 core.ui 容器打开。
    if (ctx.sdk == nullptr || ctx.sdk->panels.register_provider == nullptr ||
        !contracts::succeeded(ctx.sdk->panels.register_provider(&kPanelProvider))) {
        return 1;
    }
    return 0;
}

void plugin_deactivate(contracts::PluginContext& ctx) {
    // 资源归还包括物理写回原槽值（宿主引擎执行）；本回调只翻转上报标记（幂等）。
    if (ctx.sdk != nullptr && ctx.sdk->capabilities.require != nullptr) {
        const void* iface = nullptr;
        if (contracts::succeeded(ctx.sdk->capabilities.require(
                contracts::kUiPanelsCapabilityId,
                {contracts::kUiPanelsVersionMajor, contracts::kUiPanelsVersionMinor}, &iface)) &&
            iface != nullptr) {
            const auto* panels = static_cast<const contracts::UiPanelsApi*>(iface);
            if (panels->deactivate_owner != nullptr) (void)panels->deactivate_owner(kPluginId);
        }
    }
    g_active.store(false, std::memory_order_release);
    if (ctx.sdk != nullptr && ctx.sdk->panels.unregister_owner != nullptr) {
        (void)ctx.sdk->panels.unregister_owner(kPluginId);
    }
    save_backup_bind_context(nullptr, nullptr, nullptr, nullptr, 0);
}

void plugin_on_enabled(contracts::PluginContext& ctx) {
    (void)ctx;
    g_active.store(true, std::memory_order_release);
}

void plugin_on_disabled(contracts::PluginContext& ctx) {
    (void)ctx;
    g_active.store(false, std::memory_order_release);
}

int32_t plugin_on_resource_ready(contracts::PluginContext& ctx, const char* resource_id,
                                 void* backup) {
    (void)ctx;
    // 受限回调（§3.3）：只缓存 backup（原函数入口），常数时间、无资源操作。
    if (resource_id != nullptr && std::strcmp(resource_id, kSlotDeleteResourceId) == 0) {
        g_slot_delete_orig.store(backup, std::memory_order_release);
        SB_LOG("slot delete hook: backup handover orig=%p", backup);
    }
    return 0;
}

void plugin_fill_status(contracts::PluginStatusOut& out) {
    out.enabled = g_active.load(std::memory_order_acquire);
    out.error = nullptr;
}

const contracts::PluginDescriptor kDescriptor = {
    kPluginId,
    kPluginVersion,
    4, kRequires,  // requires=ui_panels + save.facts + http.registry + http.dispatch
    0, nullptr,   // optional_requires
    0, nullptr,   // provides
    2, kResources,// resources（删档回调 GOT 槽 + own:popup.savebackup）
    0, nullptr,   // jni（JNI 入口由 bridge/native/gamebridge_operations.cpp 保留）
    &plugin_install,
    &plugin_deactivate,
    &plugin_on_enabled,
    &plugin_on_disabled,
    &plugin_fill_status,
    &plugin_on_resource_ready,
    kPorts,
    1,
    kSbHttpRoutes,
    static_cast<uint32_t>(sizeof(kSbHttpRoutes) / sizeof(kSbHttpRoutes[0])),
};

}  // namespace

const contracts::PluginDescriptor& plugin_descriptor() {
    return kDescriptor;
}

}  // namespace qol::features::save_backup
