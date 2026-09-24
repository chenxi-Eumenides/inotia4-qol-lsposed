// plugins/character_panel_i18n 插件清单/资源 host 单测（ADR 0001 D3/D4）：
// 角色面板绘制窗口作用域 hook 的机制/策略/门禁/handler、标签表（连续性 + 35188 占位）、
// backup 交付 → wrapper 转发。文本机制本身归 core.custom_ui，其清单由 native_choice_tests 断言；
// 真机文本替换行为由真机回归验证。

#include "plugins/character_panel_i18n/plugin.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "contracts/plugin.h"
#include "plugins/character_panel_i18n/charinfo_scope.h"
#include "plugins/character_panel_i18n/feature.h"

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond) do { \
    if (cond) ++g_pass; \
    else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

namespace {

using qol::contracts::ResourceFailure;
using qol::contracts::ResourceMechanism;
using qol::contracts::ResourcePolicy;
using qol::contracts::ResourceRequirement;

bool g_scene_draw_called = false;
void fake_scene_draw() { g_scene_draw_called = true; }

void test_manifest() {
    const qol::contracts::PluginDescriptor& d =
        qol::features::character_panel_i18n::plugin_descriptor();
    CHECK(std::strcmp(d.id, "character_panel_i18n") == 0);
    CHECK(d.requires_count == 1 && d.optional_count == 0);  // requires=text_layer（机制在 core.custom_ui）
    CHECK(d.provides_count == 0);                            // 纯内容贡献者
    CHECK(d.resources_count == 1);
    CHECK(d.on_resource_ready != nullptr);

    const qol::contracts::ResourceClaim& charinfo = d.resources[0];
    CHECK(std::strcmp(charinfo.id, "hook:Scene_Draw_Popup_SC_CharacterInfo") == 0);
    CHECK(std::strcmp(charinfo.symbol, "F_SCENE_DRAW_POPUP_SC_CHARACTER_INFO_VMA") == 0);
    CHECK(charinfo.mechanism == ResourceMechanism::kEntryHook);
    CHECK(charinfo.policy == ResourcePolicy::kPrivate);
    CHECK(charinfo.requirement == ResourceRequirement::kRequired);
    CHECK(charinfo.failure == ResourceFailure::kFailSoft);
    CHECK(charinfo.resumable);
    CHECK(std::strcmp(charinfo.owner, "character_panel_i18n") == 0);
    CHECK(charinfo.gate == nullptr);
    CHECK(charinfo.step_budget == 120);
    CHECK(charinfo.handler ==
          reinterpret_cast<void*>(&qol::features::character_panel_i18n::scene_draw_charinfo_wrapper));
}

// 标签表：覆盖 35185..35196 连续升序；35188 为 nullptr 占位；未命中/占位返回 nullptr。
void test_table() {
    size_t count = 0;
    const auto* entries = qol::features::character_panel_i18n::entries(&count);
    CHECK(count == 12);
    CHECK(entries != nullptr);
    for (size_t i = 0; i < count; ++i) {
        CHECK(entries[i].text_id == static_cast<uint16_t>(35185 + i));
        CHECK(static_cast<uint8_t>(entries[i].scope) == 1);
    }
    const auto scope_panel = static_cast<qol::infra::TextScope>(1);
    // 命中：35185 物攻
    CHECK(std::strcmp(qol::features::character_panel_i18n::lookup(35185, scope_panel), "物攻") == 0);
    // 占位：35188 显式 nullptr（保持原文），不得替换
    CHECK(qol::features::character_panel_i18n::lookup(35188, scope_panel) == nullptr);
    CHECK(qol::features::character_panel_i18n::entry_by_key("charinfo.magic_def") != nullptr);
    // 作用域不匹配（0 = 非面板窗口）不替换
    CHECK(qol::features::character_panel_i18n::lookup(35185, static_cast<qol::infra::TextScope>(0)) ==
          nullptr);
    CHECK(qol::features::character_panel_i18n::lookup(35185, static_cast<qol::infra::TextScope>(2)) ==
          nullptr);
}

// backup 交付：on_resource_ready 缓存原函数入口后，wrapper 转发原调用。
void test_charinfo_backup_delivery() {
    g_scene_draw_called = false;
    qol::contracts::PluginContext ctx{};
    const qol::contracts::PluginDescriptor& d =
        qol::features::character_panel_i18n::plugin_descriptor();
    CHECK(d.on_resource_ready(ctx, "hook:Scene_Draw_Popup_SC_CharacterInfo",
                              reinterpret_cast<void*>(&fake_scene_draw)) == 0);
    qol::features::character_panel_i18n::scene_draw_charinfo_wrapper();
    CHECK(g_scene_draw_called);
}

}  // namespace

int main() {
    test_manifest();
    test_table();
    test_charinfo_backup_delivery();
    std::printf("character_panel_i18n: pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
