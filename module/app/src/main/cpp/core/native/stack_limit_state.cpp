#include "core/native/stack_limit_port.h"

#include <atomic>

// stack_limit 有效态唯一持有者（原 feature/patch/game_patch.cpp 的 g_stack_enabled）。
// 只读签名 stack_limit_enabled() 冻结；写入只允许 plugins/stack_limit 插件经
// stack_limit_set_effective 调用。指令 clamp 的物理安装/还原由宿主声明式资源完成，
// 本文件不接触游戏内存。

namespace {
std::atomic<bool> g_stack_limit_effective{false};
}  // namespace

bool stack_limit_enabled() {
    return g_stack_limit_effective.load(std::memory_order_acquire);
}

void stack_limit_set_effective(bool enabled) {
    g_stack_limit_effective.store(enabled, std::memory_order_release);
}
