#pragma once

#include <cstdint>

// 扩展背包装备/合成控制事件纯谓词（D2c-2b-2 由 plugins/inventory_hooks/inventory_hook_stage4
// 迁入 EB：均为 EB 语义、仅 EB 使用；POD 输入，header-only 供 Host 断言）。
//
// 迁移动因：切断 EB → inventory_hooks 的最后一条 include 边（stage4 头）；谓词本体即
// EB 的装备槽事件/apply 准入/finish 出口决策，归 EB 本地。

// 只有拖放事件（0x04）的扩展 apply 材料源落到装备槽才进入装备槽事件适配；其余源必须 backup。
inline bool extension_bag_is_equip_control_source(uint64_t event, bool extension_source,
                                                  bool source_is_apply_material,
                                                  bool target_is_equip_slot) {
    return event == 0x04 && extension_source && source_is_apply_material && target_is_equip_slot;
}

// S-03 apply 准入判据（R-50）：同袋 + 扩展宝石/强化卷轴源 + 目标装备类 + IsApplyStuff 判真。
inline bool extension_bag_is_apply_candidate(bool same_bag, bool source_is_apply_material,
                                             bool target_is_equip, bool apply_stuff_allowed) {
    return same_bag && source_is_apply_material && target_is_equip && apply_stuff_allowed;
}

// finish 失败后必须进入 abort/isolation 出口；finished=true 时 token 已由 finish 收尾。
inline bool extension_bag_finish_requires_abort(bool finished) {
    return !finished;
}
