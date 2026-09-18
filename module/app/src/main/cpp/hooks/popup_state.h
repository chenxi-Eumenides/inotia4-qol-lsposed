#pragma once

#include <cstdint>

// PopupState state 表注入机制（ARCHITECTURE.md §4.1 kOwnership、§4.3 资源描述符）。
//
// 语义：按 enter VMA 在原版 27×0x40 的 state 表中定位条目，快照 0x40 后写入模块的
// enter/process/f3/f4/event 五个回调；释放时整段写回快照。一 entry 一 owner（占用表
// 按条目地址仲裁，owner token = hooks.enter 地址），同 owner 幂等。
//
// 占用仲裁不使用 patch/claims：依赖矩阵禁止 hooks → patch，本模块自带等价的一地址
// 一所有者表（固定容量，静态数组）。
//
// 线程纪律：acquire/restore 只在游戏主线程调用（由资源事务的 install/release 路径
// 保证）；本模块不引入锁。resolve 只读，可在同一主线程安装期调用。
//
// state 表基址/字段偏移收口 game/access/popup_state（game_symbols.h 单一来源）。

namespace qol::hooks {

struct PopupStateHooks {
    void (*enter)();
    void (*process)();
    void (*f3)();
    void (*f4)();
    uint32_t (*event)(uint64_t, uint64_t, uint64_t);
};

struct PopupStateEntry {
    uint8_t backup[0x40];
    uint8_t* slot;
    int32_t state_id;
};

// 只读定位：按 enter VMA（g_base 偏移）在 state 表中查条目并快照 0x40；
// 未命中 / 不可读 / 基址未就绪 → false。
bool popup_state_resolve(uintptr_t enter_vma, PopupStateEntry* out);

// 只读定位（按符号名）：用 game::resolve_hook_entry 解析 symbol_name（如
// "F_PANEL_UNK1_ENTER"，须在 game/access/symbols 登记）→ 减 g_base 得 enter VMA →
// 调 popup_state_resolve。未解析 / 未就绪 / 未命中 → false。
// 供面板插件在 acquire 前定位 state 条目（禁止裸 VMA）。
bool popup_state_resolve_symbol(const char* symbol_name, PopupStateEntry* out);

// 占用并写入 5 个钩子（主线程）：一 entry 一 owner；同 owner（hooks.enter 相同）幂等返回 true，
// 并把首次安装前的快照回填到 e->backup（重入方持有的是已注入快照，restore 仍能整段还原）；
// 不同 owner 或容量耗尽 → false。成功后登记原版 enter VMA 别名（防 UI 判定误报 screen）。
bool popup_state_acquire(PopupStateEntry* e, const PopupStateHooks& h);

// 写回 0x40 快照（total，无失败路径；主线程）；释放占用。别名不反登记（按 wrapper 单调）。
void popup_state_restore(PopupStateEntry* e);

// 测试用：清空占用表（不触碰 state 表与别名表）。
void popup_state_reset_for_test();

}  // namespace qol::hooks
