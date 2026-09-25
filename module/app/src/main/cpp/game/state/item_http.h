#pragma once

#include <cstdint>
#include <string>

#include "contracts/game_ports/item_write_port.h"

namespace qol::game {

// item 写面的响应体拼装接缝（实现在 legacy 聚合 TU game_system.cpp，可访问 api/native 数据源）。
// 必须在游戏主线程调用；core.item 的帧结果处理器调用，HTTP handler 只读票据。
//
// 返回码：Ok = 成功（out 为迁移前原响应 JSON）；Failed = 业务失败（out 为原错误体）。

constexpr int32_t kItemWriteOk = 0;
constexpr int32_t kItemWriteFailed = 1;

// 按 args.kind 执行剩余兼容物品/背包写原语，并按迁移前 Kotlin 的形状组装响应：
//   inventory move → {"ok":true,"state":<inventory JSON>}
//   装备/宝石（equip/unequip/put_jewel/enchant）→ {"ok":true,"state":<party JSON>}
//   骰子接受 → {"ok":true,"state":<player JSON>}；骰子拒绝 → 原始 op JSON
//   op/inventory add|money → 原始 op JSON
int32_t build_item_write_result_json(const contracts::QolItemWriteArgs& args, char* out,
                                     uint32_t capacity, uint32_t* out_size);

}  // namespace qol::game
