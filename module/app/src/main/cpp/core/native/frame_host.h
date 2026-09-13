#pragma once

// 统一帧派发宿主锚点安装器（渲染开始前）。
//
// 通过 call_patch 把 GAMESTATE_DrawPlay 内 +0x20 的 `bl MAP_DrawBase`（首个渲染调用
// 之前；DrawPlay 内 byte==1 分支经 GRP_AddColorTone 后 b 0x9d6ec 汇聚到此，每次
// DrawPlay 必执行）改写为 wrapper：先 frame_task_dispatch(kFramePointRenderPre)，再
// 复刻 fn_map_drawbase()（原 bl 目标）。wrapper 运行在游戏主线程。
//
// 安装时机：nativeInit 在 bridge_init 成功后调用；内部自检 bridge_ready/符号与调用点
// 原指令字，未就绪或校验失败即 fail-closed（只记日志、不改写字节）。安装永久生效，
// 不做运行期卸载；幂等。

// 安装渲染开始前锚点；已安装返回 true；未就绪或失败返回 false（不改写字节）。
bool frame_host_install_if_ready();
