#pragma once

// 扩展背包 X1（ptrslot:MOVE_MERGE_GOT）机制桥（D2a-3b-1）。
//
// 落 patch/ 层（而非 feature/patch/）：依赖矩阵允许 plugins → patch，声明式资源申报方
// （plugins/extension_bag）与 wrapper 所在 plugins/extension_bag 均可依赖，不新增
// plugins→plugins 层违规。move_merge wrapper 业务体仍在
// feature/patch/game_patch_move_merge.inc（逐字不动），本头只暴露稳定取址。
// wrapper 内自门控（move_merge / extension_source_protection），原函数入口经 game_access
// 解析的 fn_ui_equip_inven_item_control_event_proc 直调，故无需 backup 交还。

namespace qol::features::game_patch {

// GOT 槽 wrapper 地址（kPtrSlot claim.handler）。
void* game_patch_move_merge_wrapper();

}  // namespace qol::features::game_patch
