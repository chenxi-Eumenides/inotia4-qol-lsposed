#include "plugins/extension_bag/model/bag_catalog.h"

namespace {
int g_active_bag_count = 5;
}

namespace virtual_bag {

int active_extension_bags() {
    return clamp_active_extension_bags(g_active_bag_count);
}

int extension_logical_bag_last() {
    return kExtensionLogicalBagFirst + active_extension_bags() - 1;
}

}  // namespace virtual_bag

void extension_bag_test_set_active_count(int count) {
    g_active_bag_count = count;
}

// Host 快照测试不加载数量编码策略，canonical 视图直接返回原值。
uint32_t original_count_view(uint32_t field) { return field; }
