// game_state.cpp —— 由 game_data.cpp 拆分生成（纯搬代码，零逻辑变更）

#include "api/native/game_ui.h"

#include "game_access.h"
#include "game_symbols.h"
#include "core/native/extension_bag_port.h"
#include "core/native/frame_task.h"

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

// HP/MP 上限缓存读取（数据层原语）：直接读 [ch+C_MAX_HP]/[ch+C_MAX_MP]（属性数组 attr 0x1e/0x1f
// 的缓存槽），不调用 CHAR_GetAttr——其 attr=0x1e 分支在 HP>maxHP 时会写回 HP（str w0,[x20,#0x1f0]），
// 在 HTTP/缓存预取线程调用会与游戏主线程属性重算竞争并永久钳低角色 HP。
// 兜底：缓存值 <= 0（世界未就绪/属性失效）时回退当前 C_HP/C_MP，保证输出数值合理。
int32_t char_max_hp(const void* ch) {
    const uint8_t* b = reinterpret_cast<const uint8_t*>(ch);
    const int32_t cached = *reinterpret_cast<const int32_t*>(b + C_MAX_HP);
    return cached > 0 ? cached : *reinterpret_cast<const int32_t*>(b + C_HP);
}

int32_t char_max_mp(const void* ch) {
    const uint8_t* b = reinterpret_cast<const uint8_t*>(ch);
    const int32_t cached = *reinterpret_cast<const int32_t*>(b + C_MAX_MP);
    return cached > 0 ? cached : *reinterpret_cast<const int32_t*>(b + C_MP);
}

// 主属性总属性（数据层原语）：CHAR_GetStat(0xdf8d0) 的直读等价实现。
// 反汇编：bl CHAR_GetStatBase / CHAR_GetStatMain / CHAR_GetStatBonus / CHAR_GetStatSub 后
// 依次 add w19 累加，末尾 add w0, w19, w0 返回；无 clamp、无条件分支。
int32_t char_stat_total(const void* ch, int index) {
    if (ch == nullptr || index < 0 || index > 4) return 0;
    const uint8_t* b = reinterpret_cast<const uint8_t*>(ch);
    const size_t i = static_cast<size_t>(index);
    const int32_t base = static_cast<int8_t>(b[C_STAT_BASE + i]);   // ldrsb [ch+0x250+i]
    const int32_t main = *reinterpret_cast<const int16_t*>(b + C_STAT_MAIN + i * 2);   // ldrsh [ch+0x256+i*2]
    const int32_t bonus = static_cast<int8_t>(b[C_STAT_BONUS + i]); // ldrsb [ch+0x260+i]
    const int32_t sub = *reinterpret_cast<const int16_t*>(b + C_STAT_SUB + i * 2);     // ldrsh [ch+0x266+i*2]
    return base + main + bonus + sub;
}

// ---- next_exp 游戏线程帧缓存 ----
// 生命周期：char_next_exp_cache_start() 在 nativeInit 注册一个常驻帧任务（kFramePointLogicPre，
// 每帧触发），随进程存活；不进 world 时置无效。读者（JSON 预取/HTTP 线程）只读 atomic 快照，
// 不调用任何游戏函数；未命中缓存时回退直读游戏自身的惰性缓存 [ch+0x320]。
namespace {

constexpr int kPartyRoles = 3;

struct CharNextExpSlot {
    std::atomic<void*> ch{nullptr};   // 缓存对应的角色对象（指针稳定；换角色即重新命中）
    std::atomic<int64_t> next_exp{0};
    std::atomic<bool> valid{false};   // false = 本槽无有效缓存（非 world / 未进队 / 符号缺失）
};

CharNextExpSlot g_char_next_exp[kPartyRoles];
FrameTaskId g_char_next_exp_task = 0;  // 仅 nativeInit 线程写入

// 游戏主线程回调：对 3 名队员各调用一次 CHAR_GetNextExperience（首次/升级失效后现算并写回），
// 结果写入 atomic 快照。返回 true 保持常驻。
bool char_next_exp_tick(int64_t /*frame*/, void* /*ctx*/) {
    if (g_base == 0 || !game_in_world()) {
        for (int i = 0; i < kPartyRoles; ++i) g_char_next_exp[i].valid.store(false);
        return true;
    }
    for (int i = 0; i < kPartyRoles; ++i) {
        void* ch = (fn_get_member != nullptr) ? fn_get_member(i) : nullptr;
        const bool ok = ch != nullptr && fn_get_next_exp != nullptr;
        g_char_next_exp[i].ch.store(ok ? ch : nullptr);
        if (ok) g_char_next_exp[i].next_exp.store(fn_get_next_exp(ch));
        g_char_next_exp[i].valid.store(ok);
    }
    return true;
}

}  // namespace

void char_next_exp_cache_start() {
    if (g_char_next_exp_task != 0) return;
    g_char_next_exp_task = frame_task_add(kFramePointLogicPre, &char_next_exp_tick, nullptr, 1, 0);
}

int64_t char_next_exp_cached(const void* ch) {
    if (ch == nullptr) return 0;
    for (int i = 0; i < kPartyRoles; ++i) {
        if (g_char_next_exp[i].valid.load() && g_char_next_exp[i].ch.load() == ch)
            return g_char_next_exp[i].next_exp.load();
    }
    // 回退：直读游戏自身惰性缓存 [ch+0x320]（等同 CHAR_GetNextExperience 非 0 分支）。
    return *reinterpret_cast<const int32_t*>(reinterpret_cast<const uint8_t*>(ch) + C_NEXT_EXP);
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
            const int internal_bag = extension_bag_internal_index(bag);
            if (internal_bag < 0) return false;
            out->native_item = extension_bag_item_at(internal_bag, slot);
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
