#pragma once

// Native 对象所有权状态机（extension-bag-control-plane §8.4，P1 模型层）。
// 纯逻辑组件：host 测试覆盖转移表与计数审计；P3 将其接入真实 borrow 路径。
// 状态语义与控制面一一对应：
//   module-owned   = 对象仅由模块持有（payload 在扩展状态，模块负责释放）
//   borrowed-view  = 仅在原版绘制/事件调用窗口内借用；窗口外必须归还
//   inventory-owned = 原版库存接管（交接即回收槽位；模块不得再释放或使用原指针）
//   released       = 已释放且不可再访问（终态）

#include <cstdint>
#include <cstring>

#include "plugins/extension_bag/model/bag_catalog.h"

namespace ownership {

// 容量口径：账本对每个已物化扩展物品占一个 module-owned 句柄，且句柄跨视图切换
// 常驻（切页签不释放重分配），因此容量必须覆盖所有扩展袋的槽位总数
// （kMaxExtensionBags × kSlotCount = 2048），另加事务期临时物化余量。
// 旧常量 128 按「5 袋 × 16」估算；bagCount 可配置到 kMaxExtensionBags 后，
// 多袋存档在 128 件物品时池耗尽，表现为「有空位但拾取/拖入提示背包已满」。
constexpr int kTransactionSlack = 64;
constexpr int kLedgerCapacity =
    virtual_bag::kMaxExtensionBags * virtual_bag::kSlotCount + kTransactionSlack;

enum class State : uint8_t {
    kNone = 0,
    kModuleOwned = 1,
    kBorrowedForView = 2,
    kInventoryOwned = 3,
    kReleased = 4,
    kReservedForRelease = 5,
};

enum class Outcome : uint8_t {
    kOk = 0,
    kRejectInvalidState = 1,  // 非法转移（状态机不允许）
    kRejectPoolExhausted = 2,
    kRejectUnknownHandle = 3,
};

struct Slot {
    State state = State::kNone;
    uint32_t generation = 0;  // 每次分配递增，句柄 = slot|generation<<8 防陈旧句柄复用
};

struct Audit {
    bool balanced = true;
    uint32_t outstanding_borrows = 0;   // 借出未归还
    uint32_t outstanding_objects = 0;   // module-owned 未释放/未交接
    uint32_t inventory_owned = 0;       // 原版接管（由原版释放，不计入模块泄漏）
    uint32_t released = 0;
    uint32_t live_handles = 0;
};

struct Ledger {
    Slot slots[kLedgerCapacity]{};
    uint32_t total_allocated = 0;
    uint32_t total_released = 0;
    uint32_t total_handed_over = 0;
};

// 句柄布局：低 kHandleSlotBits 位为槽位，高位为 generation。槽位数上限由
// kLedgerCapacity 决定（2048 → 12 bit），句柄仍是 32 位，避免全体调用方改类型。
// generation 只有 32-kHandleSlotBits 位可用：live_state 必须按位掩码比较，
// 否则超过 2^20 次分配后同一个槽位的旧句柄会永久无法匹配（而不是被拒绝）。
constexpr int kHandleSlotBits = 12;
constexpr uint32_t kHandleSlotMask = (1u << kHandleSlotBits) - 1u;
constexpr uint32_t kHandleGenerationMask = 0xFFFFFFFFu >> kHandleSlotBits;

static_assert(kLedgerCapacity <= static_cast<int>(kHandleSlotMask) + 1,
              "ledger capacity must fit the handle slot field");

inline uint32_t make_handle(int slot, uint32_t generation) {
    return (static_cast<uint32_t>(slot) & kHandleSlotMask) |
           ((generation & kHandleGenerationMask) << kHandleSlotBits);
}

inline int handle_slot(uint32_t handle) { return static_cast<int>(handle & kHandleSlotMask); }

inline uint32_t handle_generation(uint32_t handle) {
    return (handle & ~kHandleSlotMask) >> kHandleSlotBits;
}

// 唯一转移方约定（P3 接线时的调用点）：
//   allocate = 扩展物品重建为原版对象（module-owned 入口）
//   borrow_for_view / return_from_view = 原版绘制或事件窗口进出（同一窗口函数成对）
//   handover_to_inventory = 原版库存接管（INVEN 持久接管，模块此后不得触碰指针）
//   release = 模块释放（module-owned 且无借出时）
inline Outcome allocate(Ledger* ledger, uint32_t* handle) {
    if (ledger == nullptr || handle == nullptr) return Outcome::kRejectInvalidState;
    for (int i = 0; i < kLedgerCapacity; ++i) {
        Slot& slot = ledger->slots[i];
        if (slot.state != State::kNone) continue;
        slot.generation += 1;
        slot.state = State::kModuleOwned;
        ledger->total_allocated += 1;
        *handle = make_handle(i, slot.generation);
        return Outcome::kOk;
    }
    return Outcome::kRejectPoolExhausted;
}

inline bool live_state(const Ledger& ledger, uint32_t handle, State expected) {
    const int slot = handle_slot(handle);
    if (slot < 0 || slot >= kLedgerCapacity) return false;
    const Slot& entry = ledger.slots[slot];
    return (entry.generation & kHandleGenerationMask) == handle_generation(handle) &&
           entry.state == expected;
}

inline Outcome borrow_for_view(Ledger* ledger, uint32_t handle) {
    if (ledger == nullptr) return Outcome::kRejectInvalidState;
    if (!live_state(*ledger, handle, State::kModuleOwned)) return Outcome::kRejectUnknownHandle;
    ledger->slots[handle_slot(handle)].state = State::kBorrowedForView;
    return Outcome::kOk;
}

inline Outcome return_from_view(Ledger* ledger, uint32_t handle) {
    if (ledger == nullptr) return Outcome::kRejectInvalidState;
    if (!live_state(*ledger, handle, State::kBorrowedForView)) return Outcome::kRejectUnknownHandle;
    ledger->slots[handle_slot(handle)].state = State::kModuleOwned;
    return Outcome::kOk;
}

inline Outcome handover_to_inventory(Ledger* ledger, uint32_t handle) {
    if (ledger == nullptr) return Outcome::kRejectInvalidState;
    if (!live_state(*ledger, handle, State::kModuleOwned)) return Outcome::kRejectUnknownHandle;
    Slot& slot = ledger->slots[handle_slot(handle)];
    // 所有权已交原版：交接后槽位**立即回收**（generation 递增使旧句柄失效），不再占用
    // module-owned 池。终态信息仅由 total_handed_over 累计记录；否则终态槽永不释放，
    // 连续 ext→orig 交接会耗尽池（真机实证：28 物品 + 4 终态 = 32 即池满）。
    slot.state = State::kNone;
    slot.generation += 1;
    ledger->total_handed_over += 1;
    return Outcome::kOk;
}

inline Outcome release(Ledger* ledger, uint32_t handle) {
    if (ledger == nullptr) return Outcome::kRejectInvalidState;
    if (!live_state(*ledger, handle, State::kModuleOwned)) return Outcome::kRejectUnknownHandle;
    Slot& slot = ledger->slots[handle_slot(handle)];
    slot.state = State::kNone;
    slot.generation += 1;
    ledger->total_released += 1;
    return Outcome::kOk;
}

inline Outcome reserve_release(Ledger* ledger, uint32_t handle) {
    if (ledger == nullptr) return Outcome::kRejectInvalidState;
    if (!live_state(*ledger, handle, State::kModuleOwned)) return Outcome::kRejectUnknownHandle;
    ledger->slots[handle_slot(handle)].state = State::kReservedForRelease;
    return Outcome::kOk;
}

inline Outcome cancel_reserved_release(Ledger* ledger, uint32_t handle) {
    if (ledger == nullptr) return Outcome::kRejectInvalidState;
    if (!live_state(*ledger, handle, State::kReservedForRelease))
        return Outcome::kRejectUnknownHandle;
    ledger->slots[handle_slot(handle)].state = State::kModuleOwned;
    return Outcome::kOk;
}

inline Outcome commit_reserved_release(Ledger* ledger, uint32_t handle) {
    if (ledger == nullptr) return Outcome::kRejectInvalidState;
    if (!live_state(*ledger, handle, State::kReservedForRelease))
        return Outcome::kRejectUnknownHandle;
    Slot& slot = ledger->slots[handle_slot(handle)];
    slot.state = State::kNone;
    slot.generation += 1;
    ledger->total_released += 1;
    return Outcome::kOk;
}

// 计数审计：allocated == released + handed_over + 在持对象数；借出必须为 0。
inline Audit audit(const Ledger& ledger) {
    Audit report{};
    for (int i = 0; i < kLedgerCapacity; ++i) {
        const Slot& slot = ledger.slots[i];
        switch (slot.state) {
            case State::kModuleOwned:
            case State::kReservedForRelease:
                report.outstanding_objects += 1;
                report.live_handles += 1;
                break;
            case State::kBorrowedForView:
                report.outstanding_borrows += 1;
                report.live_handles += 1;
                break;
            case State::kInventoryOwned:
                // 交接后槽位已回收；该状态正常不再出现（保留兼容）。
                report.live_handles += 1;
                break;
            case State::kReleased:
            case State::kNone:
                break;
        }
    }
    report.released = ledger.total_released;
    // inventory_owned 为累计交接数（终态槽已回收，不再按句柄驻留）。
    report.inventory_owned = ledger.total_handed_over;
    report.balanced = ledger.total_allocated ==
                      ledger.total_released + ledger.total_handed_over +
                          report.outstanding_objects + report.outstanding_borrows;
    return report;
}

}  // namespace ownership
