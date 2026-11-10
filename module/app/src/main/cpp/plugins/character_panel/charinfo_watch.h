#pragma once

#include <cstdint>

#include "../../contracts/ownership.h"
#include "../../contracts/ui.h"

// 原版人物属性面板（POPUP_SC_CHARACTER_INFO）的点击检测与延迟打开。
//
// 本插件以 kOwnership 资源占用该面板的 PopupState 条目（id/symbol 均为
// `own:popup.character_info`）：enter 槽改为透传原 enter 的 wrapper（机制层用它作为
// owner 身份，且必须非空），event 槽替换为点击检测。按下（0x17）记录起点，抬起（0x18）
// 要求按点与抬点都落在 `QolUiAdapter::character_info_content_rect` 给出的内容区内、
// 且 `character_panel.overview` 开启，才置一次性 pending 标志。
//
// 原 enter/event 无论如何都会被调用并原样返回其返回值：关闭 X、属性 +/−、队伍页签等
// 原生控件的点击语义不受影响。
//
// 打开动作不能在 event 内同步执行（面板创建/入栈要求帧安全点），由插件的常驻帧任务
// `character_info_open_tick` 在下一帧消费 pending，经 `ui_panels` 能力打开总览面板。

namespace qol::features::character_panel {

// 内容区命中判定（纯函数，绝对坐标）：左上包含、右下不包含；空/负尺寸矩形恒不命中。
inline bool character_info_tap_inside(const contracts::UiRect& rect, int64_t x, int64_t y) {
    if (rect.w <= 0 || rect.h <= 0) return false;
    return x >= rect.x && x < rect.x + rect.w && y >= rect.y && y < rect.y + rect.h;
}

// OwnershipOps 业务体（资源 claim.handler 指向 kCharacterInfoPopupOps）。
extern const contracts::OwnershipOps kCharacterInfoPopupOps;

// 资源安装：解析 F_PANEL_CHARACTER_INFO_ENTER，wrap enter（透传原函数）与 event。0 = 成功。
int32_t character_info_popup_acquire(const char* id);
// 资源释放：还原 PopupState 条目（幂等）。
void character_info_popup_release(const char* id);

// 被占用的 PopupState enter 回调（机制层 owner 身份；只透传原 enter）。
void character_info_enter_wrapper();

// 被占用的 PopupState event 回调（宿主按 QolPopupStateHooks 槽位调用）。
uint32_t character_info_event_wrapper(uint64_t event, uint64_t param, uint64_t param2);

// 常驻帧任务（LogicPre，每帧）：消费 pending 并按需打开总览面板。
// 正常路径返回 1（返回 0 会被 scheduler 注销）。
int32_t character_info_open_tick(int64_t frame, void* ctx);

// deactivate 清理：丢弃尚未消费的 pending。原始 event 入口与条目快照保留到资源事务
// 归还（CharInfoPopupOps::release），避免 release 失败时 wrapper 失去原函数。
void character_info_reset();

// 测试专用：释放已占用条目（若存在）并清空 pending/按下点/缓存的原始入口。
// 生产路径不调用；不改变任何生产语义。
void character_info_test_reset();

}  // namespace qol::features::character_panel
