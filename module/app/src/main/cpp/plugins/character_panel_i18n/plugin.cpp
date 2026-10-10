#include "plugin.h"

#include <cstdint>

#include "charinfo_scope.h"
#include "feature.h"

namespace qol::features::character_panel_i18n {
namespace {

constexpr const char* kPluginId = "character_panel_i18n";
constexpr const char* kPluginVersion = "0.1.0";

// 角色面板绘制窗口入口 hook（私有资源，owner=本插件）：把该绘制窗口标记为 kCharacterPanel
// 作用域。fail-soft：安装失败记为 degraded，不整体失败（文本机制与其它内容贡献者继续）。
const contracts::ResourceClaim kResources[] = {
    {
        "hook:Scene_Draw_Popup_SC_CharacterInfo",  // id
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
};

const contracts::CapabilityRequirement kRequires[] = {
    {contracts::kTextLayerCapabilityId,
     {contracts::kTextLayerVersionMajor, contracts::kTextLayerVersionMinor}},
};

int plugin_install(contracts::PluginContext& ctx) {
    bind_sdk(ctx.sdk);
    if (ctx.sdk == nullptr) return 1;
    // 表消费者经 text_layer 能力注册；失败即 install 失败（宿主回滚并把插件置 failed）。
    if (!register_consumers()) return 1;
    return 0;
}

void plugin_deactivate(contracts::PluginContext& ctx) {
    (void)ctx;
    unregister_consumers();
    bind_sdk(nullptr);
}

void plugin_on_enabled(contracts::PluginContext& ctx) {
    (void)ctx;
    (void)register_consumers();  // 幂等（activate 重入）
}

void plugin_on_disabled(contracts::PluginContext& ctx) {
    (void)ctx;
    unregister_consumers();
}

int32_t on_resource_ready(contracts::PluginContext& ctx, const char* resource_id, void* backup) {
    (void)ctx;
    if (resource_id == nullptr) return 0;
    return charinfo_scope_on_resource_ready(backup);
}

void plugin_fill_status(contracts::PluginStatusOut& out) {
    out.enabled = true;
    out.error = nullptr;
}

// Feature callback does not replace the existing lifecycle or business controls.
constexpr contracts::FeatureDefinition kFeatures[] = {
    {"character_panel_i18n.translation", 1, contracts::FeatureCategory::kGameplay,
     contracts::FeatureType::kToggle, "角色面板中文", "在角色面板绘制作用域内提供中文文案。",
     [](contracts::PluginContext&, uint8_t) -> QolResult { return contracts::ok(); }, nullptr},
};

const contracts::PluginDescriptor kDescriptor = {
    kPluginId,
    kPluginVersion,
    1, kRequires,  // requires=text_layer（唯一 GetText owner 由 core.custom_ui 提供）
    0, nullptr,    // optional_requires
    0, nullptr,    // provides（纯内容贡献者）
    1, kResources, // resources（角色面板绘制窗口 hook）
    0, nullptr,    // jni
    &plugin_install,
    &plugin_deactivate,
    &plugin_on_enabled,
    &plugin_on_disabled,
    &plugin_fill_status,
    &on_resource_ready,
    nullptr, 0, nullptr, 0,
    kFeatures, 1,
};

}  // namespace

const contracts::PluginDescriptor& plugin_descriptor() {
    return kDescriptor;
}

}  // namespace qol::features::character_panel_i18n
