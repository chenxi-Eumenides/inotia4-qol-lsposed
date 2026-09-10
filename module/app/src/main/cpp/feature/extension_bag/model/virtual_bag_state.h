#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <string>
#include <type_traits>

#include "core/native/stack_codec.h"
#include "core/native/stack_limit_port.h"

namespace virtual_bag {

constexpr int kBagCount = 5;
constexpr int kMaxCapacity = 16;
constexpr int kSlotCount = 16;

// 原版事务域与扩展内部索引都使用 0..4，但两者不是同一个编号空间。原版索引 5
// 是任务袋，任何扩展事务都不得将其作为源、目标、投影窗口或恢复目标。
constexpr int kOriginalTransactionBagCount = kBagCount;
constexpr int kOriginalTaskBag = kOriginalTransactionBagCount;
constexpr int kExtensionLogicalBagFirst = kOriginalTaskBag + 1;
constexpr int kExtensionLogicalBagLast =
    kExtensionLogicalBagFirst + kBagCount - 1;

// ITEMSTATICBASE 静态表镜像（docs/system/bag.md §5，apk/static-data/json/tables/ITEMSTATICBASE.json）：
// 下标 = BagType，容量 4/8/12/16；kNone=0 → 未装备 → 容量 0（ADR-004：固定绘制值不进入最终架构）。
constexpr std::array<uint8_t, 5> kBagTypeCapacities = {0, 4, 8, 12, 16};

// SAVE_SaveItem 记录约束（objdump 确认）：u8 长度前缀 + 18B 头 + 4B×N 词缀，总长 ≤255。
constexpr size_t kSerializedItemBuffer = 256;  // 固定字节数组容量（State 内禁止裸指针）
constexpr size_t kMaxSerializedItem = 255;     // 可序列化最大总长（SAVE_SaveItem 返回值 uxtb 截断）
constexpr size_t kPayloadHeaderSize = 19;      // 1 前缀 + 18 头（真实物品载荷最小长度）
constexpr size_t kPayloadCountOffset = 11;     // 载荷内 u32 数量位域偏移（小端）
constexpr size_t kSerializedItemProbeBuffer = 1024;  // SaveItem 尾部写入校验窗口

#include "feature/extension_bag/model/virtual_bag_payload.h"

enum class BagType : uint8_t {
    kNone = 0,
    kHandbag = 1,
    kSmallBackpack = 2,
    kMediumBackpack = 3,
    kLargeBackpack = 4,
};

enum class Mode {
    kOriginal,
    kModule,
    kExitingModule,
};

// P5.2a 纯 session 模型。此层不保存 native 指针，也不解释原版 ABI。
#include "feature/extension_bag/model/virtual_bag_drag_model.inc"
#include "feature/extension_bag/model/virtual_bag_transaction_types.h"

struct State {
    std::array<uint8_t, kBagCount> types{};      // 各扩展袋已装备的背包物品 BagType（0=未装备）
    std::array<uint8_t, kBagCount> capacities{}; // 派生容量 = derive_capacity(types[i])，禁止直写
    std::array<std::array<Item, kSlotCount>, kBagCount> items{};
    Mode mode = Mode::kOriginal;
    int original_selected = 0;
    int selected = -1;
    int inspected = -1;   // 槽信息态（扩展物品详情高亮，槽位号）
    int info_bag = -1;    // 袋信息态（二次点击打开，袋号；与槽 inspected 语义分离）
    PendingTransfer pending{};  // 未完成事务（随 JSON 往返持久化，恢复用）
    // P4.5 隔离诊断（内存 only，不进 JSON 序列化，不写 sidecar）。
    std::array<IsolationRecord, kMaxIsolationRecords> isolations{};
    uint8_t isolation_count = 0;   // min(记录数, kMaxIsolationRecords)
    uint8_t isolation_next = 0;    // 环形写入位置
    uint64_t isolation_sequence = 0;
    uint64_t isolation_now_ms = 0; // 调用方在 normalize/隔离前注入的时间源
};

inline void push_isolation(State* state, const IsolationRecord& record) {
    if (state == nullptr || !record.valid) return;
    IsolationRecord copy = record;
    copy.sequence = state->isolation_sequence++;
    copy.valid = true;
    state->isolations[state->isolation_next] = copy;
    state->isolation_next =
        static_cast<uint8_t>((state->isolation_next + 1) % kMaxIsolationRecords);
    if (state->isolation_count < kMaxIsolationRecords) ++state->isolation_count;
}

// 袋解除结果（原版语义：有物品弹窗拒绝，空袋解除）。
enum class UnequipResult {
    kOk,        // 空袋解除：装备清零、容量归 0、标签禁用
    kBlocked,   // 袋内有物品，拒绝解除
    kInvalid,   // 非法袋号
};

enum class ClickResult {
    kIgnored,
    kSelected,
    kInspected,
};

enum class RecoveryAction {
    kRollback,   // 事务未提交：保持现状，仅清除 pending
    kComplete,   // 事务已提交：补做原版侧变更（移除源槽 / 重建入库）
    kNone,       // 无 pending
};

#include "feature/extension_bag/model/virtual_bag_transaction_rules.inc"
#include "feature/extension_bag/model/virtual_bag_state_ops.inc"
#include "feature/extension_bag/model/virtual_bag_serialization.inc"
#include "feature/extension_bag/model/virtual_bag_state_json.inc"
}  // namespace virtual_bag
