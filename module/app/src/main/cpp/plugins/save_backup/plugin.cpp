#include "plugins/save_backup/plugin.h"

#include <atomic>
#include <cstdint>
#include <cstring>

#include "contracts/ownership.h"
#include "contracts/panels.h"
#include "plugins/save_backup/feature.h"
#include "plugins/save_backup/log.h"
#include "plugins/save_backup/ui/panel.h"

namespace qol::features::save_backup {
namespace {

#define SB_LOG(...) SAVE_BACKUP_LOG_INFO(__VA_ARGS__)

constexpr const char* kPluginId = "save_backup";
constexpr const char* kPluginVersion = "0.1.0";

// 指针槽资源登记名：与 game/access/ptr_slots 的 kSlots 条目一一对应。
constexpr const char* kSlotDeleteResourceId = "ptrslot:SAVESLOT_DELETE_GOT";

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

int plugin_install(contracts::PluginContext& ctx) {
    save_backup_bind_context(ctx.sdk, ctx.game);
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
    g_active.store(false, std::memory_order_release);
    if (ctx.sdk != nullptr && ctx.sdk->panels.unregister_owner != nullptr) {
        (void)ctx.sdk->panels.unregister_owner(kPluginId);
    }
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
    0, nullptr,   // requires
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
};

}  // namespace

const contracts::PluginDescriptor& plugin_descriptor() {
    return kDescriptor;
}

}  // namespace qol::features::save_backup
