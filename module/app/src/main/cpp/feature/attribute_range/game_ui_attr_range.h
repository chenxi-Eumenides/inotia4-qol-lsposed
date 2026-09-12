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
