#pragma once

#include <cstdint>
#include <string>

#include "contracts/game_ports/item_port.h"
#include "contracts/result.h"

namespace qol::features::core_inventory {

// 读侧 item helpers：与 core.item 的同名实现保持暂存重复（快照构建与 HTTP 富化共用）。
QolResult not_ready();
QolResult error_result(int32_t code, const char* message);
bool effective_item_count(contracts::QolItemHandle item, bool original_count_supported,
                          uint32_t* out_count, bool* out_present);
bool inventory_option_counts_valid(const contracts::QolItemInventoryView& item);
void append_inventory_item(std::string* out, int32_t slot,
                           const contracts::QolItemInventoryView& item, bool count_present,
                           uint32_t count);

}  // namespace qol::features::core_inventory
