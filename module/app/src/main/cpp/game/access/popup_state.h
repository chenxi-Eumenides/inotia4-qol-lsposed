#pragma once

#include <cstdint>

// 游戏层：PopupState state 表条目访问（ARCHITECTURE.md §5 版本耦合唯一收口）。
// 27 条 × 0x40 的表基址、字段偏移（id@+0、enter@+0x10、process@+0x18、f3@+0x28、
// f4@+0x30、event@+0x38）与 GOT 槽常量全部取自 game_symbols.h；机制层
// （hooks/popup_state）只经本接口读写，不出现裸偏移/符号头。

namespace qol::game {

struct PopupStateSlot {
    uint8_t* entry = nullptr;  // state 表条目地址
    int32_t state_id = -1;     // 条目 +0 的 id
};

// 按 enter VMA（g_base 偏移；与 game_symbols.h 中 F_PANEL_*_ENTER 同口径）定位条目。
// 基址未就绪 / GOT 空 / 表不可读 / 未命中 → false。
bool popup_state_locate(uintptr_t enter_vma, PopupStateSlot* out);

// 快照条目（POPUP_ENTRY_SIZE 字节）到 out。
void popup_state_read(const PopupStateSlot& slot, uint8_t* out);

// 写入 5 个回调（绝对地址；与旧注入一致，逐槽无条件写入）。
void popup_state_write_hooks(const PopupStateSlot& slot, const void* enter, const void* process,
                             const void* f3, const void* f4, const void* event);

// 以快照整段写回条目。
void popup_state_write_back(const PopupStateSlot& slot, const uint8_t* backup);

// 从快照取 enter 的原版 VMA（原 enter - g_base）；无效返回 0。供别名登记。
uintptr_t popup_state_backup_enter_vma(const uint8_t* backup);

}  // namespace qol::game
