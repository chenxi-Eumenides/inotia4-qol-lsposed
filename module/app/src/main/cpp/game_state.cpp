// game_state.cpp —— 由 game_data.cpp 拆分生成（纯搬代码，零逻辑变更）

#include "api/native/game_ui.h"

#include "game_access.h"
#include "game_symbols.h"
#include "core/native/extension_bag_port.h"

#include <android/log.h>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#define MOVE_TAG "Inotia4Move"
#define MOVE_LOG(...) __android_log_print(ANDROID_LOG_INFO, MOVE_TAG, __VA_ARGS__)
#include "game_state.h"

bool game_in_world() {
    return g_state != nullptr && *reinterpret_cast<uint16_t*>(g_state) == 5;
}

int current_save_slot() {
    if (g_base == 0) return -1;
    uint8_t* slot = *reinterpret_cast<uint8_t**>(g_base + G_CURRENT_SLOT_GOT_VMA);
    return slot != nullptr ? static_cast<int>(*slot) : -1;
}

// UI 占据检查（v0.5.43）：world 态下 screen 非 "world" 即 UI 占据（对话框 dialog_*/面板 panel_*/教学）。
// 复用 data_ui_screen() 统一判定（与 /api/ui/screen 同源）；返回占据的 screen 名，nullptr=无占据。
// 用于世界操作（移动/战斗/交互/技能/物品）前置阻塞——UI 占据时游戏输入被接管，直接调 CHAR_Move
// 等会与 UI 竞争破坏控制态（真机实测：dialog 打开时 move 仍执行）。
const char* ui_blocked() {
    if (!game_in_world()) return nullptr;  // 非 world 态由各操作自身 game_in_world() 前置处理
    const char* sc = data_ui_screen();
    if (sc != nullptr && strcmp(sc, "world") != 0) return sc;
    return nullptr;
}

int tutorial_state() {
    if (g_base == 0) return 0;
    uint8_t* obj = *reinterpret_cast<uint8_t**>(g_base + G_TUTORIAL_OBJ_GOT_VMA);
    if (obj == nullptr) return 0;
    return static_cast<int>(*reinterpret_cast<uint64_t*>(obj));
}

void tutorial_cancel() {
    if (g_base == 0) return;
    // 复现 F_TUTORIAL_GETSTATE_VMA(0xec340)：Tutorialgetstate 轮转 + 写回 obj170 + 关闭 3 个教学标志
    uint8_t* obj = *reinterpret_cast<uint8_t**>(g_base + G_TUTORIAL_OBJ_GOT_VMA);
    if (obj == nullptr) return;
    // v0.4.50：直接写 obj170=0（无教学态）而非依赖 Tutorialgetstate 返回值——轮转结果依赖
    // 教学槽位历史，进档后旧槽残留导致轮转返回 6，教学无法退出（真机实测 v0.4.49 复现）
    *reinterpret_cast<uint64_t*>(obj) = 0;
    uint8_t* bb8 = *reinterpret_cast<uint8_t**>(g_base + G_TUTORIAL_FLAG1_GOT_VMA);
    if (bb8 != nullptr) *bb8 = 0;
    uint8_t* f170 = *reinterpret_cast<uint8_t**>(g_base + G_TUTORIAL_FLAG2_GOT_VMA);
    if (f170 != nullptr) *f170 = 1;
    uint8_t* ee0 = *reinterpret_cast<uint8_t**>(g_base + G_TUTORIAL_FLAG3_GOT_VMA);
    if (ee0 != nullptr) *ee0 = 0;
}

const char* tutorial_block_error() {
    if (tutorial_state() == 6) tutorial_cancel();
    return nullptr;
}

void* member_or_null(int role) {
    return (fn_get_member != nullptr && role >= 0 && role < 3) ? fn_get_member(role) : nullptr;
}

void* lead_member() {
    // v0.4.38 移动修复：优先读游戏主控角色 PLAYER_pActivePlayer（G_PLAYER_ACTIVE_VMA，CHAR_MoveAsPath 驱动的真实对象）。
    // 旧实现 PARTY_GetMember(0) 返回队伍槽 0 对象，其坐标是占位值（真机实测固定 240,296），
    // 用它做 BFS 起点错误 → CHAR_Move 全部判阻挡（返回 1）→ 导航任务立即终止、角色不动。
    if (g_player_active != nullptr) return *reinterpret_cast<void**>(g_player_active);
    return fn_get_member != nullptr ? fn_get_member(0) : nullptr;
}

// 佣兵槽→角色指针（CHARSYSTEM_FindAsMercenarySlot 遍历大池含未上场佣兵）
void* find_char_by_merc_slot(int slot) {
    return fn_find_merc_slot != nullptr ? fn_find_merc_slot(slot) : nullptr;
}

void* find_inventory_item(int category) {
    InventoryItemRef ref;
    return find_inventory_item_ref(category, &ref) ? ref.native_item : nullptr;
}

int inventory_count() {
    struct Ctx { int n; } ctx{0};
    for_each_inventory_item([](const InventoryItemRef&, void* c) -> bool {
        static_cast<Ctx*>(c)->n++;
        return false;
    }, &ctx);
    return ctx.n;
}

int inventory_quantity(int category) {
    if (category <= 0) return 0;
    struct Ctx { int category; int quantity; } ctx{category, 0};
    for_each_inventory_item([](const InventoryItemRef& item, void* c) -> bool {
        Ctx* p = static_cast<Ctx*>(c);
        if (item.category == p->category) p->quantity += item.count;
        return false;
    }, &ctx);
    return ctx.quantity;
}

void* inventory_item_at(int bag, int slot) {
    InventoryItemRef ref{};
    if (!inventory_item_ref_at(bag, slot, &ref)) return nullptr;
    return ref.native_item;
}

bool find_inventory_item_ref(int category, InventoryItemRef* out) {
    if (out == nullptr || category <= 0) return false;
    struct Ctx { int category; InventoryItemRef* out; bool found; } ctx{category, out, false};
    for_each_inventory_item([](const InventoryItemRef& item, void* c) -> bool {
        Ctx* p = static_cast<Ctx*>(c);
        if (item.category != p->category) return false;
        *p->out = item;
        p->found = true;
        return true;
    }, &ctx);
    return ctx.found;
}

bool inventory_item_ref_at(int bag, int slot, InventoryItemRef* out) {
    if (out == nullptr || bag < 0 || slot < 0 || slot >= 16) return false;
    *out = {};
    if (extension_bag_is_logical_bag(bag)) {
        struct Ctx { int bag; int slot; InventoryItemRef* out; } ctx{bag, slot, out};
        extension_bag_for_each_logical_item([](int item_bag, int item_slot, int category, int count,
                                               void* c) -> bool {
            Ctx* p = static_cast<Ctx*>(c);
            if (item_bag != p->bag || item_slot != p->slot) return false;
            *p->out = {InventoryItemKind::kExtension, item_bag, item_slot, category, count, nullptr};
            return true;
        }, &ctx);
        if (out->kind == InventoryItemKind::kExtension && out->bag == bag && out->slot == slot) {
            out->native_item = extension_bag_item_at(bag, slot);
        }
        return out->kind == InventoryItemKind::kExtension && out->bag == bag && out->slot == slot;
    }
    if (bag >= 5 || g_inven == nullptr || fn_get_bit == nullptr) {
        return false;
    }
    uint8_t* bag_slots = reinterpret_cast<uint8_t*>(g_inven) + bag * 0x80;
    void* item = *reinterpret_cast<void**>(bag_slots + slot * 8);
    if (item == nullptr) return false;
    const uint16_t flags = *reinterpret_cast<uint16_t*>(reinterpret_cast<uint8_t*>(item) + I_TYPE);
    *out = {InventoryItemKind::kOriginal, bag, slot, fn_get_bit(flags, 15, 6),
            fn_get_cumulate_count != nullptr ? fn_get_cumulate_count(item) : 1, item};
    return true;
}

void for_each_inventory_item(InventoryItemFn fn, void* ctx) {
    if (fn == nullptr) return;
    struct Ctx { InventoryItemFn fn; void* ctx; bool stopped; } state{fn, ctx, false};
    if (fn_get_bit != nullptr) {
        for_each_bag_slot([](void* item, int bag, int slot, void* c) -> bool {
            Ctx* p = static_cast<Ctx*>(c);
            InventoryItemRef ref;
            const uint16_t flags = *reinterpret_cast<uint16_t*>(reinterpret_cast<uint8_t*>(item) + I_TYPE);
            ref = {InventoryItemKind::kOriginal, bag, slot, fn_get_bit(flags, 15, 6),
                   fn_get_cumulate_count != nullptr ? fn_get_cumulate_count(item) : 1, item};
            p->stopped = p->fn(ref, p->ctx);
            return p->stopped;
        }, &state);
    }
    if (state.stopped) return;
    extension_bag_for_each_logical_item([](int bag, int slot, int category, int count, void* c) -> bool {
        Ctx* p = static_cast<Ctx*>(c);
        const InventoryItemRef ref{InventoryItemKind::kExtension, bag, slot, category, count, nullptr};
        p->stopped = p->fn(ref, p->ctx);
        return p->stopped;
    }, &state);
}

void for_each_bag_slot(BagSlotFn fn, void* ctx) {
    if (fn == nullptr || g_inven == nullptr) return;
    // This is a read-only physical snapshot primitive: include task bag 5 to
    // match the original query functions. Transaction callers use explicit
    // ordinary-bag validation and must not reuse this range as a target domain.
    for (int b = 0; b < 6; ++b) {
        uint8_t* bag_slots = reinterpret_cast<uint8_t*>(g_inven) + b * 0x80;
        for (int j = 0; j < 16; ++j) {
            void* item = *reinterpret_cast<void**>(bag_slots + j * 8);
            if (item == nullptr) continue;
            if (fn(item, b, j, ctx)) return;
        }
    }
}

bool pool_obj_valid(const uint8_t* obj) {
    if (obj == nullptr) return false;
    int type = static_cast<int>(reinterpret_cast<const int8_t*>(obj)[C_TYPE]);
    if (type < 0 || type > 2) return false;
    if (obj[C_STATUS] > 2) return false;
    int16_t x = *reinterpret_cast<const int16_t*>(obj + C_POS_X);
    int16_t y = *reinterpret_cast<const int16_t*>(obj + C_POS_Y);
    if (x <= 0 || x >= 1500 || y <= 0 || y >= 1500) return false;
    return true;
}
