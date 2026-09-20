#include "plugins/extension_bag/plugin.h"

#include <cstdint>
#include <cstring>

#include "contracts/events.h"
#include "contracts/inventory_refresh.h"
#include "plugins/extension_bag/extension_bag_bridge.h"
#include "plugins/extension_bag/move_merge.h"

// ===========================================================================
// 扩展背包插件：29 条声明式资源的 owner + extension_bag v1.0 provider + move_merge 门禁
// ===========================================================================
//
// 资源面（a3b-1/a3b-2）：入口 hook / PopupState 占用 / desc 槽命名空间 / move_merge 指针槽
// 与 C1–C21 call patch 全部声明式化；wrapper 业务体逐字留在 plugins/extension_bag
// （D2c-2b-2 后 feature/patch 已删除），本文件只做声明与门禁接线。
//
// 能力面（a3c）：provides `extension_bag` v1.0（contracts/extension_bag.h 的 ExtensionBagApi），
// iface 由 extension_bag_api_table() 提供（EB TU 逐条转发既有 port）。宿主在 enabled 时注册、
// revoking 时注销（§3.4.4 既有路径）；本文件不新增 JNI / 资源。
//
// move_merge 门禁（D2c-2b-2）：nativeSetMoveMergeEnabled → kCommandSetConfigBool
// ("moveMergeEnabled") → 本插件订阅 kEventConfigChanged 应用 set_move_merge_enabled
// （extension source protection 联动由 set_virtual_bag_enabled 逐字保持）。
//
// M1/M2 校正（D2a-3b-1）：ADR-0007 表中 M1/M2 记为 required × failClosed；
// 按晚就绪语义校正为 required × failSoft —— 装配期失败时资源保持 free、插件
// degraded（error=资源 id），gate 开且 free 时宿主每逻辑帧重试到成功；语义等价
// 旧的 300ms 线程重试，且状态可观测。ADR-0007 与 ledger 已同步该一条校正。

namespace qol::features::extension_bag {
namespace {

constexpr const char* kPluginId = "extension_bag";
constexpr const char* kPluginVersion = "0.1.0";
constexpr const char* kMoveMergeGateKey = "moveMergeEnabled";

const contracts::QolHostSdk* g_sdk = nullptr;
const contracts::QolGameHost* g_game = nullptr;

// moveMergeEnabled 配置变更订阅句柄；deactivate 时成对注销（install 幂等重入）。
uint64_t g_config_subscription = 0;

void on_config_changed(void* ctx, const void* payload) {
    (void)ctx;
    if (payload == nullptr) return;
    const auto* event = static_cast<const qol::contracts::ConfigChangedEvent*>(payload);
    if (event->key == nullptr || std::strcmp(event->key, kMoveMergeGateKey) != 0) return;
    set_move_merge_enabled(event->int_value != 0);
}

// 资源表在 extension_bag_claims() 首次调用时构造（函数内静态，无跨 TU 静态初始化顺序问题）。
uint32_t g_resource_count = 0;
const qol::contracts::ResourceClaim* g_resources = extension_bag_claims(&g_resource_count);

// 能力提供声明：extension_bag v1.0，iface = EB TU 的 ExtensionBagApi 静态表。
// 注册失败（iface 空/容量满/重复）由宿主只记录不阻断（plugin_registry 既有口径）。
const qol::contracts::CapabilityProvisionRef kProvides[] = {
    {
        qol::contracts::kExtensionBagCapabilityId,
        {static_cast<uint16_t>(qol::contracts::kExtensionBagVersionMajor),
         static_cast<uint16_t>(qol::contracts::kExtensionBagVersionMinor)},
        extension_bag_api_table(),
    },
};

const qol::contracts::CapabilityRequirement kRequires[] = {
    {
        qol::contracts::kInventoryRefreshCapabilityId,
        {qol::contracts::kInventoryRefreshVersionMajor,
         qol::contracts::kInventoryRefreshVersionMinor},
    },
};

int plugin_install(qol::contracts::PluginContext& ctx) {
    g_sdk = ctx.sdk;
    g_game = ctx.game;
    if (ctx.struct_size < sizeof(qol::contracts::PluginContext) ||
        ctx.sdk_major != qol::contracts::kHostSdkMajor ||
        ctx.sdk == nullptr || ctx.game == nullptr ||
        ctx.sdk->config.get_bool == nullptr || ctx.sdk->events.subscribe == nullptr ||
        ctx.sdk->events.unsubscribe == nullptr) {
        g_sdk = nullptr;
        g_game = nullptr;
        extension_bag_bind_port_sdk(nullptr);
        return 1;
    }
    if (!extension_bag_bind_host(g_sdk, g_game)) {
        g_sdk = nullptr;
        g_game = nullptr;
        extension_bag_bind_port_sdk(nullptr);
        return 1;
    }
    extension_bag_bind_port_sdk(g_sdk);
    // 自身初始化：资源由宿主声明式安装；gate 键 extensionBagEnabled 由
    // nativeSetExtensionBagEnabled 经命令端口写入。moveMergeEnabled 读初始值并订阅
    // 配置变更（inactive→resolving 幂等重入：已订阅不重复订阅）。
    int32_t move_merge_enabled = 0;
    (void)g_sdk->config.get_bool(kMoveMergeGateKey, 0, &move_merge_enabled);
    set_move_merge_enabled(move_merge_enabled != 0);
    if (g_config_subscription == 0) {
        (void)g_sdk->events.subscribe(qol::contracts::kEventConfigChanged, &on_config_changed,
                                      nullptr, &g_config_subscription);
    }
    return 0;
}

void plugin_deactivate(qol::contracts::PluginContext& ctx) {
    (void)ctx;
    // 幂等：退订后置 0，便于再次 install 重新订阅；move_merge 请求标志保持（与旧
    // 独立 JNI 开关语义一致，deactivate 不重置），资源归还包括 PopupState 回写与
    // X1 槽还原（宿主引擎执行）；本回调只关业务启用标志。
    if (g_config_subscription != 0 && g_sdk != nullptr &&
        g_sdk->events.unsubscribe != nullptr) {
        (void)g_sdk->events.unsubscribe(g_config_subscription);
        g_config_subscription = 0;
    }
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
    out.enabled = qol::features::extension_bag::extension_bag_enabled();
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
    1, kRequires,  // requires=inventory_refresh
    0, nullptr,   // optional_requires
    1, kProvides,  // provides（extension_bag v1.0）
    g_resource_count, g_resources,  // resources（29 条）
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
