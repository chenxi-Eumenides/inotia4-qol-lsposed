#include <cstdint>

// bag-state-store 快照测试不链接物品数量编码实现，有效数量回退 canonical。
uint32_t virtual_bag_effective_count(const void* item, uint32_t fallback_canonical) {
    (void)item;
    return fallback_canonical;
}
