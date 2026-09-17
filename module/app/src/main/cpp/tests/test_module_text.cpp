#include "features/text_layer/feature.h"

#include "feature/custom_recipe/custom_recipe_catalog.h"
#include "infra/text_scope.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond) do { \
    if (cond) ++g_pass; \
    else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

using qol::features::text_layer::Entry;
using qol::infra::TextScope;
namespace tl = qol::features::text_layer;

// ---------------------------------------------------------------------------
// 表完整性：键唯一、角色面板标签为二至四字纯中文（UTF-8 → 3 字节/字，故长度 ∈ {6,9,12}）。
// ---------------------------------------------------------------------------
static void test_table_integrity() {
    size_t n = 0;
    const Entry* e = tl::entries(&n);
    CHECK(e != nullptr);
    CHECK(n == 14);
    for (size_t i = 0; i < n; ++i) {
        CHECK(e[i].key != nullptr && e[i].key[0] != '\0');
        for (size_t j = i + 1; j < n; ++j) CHECK(std::strcmp(e[i].key, e[j].key) != 0);
    }
    for (size_t i = 0; i < n; ++i) {
        if (e[i].text == nullptr) continue;
        const size_t len = std::strlen(e[i].text);
        CHECK(len % 3 == 0);           // 纯中文 UTF-8：每字 3 字节
        CHECK(len >= 6 && len <= 12);  // 二至四字（上限四字）
        for (size_t k = 0; k < len; ++k) {
            CHECK(static_cast<unsigned char>(e[i].text[k]) >= 0x80);  // 无 ASCII 混入
        }
    }
}

// ---------------------------------------------------------------------------
// 角色面板段：位于表首、连续升序覆盖 35185..35196、无空洞（与 .h 的 static_assert 同口径）。
// ---------------------------------------------------------------------------
static void test_charinfo_block_contiguous() {
    size_t n = 0;
    const Entry* e = tl::entries(&n);
    for (size_t i = 0; i <= tl::kCharinfoIdMax - tl::kCharinfoIdMin; ++i) {
        CHECK(e[i].scope == TextScope::kCharacterPanel);
        CHECK(e[i].text_id == static_cast<uint16_t>(tl::kCharinfoIdMin + i));
    }
    CHECK(n > tl::kCharinfoIdCount);
    CHECK(e[tl::kCharinfoIdCount].scope != TextScope::kCharacterPanel);
}

// ---------------------------------------------------------------------------
// 查表矩阵：**作用域门控是核心语义** —— 同一个 id（35291）在配方按钮窗口内被替换，
// 在页签窗口（未登记作用域）内保持原文；35291 也是页签名，故这条断言是「不误改页签」的护栏。
// ---------------------------------------------------------------------------
static void test_lookup_scope_gating() {
    // 角色面板段：仅 kCharacterPanel 下命中。
    CHECK(std::strcmp(tl::lookup(35185, TextScope::kCharacterPanel), "物攻") == 0);
    CHECK(std::strcmp(tl::lookup(35186, TextScope::kCharacterPanel), "法攻") == 0);
    CHECK(std::strcmp(tl::lookup(35187, TextScope::kCharacterPanel), "防御力") == 0);
    CHECK(std::strcmp(tl::lookup(35189, TextScope::kCharacterPanel), "暴击率") == 0);
    CHECK(std::strcmp(tl::lookup(35190, TextScope::kCharacterPanel), "命中率") == 0);
    CHECK(std::strcmp(tl::lookup(35191, TextScope::kCharacterPanel), "爆伤") == 0);
    CHECK(std::strcmp(tl::lookup(35192, TextScope::kCharacterPanel), "物抗") == 0);
    CHECK(std::strcmp(tl::lookup(35193, TextScope::kCharacterPanel), "法抗") == 0);
    CHECK(std::strcmp(tl::lookup(35194, TextScope::kCharacterPanel), "闪避率") == 0);
    CHECK(std::strcmp(tl::lookup(35195, TextScope::kCharacterPanel), "武器格挡") == 0);
    CHECK(std::strcmp(tl::lookup(35196, TextScope::kCharacterPanel), "盾牌格挡") == 0);
    // 35188 显式登记为保持原文；35181..35184（LV/EXP/HP/MP）保持原文且不在表内。
    CHECK(tl::lookup(35188, TextScope::kCharacterPanel) == nullptr);
    CHECK(tl::lookup(35184, TextScope::kCharacterPanel) == nullptr);
    CHECK(tl::lookup(35197, TextScope::kCharacterPanel) == nullptr);
    // 作用域不匹配 → 不替换。
    CHECK(tl::lookup(35185, TextScope::kUimixRecipeButton) == nullptr);
    CHECK(tl::lookup(35185, TextScope::kNone) == nullptr);

    // 配方文案：按钮与面板标题都要显示「宝石升阶」；**页签必须保持原文**（三处同 id）。
    CHECK(std::strcmp(tl::lookup(35291, TextScope::kUimixRecipeButton), "宝石升阶") == 0);
    CHECK(std::strcmp(tl::lookup(35291, TextScope::kUimixPanelTitle), "宝石升阶") == 0);
    CHECK(tl::lookup(35291, TextScope::kUimixPageTab) == nullptr);  // 页签名不得被改名
    CHECK(tl::lookup(35291, TextScope::kCharacterPanel) == nullptr);
    CHECK(tl::lookup(35291, TextScope::kNone) == nullptr);
    CHECK(tl::lookup(0, TextScope::kUimixRecipeButton) == nullptr);
    CHECK(tl::lookup(0, TextScope::kUimixPanelTitle) == nullptr);
}

// ---------------------------------------------------------------------------
// 语义键查询 + 与配方目录的 id 同步（两处常量不得漂移）；scope token 表。
// ---------------------------------------------------------------------------
static void test_key_lookup_and_id_sync() {
    const Entry* e = tl::entry_by_key("recipe.jewel_tier_up");
    CHECK(e != nullptr);
    if (e != nullptr) {
        CHECK(e->scope == TextScope::kUimixRecipeButton);
        CHECK(std::strcmp(e->text, "宝石升阶") == 0);
        CHECK(e->text_id == custom_recipe::kJewelTierUpLabelWordId);
    }
    // 标题条目：同 id、不同作用域（面板标题框）。
    const Entry* t = tl::entry_by_key("recipe.jewel_tier_up.title");
    CHECK(t != nullptr);
    if (t != nullptr) {
        CHECK(t->scope == TextScope::kUimixPanelTitle);
        CHECK(std::strcmp(t->text, "宝石升阶") == 0);
        CHECK(t->text_id == custom_recipe::kJewelTierUpLabelWordId);
    }
    // 页签作用域只用于「显式不替换」，表中不得有任何该作用域条目。
    size_t n = 0;
    const Entry* all = tl::entries(&n);
    for (size_t i = 0; i < n; ++i) CHECK(all[i].scope != TextScope::kUimixPageTab);
    CHECK(tl::entry_by_key("nope") == nullptr);
    CHECK(tl::entry_by_key(nullptr) == nullptr);
    CHECK(std::strcmp(qol::infra::text_scope_token(TextScope::kCharacterPanel),
                      "character_panel") == 0);
    CHECK(std::strcmp(qol::infra::text_scope_token(TextScope::kUimixRecipeButton),
                      "uimix_recipe_button") == 0);
    CHECK(std::strcmp(qol::infra::text_scope_token(TextScope::kNone), "none") == 0);
}

// ---------------------------------------------------------------------------
// 作用域守卫：保存/恢复语义（嵌套可重入）。
// ---------------------------------------------------------------------------
static void test_scope_guard_restore() {
    CHECK(qol::infra::active_text_scope() == TextScope::kNone);
    {
        qol::infra::TextScopeGuard outer(TextScope::kCharacterPanel);
        CHECK(qol::infra::active_text_scope() == TextScope::kCharacterPanel);
        {
            qol::infra::TextScopeGuard inner(TextScope::kUimixRecipeButton);
            CHECK(qol::infra::active_text_scope() == TextScope::kUimixRecipeButton);
        }
        CHECK(qol::infra::active_text_scope() == TextScope::kCharacterPanel);
    }
    CHECK(qol::infra::active_text_scope() == TextScope::kNone);
}

// ---------------------------------------------------------------------------
// 方案 X：内置表是 after 阶段 Modify 消费者——注册参数为 after+Modify；回调体只按当前
// 作用域查表（命中返回字面量、占位/未命中返回 nullptr）。分派级顺序断言见
// tests/test_hooks_dispatch.cpp（本测试目标不链接 hooks/dispatch）。
// ---------------------------------------------------------------------------
static void test_owner_table_after_modify_body() {
    CHECK(tl::kOwnerTablePhase == qol::hooks::DispatchPhase::kAfter);
    CHECK(tl::kOwnerTableKind == qol::hooks::ConsumerKind::kModify);

    {
        qol::infra::TextScopeGuard scope(TextScope::kCharacterPanel);
        CHECK(std::strcmp(tl::owner_table_lookup(35185), "物攻") == 0);
        CHECK(tl::owner_table_lookup(35188) == nullptr);  // 显式占位（保持原文）
        CHECK(tl::owner_table_lookup(35184) == nullptr);  // 不在表内
    }
    CHECK(tl::owner_table_lookup(35185) == nullptr);  // 非窗口（kNone）
    {
        qol::infra::TextScopeGuard scope(TextScope::kUimixRecipeButton);
        CHECK(std::strcmp(tl::owner_table_lookup(35291), "宝石升阶") == 0);
    }
    {
        qol::infra::TextScopeGuard scope(TextScope::kUimixPageTab);
        CHECK(tl::owner_table_lookup(35291) == nullptr);  // 页签保持原文
    }
}

int main() {
    test_table_integrity();
    test_charinfo_block_contiguous();
    test_lookup_scope_gating();
    test_key_lookup_and_id_sync();
    test_scope_guard_restore();
    test_owner_table_after_modify_body();
    std::printf("module_text_tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
