# 可复用原生 UI 组件

## 目标

`game_ui_kit.h/.cpp` 是轻量的游戏内 UI 组件层，不提供完整布局系统，只封装创建少量原生风格控件所需的重复样板。

## 基础控件

```cpp
void* root = ui_create_root({root_x, root_y, 0x3c0, 0x280});
void* button = ui_create_button(
    root, {0x1f0, 0xb8, 0x100, 0x30}, "确定", on_clicked, draw_button);
void* label = ui_create_label(
    root, {0xc0, 0xb8, 0x240, 0x30}, "说明文字", draw_label);
```

- `ui_create_root` 创建独立 root，并配置安全的 TouchHandle 根处理器。
- `ui_create_button` 统一设置矩形、文本、DrawProc、Active、ControlProc、点击事件类型和 ExecuteProc。
- `ui_create_label` 创建不可点击的显示控件。
- `ui_find_child_in_area` 递归查找已有界面中的原生按钮节点。
- `ui_clone_button_style` 复制原生按钮的 DrawProc/贴图/状态数据，并替换点击回调；当前用于右上角入口复用左上角返回图标。
- `ui_hit_test` 使用控件父链计算绝对坐标，触摸事件使用 `{i64 x, i64 y, i64 id}`。

## 绘制

自定义 DrawProc 中可以复用：

```cpp
void draw_button(void* ctrl) {
    ui_draw_button_background(ctrl, {0, 0, 0x100, 0x30}, 0xFF606060);
    ui_draw_text(ctrl, 8, 6, 0xFFFFFFFF);
}
```

`ui_draw_text` 固定使用安全宽度 `0x1000`，避免 `UI_DrawStringInWidthWithFont` 的 wrap 路径触发未初始化字体崩溃。面板 Process 中使用：

```cpp
ui_begin_frame({0, 0, 0x3c0, 0x280}, {0x90, 0x70, 0x360, 0x1a0}, 0xFF707070);
// fn_ctrl_btn_draw(button/label)
ui_end_frame();
```

`ui_draw_panel_decor` 可绘制原版风格的金色边框和横向分隔线；独立 PopupState 不加入原场景控件树时，优先使用该稳定的色块/文字绘制路径，避免调用依赖原场景槽位的 DrawProc。

帧封装保留 LCD Save/Restore、GRPX Start/End 和低 alpha 遮罩规则。

## Popup state

`UiPopupStateHooks` 和 `UiPopupStateHandle` 封装 27 项 PopupState 表的查找、备份、回调注入和恢复：

```cpp
UiPopupStateHandle state;
UiPopupStateHooks hooks{enter, process, f3, f4, event};
ui_popup_state_inject(&state, F_PANEL_UNK1_ENTER, hooks);
// UI_SetPopupProcessInfo(1, state.state_id)
ui_popup_state_restore(&state);
```

## PtrHook 与 PatchSet

- `PtrHook::install_typed(slot, replacement)`：按函数签名安装函数指针 Hook，避免重复的函数指针转换。
- `PtrHook::installed()`：检查当前 Hook 是否有效。
- `PatchSet patches(entries, count)`：统一调用 `apply()` / `revert()` 管理一组可逆指令 Patch，并保留 `patch_apply` 的指令校验和失败回滚。

## 约束

- 坐标必须使用游戏逻辑坐标，并通过已有 `CalcResolutionWidth/Height` 适配 root。
- 游戏结构偏移和 VMA 仍只允许来自 `game_symbols.h`/`symbol_registry.h`。
- 独立 root 不加入游戏控件树；触摸应在 PopupState event 中用 `ui_hit_test` 分发，避免 root 无 Data 导致原生递归触摸链崩溃。

## 原版资源组件

`game_ui_components.h` 将原版资源和自绘降级路径分离：

- `ui_original::load_option_images` / `unload_option_images` 管理 Options 图像单元生命周期。
- `ui_original::draw_back_button(ctrl, pressed)` 先绘制 `0x59/0x3` 正常态；按下时再叠加 `0x59/0x2`，仅向左偏移 3 逻辑单位。
- `ui_original::draw_toggle(ctrl, enabled)` 使用标题图组 `0x0f` 开态，或 `0x10` 加 `0x9` 原版暗态叠加。
- `ui_original::draw_item_icon` 接收不可变 `ItemIcon` 描述符；调用方只能传入已确认的原版 `unit/loc/type/flip`，组件不自行绘制或猜测资源 ID。
- `ui_custom::draw_button` 是色块、边框和文字的独立自绘降级组件；资源型组件不得使用它来构造原版外观。
- 新增 `.cpp` 必须登记到 `module/app/src/main/cpp/CMakeLists.txt`。
