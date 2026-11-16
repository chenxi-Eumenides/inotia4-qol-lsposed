#pragma once

#include <cstdint>

#include "../../contracts/plugin.h"

// core.settings：设置成品页（ADR 0001 D1 的 C 档）。
// 拥有自身页面（面板 provider、版式、文案）与设置调试端点。
// 界面工具经能力取得（ui_panels / ui_components），不依赖 core.custom_ui 的实现。
namespace qol::features::core_settings {

inline constexpr uint32_t kResourceCount = 1;

// 面板 provider 声明数（PluginDescriptor.panel_provider_count）。
inline constexpr uint32_t kProviderCount = 1;

const contracts::PluginDescriptor& plugin_descriptor();

}  // namespace qol::features::core_settings
