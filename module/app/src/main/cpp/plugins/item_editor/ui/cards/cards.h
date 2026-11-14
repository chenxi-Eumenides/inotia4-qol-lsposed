#pragma once

// ui/cards/ 数字类卡片的打开入口；具体控件与状态在各 .cpp 内部，
// 通用承载与确认写入见 numeric_card.h。前置条件不满足时返回 false，
// 与拆分前 left_info_selected 的静默 return 行为一致。

namespace qol::features::item_editor {

bool open_quantity_card();
bool open_enchant_card();
bool open_socket_card();
bool open_chaos_level_card();
bool open_chaos_rate_card();

}  // namespace qol::features::item_editor
