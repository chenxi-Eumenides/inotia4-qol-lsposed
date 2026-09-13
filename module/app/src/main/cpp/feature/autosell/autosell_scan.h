#pragma once

#include "feature/autosell/autosell_rules.h"

// 自动出售扫描与主线程逐帧 tick（阶段 B）。
//
// 线程纪律：仅在游戏主线程执行（由 frame_host 的渲染开始前 wrapper 经
// frame_task_dispatch 派发）；内部不取 g_virtual_bag_mtx、不调用 op_ok()、不主动
// save。扩展袋逐槽读取自身取放锁；处置走 inventory_trade::sell（H-04 已 Hook）。

// 扫描间隔（帧）：作为 frame_task_add 的 interval，由统一帧任务管理器按帧节流。
constexpr int kAutoSellScanIntervalFrames = 60;

// 提交存档配置（UI 关闭/销毁时调用一次，面板内编辑不实时保存）：写入运行时配置快照；
// 仅当存档总开关 `enabled` 发生切换时才同步扫描任务（关→删除、开→按条件注册），规则/
// 阈值改动不触碰任务生命周期。由 JNI nativeSetAutoSellConfig 调用；任务回调每次现取配置。
void autosell_apply_config(const autosell::Config& config);

// 全局开关（模块设置第 6 项 `autoSellEnabled`）：置位/清位「已武装」标志并同步扫描任务；
// 幂等、内部加锁。
//
// 任务注册条件 = 全局开关已武装 **且** 已进入存档（save-enter）**且** 存档配置
// `config.enabled` 为真；三者任一不满足即不持有任务。启动/主菜单阶段不注册，进档后由
// autosell_register_save_enter 的回调按当前存档配置注册。
void autosell_set_global_enabled(bool enabled);

// 注册「进入存档」回调（nativeInit 调用一次，幂等）：读档 / 新档加载完成进入 world 后
// 触发一次，按当前存档槽 `current_save_slot()` 读取 sidecar `autosell` 配置应用到运行时，
// 再按「全局开关 + 已进档 + 存档总开关」同步 60 帧扫描任务。回调在游戏主线程执行。
void autosell_register_save_enter();

// 注册「退出存档」回调（nativeInit 调用一次，幂等）：world → 主菜单时触发一次，置「未进档」
// 并同步任务（已有任务即删除），避免离开存档后仍持有扫描任务。
void autosell_register_save_exit();
