#pragma once

// 启动期宿主兼容 patch 的声明式安装入口：Hive 登录跳过与 monster 特征兼容。
//
// 由 bridge_init 经注入的 boot hook 调用（game_access.h 的 BridgeBootPatchFn）。
// S2 数量布局不在 boot 阶段安装，由 stack_limit 的 999 原子组在 assembly 装配期安装；
// 本入口不因 Hive 匹配失败阻断 bridge。monster 兼容是按特征字节定位的扫描式例外，
// 不入资源表（见 .cpp）。

namespace qol::host {

// 幂等：已安装返回 true；未就绪（bridge/g_base）返回 false 允许后续重试。
bool core_patches_install();

}  // namespace qol::host
