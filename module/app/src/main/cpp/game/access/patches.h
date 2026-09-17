#pragma once

#include <cstdint>

// 游戏层：指令 patch 点登记（ARCHITECTURE.md §4.3）。机制实现不得在本层出现；
// 本层只把「登记名 → 运行期地址 + 原指令字 + 替换字」交给 patch 层，
// 使 VMA/偏移常量保持 game_symbols.h 单一来源。

namespace qol::game {

enum class PatchKind : uint8_t { kBlRedirect = 0, kWordReplace = 1 };

struct PatchSite {
    uintptr_t addr = 0;              // 运行期地址（g_base + VMA + 偏移）
    uint32_t expected_word = 0;      // 原指令字（安装前校验）
    uint32_t replacement_word = 0;   // kWordReplace 的替换字；kBlRedirect 为 0（由 handler 生成）
    PatchKind kind = PatchKind::kBlRedirect;
};

// 按登记名解析 patch 点；未登记或 g_base==0 → false。
bool patch_site(const char* name, PatchSite* out);

}  // namespace qol::game
