#pragma once

#include <cstdint>

#include "contracts/result.h"

namespace qol::features::core_inventory {

// 逻辑帧快照刷新：读 item port 与注册袋源，原子发布到 core.inventory 快照存储。
// 只在 scheduler 帧回调（主线程）调用；HTTP 查询只读已发布快照。
QolResult read_inventory();
void request_inventory_refresh();
int32_t snapshot_frame_tick(int64_t frame, void* ctx);

}  // namespace qol::features::core_inventory
