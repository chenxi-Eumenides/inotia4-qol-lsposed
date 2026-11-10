#pragma once

#include "../../contracts/host_sdk.h"
#include "../../contracts/panels.h"
#include "../../contracts/ui.h"

// 「游戏世界调整」面板：装饰方框内双列分页的倍率列表，每页最多 10 项（每列 5 行），
// 选项先填满左列再进右列，框内不滚动；框下方居中放翻页箭头与页码。
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
