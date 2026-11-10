#include "plugin.h"

#include <atomic>
#include <cstdint>
#include <cstring>

#include "../../contracts/character_attributes.h"
#include "../../contracts/game_ports/character_extras_port.h"
#include "../../contracts/ownership.h"
#include "../../contracts/panels.h"
#include "../../contracts/runtime_hooks.h"
#include "../../contracts/ui.h"
#include "charinfo_scope.h"
#include "charinfo_watch.h"
#include "feature.h"
#include "overview.h"

namespace qol::features::character_panel {
namespace {

constexpr const char* kPluginId = "character_panel";
constexpr const char* kPluginVersion = "0.1.0";
constexpr char kRegistrationFailureMessage[] = "text consumer registration failed";
constexpr char kOpenTaskFailureMessage[] = "overview frame task registration failed";
constexpr const char* kCharinfoHookResourceId = "hook:Scene_Draw_Popup_SC_CharacterInfo";
constexpr const char* kCharacterInfoOwnershipResourceId = "own:popup.character_info";
bool g_register_failed = true;
// 常驻帧任务句柄（install 注册；0 = 未注册）。
uint64_t g_open_task = 0;

// 入口 hook：把角色面板绘制窗口标记为 kCharacterPanel 作用域（本插件 owner）。
// fail-soft：安装失败记为 degraded，不整体失败（文本机制与其它内容贡献者继续）。
const contracts::ResourceClaim kResources[] = {
    {
        kCharinfoHookResourceId,                     // id
        "F_SCENE_DRAW_POPUP_SC_CHARACTER_INFO_VMA",  // symbol
        contracts::ResourceMechanism::kEntryHook,
        contracts::ResourcePolicy::kPrivate,
        contracts::ResourceRequirement::kRequired,
        contracts::ResourceFailure::kFailSoft,
        true,                      // resumable
        kPluginId,                 // owner
        "角色面板绘制窗口标记 kCharacterPanel 作用域；属性标签由本插件经 text_layer 能力注册",
        nullptr,                   // gate（常驻）
        120,                       // step_budget
        reinterpret_cast<void*>(&scene_draw_charinfo_wrapper),  // handler
    },
    {
        // 行为所有权：只占用原版人物属性面板的 PopupState event 槽做点击检测，
        // enter/process/f3/f4 仍是原函数，面板本身行为不变。
        kCharacterInfoOwnershipResourceId,  // id
        kCharacterInfoOwnershipResourceId,  // symbol（own: 前缀，经宿主所有权槽解析）
        contracts::ResourceMechanism::kOwnership,
        contracts::ResourcePolicy::kPrivate,
        contracts::ResourceRequirement::kRequired,
        contracts::ResourceFailure::kFailSoft,
        true,       // resumable
        kPluginId,  // owner
        "原版人物属性面板内容区点击 → 延迟打开属性总览面板",
        nullptr,  // gate（常驻；用户开关在点击与打开处判定）
        120,      // step_budget
        reinterpret_cast<void*>(
            const_cast<contracts::OwnershipOps*>(&kCharacterInfoPopupOps)),  // handler
    },
};

const contracts::CapabilityRequirement kRequires[] = {
    {contracts::kTextLayerCapabilityId,
     {contracts::kTextLayerVersionMajor, contracts::kTextLayerVersionMinor}},
    {contracts::kUiComponentsCapabilityId,
     {contracts::kUiContractMajor, contracts::kUiContractMinor}},
    {contracts::kUiPanelsCapabilityId,
     {contracts::kUiPanelsVersionMajor, contracts::kUiPanelsVersionMinor}},
    {contracts::kCharacterAttributesCapabilityId,
     {contracts::kCharacterAttributesVersion.major,
      contracts::kCharacterAttributesVersion.minor}},
};

const contracts::QolPortRequirement kPorts[] = {
    {contracts::QolPortId::QOL_PORT_CHARACTER_EXTRAS,
     {contracts::kQolPortCharacterExtrasVersion.major,
      contracts::kQolPortCharacterExtrasVersion.minor}},
};

const contracts::PanelCallbacks kPanelCallbacks{&overview_panel_enter, &overview_panel_process,
                                                &overview_panel_f3, &overview_panel_f4,
                                                &overview_panel_event};

const contracts::PanelProviderSpec kPanelProvider{
    kOverviewPanelId,
    kPluginId,
    &overview_provider_open,
    &overview_provider_close,
    &overview_provider_active,
    &overview_provider_block_reason,
    &kPanelCallbacks,
};

int32_t overview_open_tick(int64_t frame, void* ctx) {
    return character_info_open_tick(frame, ctx);
}

void log_info(int32_t level, const char* message) {
    const contracts::QolHostSdk* sdk = host_sdk();
    if (sdk == nullptr || sdk->log.write == nullptr || message == nullptr) return;
    (void)sdk->log.write(level, 1, kPluginId, message,
                         static_cast<uint32_t>(std::strlen(message)));
}

bool remove_open_task() {
    if (g_open_task == 0) return true;
    const uint64_t task = g_open_task;
    // 句柄与注册状态保持一致：无论 remove 结果如何都清空句柄，避免下次 activate 时
    // 因陈旧 id 跳过重新注册。
    g_open_task = 0;
    const contracts::QolHostSdk* sdk = host_sdk();
    if (sdk == nullptr || sdk->scheduler.remove == nullptr) return false;
    return contracts::succeeded(sdk->scheduler.remove(task));
}

// 常驻帧任务注册（幂等）：install 与 activate/on_enabled 都可调用。
bool ensure_open_task() {
    if (g_open_task != 0) return true;
    const contracts::QolHostSdk* sdk = host_sdk();
    if (sdk == nullptr || sdk->scheduler.add == nullptr) return false;
    uint64_t task = 0;
    if (!contracts::succeeded(sdk->scheduler.add(contracts::kQolRuntimeFramePointLogicPre,
                                                 &overview_open_tick, nullptr, 1, 0, &task))) {
        return false;
    }
    g_open_task = task;
    return true;
}

int plugin_install(contracts::PluginContext& ctx) {
    bind_sdk(ctx.sdk);
    if (ctx.sdk == nullptr || ctx.sdk->capabilities.require == nullptr) {
        bind_sdk(nullptr);
        g_register_failed = true;
        return 1;
    }
    // 端口：总览数据只经 CHARACTER_EXTRAS 读面获取（角色指针 + 队伍/菜单角色回退）。
    if (ctx.ports == nullptr || ctx.port_count != 1 || ctx.ports[0] == nullptr) {
        bind_sdk(nullptr);
        return 1;
    }
    const auto* character_extras =
        static_cast<const contracts::QolCharacterExtrasPort*>(ctx.ports[0]);
    if (!contracts::satisfies(character_extras->abi_version,
                              {contracts::kQolPortCharacterExtrasVersion.major,
                               contracts::kQolPortCharacterExtrasVersion.minor})) {
        bind_sdk(nullptr);
        return 1;
    }
    const void* ui_components_iface = nullptr;
    if (!contracts::succeeded(ctx.sdk->capabilities.require(
            contracts::kUiComponentsCapabilityId,
            {contracts::kUiContractMajor, contracts::kUiContractMinor},
            &ui_components_iface)) ||
        ui_components_iface == nullptr) {
        bind_sdk(nullptr);
        return 1;
    }
    const void* attributes_iface = nullptr;
    if (!contracts::succeeded(ctx.sdk->capabilities.require(
            contracts::kCharacterAttributesCapabilityId,
            {contracts::kCharacterAttributesVersion.major,
             contracts::kCharacterAttributesVersion.minor},
            &attributes_iface)) ||
        attributes_iface == nullptr) {
        bind_sdk(nullptr);
        return 1;
    }
    overview_bind_components(ui_components_iface);
    overview_bind_attributes(attributes_iface);
    overview_bind_context(ctx.game, ctx.ports[0]);
    set_translation_enabled(true);
    // 表消费者经 text_layer 能力注册；失败即 install 失败（宿主回滚并把插件置 failed）。
    const bool registered = register_consumers();
    g_register_failed = !registered;
    if (!registered) {
        overview_bind_context(nullptr, nullptr);
        overview_bind_components(nullptr);
        overview_bind_attributes(nullptr);
        bind_sdk(nullptr);
        return 1;
    }
    if (ctx.sdk->panels.register_provider == nullptr ||
        !contracts::succeeded(ctx.sdk->panels.register_provider(&kPanelProvider))) {
        unregister_consumers();
        overview_bind_context(nullptr, nullptr);
        overview_bind_components(nullptr);
        overview_bind_attributes(nullptr);
        bind_sdk(nullptr);
        return 1;
    }
    // 常驻帧任务：消费原版面板点击留下的 pending，在帧安全点经 ui_panels 打开总览。
    if (!ensure_open_task()) log_info(1, kOpenTaskFailureMessage);
    return 0;
}

void plugin_deactivate(contracts::PluginContext& ctx) {
    // 所有权/入口资源由宿主资源事务归还；本回调只撤销消费者、面板与会话状态。
    set_translation_enabled(false);
    unregister_consumers();
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
    if (ctx.sdk != nullptr && ctx.sdk->panels.unregister_owner != nullptr) {
        (void)ctx.sdk->panels.unregister_owner(kPluginId);
    }
    (void)remove_open_task();
    character_info_reset();
    overview_provider_close();
    overview_bind_context(nullptr, nullptr);
    overview_bind_components(nullptr);
    overview_bind_attributes(nullptr);
    bind_sdk(nullptr);
}

void plugin_on_enabled(contracts::PluginContext& ctx) {
    (void)ctx;
    set_translation_enabled(true);
    g_register_failed = !register_consumers();  // 幂等（activate 重入）
    if (g_register_failed) log_info(2, kRegistrationFailureMessage);
    // activate 重入时帧任务可能已在；deactivate 后再 activate 时重新注册。
    if (!ensure_open_task()) log_info(1, kOpenTaskFailureMessage);
}

void plugin_on_disabled(contracts::PluginContext& ctx) {
    (void)ctx;
    set_translation_enabled(false);
    unregister_consumers();
}

int32_t on_resource_ready(contracts::PluginContext& ctx, const char* resource_id, void* backup) {
    (void)ctx;
    if (resource_id == nullptr) return 0;
    if (std::strcmp(resource_id, kCharinfoHookResourceId) == 0) {
        return charinfo_scope_on_resource_ready(backup);
    }
    // ownership 资源没有原函数入口：启动期定向恢复也会以 backup=nullptr 调到这里。
    return 0;
}

void plugin_fill_status(contracts::PluginStatusOut& out) {
    out.enabled = !g_register_failed && host_sdk() != nullptr;
    out.error = out.enabled ? nullptr : kRegistrationFailureMessage;
}

// Feature callback does not replace the existing lifecycle or business controls.
constexpr contracts::FeatureDefinition kFeatures[] = {
    {"character_panel.i18n", 1, contracts::FeatureCategory::kBasicOptimization,
     contracts::FeatureType::kToggle, "属性面板汉化", "人物属性显示为中文",
     [](contracts::PluginContext&, uint8_t enabled) -> QolResult {
         set_translation_enabled(enabled != 0);
         return contracts::ok();
     },
     nullptr},
    {"character_panel.overview", 1, contracts::FeatureCategory::kBasicOptimization,
     contracts::FeatureType::kToggle, "属性总览", "点击人物属性面板内容区打开属性总览",
     [](contracts::PluginContext&, uint8_t enabled) -> QolResult {
         set_overview_enabled(enabled != 0);
         return contracts::ok();
     },
     nullptr},
};

const contracts::PluginDescriptor kDescriptor = {
    kPluginId,
    kPluginVersion,
    4, kRequires,  // requires=text_layer + ui_components + ui_panels + character.attributes
    0, nullptr,    // optional_requires
    0, nullptr,    // provides（纯消费者）
    2, kResources, // resources（面板绘制作用域 hook + 面板 PopupState 所有权）
    0, nullptr,    // jni
    &plugin_install,
    &plugin_deactivate,
    &plugin_on_enabled,
    &plugin_on_disabled,
    &plugin_fill_status,
    &on_resource_ready,
    kPorts,
    1,
    nullptr, 0,
    kFeatures, 2,
};

}  // namespace

const contracts::PluginDescriptor& plugin_descriptor() {
    return kDescriptor;
}

}  // namespace qol::features::character_panel
