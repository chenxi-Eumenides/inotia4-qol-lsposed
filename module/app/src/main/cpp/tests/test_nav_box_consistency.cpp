// 三页导航/返回按钮标准盒一致性（P1 坐标统一）。
//
// 设置页（core.settings settings.cpp kBackAbs*）、调试面板（core.custom_ui
// debug_components.cpp kPanelNavAbsRect + 创建时 inset 换算）、存档备份（save_backup
// panel.h kBackBtn*）都必须派生自 contracts/ui_theme.h 的同一 kNavBox* 常量：
// 屏幕绝对左上 (8,8)、盒 78×86（原生 X/返回按钮贴图盒）。
// 各页创建路径的行为断言分别在 test_settings_panel_widget / test_debug_two_column；
// 此处锁定「单一来源」与 save_backup 尺寸（其 panel.cpp 仅参与 Android 构建，
// host 测试经 panel.h 常量断言）。

#include "contracts/ui_theme.h"
#include "plugins/save_backup/ui/panel.h"

static_assert(qol::contracts::ui_theme::kNavBoxAbsX == 8 &&
                  qol::contracts::ui_theme::kNavBoxAbsY == 8 &&
                  qol::contracts::ui_theme::kNavBoxW == 78 &&
                  qol::contracts::ui_theme::kNavBoxH == 86,
              "nav box is the top-left 78x86 native texture box");

static_assert(qol::features::save_backup::kBackBtnW == qol::contracts::ui_theme::kNavBoxW &&
                  qol::features::save_backup::kBackBtnH == qol::contracts::ui_theme::kNavBoxH,
              "save_backup back button shares the nav box size (78x86)");

int main() { return 0; }
