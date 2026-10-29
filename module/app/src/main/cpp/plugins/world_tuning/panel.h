#pragma once

#include "../../contracts/host_sdk.h"
#include "../../contracts/panels.h"
#include "../../contracts/ui.h"

// 「游戏世界调整」面板：单页 6 行倍率，行内左右箭头在固定档位间移动。
// 面板内容归本插件所有；注册/导航与控件绘制经 core.custom_ui 的 ui_panels / ui_components。
// 只有已接线的倍率可调；其余行可见但箭头禁用（界面先行）。
namespace qol::features::world_tuning {

// 插件装配期绑定；未绑定时面板不可用。
void panel_bind(const contracts::QolHostSdk* sdk, const contracts::QolUiComponents* components,
                const contracts::QolUiAdapter* ui);
void panel_unbind();

const contracts::PanelProviderSpec& panel_provider();
bool panel_available();

}  // namespace qol::features::world_tuning
