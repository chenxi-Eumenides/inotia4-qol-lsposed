#pragma once

#include <string>

// UIEquip 背包格事件 wrapper（v0.6.8）：move_merge 的开关与扩展源保护。
// 注：原 game_patch 的常驻指令 patch（S2 布局/clamp/重定向/hub）已迁 host/core_patches
// 声明式资源；stack_limit 状态与开关已迁 core/native/stack_limit_state + plugins/stack_limit。
// 本文件在 B2a-3 前只保留 move_merge 相关开关（GOT 槽指针 hook 归该 lane）。
bool set_move_merge_enabled(bool enabled);
bool move_merge_enabled();
bool set_extension_source_protection_enabled(bool enabled);

// ---- IAP 恢复（v0.5.18 hive 屏蔽恢复）----
std::string data_recover_after_hive_block();
