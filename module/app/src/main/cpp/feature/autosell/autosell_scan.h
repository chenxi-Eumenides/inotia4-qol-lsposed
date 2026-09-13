#pragma once

#include "feature/autosell/autosell_rules.h"

// 自动出售扫描与主线程逐帧 tick（阶段 B）。
//
// 线程纪律：仅在游戏主线程执行（由 frame_host 的渲染开始前 wrapper 经
// frame_task_dispatch 派发）；内部不取 g_virtual_bag_mtx、不调用 op_ok()、不主动
// save。扩展袋逐槽读取自身取放锁；处置走 inventory_trade::sell（H-04 已 Hook）。

// 扫描间隔（帧）：作为 frame_task_add 的 interval，由统一帧任务管理器按帧节流。
constexpr int kAutoSellScanIntervalFrames = 60;

// 应用配置：写入运行时配置快照，并按 enabled 注册 / 删除 60 帧周期扫描任务（幂等）。
// 由配置下发路径（JNI nativeSetAutoSellConfig）调用。任务回调每次现取配置、无跨帧状态。
// 注：进入存档时按 sidecar 自动注册任务的时机待定——后续需寻找合适时机，或由帧任务
// 管理器提供更多触发点后再接。
void autosell_apply_config(const autosell::Config& config);
