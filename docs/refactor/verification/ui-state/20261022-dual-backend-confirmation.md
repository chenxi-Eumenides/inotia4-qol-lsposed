# V1 面板补绘验证记录（原 dual-backend confirmation）

## 范围

用户决策：删除 V2 显式文案与自绘确认框后端，仅保留 V1 `ui_confirmation` 1.0（TextData 原生路径）与“自定义面板帧内补绘原生外观”。本记录保留原始 trace 根因与 Gate 历史，结论范围改为：V1 补绘 + choice 单次 pop + 模块 choice 首帧去暗化 + LCD 主动快照。

## 原始根因（保留）

只读 Frida trace（panel+confirmation active，68 渲染帧）：`UIPopupMsg_Draw` 调用 **0** 次；`MainProcess` 每帧执行 `UIPopupMsg_Process`（输入模态）与 `POPUPSTATE_Process`（panel GRPX 帧）。不可见确认框根因是该状态下原生绘制缺失，非 z-order；Return 双关来自同帧原生弹窗关闭与 panel F3 并行。完整数据见会话记录 `phase0f-runtime-trace.md`。

## 当前实现

- V1 open 仍创建原生 `UIPopupMsg`；顶层自定义 provider 有 `process` 回调时，`PopupHost::dispatch_process` arm `frame_overlay` scope，宿主在 panel GRPX 帧的 `end_frame` 前调用一次注册回调，`bOn==1` 时调用 Adapter 1.30 `call_ui_popup_msg_draw()`，保留 100% 原生外观。成功绘制一次后才启用模态门禁；`bOn==0` 结束 scope。
- 补绘失败只记录日志，不改变 V1 token/close 语义。模态期间 F3 消费且不下传 parent（游戏 `UIPopupMsg_Process` 清 bOn），F4/所有事件消费；触摸事件只消费、不做模块 hit_test 分发。
- 原生选择框不再使用 panel 侧 Return gate（choice 用独立 entry，panel 的 `dispatch_f3` 不执行，原 gate 无效，已移除）。改为防双 pop：用户点选项时原生 `UIChoice_ButtonListExe` 必排一次 `(3,0)`，模块 wrapper 不再额外 `close_popup`；模块主动关闭按 generation 去重，同一 choice 恰好一次 pop。
- 面板 open 时主动 `GRP_SaveLCD`，首帧 RestoreLCD 显示打开前画面。choice 的 push 快照已移除：新增 `Scene_Draw_POPUP_SC_CHOICE` entry hook，模块 choice 首帧在游戏压暗前 SaveLCD、清 refresh flag，再执行 `GRPX_Start → UIChoice_Draw → GRPX_End`，跳过 fpResume/AddAlpha/FillRect/RemoveFilter；非模块 choice 与 flag==0 直接走原函数。
- V2/自绘 overlay、Adapter 1.29 raw YesNo、ui.state 1.2 V2 快照均已删除；Adapter 序号保留 1.31（1.29 空号）。

## 验证状态

- Host：删除后全量 Host targets + `ui_choice_save_lcd_tests` CTest 全绿；designer render test 随组件删除移除。
- 真机：V1 面板补绘已经用户确认可用；choice 单次 pop、去暗化首帧、LCD 快照与嵌套面板背景仍待真机视觉验收。整体视觉结论 **NOT_ACCEPTED**（未重新部署本批构建）。
