#pragma once

// 过渡期只读端口：实现已归 stack_limit；P1-B5 删除本头的全局函数并改由 facade 访问。
bool stack_limit_enabled();

// 过渡期 effective-state 写入：由 plugins/stack_limit 插件在门禁 enable/disable 时调用。
// 只改状态、不触碰指令 patch（clamp 的物理安装/还原归宿主声明式资源）。
void stack_limit_set_effective(bool enabled);
