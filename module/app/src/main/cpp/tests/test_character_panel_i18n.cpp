// plugins/character_panel_i18n 插件清单/资源 host 单测（ADR 0001 D3/D4）：
// 角色面板绘制窗口作用域 hook 的机制/策略/门禁/handler、标签表（连续性 + 35188 占位）、
// backup 交付 → wrapper 转发、作用域 push/pop（含嵌套恢复与 push 失败透传）、空 backup 拒绝、
// 消费者注册失败可观测。文本机制本身归 core.custom_ui，其清单由 native_choice_tests 断言；
// 真机文本替换行为由真机回归验证。

#include "plugins/character_panel_i18n/plugin.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "contracts/host_sdk.h"
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
namespace i18n = qol::features::character_panel_i18n;

constexpr int32_t kPanel = qol::contracts::kTextScopeCharacterPanel;

// ---- wrapper 测试用 Host SDK stub（只填 text_scope 面）----
int g_push_count = 0;
int g_pop_count = 0;
int32_t g_last_push_scope = -1;
bool g_push_ok = true;

QolResult stub_push(int32_t scope) {
    ++g_push_count;
    g_last_push_scope = scope;
    if (!g_push_ok) return qol::contracts::error(kQolErrBlocked, "blocked", 7);
    return qol::contracts::ok();
}

QolResult stub_pop() {
    ++g_pop_count;
    return qol::contracts::ok();
}

bool g_scene_draw_called = false;
bool g_allow_nested = false;
bool g_nested_done = false;

// 原绘制函数替身；g_allow_nested 时在原调用内部再次进入同一 wrapper，验证作用域嵌套恢复。
void fake_scene_draw() {
    g_scene_draw_called = true;
    if (g_allow_nested && !g_nested_done) {
        g_nested_done = true;
        i18n::scene_draw_charinfo_wrapper();
    }
}

void bind_stub_sdk() {
    static qol::contracts::QolHostSdk sdk{};
    sdk.text_scope.push = &stub_push;
    sdk.text_scope.pop = &stub_pop;
    i18n::bind_sdk(&sdk);
}

void reset_state() {
    g_push_count = 0;
    g_pop_count = 0;
    g_last_push_scope = -1;
    g_push_ok = true;
    g_scene_draw_called = false;
    g_allow_nested = false;
    g_nested_done = false;
    i18n::set_active_scope_value(qol::contracts::kTextScopeNone);
}

void test_manifest() {
    const qol::contracts::PluginDescriptor& d = i18n::plugin_descriptor();
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
    CHECK(charinfo.handler == reinterpret_cast<void*>(&i18n::scene_draw_charinfo_wrapper));
}

// 标签表：覆盖 35185..35196 连续升序；35188 为 nullptr 占位；未命中/占位返回 nullptr。
void test_table() {
    size_t count = 0;
    const auto* entries = i18n::entries(&count);
    CHECK(count == 12);
    CHECK(entries != nullptr);
    for (size_t i = 0; i < count; ++i) {
        CHECK(entries[i].text_id == static_cast<uint16_t>(35185 + i));
        CHECK(entries[i].scope == kPanel);
    }
    CHECK(std::strcmp(i18n::lookup(35185, kPanel), "物攻") == 0);
    // 占位：35188 显式 nullptr（保持原文），不得替换
    CHECK(i18n::lookup(35188, kPanel) == nullptr);
    CHECK(i18n::entry_by_key("charinfo.magic_def") != nullptr);
    // 作用域不匹配（非面板窗口）不替换
    CHECK(i18n::lookup(35185, qol::contracts::kTextScopeNone) == nullptr);
    CHECK(i18n::lookup(35185, qol::contracts::kTextScopeUimixRecipeButton) == nullptr);
    // 分派阶段/权限数值来自 contracts（不再依赖 hooks 私有枚举）
    CHECK(i18n::kOwnerTablePhase == qol::contracts::kTextPhaseAfter);
    CHECK(i18n::kOwnerTableKind == qol::contracts::kTextConsumerModify);
}

// 消费者回调：按当前线程作用域查表；超 uint16 的 text_id 不得回绕折叠命中面板 id。
void test_owner_table_modify_guard() {
    i18n::set_active_scope_value(kPanel);
    CHECK(std::strcmp(i18n::owner_table_modify(35185u, nullptr, nullptr), "物攻") == 0);
    CHECK(i18n::owner_table_modify(35188u, nullptr, nullptr) == nullptr);  // 占位保持原文
    CHECK(i18n::owner_table_modify(65536u, nullptr, nullptr) == nullptr);  // 超 uint16 不折叠
    CHECK(i18n::owner_table_modify(0x10000u + 35185u, nullptr, nullptr) == nullptr);
    i18n::set_active_scope_value(qol::contracts::kTextScopeNone);
    CHECK(i18n::owner_table_modify(35185u, nullptr, nullptr) == nullptr);  // 非面板作用域
}

// backup 交付与作用域：空 backup 拒绝；非空 → wrapper push(面板)→转发→pop，嵌套恢复。
void test_backup_delivery_and_scope() {
    reset_state();
    bind_stub_sdk();
    qol::contracts::PluginContext ctx{};
    const qol::contracts::PluginDescriptor& d = i18n::plugin_descriptor();

    CHECK(d.on_resource_ready(ctx, "hook:Scene_Draw_Popup_SC_CharacterInfo", nullptr) != 0);
    reset_state();
    CHECK(d.on_resource_ready(ctx, "hook:Scene_Draw_Popup_SC_CharacterInfo",
                              reinterpret_cast<void*>(&fake_scene_draw)) == 0);

    g_allow_nested = true;
    i18n::scene_draw_charinfo_wrapper();
    CHECK(g_scene_draw_called);
    CHECK(g_nested_done);
    CHECK(g_push_count == 2);   // 外层 + 嵌套
    CHECK(g_pop_count == 2);
    CHECK(g_last_push_scope == kPanel);
    CHECK(i18n::active_scope_value() == qol::contracts::kTextScopeNone);  // 恢复上一层
    i18n::bind_sdk(nullptr);
}

// push 失败：不写作用域、不 pop，但仍转发原绘制（fail-soft 透传）。
void test_push_failure_passthrough() {
    reset_state();
    bind_stub_sdk();
    qol::contracts::PluginContext ctx{};
    const qol::contracts::PluginDescriptor& d = i18n::plugin_descriptor();
    CHECK(d.on_resource_ready(ctx, "hook:Scene_Draw_Popup_SC_CharacterInfo",
                              reinterpret_cast<void*>(&fake_scene_draw)) == 0);

    g_push_ok = false;
    i18n::scene_draw_charinfo_wrapper();
    CHECK(g_scene_draw_called);
    CHECK(g_push_count == 1);
    CHECK(g_pop_count == 0);
    CHECK(i18n::active_scope_value() == qol::contracts::kTextScopeNone);
    i18n::bind_sdk(nullptr);
}

// 无 SDK 时 on_enabled 注册失败，fill_status 如实报告（不再恒报 enabled）。
void test_registration_failure_observable() {
    i18n::bind_sdk(nullptr);
    qol::contracts::PluginContext ctx{};
    const qol::contracts::PluginDescriptor& d = i18n::plugin_descriptor();
    d.on_enabled(ctx);
    qol::contracts::PluginStatusOut out{};
    out.enabled = true;
    out.error = nullptr;
    d.fill_status(out);
    CHECK(!out.enabled);
    CHECK(out.error != nullptr);
}

}  // namespace

int main() {
    test_manifest();
    test_table();
    test_owner_table_modify_guard();
    test_backup_delivery_and_scope();
    test_push_failure_passthrough();
    test_registration_failure_observable();
    std::printf("character_panel_i18n: pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
