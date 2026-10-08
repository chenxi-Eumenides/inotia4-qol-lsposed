#pragma once

#include <cstdint>

#include "rules.h"

// 自动出售扫描与主线程逐帧 tick（阶段 B）。
//
// 线程纪律：仅在游戏主线程执行（由 host/frame_source 的渲染开始前锚点经
// infra::scheduler 的 kRenderPre 派发）；内部不取 g_virtual_bag_mtx、不调用 op_ok()、
// 不主动 save。扩展袋逐槽读取自身取放锁；出售经 inventory.action capability 调用 owner。
//
// 过渡依赖：save_enter/save_exit 设施由共享基础设施提供，inventory.action 由
// inventory_hooks 提供。

// 扫描间隔（帧）：作为 scheduler_add 的 interval，由统一帧调度器按帧节流。
constexpr int kAutoSellScanIntervalFrames = 60;

// 更新存档配置运行时快照；
// 仅当存档总开关 `enabled` 发生切换时才同步扫描任务（关→删除、开→按条件注册），规则/
// 阈值改动不触碰任务生命周期。配置提交由 panel/JNI 共用 slot-data 路径；任务回调每次现取配置。
void autosell_apply_config(const autosell::Config& config);

// 全局开关（模块设置第 6 项 `autoSellEnabled`）：置位/清位「已武装」标志并同步扫描任务；
// 幂等、内部加锁。
//
// 任务注册条件 = 全局开关已武装、slot-data 当前 session 加载成功且存档配置 `enabled` 为真。
// save-enter 事件只校验并同步扫描生命周期，不提供存档读写槽来源。
void autosell_set_global_enabled(bool enabled);

int autosell_active_save_slot();
void autosell_scan_session_loaded(int slot, uint64_t epoch);
void autosell_scan_session_closed(int slot, uint64_t epoch);
bool autosell_scan_save_enter(int slot);
bool autosell_scan_session_active(int slot);
int autosell_scan_session_slot();
void autosell_scan_sync_after_session_change();

// 注册「进入存档」回调（插件 install 调用，幂等）：只标记 world session 活跃并同步任务；
// 存档配置读取由 save.slot_data 的 SessionBound participant 负责。
void autosell_register_save_enter();

// 注册「退出存档」回调（插件 install 调用，幂等）：world → 主菜单时触发一次，置「未进档」
// 并同步任务（已有任务即删除），避免离开存档后仍持有扫描任务。
void autosell_register_save_exit();

// 注销进档/退档回调（插件 deactivate 调用，与注册成对，幂等）：设施迁移前避免停用后仍收到回调。
void autosell_unregister_save_callbacks();
