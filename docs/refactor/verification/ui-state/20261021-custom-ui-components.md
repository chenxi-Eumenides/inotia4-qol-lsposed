# core.custom_ui 基础形状与原生确认框验证记录

## 范围

- `ui_components` 1.22：基础矩形、圆角矩形、圆形绘制原语。
- `ui_confirmation` 1.0：以 `UIPopupMsg_CreateYesNoFromTextData` 实现的单 owner 原生确认/价格确认框；价格和消费者 context 分离，token 约束 active/close。
- `QolUiAdapter` minor 27：popup 状态、回调槽条件操作、创建与 owner-checked 关闭入口。
- Debug Components 提供“基础形状”和“原生对话框”示例；原生对话框入口只消费共享能力并记录状态，不执行游戏业务。
- 未改原生 choice 绘制、alpha 或页面 z-order；扩展背包的 `create_confirmation/restore_confirmation` 专属回调流程保留。

## Host 与静态验证

| 验证项 | 命令与结果 | 证据 |
|---|---|---|
| 受影响 Host targets | `cmake --build .tmp/custom-ui-confirmation/host --target core_ui_components_tests core_ui_confirmation_tests ui_adapter_tests core_custom_ui_host_tests plugin_registry_tests -j2`；CTest `-R '^(core_ui_components_tests|core_ui_confirmation_tests|ui_adapter_tests|core_custom_ui_host_tests|plugin_registry_tests)$'`：**5/5 PASS**。覆盖形状 alpha span 不重叠、圆角像素 mask、价格范围、token、Busy、回调时序、混合 owner、失败关闭重试。 | `.tmp/custom-ui-final/host-build.log`、`affected-ctest.log` |
| Debug Components 交互样例 | `cmake --build .tmp/custom-ui-debug-showcase/host --target debug_two_column_tests -j2`；CTest `-R '^debug_two_column_tests$'`：**1/1 PASS，130 checks**。覆盖形状绘制、choice API 参数、无价/价格 confirmation 参数、确认/取消状态与关闭时 owner 清理。 | `.tmp/custom-ui-debug-showcase/host-build-final.log`、`host-ctest-final.log` |
| 架构与 ABI 静态检查 | `uv run python scripts/maintenance/check_arch.py`：layers/offsets/JNI/resources 全部通过，违规 **0**。 | `.tmp/custom-ui-final/check-arch.log` |
| Diff 格式 | `git diff --check`：clean。 | `.tmp/custom-ui-final/diff-check.log` |
| Debug 构建 | `scripts/build-debug.sh --offline`：`BUILD SUCCESSFUL`；模块 `output/inotia4_qol_lsposed_debug_2610211441_cf1f174c03bf.apk`，SHA-256 `cf1f174c03bf11cb01442bd1ad5862b95d38bc3cce7393b019f20e0b4aa8d70e`。 | `.tmp/custom-ui-debug-showcase/build-debug-final.log` |

## 真机状态与限制

**NOT_ACCEPTED（未做设备行为/视觉验收）**：候选 APK 未安装，未重启游戏。Debug Components 提供可操作样例，但用户尚未目视确认。示例位置：打开 `core.custom_ui.open_debug_components` → 左栏“基础形状”检查矩形/圆角/圆形与 alpha → “原生对话框”依次打开原生选择框、无价格确认框、价格 100 确认框；检查黑底、是否覆盖调试页、价格显示、确认/取消和关闭面板时的清理。样例回调不写物品/金币/存档。扩展背包继续使用其 operation-specific callback template，不作为新 token API 的运行时证据。

本次 Debug 构建基于含 10 个既有未提交修改路径的工作树；这些路径未纳入本功能提交，但 APK 构建过程会编译当前工作树整体，因此该 APK 不是 pristine commit-only 构建产物。

`native_choice_open` 仅写选择数据并 push 游戏 PopupState，未找到 choice 背景的原版绘制体或 alpha 控制点；黑底具体来源未知。原生 PopupState 可压在底层原生页面上，但与自定义 panel stack 的叠层关系未获真机证明。此项未改动、不得报告已支持背景透明度或任意页面叠层。

`QolUiAdapter` 的 callback 槽操作是 `core.custom_ui` provider 内部实现入口；其他插件必须消费 `ui_confirmation` capability，不得直接缓存或调用 trampoline。绕过此契约持有旧 trampoline 并跨弹窗代际手动调用不受支持。

扩展背包 VM-16/17/18 仍按其原验收卡独立验证，本 Debug 样例与 Host 测试不替代交易回归。
