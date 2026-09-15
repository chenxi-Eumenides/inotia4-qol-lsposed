#include "game_ui_charinfo_zh.h"

#include "feature/ui/module_text.h"

#include "core/native/qol_log.h"
#include "feature/patch/native_inventory_hook.h"
#include "game_access.h"
#include "game_symbols.h"

#include <atomic>

namespace {

// LSPosed native hook 写入的原始函数指针 backup。
SceneDrawCharinfoFn g_backup_scene_draw_charinfo = nullptr;

std::atomic<bool> g_installed{false};
std::atomic<bool> g_attempted{false};

// 面板每帧绘制入口：进出各写一次作用域（守卫析构时恢复上一层），把整个绘制窗口标记为
// kCharacterPanel。文案与 id 表由 module_text 持有 —— 这里只负责「画的是哪个窗口」。
void scene_draw_charinfo_wrapper() {
    module_text::TextScopeGuard scope(module_text::Scope::kCharacterPanel);
    if (g_backup_scene_draw_charinfo != nullptr) g_backup_scene_draw_charinfo();
}

}  // namespace

void charinfo_zh_install_if_ready() {
    if (g_installed.load(std::memory_order_acquire)) return;
    if (!bridge_ready()) return;
    NativeHookFunType hook = native_hook_func();
    if (hook == nullptr) return;

    bool expected = false;
    if (!g_attempted.compare_exchange_strong(expected, true)) return;

    const uintptr_t scene_draw = g_base +
        fn_resolve("F_SCENE_DRAW_POPUP_SC_CHARACTER_INFO_VMA",
                   F_SCENE_DRAW_POPUP_SC_CHARACTER_INFO_VMA);
    const int rc = hook(reinterpret_cast<void*>(scene_draw),
                        reinterpret_cast<void*>(&scene_draw_charinfo_wrapper),
                        reinterpret_cast<void**>(&g_backup_scene_draw_charinfo));
    if (rc != 0 || g_backup_scene_draw_charinfo == nullptr) {
        QOL_LOG_ERROR(QolDomain::kUi, "charinfo zh panel scope hook failed rc=%d", rc);
        return;
    }
    g_installed.store(true, std::memory_order_release);
    QOL_LOG_INFO(QolDomain::kUi, "charinfo zh panel scope hook installed (texts in module_text)");
}
