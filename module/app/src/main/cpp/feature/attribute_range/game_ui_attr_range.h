#pragma once

// 属性显示范围（attribute-range-display）：详情渲染注入。
//
// 在宝石/装备详情中，按随机属性值在其可能范围内的百分位改写该行内联颜色码
// （金/紫/蓝/绿/白/灰）。设计与证据见
// docs/development/features/attribute-range-display.md。
//
// 安装时机：nativeInit 在 bridge_init 成功后调用；内部自检 bridge_ready 与
// LSPosed native hook API 是否就绪，未就绪时静默返回（可后续重试）。
void attr_range_ui_install_if_ready();

// 属性范围探测是否可用（MATH_GetRandom hook 已安装完成）。
bool attr_range_ready();

// 探测独立宝石（type = 属性类型 bits18-23）的属性值随机区间 [X, 2X]。
// 内部调用游戏自身 ITEMSYSTEM_GetJewelOptionValue，由已安装的 MATH_GetRandom hook
// 截获区间（不消耗游戏 RNG）；不依赖详情渲染缓存（t_current_item），供自动出售等
// 其他主线程调用方复用。hook 未安装/参数非法/探测失败返回 false。
bool attr_range_probe_jewel_range(int type, void* item, int* out_min, int* out_max);
