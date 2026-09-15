#include "module_text.h"

#include "core/native/qol_log.h"
#include "feature/patch/native_inventory_hook.h"
#include "game_access.h"
#include "game_symbols.h"

#include <atomic>

namespace {

// LSPosed native hook 写入的原始函数指针 backup。
MemorytextGetTextFn g_backup_memorytext_get_text = nullptr;

std::atomic<bool> g_installed{false};
std::atomic<bool> g_attempted{false};

// 当前线程所在的文本作用域。绘制与取文本同线程同步进行，故用 thread_local 而非原子量：
// 既无跨线程可见性需求，又能避免 HTTP/预取线程恰好与绘制重叠时被误替换。
thread_local module_text::Scope t_active_scope = module_text::Scope::kNone;

// 全游戏热点取文本函数：先无条件调原函数（保持原版行为与任何内部副作用），
// 仅当本线程处于某个登记窗口且 (text_id, scope) 命中内置表时改返回模块字面量。
const char* memorytext_get_text_wrapper(uint16_t text_id) {
    const char* original =
        (g_backup_memorytext_get_text != nullptr) ? g_backup_memorytext_get_text(text_id) : nullptr;
    const char* overridden = module_text::lookup(text_id, t_active_scope);
    return (overridden != nullptr) ? overridden : original;
}

}  // namespace

namespace module_text {

TextScopeGuard::TextScopeGuard(Scope scope) : previous_(t_active_scope) { t_active_scope = scope; }

TextScopeGuard::~TextScopeGuard() { t_active_scope = previous_; }

void module_text_install_if_ready() {
    if (g_installed.load(std::memory_order_acquire)) return;
    if (!bridge_ready()) return;
    NativeHookFunType hook = native_hook_func();
    if (hook == nullptr) return;

    bool expected = false;
    if (!g_attempted.compare_exchange_strong(expected, true)) return;

    const uintptr_t get_text =
        g_base + fn_resolve("F_MEMORYTEXT_GET_TEXT_VMA", F_MEMORYTEXT_GET_TEXT_VMA);
    const int rc = hook(reinterpret_cast<void*>(get_text),
                        reinterpret_cast<void*>(&memorytext_get_text_wrapper),
                        reinterpret_cast<void**>(&g_backup_memorytext_get_text));
    if (rc != 0 || g_backup_memorytext_get_text == nullptr) {
        QOL_LOG_ERROR(QolDomain::kUi, "module text MEMORYTEXT_GetText hook failed rc=%d", rc);
        return;
    }
    g_installed.store(true, std::memory_order_release);
    QOL_LOG_INFO(QolDomain::kUi, "module text hook installed entries=%zu", kEntryCount);
}

bool module_text_installed() { return g_installed.load(std::memory_order_acquire); }

}  // namespace module_text
