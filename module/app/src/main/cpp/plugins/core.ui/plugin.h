#pragma once

#include "contracts/plugin.h"

// core.ui 出口面插件清单入口（ARCHITECTURE.md §7.1）：
//   · provides `ui_panels` v1.0（面板注册/打开/占用原因）；
//   · 1 条 `own:popup.settings`（kOwnership/kRequired/kFailClosed/resumable/gate=nullptr）：
//     整段占用 F_PANEL_UNK1_ENTER 的 PopupState 5 回调，acquire/release 走 hooks/popup_state。

namespace qol::features::core_ui {

const contracts::PluginDescriptor& plugin_descriptor();

}  // namespace qol::features::core_ui
