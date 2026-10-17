#pragma once

#include "../../contracts/game_ports/choice_port.h"

namespace qol::features::world_teleport {

const contracts::QolChoicePort* choice_port();

// 世界传送 4 选项插件业务层：拦截地图名横幅尾部，改为原生 UICHOICE 面板。
// 入口指令 patch（UIPlay_CallMapName 尾部）由 plugin.cpp 声明，宿主机制层负责安装；本层
// 只做面板/确认/扣费/切图的业务编排。
// 选择框组件经能力契约 `ui_choice`（provider=plugins/native_choice）消费，每次现取、不缓存。

// UIPlay_CallMapName 尾部 trampoline 调用的业务入口（游戏主线程）：非世界态忽略，
// 世界态请求打开选择面板。
void world_teleport_call_map_name();

// 插件停用/deactivate 时使已排队的帧任务与游戏回调失效：停用后不得再开面板、扣费或切图。
void invalidate_operations();

}  // namespace qol::features::world_teleport
