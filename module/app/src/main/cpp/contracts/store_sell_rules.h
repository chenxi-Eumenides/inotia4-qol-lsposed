#pragma once

#include <cstdint>

// 商店扩展物品出售的计数/计价判定（纯函数，供 wrapper 与 Host 断言）。
// 原版 UIStore_SellItem@0xd25f0 只接收一个总价 x0（弹窗载荷），不接收数量；
// 模块在入口层由「载荷 / 单价」反推数量，再决定整堆删除或部分递减。
//
// 计数口径（R-47 决策 b）：view 为模式视图数量（启用态 canonical 128a+b、关闭态 b）。
// - 非 count-encoded（装备等）或 view<=1：原版语义即卖 1 件 → kWhole(count=1)。
// - count-encoded 且 view>1：允许 1..view 的部分出售 → kPartial(count=displayed/unit)；
//   整除不成立、单价/载荷非正或 count 越界 → kReject（调用方拒绝且不 backup）。
namespace qol::store_sell {

enum class PlanKind { kBackup, kWhole, kPartial, kReject };

struct Plan {
    PlanKind kind;
    int32_t count;
};

inline Plan plan(bool count_encoded, int32_t view, int64_t unit, int64_t displayed) {
    if (!count_encoded || view <= 1) {
        return Plan{PlanKind::kWhole, 1};
    }
    if (unit <= 0 || displayed <= 0 || displayed % unit != 0) {
        return Plan{PlanKind::kReject, 0};
    }
    const int64_t count = displayed / unit;
    if (count < 1 || count > static_cast<int64_t>(view)) {
        return Plan{PlanKind::kReject, 0};
    }
    return Plan{PlanKind::kPartial, static_cast<int32_t>(count)};
}

}  // namespace qol::store_sell
