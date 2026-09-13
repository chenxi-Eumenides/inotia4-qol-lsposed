#pragma once

#include <cstdint>

// 背包处置原语（自动出售阶段 A 基础设施）。
//
// 语义严格等同 data_op_sell_item / data_op_discard_item：模块循环对单件物品调用，
// 复用原版单件销毁/出售链结算语义。
// 线程纪律：内部 std::mutex 串行；不取 g_virtual_bag_mtx；不调用 op_ok()（不触发
// 缓存刷新）；不写游戏内存以外的状态；不主动 save。

namespace inventory_trade {

enum class Status {
    kOk,
    kNotInWorld,
    kTaskBagExcluded,
    kEmptySlot,
    kNoNativeItem,
    // 解析出的槽位物品与调用方给出的 expected_item 不一致（并发下槽位被改动）。
    // 不结算、不退款；调用方按「跳过」处理。
    kStale,
    kSymbolsUnresolved,
    kNoSell,
    kPriceInvalid,
    kCreditFailed,
    kRemoveFailed,
};

struct Result {
    Status status = Status::kOk;
    int64_t price = 0;
    int category = 0;
    int count = 0;
};

// 出售单件（bag 5=任务袋拒绝）。source 仅用于日志标记（API 路径传 "api"）。
// expected_item 非空时做处置身份复核：内部重新解析槽位后若物品指针不一致，
// 返回 kStale 且不结算/不退款（避免按槽位误结算并发下被替换/释放的对象）。
// expected_item 为空表示不做身份校验（兼容旧行为）。
Result sell(int bag, int slot, const void* expected_item, const char* source);

// 销毁单件（仅 remove + 投影同步，无收益）。
// expected_item 语义同 sell：非空且身份不一致时返回 kStale。
Status discard(int bag, int slot, const void* expected_item);

}  // namespace inventory_trade
