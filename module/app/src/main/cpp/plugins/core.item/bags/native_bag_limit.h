#pragma once

#include <cstdint>

namespace qol::features::core_item::bags {

// 原生物理袋扫描上界（不含）：原版 6，C18 为 6 + 改版袋数，最大 18。
// 由 lifecycle 在端口就绪后写入；hook 层只读，避免把端口依赖带进机制层。
int native_bag_limit();
void set_native_bag_limit(int32_t limit);

}  // namespace qol::features::core_item::bags
