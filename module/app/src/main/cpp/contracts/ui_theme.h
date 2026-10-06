#pragma once

// 统一 UI 颜色 token：单点定义模块 UI 的语义色，供 core.custom_ui、core.settings 及后续
// 插件取值。以下均为「推荐色」，插件可自行覆盖（绘制接口接收的仍是普通 uint32_t）。
//
// 默认编码：0xAABBGGRR，与 GRPX_FillRect、draw_text/draw_control_text/fill_rect、
// draw_panel_decor 等口径一致，低字节为 R、其次 G、其次 B、最高字节为 A。
// GRPX_FillRectAlpha 是例外，只接收 RGB565 + 0..100 alpha；使用它时必须选 *Rgb565 token，
// 不得把下方 ABGR8888 token 直接传入。RGB565 示例 0x1904 = RGB(24,32,32)。
//
// 本头只放 constexpr，不含实现、STL 或游戏地址；不得在此出现 CO_*、CB_*、VMA 或游戏对象。

#include <cstdint>

namespace qol::contracts::ui_theme {

// —— 面板 / 容器 ——
// 面板、背包、弹窗的深色底。
constexpr uint32_t kPanelBg = 0xFF24201Cu;       // RGB(28,32,36) 深灰
constexpr uint32_t kPanelBgRgb565 = 0x1904u;     // RGB(24,32,32)，仅供 GRPX_FillRectAlpha
// 交替行底色，用于列表斑马纹（比 kPanelBg 略亮）。
constexpr uint32_t kPanelBgAlt = 0xFF2B2824u;    // RGB(36,40,43)，轻微交替，不形成强斑马纹
// 选中行 / 选中项底色，比普通行稍亮，仍保持深灰。
constexpr uint32_t kRowSelectedBg = 0xFF383430u; // RGB(48,52,56)，微亮中性灰

// —— 金色（边框 / 分割线 / 可选中文字）——
// 原生 Options 风格的柔和金：框线与分割线共用同一颜色。
constexpr uint32_t kFrameGold = 0xFF7EA4B8u;     // RGB(184,164,126)
constexpr uint32_t kDividerGold = kFrameGold;
// 可选中 / 选中态文字：亮暖金。
constexpr uint32_t kTextGold = 0xFFA0E0FFu;      // RGB(255,224,160)

// —— 文字 ——
// 普通文字。
constexpr uint32_t kTextNormal = 0xFFC8C7C1u;    // RGB(193,199,200) 柔和浅灰
// 禁用 / 次级文字：淡灰。
constexpr uint32_t kTextDisabled = 0xFFA8AAA7u;  // RGB(167,170,168)，与深底保持可读对比

// —— 状态 ——
constexpr uint32_t kError = 0xFF6060FFu;         // RGB(255,96,96)   错误
constexpr uint32_t kOk = 0xFF90E090u;            // RGB(144,224,144) 成功

// —— 可缩放开关组件专用 ——
// 仅供插件自绘开关的推荐 fallback；不改变游戏 Options 原生贴图。
// 关：深灰轨道 + 灰滑块；开：暗暖轨道 + 金滑块。
constexpr uint32_t kSwitchTrackOff = 0xFF383430u;  // RGB(48,52,56)
constexpr uint32_t kSwitchTrackOn = 0xFF302824u;   // RGB(36,40,48)
constexpr uint32_t kSwitchThumbOff = 0xFFA8AAA7u;  // RGB(167,170,168)
constexpr uint32_t kSwitchThumbOn = kTextGold;     // RGB(255,224,160)

}  // namespace qol::contracts::ui_theme
