#pragma once

// 启动期常驻指令 patch 的声明式安装入口（原 feature/patch 的 apply_* 序列：
// S2 位段缩窄 / 上限 clamp / 直接位读重定向 / monster 兼容 / hub 登录跳过）。
//
// 由 bridge_init 经注入的 boot hook 调用（game_access.h 的 BridgeBootPatchFn）：
// 必需步骤失败 → 返回 false，bridge 不得就绪；hub 为 fail-soft，仅记录并继续。
// 13 条声明（12 required/fail-closed + 1 hub optional/fail-soft）经 hooks/registry
// 登记、patch/instr_patch 物理安装；monster 兼容是按特征字节定位的扫描式例外，
// 不入资源表（见 .cpp）。

namespace qol::host {

// 幂等：已安装返回 true；未就绪（bridge/g_base）返回 false 允许后续重试。
bool core_patches_install();

}  // namespace qol::host
