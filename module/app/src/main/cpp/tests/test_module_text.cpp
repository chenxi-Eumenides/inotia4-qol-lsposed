#include "feature/ui/module_text.h"

#include "feature/custom_recipe/custom_recipe_catalog.h"

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

using module_text::Entry;
using module_text::Scope;

// ---------------------------------------------------------------------------
// 表完整性：键唯一、角色面板标签为二至四字纯中文（UTF-8 → 3 字节/字，故长度 ∈ {6,9,12}）。
// ---------------------------------------------------------------------------
static void test_table_integrity() {
    size_t n = 0;
    const Entry* e = module_text::entries(&n);
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
        CHECK(len >= 6 && len <= 12);  // 二至四字（用户裁决上限四字）
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
    const Entry* e = module_text::entries(&n);
    for (size_t i = 0; i <= module_text::kCharinfoIdMax - module_text::kCharinfoIdMin; ++i) {
        CHECK(e[i].scope == Scope::kCharacterPanel);
        CHECK(e[i].text_id == static_cast<uint16_t>(module_text::kCharinfoIdMin + i));
    }
    CHECK(n > module_text::kCharinfoIdCount);
    CHECK(e[module_text::kCharinfoIdCount].scope != Scope::kCharacterPanel);
}

// ---------------------------------------------------------------------------
// 查表矩阵：**作用域门控是核心语义** —— 同一个 id（35291）在配方按钮窗口内被替换，
// 在页签窗口（未登记作用域）内保持原文；35291 也是页签名，故这条断言是「不误改页签」的护栏。
// ---------------------------------------------------------------------------
static void test_lookup_scope_gating() {
    // 角色面板段：仅 kCharacterPanel 下命中。
    CHECK(std::strcmp(module_text::lookup(35185, Scope::kCharacterPanel), "物攻") == 0);
    CHECK(std::strcmp(module_text::lookup(35186, Scope::kCharacterPanel), "法攻") == 0);
    CHECK(std::strcmp(module_text::lookup(35187, Scope::kCharacterPanel), "防御力") == 0);
    CHECK(std::strcmp(module_text::lookup(35189, Scope::kCharacterPanel), "暴击率") == 0);
    CHECK(std::strcmp(module_text::lookup(35190, Scope::kCharacterPanel), "命中率") == 0);
    CHECK(std::strcmp(module_text::lookup(35191, Scope::kCharacterPanel), "爆伤") == 0);
    CHECK(std::strcmp(module_text::lookup(35192, Scope::kCharacterPanel), "物抗") == 0);
    CHECK(std::strcmp(module_text::lookup(35193, Scope::kCharacterPanel), "法抗") == 0);
    CHECK(std::strcmp(module_text::lookup(35194, Scope::kCharacterPanel), "闪避率") == 0);
    CHECK(std::strcmp(module_text::lookup(35195, Scope::kCharacterPanel), "武器格挡") == 0);
    CHECK(std::strcmp(module_text::lookup(35196, Scope::kCharacterPanel), "盾牌格挡") == 0);
    // 35188 显式登记为保持原文；35181..35184（LV/EXP/HP/MP）按用户裁决保持原文且不在表内。
    CHECK(module_text::lookup(35188, Scope::kCharacterPanel) == nullptr);
    CHECK(module_text::lookup(35184, Scope::kCharacterPanel) == nullptr);
    CHECK(module_text::lookup(35197, Scope::kCharacterPanel) == nullptr);
    // 作用域不匹配 → 不替换。
    CHECK(module_text::lookup(35185, Scope::kUimixRecipeButton) == nullptr);
    CHECK(module_text::lookup(35185, Scope::kNone) == nullptr);

    // 配方文案：按钮与面板标题都要显示「宝石升阶」；**页签必须保持原文**（三处同 id）。
    CHECK(std::strcmp(module_text::lookup(35291, Scope::kUimixRecipeButton), "宝石升阶") == 0);
    CHECK(std::strcmp(module_text::lookup(35291, Scope::kUimixPanelTitle), "宝石升阶") == 0);
    CHECK(module_text::lookup(35291, Scope::kUimixPageTab) == nullptr);  // 页签名不得被改名
    CHECK(module_text::lookup(35291, Scope::kCharacterPanel) == nullptr);
    CHECK(module_text::lookup(35291, Scope::kNone) == nullptr);
    CHECK(module_text::lookup(0, Scope::kUimixRecipeButton) == nullptr);
    CHECK(module_text::lookup(0, Scope::kUimixPanelTitle) == nullptr);
}

// ---------------------------------------------------------------------------
// 语义键查询 + 与配方目录的 id 同步（两处常量不得漂移）；scope token 表。
// ---------------------------------------------------------------------------
static void test_key_lookup_and_id_sync() {
    const Entry* e = module_text::entry_by_key("recipe.jewel_tier_up");
    CHECK(e != nullptr);
    if (e != nullptr) {
        CHECK(e->scope == Scope::kUimixRecipeButton);
        CHECK(std::strcmp(e->text, "宝石升阶") == 0);
        CHECK(e->text_id == custom_recipe::kJewelTierUpLabelWordId);
    }
    // 标题条目：同 id、不同作用域（面板标题框）。
    const Entry* t = module_text::entry_by_key("recipe.jewel_tier_up.title");
    CHECK(t != nullptr);
    if (t != nullptr) {
        CHECK(t->scope == Scope::kUimixPanelTitle);
        CHECK(std::strcmp(t->text, "宝石升阶") == 0);
        CHECK(t->text_id == custom_recipe::kJewelTierUpLabelWordId);
    }
    // 页签作用域只用于「显式不替换」，表中不得有任何该作用域条目。
    size_t n = 0;
    const Entry* all = module_text::entries(&n);
    for (size_t i = 0; i < n; ++i) CHECK(all[i].scope != Scope::kUimixPageTab);
    CHECK(module_text::entry_by_key("nope") == nullptr);
    CHECK(module_text::entry_by_key(nullptr) == nullptr);
    CHECK(std::strcmp(module_text::scope_token(Scope::kCharacterPanel), "character_panel") == 0);
    CHECK(std::strcmp(module_text::scope_token(Scope::kUimixRecipeButton),
                      "uimix_recipe_button") == 0);
    CHECK(std::strcmp(module_text::scope_token(Scope::kNone), "none") == 0);
}

int main() {
    test_table_integrity();
    test_charinfo_block_contiguous();
    test_lookup_scope_gating();
    test_key_lookup_and_id_sync();
    std::printf("module_text_tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
