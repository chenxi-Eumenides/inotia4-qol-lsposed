#pragma once

#include <cstdint>

// 文本替换作用域（纯基础设施，无游戏/机制依赖）。
//
// 文本层（plugins/text_layer）只在「当前线程正处于某窗口」时替换文本；窗口标记由绘制侧
// 功能在本层构造守卫完成。放在 infra 的原因：作用域是跨 feature 的共享线程上下文，
// features 之间不得互相 include（矩阵 §2.2），故下沉到 L2 基础设施。

namespace qol::infra {

// 文本作用域：替换只在「当前线程正处于该窗口」时生效。
enum class TextScope : uint8_t {
    kNone = 0,
    kCharacterPanel,     // 角色属性面板绘制
    kUimixRecipeButton,  // UIMix 配方按钮绘制
    kUimixPanelTitle,    // UIMix 面板标题绘制
    kUimixPageTab,       // UIMix 页签按钮绘制（显式不替换：与配方按钮同 id）
};

inline const char* text_scope_token(TextScope scope) {
    switch (scope) {
        case TextScope::kNone:
            return "none";
        case TextScope::kCharacterPanel:
            return "character_panel";
        case TextScope::kUimixRecipeButton:
            return "uimix_recipe_button";
        case TextScope::kUimixPanelTitle:
            return "uimix_panel_title";
        case TextScope::kUimixPageTab:
            return "uimix_page_tab";
    }
    return "unknown";
}

// 当前线程作用域。绘制与取文本同线程同步进行，故用 thread_local：无跨线程可见性需求，
// 也可避免 HTTP/预取线程恰好与绘制重叠时被误替换。
inline thread_local TextScope t_active_text_scope = TextScope::kNone;

inline TextScope active_text_scope() { return t_active_text_scope; }

// 作用域守卫：窗口入口构造、出口析构，恢复上一层作用域（保存/恢复而非置空，
// 对「原函数内部再次进入同一 wrapper」天然可重入）。
class TextScopeGuard {
public:
    explicit TextScopeGuard(TextScope scope) : previous_(t_active_text_scope) {
        t_active_text_scope = scope;
    }
    ~TextScopeGuard() { t_active_text_scope = previous_; }
    TextScopeGuard(const TextScopeGuard&) = delete;
    TextScopeGuard& operator=(const TextScopeGuard&) = delete;

private:
    TextScope previous_;
};

}  // namespace qol::infra
