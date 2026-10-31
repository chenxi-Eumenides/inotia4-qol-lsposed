#pragma once

#include <cstdint>

namespace qol::features::core_inventory::bags {

// 原生物理袋扫描上界（不含）：原版袋数 + C18 探测到的追加袋数，由 item port 统一夹取。
// 由 plugin lifecycle 在端口就绪后写入；hook 层只读，避免把端口依赖带进机制层。
// 与 core.item 的同名实现保持暂存重复：三插件拆分期内两侧各自持有扫描上界。
int native_bag_limit();
void set_native_bag_limit(int32_t limit);

}  // namespace qol::features::core_inventory::bags
