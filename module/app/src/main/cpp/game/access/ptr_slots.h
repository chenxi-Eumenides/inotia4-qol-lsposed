#pragma once

#include <cstdint>

// 游戏层：指针槽站点登记（ARCHITECTURE.md §4.1 指针槽、§4.3 资源描述符）。
// 与 game/access/patches.h 的 patch_site 对称：把「登记名 → 运行期槽地址 + 安装前
// 期望值」交给 patch 层，使指针槽机制的输入不含裸地址，游戏侧 VMA/偏移保持
// game_symbols.h 单一来源。机制实现不得在本层出现。

namespace qol::game {

struct PtrSlotSite {
    uintptr_t slot_addr = 0;       // 运行期函数指针槽地址（如 GOT 条目）
    void* expected_value = nullptr;  // 安装前该槽应存的原函数入口
};

// 按登记名解析指针槽站点；未登记 / 基址未就绪 / 符号未解析 / 槽不可读 → false。
bool ptr_slot_site(const char* name, PtrSlotSite* out);

}  // namespace qol::game
