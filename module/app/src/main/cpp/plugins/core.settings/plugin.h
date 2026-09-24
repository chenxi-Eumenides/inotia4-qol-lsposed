#pragma once

#include "../../contracts/plugin.h"

// core.settings：设置成品页（ADR 0001 D1 的 C 档）。
// 拥有自身页面（面板 provider、版式、文案）、资源 `own:popup.settings` 与设置调试端点。
// 界面工具经能力取得（ui_panels / ui_components），不依赖 core.custom_ui 的实现。
namespace qol::features::core_settings {

const contracts::PluginDescriptor& plugin_descriptor();

}  // namespace qol::features::core_settings
