#pragma once

#include "contracts/plugin.h"

// 世界传送插件清单入口（ARCHITECTURE.md §3.3）：零资源、常驻。
//
// 旧实现安装一个入口指令 patch（UIPlay_CallMapName 尾部：原版 `mov x0,#1` / 改版 `b 注入块`
// 双版本指纹 + 近址 trampoline 跳回 epilogue），现有声明式 kCallPatch 机制无法表达
// 运行时 branch 目标与双 expected_word，故按 save_backup 先例保留为 install 内的 legacy
// 机制；待 patch/ 原语支持运行时目标后再转声明式资源。

namespace qol::features::world_teleport {

const contracts::PluginDescriptor& plugin_descriptor();

}  // namespace qol::features::world_teleport
