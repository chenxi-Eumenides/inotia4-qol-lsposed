#pragma once

namespace qol::features::world_teleport {

// 世界传送 4 选项插件业务层：接管地图名横幅入口，改为原生 UICHOICE 面板。
// 入口为 UIPlay_CallMapName 的函数级 kEntryHook（宿主机制，LSPosed native hook）；本层
// 只做面板/确认/扣费/切图的业务编排。
// 选择框组件经能力契约 `ui_choice`（provider=core.custom_ui）消费，每次现取、不缓存。
// UI 诊断经能力契约 `ui.state`（provider=core.ui_state）的 `read_layers` 读取，失败不影响流程。

// UIPlay_CallMapName 入口 hook wrapper 调用的业务入口（游戏主线程）：先复刻原函数地图名横幅，
// 非世界态忽略，世界态请求打开选择面板。
void world_teleport_call_map_name();

// 插件停用/deactivate 时使已排队的帧任务与游戏回调失效：停用后不得再开面板、扣费或切图。
void invalidate_operations();

}  // namespace qol::features::world_teleport
