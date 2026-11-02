#pragma once

// 统一 UI 颜色 token：单点定义模块 UI 的语义色，供 core.custom_ui、core.settings 及后续
// 插件取值。以下均为「推荐色」，插件可自行覆盖（绘制接口接收的仍是普通 uint32_t）。
//
// 默认编码：0xAABBGGRR，与 GRPX_FillRect、draw_text/draw_control_text/fill_rect、
// draw_panel_decor 等口径一致，低字节为 R、其次 G、其次 B、最高字节为 A。
// GRPX_FillRectAlpha 是例外，只接收 RGB565 + 0..100 alpha；使用它时必须选 *Rgb565 token，
// 不得把下方 ABGR8888 token 直接传入。RGB565 示例 0x6B6D ≈ RGB(104,108,104)（#6F6F6F 的量化）。
//
// 本头只放 constexpr，不含实现、STL 或游戏地址；不得在此出现 CO_*、CB_*、VMA 或游戏对象。

#include <cstdint>

namespace qol::contracts::ui_theme {

// —— 面板 / 容器（ABGR8888：fill_rect 等直绘接口）——
// 面板、背包、弹窗的半透明黑底（黑 60%），与双栏 Debug 面板外壳的 RGB565 0x0000/60 视觉一致。
constexpr uint32_t kPanelBg = 0x99000000u;       // 黑 60%，只用于 ABGR 直绘通道
constexpr uint32_t kPanelAlpha = 60u;            // kPanelBg 的百分比口径，供 RGB565 通道取用
constexpr uint32_t kPanelBgRgb565 = 0x0000u;     // 黑色，仅供 GRPX_FillRectAlpha 等形状通道
// 普通行底色（#6F6F6F / 50%），供行状态、色板样例使用。
constexpr uint32_t kRowBg = 0x7F6F6F6Fu;         // RGB(111,111,111) / 50%
constexpr uint32_t kRowBgRgb565 = 0x6B6Du;       // #6F6F6F 的 RGB565，仅供形状通道
constexpr uint32_t kRowAlpha = 50u;              // kRowBg 的百分比口径
// 当前生产选中行 / 选中项底色（旧值，尚未迁移）。
// 统一目标为 #695031 / 90%：ABGR 0xE5315069；形状通道用下面的 RGB565 0x6A86 + 90。
constexpr uint32_t kRowSelectedBg = 0xFF383430u; // RGB(48,52,56)，微亮中性灰
constexpr uint32_t kRowSelectedBgRgb565 = 0x6A86u; // #695031 的 RGB565，迁移目标预览用
constexpr uint32_t kRowSelectedAlpha = 90u;      // 选中行迁移目标的不透明度
// 按下行底色（#695031 / 100%），不透明。
constexpr uint32_t kRowPressedBg = 0xFF315069u;  // RGB(105,80,49)

// —— 金色（边框 / 分割线 / 可选中文字）——
// 原生 Options 风格的柔和金：框线与分割线共用同一颜色。
constexpr uint32_t kFrameGold = 0xFF7EA4B8u;     // RGB(184,164,126)
constexpr uint32_t kFrameGoldRgb565 = 0xBD2Fu;   // #B8A47E 的 RGB565，仅供形状通道
constexpr uint32_t kDividerGold = kFrameGold;
// 可选中 / 选中态文字：亮暖金。
constexpr uint32_t kTextGold = 0xFFA0E0FFu;      // RGB(255,224,160)

// —— 文字 ——
// 次级 / 说明文字。
constexpr uint32_t kTextNormal = 0xFFC8C7C1u;    // RGB(193,199,200) 柔和浅灰
// 正文文字：纯白。
constexpr uint32_t kTextBody = 0xFFFFFFFFu;      // RGB(255,255,255)
// 禁用 / 次级文字：淡灰。
constexpr uint32_t kTextDisabled = 0xFFA8AAA7u;  // RGB(167,170,168)，与深底保持可读对比

// —— 状态 ——
constexpr uint32_t kError = 0xFF6060FFu;         // RGB(255,96,96)   错误
constexpr uint32_t kOk = 0xFF90E090u;            // RGB(144,224,144) 成功

// —— 自绘按钮 ——
// 自绘平面按钮的底色（无原生纹理时的推荐配色）。
constexpr uint32_t kButtonBg = 0xFF24201Cu;        // RGB(28,32,36)
constexpr uint32_t kButtonBgPressed = 0xFF474139u; // RGB(57,65,71)

// —— 禁用遮罩 ——
// 禁用态整页/整块遮罩的不透明度（配合黑色 ABGR 直绘使用）。
constexpr uint32_t kOverlayDisabledAlpha = 30u;

// —— 可缩放开关组件专用 ——
// 仅供插件自绘开关的推荐 fallback；不改变游戏 Options 原生贴图。
// 关：深灰轨道 + 灰滑块；开：暗暖棕轨道 + 金滑块。
constexpr uint32_t kSwitchTrackOff = 0xFF383430u;  // RGB(48,52,56)
constexpr uint32_t kSwitchTrackOn = 0xFF1C2E3Au;   // RGB(58,46,28)
constexpr uint32_t kSwitchThumbOff = 0xFFA8AAA7u;  // RGB(167,170,168)
constexpr uint32_t kSwitchThumbOn = kTextGold;     // RGB(255,224,160)

// —— 导航/返回按钮标准盒（屏幕绝对逻辑坐标）——
// 模块设置 / 调试面板 / 存档备份三页的返回与 X 按钮统一使用同一屏幕绝对 rect：
// 左上 (8,8)、盒 78×86（原版 UIGameMenu_InitBackButton SetRect(_,6,78,86) 的贴图盒，
// 使 X 与返回在同一盒内共用同款金属框）。各页创建控件时自行把绝对 rect 换算为
// root 相对（root 原点 = 屏幕 inset），换算口径与设置页 settings.cpp 一致。
constexpr int64_t kNavBoxAbsX = 8;
constexpr int64_t kNavBoxAbsY = 8;
constexpr int64_t kNavBoxW = 78;
constexpr int64_t kNavBoxH = 86;

}  // namespace qol::contracts::ui_theme
