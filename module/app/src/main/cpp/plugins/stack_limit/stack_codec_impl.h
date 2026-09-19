#pragma once

#include <cstdint>

#include "contracts/stack_codec.h"
#include "contracts/stack_limit.h"

// 过渡期单一来源：P1-B5 将把实现整体搬入本头并删除 contracts/stack_codec.h。
// 此前 stack_limit 的 S2 逻辑只能通过本头转发，避免公共头与插件实现出现两份 codec。

namespace qol::features::stack_limit::impl {

using stack_codec::effective_clamp;
using stack_codec::effective_view_count;
using stack_codec::effective_write_count;
using stack_codec::kExtendedMax;
using stack_codec::kLegacyMax;
using stack_codec::max_count;
using stack_codec::s2_read_count;

// 这些原语由 game/access 的过渡 adapter 提供；插件不直接 include game_symbols.h。
uint8_t item_count_encoding(const void* item);
bool read_item_count_field(const void* item, uint32_t* out_value);
bool write_item_count_field(void* item, uint32_t value);

}  // namespace qol::features::stack_limit::impl

namespace qol::host {

void host_item_count_set_provider(const contracts::QolItemCountApi* api);

}  // namespace qol::host
