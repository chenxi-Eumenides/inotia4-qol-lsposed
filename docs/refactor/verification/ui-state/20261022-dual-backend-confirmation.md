# core.custom_ui 双后端确认框验证记录（2026-10-22）

## 范围

- `ui_confirmation` 1.1：`UiConfirmationSpecV2`（size 首位、显式 message/labels/price_unit）与 `open_v2`；按活动 `ui_panels` 栈选择自绘 overlay / 原生后端。
- 自绘 overlay 基座：`QolUiAdapter` 1.28（`begin_overlay_scope/end_overlay_scope/set_frame_overlay`，宿主 `end_frame` 内、原生 `GRPX_End` 前单次消费）与 1.29（raw `UIPopupMsg_CreateYesNo`）；`panels/frame_overlay` 一次性帧作用域（token+provider+generation、嵌套 sentinel、幂等 owner 栈）；`ui_confirmation_overlay` 呈现组件（布局/绘制/命中测试）。
- `ui.state` 1.2：带 size 的 `UiCustomDialogSnapshotV2`（body/options/price）与 `publish_custom_dialog_v2`；`/api/ui/dialog` 对自定义对话框投影 body/options/price。
- PopupHost 模态输入门禁：F3（Return）取消确认框且不下传 parent；F4/event 自 open 起消费；触摸白名单 `0x17/0x18/0x19` 走 press/move/release + hit_test 恰一次分发。
- 未改：原生 choice 绘制与黑底、扩展背包 `create_confirmation/restore_confirmation` 交易路径、settings/autosell/save_backup panel 行为。

## 根因记录（2026-10-21 设备 trace）

只读 Frida trace（panel+confirmation active，68 渲染帧）：`UIPopupMsg_Draw` 调用 **0** 次；`MainProcess` 每帧同时执行 `UIPopupMsg_Process`（输入模态）与 `POPUPSTATE_Process`（panel GRPX 帧）。不可见确认框的根因是该状态下原生绘制调用缺失，非 z-order 遮挡；Return 双关源于同帧两条输入路径并行。完整数据见会话记录 `phase0f-runtime-trace.md`（任务工作区）。

## Host 与静态验证

| 验证项 | 命令与结果 | 证据 |
|---|---|---|
| 受影响 Host targets | `cmake --build .tmp/custom-ui-overlay-phase1/host` + CTest：core_ui_panels / core_ui_confirmation / core_frame_overlay / ui_adapter / core_custom_ui_host / native_choice / debug_two_column / core_ui_state / core_ui_confirmation_overlay_render **9/9 PASS**（最终轮 `g3-full-ctest.log`）。覆盖：帧作用域 arm/disarm/单次消费/嵌套 sentinel、generation 失效、无 process provider 拒绝 open、首帧失败 fail-close、F3/F4/event 门禁与 Return latch、事件白名单（非法指针用例）、V2 契约校验、双后端路由、按钮恰一次分发、注销 orphaned 重试、native_choice fixture（capability 5 项）、ui.state V2 校验/投影/清理。 | `.tmp/custom-ui-overlay-phase1/g3-full-build.log`、`g3-full-ctest.log` |
| overlay 呈现组件 | `.tmp/custom-ui-overlay-phase1/designer` 下 `core_ui_confirmation_overlay_render_tests` **1/1 PASS**。覆盖布局矩形、默认/自定义按钮文案、price 有无、hit_test 边界、遮罩与面板互斥、layout 失败零绘制。 | `final-designer-build.log`、`g3-designer-ctest.log` |
| 架构与 ABI 静态检查 | `uv run python scripts/maintenance/check_arch.py`：layers/offsets/JNI/resources 全部通过，违规 **0**。 | 本节执行输出 |
| Diff 格式 | `git diff --check`：clean。 | `.tmp/custom-ui-overlay-phase1/g3-diff-check.log` |
| Debug 构建 | `scripts/build-debug.sh --offline`：`BUILD SUCCESSFUL`；产物 `output/inotia4_qol_lsposed_debug_2610220054_5fb5dc2076ff.apk`，SHA-256 `5fb5dc2076ff100290729cfc04ff5e2f76efa1599b2b4d0fd4f2f5342ab229f0`。构建时工作树含既有未提交修改路径（`docs/reference/plugins/autosell.md`、`game/access/ui_kit_controls.inc`、`core.item/inventory/native_inventory_hook.*`、`extension_bag/runtime/projection.cpp`、`tests/test_inventory_hook_stage4.cpp`、`tests/test_ui_control_item_identity.cpp`），均未纳入本功能提交；该 APK 编译当前工作树整体，不是 pristine commit-only 产物。 | `.tmp/dual-backend-confirmation/build-debug.log` |

## 审查门（deepwork）

- Gate 0（3/3）：V2 size-versioned 契约、显式 message 双后端、TextData 仅原生回退、raw YesNo 参数固定（display_type=0, type=2）冻结；否决无版本尾字段。
- Gate 1（3/3）：否决 post-nativeProcess 新开 GRPX 帧（二次 blit/重复 requestRender 未证）；冻结 parent panel 自身帧内 `end_frame` 前单次绘制 seam，前提为六类 parent provider 的 end_frame 唯一且最终（源码核查）+ 设备 trace 每帧恰一对 GRPX Start/End。
- Gate 2（3/3）：substrate 通过；5 项遗留（V2 接线、触摸路由、Return latch、嵌套 sentinel、注销重试）与 3 项关闭条件（事件白名单、scope 前置校验 fail-close、首帧统一消费）全部闭合后放行。

## 真机状态与限制

**NOT_ACCEPTED（待设备/视觉验收）**。需在设备上确认：

1. 原生页面（无自定义 panel）打开 V1/V2 确认框：原生外观与价格栏正常，确认/取消回调恰一次。
2. 自定义 panel 上打开 V2 确认框：panel 保持可见，overlay 覆盖其上（遮罩/正文/价格/按钮），无重复 blit 或闪烁；每帧仅一次 `jRequestRender`/swap。
3. 触摸不穿透 panel；按钮点击恰一次分发；Return 只取消确认框、parent 不关闭。
4. 关闭后 panel 栈深与状态不变；`/api/ui/dialog` 返回自绘 body/options/price；停用/重启用无残留 token、回调或 overlay。
5. 事件白名单外的系统子事件不引发崩溃。

样例入口：`core.custom_ui.open_debug_components` → 左栏「原生对话框」→「自绘确认框」。样例回调不写物品/金币/存档。

真机验收通过前，不得把本卡或相关 capability 描述为视觉已验收；扩展背包 VM-16/17/18 仍按其原验收卡独立验证。
