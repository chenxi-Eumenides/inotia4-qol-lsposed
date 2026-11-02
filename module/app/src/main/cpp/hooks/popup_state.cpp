#include "hooks/popup_state.h"

#include <cstring>

#include "infra/popup_enter_alias.h"
#include "game/access/popup_state.h"
#include "game/access/symbols.h"
#include "game/access/game_access.h"

namespace qol::hooks {
namespace {

// 一 entry 一 owner：按条目地址仲裁，owner token = 模块 wrapper 的 enter 地址
// （每个面板的 wrapper 唯一，故同 owner 可识别幂等、异 owner 被拒）。固定容量，无锁。
constexpr uint32_t kMaxOccupied = 16;

struct Occupied {
    uint8_t* slot = nullptr;
    uintptr_t owner = 0;
    uint8_t backup[0x40] = {};  // 首次 acquire 前的条目快照，幂等重入时回填调用方
};

Occupied g_occupied[kMaxOccupied];
uint32_t g_occupied_count = 0;

int find_occupied(const uint8_t* slot) {
    for (uint32_t i = 0; i < g_occupied_count; ++i) {
        if (g_occupied[i].slot == slot) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

}  // namespace

bool popup_state_resolve(uintptr_t enter_vma, PopupStateEntry* out) {
    if (out == nullptr) return false;
    qol::game::PopupStateSlot slot{};
    if (!qol::game::popup_state_locate(enter_vma, &slot)) return false;
    out->slot = slot.entry;
    out->state_id = slot.state_id;
    qol::game::popup_state_read(slot, out->backup);
    return true;
}

bool popup_state_resolve_symbol(const char* symbol_name, PopupStateEntry* out) {
    if (symbol_name == nullptr || out == nullptr) return false;
    bool followed = false;
    const uintptr_t entry = qol::game::resolve_hook_entry(symbol_name, &followed);
    if (entry == 0 || g_base == 0 || entry <= g_base) return false;
    return popup_state_resolve(entry - g_base, out);
}

bool popup_state_acquire(PopupStateEntry* e, const PopupStateHooks& h) {
    if (e == nullptr || e->slot == nullptr || h.enter == nullptr) return false;
    const uintptr_t owner = reinterpret_cast<uintptr_t>(h.enter);
    const int index = find_occupied(e->slot);
    if (index >= 0) {
        // 同 owner 幂等；不同 owner 拒绝（一 entry 一 owner）。
        if (g_occupied[index].owner != owner) return false;
        // 重入方可能持有「已注入」的新快照：回填首次安装前的快照，保证 restore 正确。
        std::memcpy(e->backup, g_occupied[index].backup, sizeof(e->backup));
        return true;
    }
    if (g_occupied_count >= kMaxOccupied) return false;

    // 部分 wrap：空指针槽从安装前快照取原值回写，保持该槽不变（backup 为全 0x40 快照）。
    const qol::game::PopupStateSlot slot{e->slot, e->state_id};
    const auto keep_or_original = [e](void* hook, size_t offset) -> const void* {
        if (hook != nullptr) return hook;
        uintptr_t original = 0;
        std::memcpy(&original, e->backup + offset, sizeof(original));
        return reinterpret_cast<const void*>(original);
    };
    qol::game::popup_state_write_hooks(
        slot, keep_or_original(reinterpret_cast<void*>(h.enter), POPUP_ENTRY_ENTER),
        keep_or_original(reinterpret_cast<void*>(h.process), POPUP_ENTRY_PROCESS),
        keep_or_original(reinterpret_cast<void*>(h.f3), POPUP_ENTRY_F3),
        keep_or_original(reinterpret_cast<void*>(h.f4), POPUP_ENTRY_F4),
        keep_or_original(reinterpret_cast<void*>(h.event), POPUP_ENTRY_EVENT));
    g_occupied[g_occupied_count].slot = e->slot;
    g_occupied[g_occupied_count].owner = owner;
    std::memcpy(g_occupied[g_occupied_count].backup, e->backup, sizeof(e->backup));
    ++g_occupied_count;

    // 原版 enter VMA 别名：栈顶换成模块 wrapper 后 UI 判定层据此还原面板 VMA。
    const uintptr_t original_vma = qol::game::popup_state_backup_enter_vma(e->backup);
    if (original_vma != 0) {
        popup_enter_alias_register(owner, original_vma);
    }
    return true;
}

void popup_state_restore(PopupStateEntry* e) {
    if (e == nullptr || e->slot == nullptr) return;
    const qol::game::PopupStateSlot slot{e->slot, e->state_id};
    qol::game::popup_state_write_back(slot, e->backup);
    const int index = find_occupied(e->slot);
    if (index >= 0) {
        g_occupied[index] = g_occupied[g_occupied_count - 1];
        --g_occupied_count;
    }
    e->slot = nullptr;
    e->state_id = -1;
}

void popup_state_reset_for_test() {
    g_occupied_count = 0;
}

}  // namespace qol::hooks
