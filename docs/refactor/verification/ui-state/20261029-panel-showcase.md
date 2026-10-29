# Debug Components 外层面板观察页

## 范围与约束

- 后续组件视觉调整统一在 Debug Components 演示，不以业务页面代替观察入口。
- 清空原分类页和按钮、开关、形状、弹窗示例，本轮只保留一个装饰容器、尺寸标注、主题色参考块和退出导航。
- 不修改公共组件契约、原生绘制、圆角、背景或弹窗行为；Debug 命令和面板 ID 保持不变。
- `kPanelBg` 色块只是主题色参考，不代表原生容器实际背景色。
- 20261021 验收记录中的旧分类页操作步骤不再适用于当前 Components 演示；该记录不作为本轮视觉验收证据。

## 源码与二进制核对

- `ui_components.cpp` 的 `draw_container`、`draw_container_ornamented` 均限制宽高至少 48，底层调用原生 `UI_DrawBorderRect`；后者另叠加四角装饰图。
- `panel_create` 仅使用 `rect`、`flags`，没有使用 `panel_color`、`mask_rect` 自动绘制或裁剪背景。
- 核对输入 `apk/game-apk/艾诺迪亚4_monster_v29.apk` 内的 `lib/arm64-v8a/libgame.so`：导出 `UI_DrawBorderRect` 位于 `0xafc18`，与当前符号表一致。
- 该函数先绘制边框图集，随后调用 `GRPX_FillRectAlpha(x+24,y+24,w-48,h-48,0xFF000000,60)`；这是内缩矩形，不是圆角蒙版。结论仅限该二进制与 ABI。
- 现有路径不足以定位用户所见背景外溢的具体来源，不将其标记为已定位或已修复。

## 验证

- Host `debug_two_column_tests`：1/1 PASS。覆盖单容器绘制矩形、尺寸标注、48 边界、旧弹窗入口移除、导航输入和关闭生命周期；保留独立 HelpPage 契约回归。
- 命令：`ctest --test-dir .tmp/ui-panel-study/host -R '^debug_two_column_tests$' --output-on-failure`。
- `scripts/build-debug.sh --offline` 成功生成 `output/inotia4_qol_lsposed_debug_2610292155_7c6ca9b029f3.apk`。
- APK SHA-256：`7c6ca9b029f34a73e613d3023a440ab8eea3d2c45fb1b12139448bf5d0680d56`。
- APK 构建后仅更新 Host 测试和本文；用户已有 `docs/refactor/extension-bag/target-architecture.md` 工作树修改未纳入本功能。
- 诊断输出：`.tmp/ui-panel-study/build-debug.log`、`ctest-final.log`、`UI_DrawBorderRect.asm`，均为可重建临时文件，不纳入提交。

## 真机状态

**NOT_ACCEPTED**：未安装、未采集设备截图、未完成用户目视验收。Host 测试不证明角片、底色或文字的实际设备效果。

设备验收入口：Debug 命令 `POST /api/debug/custom-ui/components`，正文 `{"open":true}`；检查四角、底色、尺寸文字以及退出导航。当前阶段不以修改公共绘制代码消除尚未定位的外溢。
