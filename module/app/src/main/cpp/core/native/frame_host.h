#pragma once

// 统一帧派发宿主锚点安装器（渲染开始前 + 逻辑帧开始前）。
//
// 通过 call_patch 安装两个锚点：
//  1) GAMESTATE_DrawPlay 内 +0x20 的 `bl MAP_DrawBase`（原字 0x9401d18a）→ render wrapper：
//     先 frame_task_dispatch(kFramePointRenderPre)，再复刻 fn_map_drawbase()。
//  2) MainProcess 内 +0x40 的 `bl STATE_NextStartProcess`（原字 0x97ffff3d）→ logic wrapper：
//     先 frame_task_dispatch(kFramePointLogicPre)，再复刻 fn_state_next_start_process()。
// 两锚点 wrapper 均运行在游戏主线程；状态切换消费点位于帧计数自增与 Draw 之前。
//
// 安装时机：nativeInit 在 bridge_init 成功后调用；内部自检 bridge_ready/符号与调用点
// 原指令字，未就绪或校验失败即 fail-closed（只记日志、不改写字节）；两个锚点全成或全退。
// 安装永久生效，不做运行期卸载；幂等。

// 安装渲染开始前锚点；已安装返回 true；未就绪或失败返回 false（不改写字节）。
bool frame_host_install_if_ready();

// 两个锚点是否都已安装（transition 派发器据此 fail-fast：未安装则不应接收转换请求）。
bool frame_host_anchors_installed();
