#pragma once

// 世界传送 4 选项插件业务层：拦截地图名横幅尾部，改为原生 UICHOICE 面板。
// 入口指令 patch（UIPlay_CallMapName 尾部）属过渡期 legacy 机制，由 plugin.cpp 的
// install 安装（见 plugin.h 说明）；本层只做面板/确认/扣费/切图的业务编排。

// UIPlay_CallMapName 尾部 trampoline 调用的业务入口（游戏主线程）：非世界态忽略，
// 世界态请求打开选择面板。
void world_teleport_call_map_name();

// 转发 native_choice 组件安装（feature→feature 依赖的唯一挂载点；由 plugin install 调用）。
// 组件归属 feature/ui，D1 迁移后改为共享组件接口。
bool world_teleport_native_choice_install();
