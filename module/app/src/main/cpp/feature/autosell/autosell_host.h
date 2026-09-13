#pragma once

// 自动出售主线程逐帧宿主安装（阶段 B）。
//
// 通过 call_patch 把 GAMESTATE_DrawPlay 内 +0x74 的 `bl GRPX_Start` 改写为
// wrapper：先复刻 fn_grpx_start()，再 frame_tick_dispatch()。wrapper 运行在
// 游戏主线程，autosell_tick 由此逐帧执行。
//
// 安装时机：nativeInit 在 bridge_init 成功后调用；内部自检 bridge_ready/符号
// 与调用点原指令字，未就绪或校验失败即 fail-closed（只记日志、不改写字节）。

// 安装 draw-end 宿主；成功返回 true（已安装/幂等）。
bool autosell_host_install_if_ready();

// 卸载：还原调用点并注销 tick 回调（幂等）。
void autosell_host_shutdown();
