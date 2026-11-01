#pragma once

#include <cstdint>

#include "contracts/game_ports/item_write_port.h"
#include "contracts/http.h"
#include "contracts/result.h"

namespace qol::features::core_inventory {

// 写命令载荷：在 Host 端口参数之外携带解析后的袋空间，供主线程结果处理器
// 精确重建 source/target（`QolItemWriteArgs` 只有扁平袋号，C18 下无法区分 6..10）。
struct InventoryWritePayload {
    contracts::QolItemWriteArgs args;
    int32_t from_space;
    int32_t to_space;
};

// 可写结果的 typed 命令 handler id（与 core.item 的 kItemWriteHandlerId 同一 id 空间）。
inline constexpr uint32_t kInventoryWriteHandlerId = 0x49560001;

// inventory HTTP 写侧 2 条 POST 的命令 handler：解析 body 并投递 typed 命令，
// ack 与迁移前一致为 `{"ok":true,"ticket":N}`。
QolResult inventory_write_command(const contracts::HttpReq* request, contracts::HttpRes* response,
                                  void* ctx);

}  // namespace qol::features::core_inventory
