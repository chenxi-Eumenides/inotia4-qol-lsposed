#pragma once

// 与 core.item/usecases/consume_item.{h,cpp} 的 storage 纯函数保持逐字一致；
// 两份实现期间任何语义修复必须同时修改两处。

#include <cstdint>

#include "../../../contracts/game_host.h"

// core.inventory 存储侧 stage4 纯函数：从 core.item/usecases/consume_item 暂存复制，
// 只保留 storage hooks（S2-S4/S6/S7/S11）需要的部分。命名空间隔离避免与 core.item
// 的全局 stage4 符号链接冲突；两侧实现暂存重复，行为逐字一致。

namespace qol::features::core_inventory::stage4 {

using Stage4IdentifyItem = bool (*)(void* item, int32_t* bag, int32_t* slot);
using Stage4ConsumeExtension = bool (*)(void* item);
using Stage4ConsumeBackup = void (*)(void* item);
using Stage4RemoveExtension = bool (*)(void* item);
using Stage4RemoveBackup = int (*)(void* item);

bool stage4_consume_item(void* item, Stage4IdentifyItem identify,
                         Stage4ConsumeExtension extension_consume,
                         Stage4ConsumeBackup backup, bool& recursive_guard);
int stage4_remove_item(void* item, Stage4IdentifyItem identify,
                       Stage4RemoveExtension extension_remove,
                       Stage4RemoveBackup backup, bool& recursive_guard);

// ---------------------------------------------------------------------------
// S2 写侧 INVEN_RemoveItemData 修正计划（R-45/R-49，纯函数供 Host 断言）。
// 原版（0x1040a8）顺序遍历 6 袋 × 槽，对匹配类别的可堆叠堆：整堆删除累计
// w22，最后一堆部分删除时写 `b = cum(Getter 全量) + w22 - count`。启用态下
// cum 已由 H-17 getter 解码为 S2 全量（算术正确），但 b 写对 remain > 127
// 或旧 a 残留的情形丢失进位 → 由调用方快照 + 重扫后按本函数计算修正。
// ---------------------------------------------------------------------------
struct Stage4RemoveDataEntry {
    void* item;        // 快照槽内对象指针
    uint32_t pre_raw;  // 快照时 I_COUNT 原始字段
    uint32_t pre_full; // 快照时启用态 S2 全量
};

struct Stage4RemoveDataPost {
    void* item;        // 重扫槽内对象指针（nullptr = 槽已清）
    uint32_t post_raw; // 重扫时 I_COUNT 原始字段
    uint32_t post_view; // 重扫时启用态 S2 解码值
};

struct Stage4RemoveDataPlan {
    bool correct = false;  // true = 必须对 entry_index 槽回写 remain（a+b）
    int entry_index = -1;  // 需修正的快照条目下标
    uint32_t remain = 0;   // 正确剩余全量（0..999）
};

// 输入：pre（快照数组）、post（与 pre 等长、按同一槽序重扫）、count（原版入参）。
// 只接受唯一的原版 b-only 写回形态；对象替换、其它同对象槽原始字段变化或 API
// 缺失/失败均 fail-closed。关闭态（R-47）原版 b 写天然正确，调用方不应以启用态
// 数据调用本函数。
Stage4RemoveDataPlan stage4_remove_item_data_plan(
    const qol::contracts::QolItemCountApi* item_count, const Stage4RemoveDataEntry* pre,
    const Stage4RemoveDataPost* post, int entry_count, int32_t count);

// H-21 扩展桥接（R-56）物理实扣量 → 扩展补扣量（纯函数供 Host 断言）。
// 原版 INVEN_RemoveItemData 只遍历物理袋 0..5；物理实扣 = 调用前后 H-03 总数差
// （H-03 = 物理 + 扩展，原版不动扩展，故差值即物理实扣）。返回需从扩展袋补扣的
// 数量 = max(0, requested - physical_removed)，requested<=0 或数据反向（after>before）
// 时按 0 处理，避免负补扣。
int stage4_remove_data_extension_shortfall(int total_before, int total_after,
                                           int requested);

}  // namespace qol::features::core_inventory::stage4
