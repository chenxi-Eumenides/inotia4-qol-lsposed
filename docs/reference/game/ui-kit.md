# 可复用原生 UI 组件

## 目标

`game/access/ui_kit.{h,cpp}` 是轻量的游戏内 UI 组件层，不提供完整布局系统，只封装创建少量原生风格控件所需的重复样板。

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

PopupState 表的注入不再由 `ui_kit` 提供。按 enter 符号定位与「一 entry 一 owner」的事务占用、
快照/写回由 `hooks/popup_state`（机制层，经 `game/access/popup_state` 访问原版 27×0x40 表）
负责；面板插件以资源声明方式消费，见各 `plugins/*/plugin.cpp` 的资源表。

## 约束

- 坐标必须使用游戏逻辑坐标，并通过已有 `CalcResolutionWidth/Height` 适配 root。
- 游戏结构偏移和 VMA 仍只允许来自 `game_symbols.h`/`symbol_registry.h`。
- 独立 root 不加入游戏控件树；触摸应在 PopupState event 中用 `ui_hit_test` 分发，避免 root 无 Data 导致原生递归触摸链崩溃。

## 原版资源组件

`game/access/ui_components.h` 将原版资源和自绘降级路径分离：

- `ui_original::load_option_images` / `unload_option_images` 管理 Options 图像单元生命周期。
- `ui_original::draw_back_button(ctrl, pressed)` 先绘制 `0x59/0x3` 正常态；按下时再叠加 `0x59/0x2`，仅向左偏移 3 逻辑单位。
- `ui_original::draw_toggle(ctrl, enabled)` 使用标题图组 `0x0f` 开态，或 `0x10` 加 `0x9` 原版暗态叠加。
- `ui_original::draw_item_icon` 接收不可变 `ItemIcon` 描述符；调用方只能传入已确认的原版 `unit/loc/type/flip`，组件不自行绘制或猜测资源 ID。
- `ui_custom::draw_button` 是色块、边框和文字的独立自绘降级组件；资源型组件不得使用它来构造原版外观。
- 新增 `.cpp` 必须登记到 `module/app/src/main/cpp/CMakeLists.txt`。

## 原生文本按钮

`core.custom_ui` 的 `text_button_create` 创建真实 `ControlButton`，保留引擎命中区域、按下/抬起取消和 ExecuteProc 点击派发。组件持有 UTF-8 标签；绘制时不写入 32 字节原生文本槽，Game Adapter 按当前游戏字体行高预算标签宽度，并在胶囊可见区域内居中；绘制沿用调用方现有裁切，不重置滚动视口。按钮矩形至少为 `204×48` 逻辑单位，使原生底图不会因宽度限制缩成细条。`text_button_set_enabled` 同步原生按钮 Enabled 位并阻止回调，`text_button_draw` 绘制按钮与标签；`button_set_text` 可更新标签，销毁按钮会递归释放其控件绑定。

按钮底图由 Game Adapter 从共享图像单元 `0x12`、位置 `0x01` 借用。Game 层使用该位置中灰色胶囊主体的源区域（`x=35..439, y=146..222`，半开区间 `x=35..440, y=146..223`），已含圆角金边、必要阴影与一圈黑色采样 guard，排除更外侧的透明留白和两侧装饰；胶囊较窄时保留 100 px 圆角端部，并从横向同色区域裁取 70 px 中间填充。切片缩放使用偶数百分比，使三个源片映射到整数目的宽度，接缝连续且不重叠。主体目标大小取自同一矩形下原生开关的真实绘制 alpha 主体；例如简中开态 unit `0x54`, loc `0x0f` 的 metadata `SrcRect={2,491,246,87}`，alpha 主体 bbox 相对 metadata 为 `{13,13,220,61}`。裁剪高度 77 与偶数百分比下高度误差 ≤1 仅允许 `70%` 一档，该档位下整数片宽要求端部与中间填充源宽为 10 px 的倍数，因此胶囊绘制宽度只能按 7 逻辑单位步进（`70 + 49 + 70 = 189` 或 `70 + 56 + 70 = 196`）；可见目标宽度取 `212`（映射 189 档），绘制时整体右移 3 逻辑单位使胶囊在行内保持居中，标签相对胶囊中心偏移不超过 1 逻辑单位。设置行 `{220×80}` 下胶囊主体为 `189×54`，高度与开关主体（`196×54`）一致、每侧各窄约 3.5 逻辑单位。胶囊比例合适时仍按原图比例绘制，因此 `220×48` 调试示例不变形。共享图像单元只确保加载，不由按钮卸载。文字按钮的字号由 `UiButtonDesc.style` 选择：`kUiTextButtonFontDefault`(0) 对应原生 font 0（行高 22），`kUiTextButtonFontLarger`(1) 对应原生 font 1（行高 26）；设置页「打开」动作使用后者，调试示例与其它调用保持默认档。按下时组件依据原生按压状态调整文字颜色，禁用态使用禁用文字颜色，不借用开/关或未经确认的分片。

## 原生 X 关闭按钮

`core.custom_ui` 的 `close_button_create` 创建真实 `ControlButton`，引擎事件树保证命中、按下/移出取消与 ExecuteProc 点击派发；组件只提供原生视觉、按下态与 `on_click` 回调，关闭/退出语义由消费者决定。`close_button_set_enabled` 同步原生 Enabled 位并阻止回调，`close_button_draw` 读取 control_pressed 绘制对应状态。

外观由 Game 层 `ui_native_close_button` 从共享图集 unit `0x0f`（descriptor 文件名 UI_003.png，explorer 静态解析与 PNG 目视双重确认）借用：常态 loc `0x3a` sourceRect `{234,441,38,39}`（米金 X 含投影），按下叠加 loc `0x34` sourceRect `{275,419,62,61}`（橙底圆角方块+白 X）；两图以常态图的等比缩放档绘制、中心对齐（与 UIGameMenu_DrawBackButton 的原版组合一致，源高度奇偶带来 ±1 px），经 `SGL_DrawTexturePartEx`（mode 1、alpha 0、rotation 0）绘制，不修改全局 scale/clip。源矩形运行时从 loc 内存读取，不写入编译期常量。unit 只确保加载，任何路径不得卸载。背包/人物信息页左上角 X 即此资源；Options 返回箭头（unit `0x59` loc `3`/`2`）是另一资源，不复用本组件。

## 自适应导航按钮

`core.custom_ui` 的 `nav_button_create` 融合 X 与返回箭头，按 `UiNavButtonMode` 选态：`kUiNavButtonAuto` 绘制时查面板栈深（`ui_panels_depth()`），栈顶仅一层显示 X（复用关闭按钮的 `0x0f/0x3a` 常态 + `0x34` 按下叠加路径），多层级显示返回箭头（复用原版 `draw_back_button` 的 `0x59/loc 3` 常态 + `loc 2` 按下叠加路径）；`kUiNavButtonForceClose` / `kUiNavButtonForceBack` 覆盖自动判定，供特殊页面与演示。点击一律经 `ui_panels_request_back()` 延迟请求弹面板栈顶（帧安全点执行；栈顶仅一层即退出本页，与返回语义一致），`on_click` 为可空的消费者附加通知，在弹栈请求之后调用。`nav_button_set_enabled` 同步原生 Enabled 位并阻止点击，`nav_button_draw` 读 control_pressed 透传两条绘制路径。unit `0x0f` 与 `0x59` 只确保加载，任何路径不得卸载。

## 调试双栏内容页

`debug_components` 面板（provider id 与 `/api/debug/custom-ui/components` 不变，仍受 `QOL_DEBUG_COMPONENTS` 保护）采用主从双栏结构：左栏为分类原生滚动列表（`help_page` 的 `scroll_list`，行高 56，行自绘分类名），右栏为所选分类的内容区。分类当前为文本按钮、开关胶囊、X/导航按钮、主题调色板、框架边框；新增调试内容 = 增加一条分类名并在 `build_category_controls`（交互控件）与 `draw_category_detail`（静态绘制）各加一个分支，两类内容互不冲突。切换分类只在 `panel_process` 帧安全点重建右栏交互控件（轮询 `help_page_get_selected` 后销毁/创建，事件回调内不重建）；纯绘制分类由 detail 回调直接绘制。左栏滚动显式 `help_page_process/draw`，右栏交互控件经原生事件树或手工命中派发，二者不混用。右上角为自适应导航按钮作为面板关闭入口（栈顶仅一层显示 X，多层级显示返回）。
