#include "game_ui_charinfo_zh.h"

#include "core/native/qol_log.h"
#include "feature/patch/native_inventory_hook.h"
#include "game_access.h"
#include "game_symbols.h"

#include <atomic>
#include <cstdint>

namespace {

// LSPosed native hook 写入的原始函数指针 backup。
SceneDrawCharinfoFn g_backup_scene_draw_charinfo = nullptr;
MemorytextGetTextFn g_backup_memorytext_get_text = nullptr;

std::atomic<bool> g_installed{false};
std::atomic<bool> g_attempted{false};

// 角色面板绘制只发生在游戏主线程的绘制阶段；用 thread_local 标记「当前线程正在执行
// Scene_Draw_POPUP_SC_CHARACTER_INFO」，MEMORYTEXT_GetText wrapper 据此判定是否替换。
// 不用全局 atomic：绘制与取文本同线程同步进行，thread_local 无跨线程可见性需求，
// 且能避免其它线程（HTTP/预取）恰好与绘制重叠时被误替换。
thread_local bool t_charinfo_draw_active = false;

// 面板每帧绘制入口：进出各写一次 thread_local，包裹原函数执行窗口。
// 保存/恢复旧值而非直接置 false，防御性覆盖原函数内部再次进入本 wrapper 的情形。
void scene_draw_charinfo_wrapper() {
    const bool previous = t_charinfo_draw_active;
    t_charinfo_draw_active = true;
    if (g_backup_scene_draw_charinfo != nullptr) g_backup_scene_draw_charinfo();
    t_charinfo_draw_active = previous;
}

// 全游戏热点取文本函数：先无条件调原函数（保持原版行为与任何内部副作用），
// 仅当本线程处于角色面板绘制窗口且 id 命中白名单时改返回中文静态字面量。
const char* memorytext_get_text_wrapper(uint16_t text_id) {
    const char* original =
        (g_backup_memorytext_get_text != nullptr) ? g_backup_memorytext_get_text(text_id) : nullptr;
    if (!t_charinfo_draw_active) return original;
    const char* zh = charinfo_zh::lookup(text_id);
    return (zh != nullptr) ? zh : original;
}

bool install_one(NativeHookFunType hook, uintptr_t target, void* replacement, void** backup,
                 const char* name) {
    const int rc = hook(reinterpret_cast<void*>(target), replacement, backup);
    if (rc != 0 || backup == nullptr || *backup == nullptr) {
        QOL_LOG_ERROR(QolDomain::kUi, "charinfo zh %s hook failed rc=%d", name, rc);
        return false;
    }
    return true;
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
    const uintptr_t get_text =
        g_base + fn_resolve("F_MEMORYTEXT_GET_TEXT_VMA", F_MEMORYTEXT_GET_TEXT_VMA);

    if (!install_one(hook, scene_draw, reinterpret_cast<void*>(&scene_draw_charinfo_wrapper),
                     reinterpret_cast<void**>(&g_backup_scene_draw_charinfo),
                     "Scene_Draw_POPUP_SC_CHARACTER_INFO") ||
        !install_one(hook, get_text, reinterpret_cast<void*>(&memorytext_get_text_wrapper),
                     reinterpret_cast<void**>(&g_backup_memorytext_get_text),
                     "MEMORYTEXT_GetText")) {
        return;
    }

    g_installed.store(true, std::memory_order_release);
    QOL_LOG_INFO(QolDomain::kUi, "charinfo zh label hooks installed");
}
