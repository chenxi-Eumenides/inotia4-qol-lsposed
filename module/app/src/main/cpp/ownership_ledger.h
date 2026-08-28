#pragma once

// Native 对象所有权状态机（extension-bag-control-plane §8.4，P1 模型层）。
// 纯逻辑组件：host 测试覆盖转移表与计数审计；P3 将其接入真实 borrow 路径。
// 状态语义与控制面一一对应：
//   module-owned   = 对象仅由模块持有（payload 在扩展状态，模块负责释放）
//   borrowed-view  = 仅在原版绘制/事件调用窗口内借用；窗口外必须归还
//   inventory-owned = 原版库存接管（终态：模块不得再释放或使用原指针）
//   released       = 已释放且不可再访问（终态）

#include <cstdint>
#include <cstring>

namespace ownership {

constexpr int kLedgerCapacity = 32;

enum class State : uint8_t {
    kNone = 0,
    kModuleOwned = 1,
    kBorrowedForView = 2,
    kInventoryOwned = 3,
    kReleased = 4,
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

inline uint32_t make_handle(int slot, uint32_t generation) {
    return static_cast<uint32_t>(slot) | (generation << 8);
}

inline int handle_slot(uint32_t handle) { return static_cast<int>(handle & 0xFF); }

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
    return entry.generation == (handle >> 8) && entry.state == expected;
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
    slot.state = State::kInventoryOwned;
    slot.generation += 1;  // 句柄立即失效：原指针所有权已转移，模块不得复用
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

// 计数审计：allocated == released + handed_over + 在持对象数；借出必须为 0。
inline Audit audit(const Ledger& ledger) {
    Audit report{};
    for (int i = 0; i < kLedgerCapacity; ++i) {
        const Slot& slot = ledger.slots[i];
        switch (slot.state) {
            case State::kModuleOwned:
                report.outstanding_objects += 1;
                report.live_handles += 1;
                break;
            case State::kBorrowedForView:
                report.outstanding_borrows += 1;
                report.live_handles += 1;
                break;
            case State::kInventoryOwned:
                report.inventory_owned += 1;
                report.live_handles += 1;
                break;
            case State::kReleased:
            case State::kNone:
                break;
        }
    }
    report.released = ledger.total_released;
    report.balanced = ledger.total_allocated ==
                      ledger.total_released + ledger.total_handed_over +
                          report.outstanding_objects + report.outstanding_borrows;
    return report;
}

}  // namespace ownership
