#pragma once

#include <cstdint>

// 自动出售扫描与主线程逐帧 tick（阶段 B）。
//
// 线程纪律：仅在游戏主线程执行（由 autosell_host 的 draw-end wrapper 经
// frame_tick_dispatch 派发）；内部不取 g_virtual_bag_mtx、不调用 op_ok()、不主动
// save。扩展袋逐槽读取自身取放锁；处置走 inventory_trade::sell（H-04 已 Hook）。

// 60 帧扫描间隔（M-2 纯函数）。
constexpr int64_t kAutoSellScanIntervalFrames = 60;

// 帧节流纯函数（M-2，供 host 单测，无游戏依赖）：
//   - immediate 为真：立即扫描（跳过节流）；
//   - 帧号无效（<=0）：不扫描；
//   - last<0（从未扫描）或帧号回退（frame<last，异常/重启）：视为应扫描；
//   - 其余：距上次扫描 >= 60 帧才扫描。
constexpr bool autosell_should_scan(int64_t frame, int64_t last, bool immediate) {
    if (immediate) return true;
    if (frame <= 0) return false;
    if (last < 0) return true;
    if (frame < last) return true;
    return (frame - last) >= kAutoSellScanIntervalFrames;
}

// 注册逐帧回调（幂等）。由 autosell_host_install_if_ready 在 BL patch 成功后调用。
void autosell_init();

// frame_tick 回调：门控 + 60 帧节流 + 扫描；立即执行请求跳过节流。
void autosell_tick(void* ctx);

// 立即执行一次扫描（含全部门控）；供 tick 与测试/诊断调用。
void autosell_scan_once();
