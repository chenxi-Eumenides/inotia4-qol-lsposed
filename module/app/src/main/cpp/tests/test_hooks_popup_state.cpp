// hooks/popup_state.h + game/access/popup_state.h host 单测：
// 覆盖 state 表条目定位（命中/未命中/不可读/基址未就绪）、占用写入 5 槽 + 0x40 快照 +
// enter 别名登记、一 entry 一 owner、同 owner 幂等、restore 逐字节还原与释放后重新 acquire。
//
// 测试注入：不改生产路径。把 g_base 设为「假 GOT 变量地址 − G_POPUP_STATE_LIST_GOT_VMA」，
// 使 *(g_base + G_POPUP_STATE_LIST_GOT_VMA) 落在测试可控的假 GOT；enter_vma 用
// 「假 enter 函数绝对地址 − g_base」构造，与生产口径一致。

#include "hooks/popup_state.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "core/native/popup_enter_alias.h"
#include "game/symbols/game_symbols.h"
#include "game_access.h"

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond) do { \
    if (cond) ++g_pass; \
    else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

uintptr_t g_base = 0;

static bool g_memory_accessible = true;
bool game_memory_accessible(const void*, size_t, char) { return g_memory_accessible; }

// 假符号表：popup_state_resolve_symbol 经 qol::game::resolve_hook_entry 解析；
// 本 target 不编译 game/access/symbols.cpp，用可控桩（0 = 未解析）。
namespace qol::game {
uintptr_t g_test_resolve_entry = 0;
uintptr_t resolve_hook_entry(const char*, bool* out_followed) {
    if (out_followed != nullptr) *out_followed = false;
    return g_test_resolve_entry;
}
}  // namespace qol::game

namespace {

using qol::hooks::PopupStateEntry;
using qol::hooks::PopupStateHooks;
using qol::hooks::popup_state_acquire;
using qol::hooks::popup_state_reset_for_test;
using qol::hooks::popup_state_resolve;
using qol::hooks::popup_state_resolve_symbol;
using qol::hooks::popup_state_restore;

void fake_enter() {}
void fake_process() {}
void fake_f3() {}
void fake_f4() {}
uint32_t fake_event(uint64_t, uint64_t, uint64_t) { return 0; }

// 假 state 表：27 条 × 0x40；g_fake_got 扮演 *(g_base + G_POPUP_STATE_LIST_GOT_VMA)。
alignas(16) uint8_t g_table[POPUP_STATE_COUNT * POPUP_ENTRY_SIZE];
void* g_fake_got = nullptr;

void arm_fake_table() {
    popup_enter_alias_clear();
    qol::hooks::popup_state_reset_for_test();
    g_memory_accessible = true;
    g_base = reinterpret_cast<uintptr_t>(&g_fake_got) - G_POPUP_STATE_LIST_GOT_VMA;
    g_fake_got = g_table;
}

// 全表确定性填充，便于 restore 逐字节比对；再在指定条目写 id/enter。
uint8_t* put_entry(size_t index, int32_t id, void (*enter)()) {
    for (size_t i = 0; i < sizeof(g_table); ++i) {
        g_table[i] = static_cast<uint8_t>(0x40 + (i & 0x3f));
    }
    uint8_t* entry = g_table + index * POPUP_ENTRY_SIZE;
    *reinterpret_cast<int32_t*>(entry) = id;
    *reinterpret_cast<uintptr_t*>(entry + POPUP_ENTRY_ENTER) =
        reinterpret_cast<uintptr_t>(enter);
    return entry;
}

uintptr_t vma_of(void (*fn)()) {
    return reinterpret_cast<uintptr_t>(fn) - g_base;
}

PopupStateHooks hooks_of(void (*enter)()) {
    return PopupStateHooks{enter, &fake_process, &fake_f3, &fake_f4, &fake_event};
}

bool entry_field_eq(uint8_t* entry, size_t offset, uintptr_t expected) {
    return *reinterpret_cast<uintptr_t*>(entry + offset) == expected;
}

void test_resolve_hit_and_snapshot() {
    arm_fake_table();
    uint8_t* entry = put_entry(3, 7, &fake_enter);
    PopupStateEntry out{};
    CHECK(popup_state_resolve(vma_of(&fake_enter), &out));
    CHECK(out.slot == entry);
    CHECK(out.state_id == 7);
    CHECK(std::memcmp(out.backup, entry, POPUP_ENTRY_SIZE) == 0);
}

void test_resolve_miss_and_invalid() {
    arm_fake_table();
    put_entry(0, 1, &fake_enter);
    PopupStateEntry out{};
    CHECK(!popup_state_resolve(vma_of(&fake_process), &out));  // 未命中
    CHECK(!popup_state_resolve(0, &out));
    CHECK(!popup_state_resolve(vma_of(&fake_enter), nullptr));
}

void test_resolve_not_readable_or_not_ready() {
    arm_fake_table();
    put_entry(0, 1, &fake_enter);
    PopupStateEntry out{};
    g_memory_accessible = false;
    CHECK(!popup_state_resolve(vma_of(&fake_enter), &out));
    g_memory_accessible = true;

    g_fake_got = nullptr;  // GOT 空
    CHECK(!popup_state_resolve(vma_of(&fake_enter), &out));
    g_fake_got = g_table;

    const uintptr_t saved = g_base;
    g_base = 0;  // 基址未就绪
    CHECK(!popup_state_resolve(vma_of(&fake_enter), &out));
    g_base = saved;
}

void test_acquire_writes_hooks_and_alias() {
    arm_fake_table();
    uint8_t* entry = put_entry(5, 11, &fake_enter);
    PopupStateEntry out{};
    CHECK(popup_state_resolve(vma_of(&fake_enter), &out));

    const PopupStateHooks h = hooks_of(&fake_enter);
    CHECK(popup_state_acquire(&out, h));
    CHECK(entry_field_eq(entry, POPUP_ENTRY_ENTER, reinterpret_cast<uintptr_t>(&fake_enter)));
    CHECK(entry_field_eq(entry, POPUP_ENTRY_PROCESS, reinterpret_cast<uintptr_t>(&fake_process)));
    CHECK(entry_field_eq(entry, POPUP_ENTRY_F3, reinterpret_cast<uintptr_t>(&fake_f3)));
    CHECK(entry_field_eq(entry, POPUP_ENTRY_F4, reinterpret_cast<uintptr_t>(&fake_f4)));
    CHECK(entry_field_eq(entry, POPUP_ENTRY_EVENT, reinterpret_cast<uintptr_t>(&fake_event)));
    // 原版 enter VMA 别名已登记（UI 判定层还原用）。
    CHECK(popup_enter_alias_resolve(reinterpret_cast<uintptr_t>(&fake_enter)) ==
          vma_of(&fake_enter));
}

// 部分 wrap：仅 enter+event 非空 → 其余 3 槽保留原值；backup 仍为注入前全量；restore 全量还原。
void test_acquire_partial_wrap_keeps_original_slots() {
    arm_fake_table();
    uint8_t* entry = put_entry(6, 13, &fake_enter);
    uint8_t before[POPUP_ENTRY_SIZE];
    std::memcpy(before, entry, POPUP_ENTRY_SIZE);

    PopupStateEntry out{};
    CHECK(popup_state_resolve(vma_of(&fake_enter), &out));
    CHECK(std::memcmp(out.backup, before, POPUP_ENTRY_SIZE) == 0);  // 快照仍为全量

    PopupStateHooks h = hooks_of(&fake_enter);
    h.process = nullptr;
    h.f3 = nullptr;
    h.f4 = nullptr;
    CHECK(popup_state_acquire(&out, h));

    // 非空槽被替换。
    CHECK(entry_field_eq(entry, POPUP_ENTRY_ENTER, reinterpret_cast<uintptr_t>(&fake_enter)));
    CHECK(entry_field_eq(entry, POPUP_ENTRY_EVENT, reinterpret_cast<uintptr_t>(&fake_event)));
    // 空指针槽保留原值。
    CHECK(entry_field_eq(entry, POPUP_ENTRY_PROCESS,
                         *reinterpret_cast<uintptr_t*>(before + POPUP_ENTRY_PROCESS)));
    CHECK(entry_field_eq(entry, POPUP_ENTRY_F3,
                         *reinterpret_cast<uintptr_t*>(before + POPUP_ENTRY_F3)));
    CHECK(entry_field_eq(entry, POPUP_ENTRY_F4,
                         *reinterpret_cast<uintptr_t*>(before + POPUP_ENTRY_F4)));

    // restore 整段写回注入前快照。
    popup_state_restore(&out);
    CHECK(std::memcmp(entry, before, POPUP_ENTRY_SIZE) == 0);
}

void test_one_entry_one_owner() {
    arm_fake_table();
    uint8_t* entry = put_entry(0, 1, &fake_enter);
    PopupStateEntry first{};
    CHECK(popup_state_resolve(vma_of(&fake_enter), &first));
    const PopupStateHooks h1 = hooks_of(&fake_enter);
    CHECK(popup_state_acquire(&first, h1));

    // 第二个 owner（不同 wrapper enter）申请同一条目 → 拒绝，且不改写已装槽。
    PopupStateEntry second{};
    CHECK(popup_state_resolve(vma_of(&fake_enter), &second));
    const PopupStateHooks h2 = hooks_of(&fake_process);
    CHECK(!popup_state_acquire(&second, h2));
    CHECK(entry_field_eq(entry, POPUP_ENTRY_PROCESS, reinterpret_cast<uintptr_t>(&fake_process)));  // 仍为 h1 的 process
}

void test_same_owner_idempotent() {
    arm_fake_table();
    uint8_t* entry = put_entry(0, 1, &fake_enter);
    uint8_t snapshot[POPUP_ENTRY_SIZE];
    std::memcpy(snapshot, entry, POPUP_ENTRY_SIZE);

    PopupStateEntry first{};
    CHECK(popup_state_resolve(vma_of(&fake_enter), &first));
    const PopupStateHooks h = hooks_of(&fake_enter);
    CHECK(popup_state_acquire(&first, h));

    PopupStateEntry again{};
    CHECK(popup_state_resolve(vma_of(&fake_enter), &again));
    CHECK(popup_state_acquire(&again, h));  // 同 owner 幂等 true
    // 幂等重入方持有的「已注入」快照被回填为安装前快照，restore 仍整段还原。
    popup_state_restore(&again);
    CHECK(std::memcmp(entry, snapshot, POPUP_ENTRY_SIZE) == 0);
}

void test_acquire_null_args() {
    arm_fake_table();
    const PopupStateHooks h = hooks_of(&fake_enter);
    CHECK(!popup_state_acquire(nullptr, h));
    PopupStateEntry empty{};
    empty.slot = nullptr;
    CHECK(!popup_state_acquire(&empty, h));
    PopupStateEntry out{};
    put_entry(0, 1, &fake_enter);
    CHECK(popup_state_resolve(vma_of(&fake_enter), &out));
    PopupStateHooks no_enter = h;
    no_enter.enter = nullptr;
    CHECK(!popup_state_acquire(&out, no_enter));
}

void test_restore_total_and_reacquire() {
    arm_fake_table();
    uint8_t* entry = put_entry(9, 21, &fake_enter);
    uint8_t snapshot[POPUP_ENTRY_SIZE];
    std::memcpy(snapshot, entry, POPUP_ENTRY_SIZE);

    PopupStateEntry out{};
    CHECK(popup_state_resolve(vma_of(&fake_enter), &out));
    const PopupStateHooks h = hooks_of(&fake_enter);
    CHECK(popup_state_acquire(&out, h));
    CHECK(std::memcmp(entry, snapshot, POPUP_ENTRY_SIZE) != 0);

    // 快照中的 hook 槽已被 acquire 前的 out.backup 保存：restore 整段写回。
    popup_state_restore(&out);
    CHECK(std::memcmp(entry, snapshot, POPUP_ENTRY_SIZE) == 0);
    CHECK(out.slot == nullptr);
    CHECK(out.state_id == -1);

    // 释放后同一条目可被重新 acquire。
    PopupStateEntry re{};
    CHECK(popup_state_resolve(vma_of(&fake_enter), &re));
    CHECK(popup_state_acquire(&re, h));
    CHECK(entry_field_eq(entry, POPUP_ENTRY_ENTER, reinterpret_cast<uintptr_t>(&fake_enter)));
}

void test_restore_noop_on_empty() {
    arm_fake_table();
    popup_state_restore(nullptr);
    PopupStateEntry empty{};
    empty.slot = nullptr;
    popup_state_restore(&empty);  // 不崩溃、无副作用
    CHECK(true);
}

// 按符号名定位：假符号表返回 0（未解析）→ false；返回条目绝对地址 → 命中并快照；
// 非法入参 / 解析结果 <= g_base → false。
void test_resolve_symbol() {
    arm_fake_table();
    uint8_t* entry = put_entry(2, 5, &fake_enter);
    const uintptr_t enter_abs = reinterpret_cast<uintptr_t>(&fake_enter);
    PopupStateEntry out{};

    qol::game::g_test_resolve_entry = 0;  // 未解析
    CHECK(!popup_state_resolve_symbol("F_PANEL_UNK1_ENTER", &out));
    CHECK(!popup_state_resolve_symbol(nullptr, &out));
    CHECK(!popup_state_resolve_symbol("F_PANEL_UNK1_ENTER", nullptr));

    qol::game::g_test_resolve_entry = enter_abs;  // 命中
    CHECK(popup_state_resolve_symbol("F_PANEL_UNK1_ENTER", &out));
    CHECK(out.slot == entry);
    CHECK(out.state_id == 5);
    CHECK(std::memcmp(out.backup, entry, POPUP_ENTRY_SIZE) == 0);

    qol::game::g_test_resolve_entry = g_base;  // <= g_base
    CHECK(!popup_state_resolve_symbol("F_PANEL_UNK1_ENTER", &out));

    // 解析到未登记进 state 表的地址（未命中条目）→ false。
    qol::game::g_test_resolve_entry = reinterpret_cast<uintptr_t>(&fake_process);
    CHECK(!popup_state_resolve_symbol("F_PANEL_UNK1_ENTER", &out));
}

}  // namespace

int main() {
    test_resolve_hit_and_snapshot();
    test_resolve_miss_and_invalid();
    test_resolve_not_readable_or_not_ready();
    test_acquire_writes_hooks_and_alias();
    test_acquire_partial_wrap_keeps_original_slots();
    test_one_entry_one_owner();
    test_same_owner_idempotent();
    test_acquire_null_args();
    test_restore_total_and_reacquire();
    test_restore_noop_on_empty();
    test_resolve_symbol();
    std::printf("hooks_popup_state: pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
