#include "plugins/extension_bag/runtime/entry_guard.h"

#include "plugins/extension_bag/runtime/state.h"

#include <cstdarg>
#include <cstdio>

namespace qol::extension_bag {
namespace {

// 单线程模型下深度即可表达重入；thread_local 保证即使过渡期有 off-main 调用
// 也不会误判主线程的重入状态。
thread_local int g_entry_depth = 0;
EntryGuardStats g_stats{};
bool g_thread_api_missing_logged = false;

// 直接走 host sdk 写日志，避免 entry_guard 依赖插件级日志符号（host 测试
// 目标只链接本文件时也能成立）。
void entry_log(int32_t level, const char* format, ...) {
    const auto* sdk = runtime_state().host.sdk;
    if (sdk == nullptr || sdk->log.write == nullptr || format == nullptr) return;
    char message[256]{};
    va_list args;
    va_start(args, format);
    const int written = std::vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    if (written <= 0) return;
    const uint32_t length = static_cast<uint32_t>(written) >= sizeof(message)
                                ? static_cast<uint32_t>(sizeof(message) - 1)
                                : static_cast<uint32_t>(written);
    (void)sdk->log.write(level, 6, "extension_bag", message, length);
}

}  // namespace

bool extension_bag_on_main_thread() {
    const auto* game = runtime_state().host.game;
    if (game == nullptr || game->thread.is_main_thread == nullptr) {
        if (!g_thread_api_missing_logged) {
            g_thread_api_missing_logged = true;
            entry_log(2, "entry thread api unavailable; main-thread assert disabled");
        }
        return true;
    }
    return game->thread.is_main_thread() != 0;
}

bool extension_bag_entry_active() { return g_entry_depth > 0; }

EntryGuard::EntryGuard(const char* op, ReentryPolicy policy) : op_(op), policy_(policy) {
    if (!extension_bag_on_main_thread()) {
        ++g_stats.main_thread_violations;
        entry_log(3, "entry off-main thread op=%s policy=%d",
                  op_ != nullptr ? op_ : "?", static_cast<int>(policy_));
        return;
    }
    if (g_entry_depth > 0) {
        if (policy_ == ReentryPolicy::kReadSnapshot) {
            ++g_stats.read_reentries;
        } else {
            ++g_stats.write_reentries;
        }
        entry_log(2, "entry reentry op=%s policy=%d",
                  op_ != nullptr ? op_ : "?", static_cast<int>(policy_));
        return;
    }
    g_entry_depth = 1;
    entered_ = true;
}

EntryGuard::~EntryGuard() {
    if (entered_) g_entry_depth = 0;
}

EntryGuardStats extension_bag_entry_stats() { return g_stats; }

}  // namespace qol::extension_bag
