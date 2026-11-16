#pragma once

#include <cstdint>

// ui/cards/ 数字类卡片的打开入口；具体控件与状态在各 .cpp 内部，
// 通用承载与确认写入见 numeric_card.h。前置条件不满足时返回 false，
// 与拆分前 left_info_selected 的静默 return 行为一致。

namespace qol::features::item_editor {

bool open_quantity_card();
bool open_enchant_card();
bool open_enchant_strength_card();
bool open_enchant_level_card();
bool open_socket_card();
bool open_chaos_rate_card();

// ---- 类别卡 / 物品卡 / 前缀卡 ----
// 独立卡片：状态与控件句柄留在各自 .cpp 内，panel.cpp 只做路由编排。
// 生命周期约定与 numeric_card 一致：open 只置打开请求，panel_process 轮询
// open_requested 后 clear 请求、销毁其余卡片并 create；apply 消费确认/取消
// 动作并写 draft；handle_event 派发本卡控件，返回卡是否存活（未存活时
// 事件继续交给主面板）。前置条件不满足时 open 静默忽略，create 失败
// 返回 false 且不留句柄，与拆分前行为一致。

void category_card_open();
bool category_card_open_requested();
void category_card_clear_open_request();
bool create_category_card();
void destroy_category_card();
void category_card_apply();
void draw_category_card();
bool category_card_active();
bool category_card_handle_event(uint64_t event, uint64_t param, uint64_t param2,
                                uint32_t* consumed);

void item_card_open();
bool item_card_open_requested();
void item_card_clear_open_request();
bool create_item_card();
void destroy_item_card();
void item_card_apply();
void draw_item_card();
bool item_card_active();
bool item_card_handle_event(uint64_t event, uint64_t param, uint64_t param2,
                            uint32_t* consumed);

void prefix_card_open();
bool prefix_card_open_requested();
void prefix_card_clear_open_request();
bool create_prefix_card();
void destroy_prefix_card();
void prefix_card_apply();
void draw_prefix_card();
bool prefix_card_active();
bool prefix_card_handle_event(uint64_t event, uint64_t param, uint64_t param2,
                              uint32_t* consumed);

// ---- 删除词条确认卡 ----
// 右列表行内 × 触发；open 携带待删段与索引，确认后从对应 draft 数组删除
// 并压缩（宝石孔数不回缩），取消不改任何字段。生命周期与其它卡片一致。
void delete_option_card_open(uint32_t section, uint32_t index);
bool delete_option_card_open_requested();
void delete_option_card_clear_open_request();
bool create_delete_option_card();
void destroy_delete_option_card();
void delete_option_card_apply();
void draw_delete_option_card();
bool delete_option_card_active();
bool delete_option_card_handle_event(uint64_t event, uint64_t param, uint64_t param2,
                                     uint32_t* consumed);

// 统一词条编辑卡：section 0 原生、1 宝石、2 附魔。
void option_card_open(uint32_t section, uint32_t index);
bool option_card_open_requested();
void option_card_clear_open_request();
bool create_option_card();
void destroy_option_card();
void option_card_apply();
void draw_option_card();
bool option_card_active();
bool option_card_handle_event(uint64_t event, uint64_t param, uint64_t param2,
                              uint32_t* consumed);

}  // namespace qol::features::item_editor
