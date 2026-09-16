#include "core/native/popup_enter_alias.h"

#include <atomic>

namespace {

constexpr int kSlots = 16;

struct Slot {
    std::atomic<uintptr_t> wrapper{0};
    std::atomic<uintptr_t> vma{0};
};

Slot g_slots[kSlots];

}  // namespace

void popup_enter_alias_register(uintptr_t wrapper, uintptr_t original_vma) {
    if (wrapper == 0 || original_vma == 0) return;
    for (int i = 0; i < kSlots; ++i) {
        if (g_slots[i].wrapper.load(std::memory_order_acquire) == wrapper) {
            g_slots[i].vma.store(original_vma, std::memory_order_release);
            return;
        }
    }
    for (int i = 0; i < kSlots; ++i) {
        uintptr_t expected = 0;
        if (!g_slots[i].wrapper.compare_exchange_strong(expected, wrapper,
                                                       std::memory_order_acq_rel)) {
            continue;
        }
        // wrapper 先发布、VMA 后写入：并发查询可能短暂看到 (wrapper, 0) 而退回原换算
        // 路径，下一帧即可恢复；安装期瞬时误判不影响功能。
        g_slots[i].vma.store(original_vma, std::memory_order_release);
        return;
    }
}

bool popup_enter_alias_unregister(uintptr_t wrapper) {
    if (wrapper == 0) return false;
    for (int i = 0; i < kSlots; ++i) {
        if (g_slots[i].wrapper.load(std::memory_order_acquire) != wrapper) continue;
        g_slots[i].vma.store(0, std::memory_order_release);
        g_slots[i].wrapper.store(0, std::memory_order_release);
        return true;
    }
    return false;
}

uintptr_t popup_enter_alias_resolve(uintptr_t enter) {
    if (enter == 0) return kPopupEnterAliasNone;
    for (int i = 0; i < kSlots; ++i) {
        if (g_slots[i].wrapper.load(std::memory_order_acquire) != enter) continue;
        return g_slots[i].vma.load(std::memory_order_acquire);
    }
    return kPopupEnterAliasNone;
}

void popup_enter_alias_clear() {
    for (auto& slot : g_slots) {
        slot.vma.store(0, std::memory_order_release);
        slot.wrapper.store(0, std::memory_order_release);
    }
}
