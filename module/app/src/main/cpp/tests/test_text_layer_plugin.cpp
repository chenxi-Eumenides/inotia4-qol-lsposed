// plugins/text_layer 插件清单/资源 host 单测：
// 2 条入口 hook 资源（MEMORYTEXT_GetText 共享 + 角色面板作用域私有）的机制/策略/门禁/handler，
// 以及 on_resource_ready 的 backup 交付 → wrapper 转发。
//
// 本 target 链接 plugin.cpp + feature.cpp + charinfo_scope.cpp + hooks/dispatch.cpp；
// 真机安装与文本替换行为由真机回归验证。

#include "plugins/text_layer/plugin.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "contracts/plugin.h"
#include "plugins/text_layer/charinfo_scope.h"
#include "plugins/text_layer/feature.h"

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond) do { \
    if (cond) ++g_pass; \
    else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

// plugin.cpp/feature.cpp 以按引用传参调用生命周期回调，需要 PluginContext 实体。
namespace qol::contracts {
struct PluginContext {};
}  // namespace qol::contracts

namespace {

using qol::contracts::ResourceFailure;
using qol::contracts::ResourceMechanism;
using qol::contracts::ResourcePolicy;
using qol::contracts::ResourceRequirement;

bool g_scene_draw_called = false;
void fake_scene_draw() { g_scene_draw_called = true; }

void test_manifest() {
    const qol::contracts::PluginDescriptor& d =
        qol::features::text_layer::plugin_descriptor();
    CHECK(std::strcmp(d.id, "text_layer") == 0);
    CHECK(d.requires_count == 0 && d.optional_count == 0);
    CHECK(d.provides_count == 1);
    CHECK(d.resources_count == 2);
    CHECK(d.on_resource_ready != nullptr);

    const qol::contracts::ResourceClaim& get_text = d.resources[0];
    CHECK(std::strcmp(get_text.id, qol::features::text_layer::kResourceIdGetText) == 0);
    CHECK(std::strcmp(get_text.symbol, "F_MEMORYTEXT_GET_TEXT_VMA") == 0);
    CHECK(get_text.mechanism == ResourceMechanism::kEntryHook);
    CHECK(get_text.policy == ResourcePolicy::kShared);
    CHECK(get_text.requirement == ResourceRequirement::kRequired);
    CHECK(get_text.failure == ResourceFailure::kFailClosed);
    CHECK(get_text.resumable);
    CHECK(std::strcmp(get_text.owner, "text_layer") == 0);
    CHECK(get_text.gate == nullptr);
    CHECK(get_text.step_budget == 120);
    CHECK(get_text.handler ==
          reinterpret_cast<void*>(&qol::features::text_layer::memorytext_get_text_wrapper));

    const qol::contracts::ResourceClaim& charinfo = d.resources[1];
    CHECK(std::strcmp(charinfo.id, qol::features::text_layer::kResourceIdCharinfoScope) == 0);
    CHECK(std::strcmp(charinfo.symbol, "F_SCENE_DRAW_POPUP_SC_CHARACTER_INFO_VMA") == 0);
    CHECK(charinfo.mechanism == ResourceMechanism::kEntryHook);
    CHECK(charinfo.policy == ResourcePolicy::kPrivate);
    CHECK(charinfo.requirement == ResourceRequirement::kRequired);
    CHECK(charinfo.failure == ResourceFailure::kFailSoft);
    CHECK(charinfo.resumable);
    CHECK(std::strcmp(charinfo.owner, "text_layer") == 0);
    CHECK(charinfo.gate == nullptr);
    CHECK(charinfo.step_budget == 120);
    CHECK(charinfo.handler ==
          reinterpret_cast<void*>(&qol::features::text_layer::scene_draw_charinfo_wrapper));
}

// backup 交付：on_resource_ready 缓存原函数入口后，wrapper 转发原调用。
void test_charinfo_backup_delivery() {
    g_scene_draw_called = false;
    qol::contracts::PluginContext ctx{};
    const qol::contracts::PluginDescriptor& d =
        qol::features::text_layer::plugin_descriptor();
    CHECK(d.on_resource_ready(ctx, qol::features::text_layer::kResourceIdCharinfoScope,
                              reinterpret_cast<void*>(&fake_scene_draw)) == 0);
    qol::features::text_layer::scene_draw_charinfo_wrapper();
    CHECK(g_scene_draw_called);
}

}  // namespace

int main() {
    test_manifest();
    test_charinfo_backup_delivery();
    std::printf("text_layer_plugin: pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
