#pragma once

#include <cstdint>

#include "item_handle.h"
#include "result.h"

// provider 自管物品的权威存储回写契约。
//
// 定位：provider（当前为 extension_bag）的物品在内存中以“物化对象”显示和参与游戏逻辑，
// 权威存储是 provider 自己的 payload。游戏侧代码经句柄原地改写物化对象后（例如
// custom_recipe 升阶直接写 jewel word / 原生词条），必须通知 provider 把对象的新内容
// 同步回权威存储；否则后续 export/import、存档都会读回旧内容。
//
// 语义：
// - 只在游戏主线程调用。
// - 句柄不属于调用方认识的 provider 时返回 kQolOk（无操作），消费者可以无条件调用。
// - 返回非成功码表示对象当前不可同步（句柄失效、序列化失败等）；调用方已完成的
//   原地写入不会被回滚。
namespace qol::contracts {

constexpr const char* kInventoryItemWritebackCapabilityId = "inventory.item_writeback";
constexpr int32_t kInventoryItemWritebackVersionMajor = 1;
constexpr int32_t kInventoryItemWritebackVersionMinor = 0;

struct InventoryItemWritebackApi {
    QolResult (*commit_item)(QolItemHandle item);
};

}  // namespace qol::contracts
