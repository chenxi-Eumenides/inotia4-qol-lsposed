// host 测试桩：提供被测源文件（game_nav.cpp）引用的 game_access/game_state 符号。
// 纯算法测试不依赖真实游戏内存：g_base=0 使 nav_unit_blocks 退化为空，
// nav_tiles 走静态瓦片（set_static_tiles 注入）。

#include <cstdint>

#include "game_access.h"
#include "game_state.h"

uintptr_t g_base = 0;

// game/access/patches.cpp 的 kBlRedirectToSymbol 站点经 fn_resolve 解析目标；纯算法测试
// 不解析真实符号，恒等返回 vma。
uintptr_t fn_resolve(const char*, uintptr_t vma) { return vma; }

uint32_t current_map_id() { return 0; }

void* lead_member() { return nullptr; }

CharGetAreaRectFn fn_char_get_area_rect = nullptr;

namespace {
bool g_host_stack_limit_enabled = false;
}

bool stack_limit_enabled() { return g_host_stack_limit_enabled; }

void set_host_stack_limit_enabled(bool enabled) {
    g_host_stack_limit_enabled = enabled;
}
