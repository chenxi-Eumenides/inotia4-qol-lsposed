#include "plugins/save_backup/plugin.h"

#include <sys/mman.h>

#include <atomic>
#include <cerrno>
#include <cstdint>

#include "core/native/qol_log.h"
#include "plugins/save_backup/feature.h"
#include "game/access/save.h"
#include "game_access.h"
#include "game_ptr_hook.h"

namespace qol::features::save_backup {
namespace {

#define SB_LOG(...) QOL_LOG_INFO(QolDomain::kSaveBackup, __VA_ARGS__)

constexpr const char* kPluginId = "save_backup";
constexpr const char* kPluginVersion = "0.1.0";

// 常开标记：零资源、无配置门禁；仅用于 fill_status 上报（HTTP/UI 业务不受其门控）。
std::atomic<bool> g_active{true};

// 删档同步（过渡机制）：PtrHook 覆盖游戏删档回调 SaveSlot_Delete 的 GOT 槽。
// 目标符号 F_SAVESLOT_DELETE_VMA 与槽址 G_SAVESLOT_DELETE_GOT_VMA 的解析收口在
// game/access/save；本处只做机制安装。待 patch/ptr_slot（B2a-3，随 D2）转声明式资源后，
// 本文件删除 PtrHook 直装，改由宿主通用执行。
constexpr size_t kSlotDeletePageSize = 0x1000;
PtrHook g_slot_delete_hook;

// 执行上下文：游戏主线程（UIPopupMsg_ButtonOKExe → blr），x0 = slot(int32_t)。
// 先回调原函数（删 save{n}.dat + 重载三槽），再调业务同步体删该槽 sidecar。
void slot_delete_hook_wrapper(int32_t slot) {
    if (g_slot_delete_hook.installed()) {
        g_slot_delete_hook.call_orig(slot);
    }
    save_backup_slot_deleted_sync(slot);
}

// fail-closed：bridge 未就绪/符号未解析/GOT 当前值 != SaveSlot_Delete 时不安装。
// 安装一次即常驻；幂等重入直接返回（install 可能在 activate 重试时再次执行）。
void install_slot_delete_hook_if_ready() {
    if (g_slot_delete_hook.installed()) return;
    if (!bridge_ready() || g_base == 0) return;
    void* slot = nullptr;
    uintptr_t expected = 0;
    if (!qol::game::save_slot_delete_hook_target(&slot, &expected)) {
        SB_LOG("slot delete hook: F_SAVESLOT_DELETE_VMA unresolved");
        return;
    }
    if (!game_memory_accessible(slot, sizeof(void*), 'r')) {
        SB_LOG("slot delete hook: got not accessible slot=%p", slot);
        return;
    }
    if (*reinterpret_cast<void**>(slot) != reinterpret_cast<void*>(expected)) {
        SB_LOG("slot delete hook: unexpected got value=%p expected=%p",
               *reinterpret_cast<void**>(slot), reinterpret_cast<void*>(expected));
        return;
    }
    const uintptr_t page = reinterpret_cast<uintptr_t>(slot) & ~(kSlotDeletePageSize - 1);
    if (mprotect(reinterpret_cast<void*>(page), kSlotDeletePageSize, PROT_READ | PROT_WRITE) != 0) {
        SB_LOG("slot delete hook: mprotect failed errno=%d", errno);
        return;
    }
    if (!g_slot_delete_hook.install_typed(slot, &slot_delete_hook_wrapper)) {
        SB_LOG("slot delete hook: install failed slot=%p", slot);
        return;
    }
    SB_LOG("slot delete hook installed slot=%p orig=%p", slot, g_slot_delete_hook.orig);
}

int plugin_install(contracts::PluginContext& ctx) {
    (void)ctx;
    g_active.store(true, std::memory_order_release);
    // 删档 PtrHook 为 legacy 机制，install 安装一次即常驻（幂等）。
    install_slot_delete_hook_if_ready();
    return 0;
}

void plugin_deactivate(contracts::PluginContext& ctx) {
    (void)ctx;
    // 零资源插件无宿主托管资源可归还；删档 PtrHook 属未托管 legacy 机制，进程内常驻，
    // 待 patch/ptr_slot（B2a-3）迁移后由宿主管理。此处仅翻转上报标记（幂等）。
    g_active.store(false, std::memory_order_release);
}

void plugin_on_enabled(contracts::PluginContext& ctx) {
    (void)ctx;
    g_active.store(true, std::memory_order_release);
}

void plugin_on_disabled(contracts::PluginContext& ctx) {
    (void)ctx;
    g_active.store(false, std::memory_order_release);
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
    0, nullptr,   // resources（零资源）
    0, nullptr,   // jni（JNI 入口由 bridge/native/gamebridge_operations.cpp 保留）
    &plugin_install,
    &plugin_deactivate,
    &plugin_on_enabled,
    &plugin_on_disabled,
    &plugin_fill_status,
    nullptr,      // on_resource_ready（无资源）
};

}  // namespace

const contracts::PluginDescriptor& plugin_descriptor() {
    return kDescriptor;
}

}  // namespace qol::features::save_backup
