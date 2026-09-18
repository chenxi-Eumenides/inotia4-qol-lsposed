#include "plugins/extension_bag/plugin.h"

#include <cstdint>

#include "feature/extension_bag/extension_bag_bridge.h"

// ===========================================================================
// 扩展背包插件（D2a-3b-1）：8 条声明式资源的 owner
// ===========================================================================
//
// 本批只把「入口 hook / PopupState 占用 / desc 槽命名空间 / move_merge 指针槽」
// 声明式化；C1–C21 call patch 与两条轮询线程保留（a3b-2 收口）。wrapper 业务体
// 逐字留在 feature/extension_bag 与 feature/patch，本文件只做声明与门禁接线。
//
// M1/M2 校正（D2a-3b-1）：ADR-0007 表中 M1/M2 记为 required × failClosed；
// 按晚就绪语义校正为 required × failSoft —— 装配期失败时资源保持 free、插件
// degraded（error=资源 id），gate 开且 free 时宿主每逻辑帧重试到成功；语义等价
// 旧的 300ms 线程重试，且状态可观测。ADR-0007 与 ledger 已同步该一条校正。

namespace qol::features::extension_bag {
namespace {

constexpr const char* kPluginId = "extension_bag";
constexpr const char* kPluginVersion = "0.1.0";

// 资源表在 extension_bag_claims() 首次调用时构造（函数内静态，无跨 TU 静态初始化顺序问题）。
uint32_t g_resource_count = 0;
const qol::contracts::ResourceClaim* g_resources = extension_bag_claims(&g_resource_count);

int plugin_install(qol::contracts::PluginContext& ctx) {
    (void)ctx;
    // 自身初始化无额外动作：资源由宿主声明式安装；gate 键 extensionBagEnabled 由
    // nativeSetExtensionBagEnabled 经命令端口写入（无独立注册表，config 按需建键）。
    return 0;
}

void plugin_deactivate(qol::contracts::PluginContext& ctx) {
    (void)ctx;
    // 幂等：资源归还包括 PopupState 回写与 X1 槽还原（宿主引擎执行）；本回调只关业务启用标志。
    extension_bag_set_enabled(false);
}

// 启停仅切门禁，资源安装/还原由宿主完成（§3.3）。
void plugin_on_enabled(qol::contracts::PluginContext& ctx) {
    (void)ctx;
    extension_bag_set_enabled(true);
}

void plugin_on_disabled(qol::contracts::PluginContext& ctx) {
    (void)ctx;
    extension_bag_set_enabled(false);
}

void plugin_fill_status(qol::contracts::PluginStatusOut& out) {
    out.enabled = extension_bag_enabled();
    out.error = nullptr;
}

int32_t plugin_on_resource_ready(qol::contracts::PluginContext& ctx, const char* resource_id,
                                 void* backup) {
    (void)ctx;
    // 受限回调（§3.3）：只缓存入口 backup（常数时间），无资源操作。
    return extension_bag_bind_backup(resource_id, backup);
}

const qol::contracts::PluginDescriptor kDescriptor = {
    kPluginId,
    kPluginVersion,
    0, nullptr,   // requires
    0, nullptr,   // optional_requires
    0, nullptr,   // provides
    g_resource_count, g_resources,  // resources（8 条）
    0, nullptr,   // jni（JNI 入口由 bridge/native 保留，名/签名冻结）
    &plugin_install,
    &plugin_deactivate,
    &plugin_on_enabled,
    &plugin_on_disabled,
    &plugin_fill_status,
    &plugin_on_resource_ready,
};

}  // namespace

const qol::contracts::PluginDescriptor& plugin_descriptor() {
    return kDescriptor;
}

}  // namespace qol::features::extension_bag
