#pragma once

#include "../plugin.h"
#include "../../../contracts/ui.h"
#include "../../../contracts/ui_theme.h"
#include "../model/draft.h"

#include <cstdint>

// item_editor 主面板（panel.cpp）与 ui/cards/ 卡片模块共享的内部接口。
// 只暴露草稿镜像和绘制/重建 helper；各卡片的控件 handle 与静态状态留在
// 各自 .cpp 的匿名命名空间内，不进入本头文件。
namespace qol::features::item_editor::panel_internal {

// draw_control_text / draw_string_with_font 的颜色是 AABBGGRR；
// 金色取统一主题常量。
inline constexpr uint32_t kText = 0xFFD5E8F2u;
inline constexpr uint32_t kMuted = 0xFF727A81u;
inline constexpr uint32_t kGold = contracts::ui_theme::kTextGold;

// 主面板草稿镜像；panel_enter 时 read_item 填充，卡片确认只写这里。
extern ItemEditorDraft g_draft;

contracts::QolUiComponents const* components();
contracts::QolUiAdapter const* ui();
contracts::UiRect panel_rect();
void log_panel(const char* message);

void draw_text(contracts::QolUiHandle control, const char* text, int32_t x,
               int32_t y, uint32_t color, int32_t align = 0, int32_t font = 1);
int32_t text_pixel_width(const char* text, int32_t font);
void draw_center_text(contracts::QolUiHandle control, const char* text,
                      int32_t center, int32_t y, uint32_t color,
                      int32_t font = 0);

void read_item();
void build_left_rows();
void rebuild_left_list();
void rebuild_right_list();
void rebuild_item_slot();

// 读取静态文本表；端口不可用或无该条目时返回 nullptr。
const char* static_text(contracts::QolItemStaticTextKind kind, int32_t id);

// draft 的 variant_series 为 0 时向游戏查询当前变种并写入 draft。
void resolve_variant_series();

}  // namespace qol::features::item_editor::panel_internal
