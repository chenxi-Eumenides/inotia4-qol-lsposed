#pragma once

// 世界传送 4 选项插件业务层：拦截地图名横幅尾部，改为原生 UICHOICE 面板。
// 入口指令 patch（UIPlay_CallMapName 尾部）属过渡期 legacy 机制，由 plugin.cpp 的
// install 安装（见 plugin.h 说明）；本层只做面板/确认/扣费/切图的业务编排。
// 选择框组件经能力契约 `ui_choice`（provider=plugins/native_choice）消费，每次现取、不缓存。

// UIPlay_CallMapName 尾部 trampoline 调用的业务入口（游戏主线程）：非世界态忽略，
// 世界态请求打开选择面板。
void world_teleport_call_map_name();
