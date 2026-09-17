#pragma once

// Stable read-only port for API-side stack count normalization.
// The implementation is owned by the stack-limit enhancement.
bool stack_limit_enabled();

// Effective-state 写入：由 plugins/stack_limit 插件在门禁 enable/disable 时调用。
// 只改状态、不触碰指令 patch（clamp 的物理安装/还原归宿主声明式资源）。
void stack_limit_set_effective(bool enabled);
