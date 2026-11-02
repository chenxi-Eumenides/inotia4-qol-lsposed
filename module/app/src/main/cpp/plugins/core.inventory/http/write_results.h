#pragma once

#include <cstdint>

namespace qol::features::core_inventory {

// 主线程结果处理器：执行 move（原生同空间走 item state write 端口，其余走
// inventory.transfer）与 money（money write 端口），结果体与迁移前逐字同形。
bool inventory_write_result(void* ctx, const uint8_t* payload, uint32_t size, char* out,
                            uint32_t capacity, uint32_t* out_size);

}  // namespace qol::features::core_inventory
