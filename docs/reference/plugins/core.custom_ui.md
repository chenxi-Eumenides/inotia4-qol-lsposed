# `core.custom_ui` 插件

## 1. 作用与 owner 定位

`core.custom_ui` 是普通核心共享 UI 工具/框架插件，注册 ID 为 `core.custom_ui`、版本 `0.1.1`。它拥有可复用 UI 组件、面板注册/导航、原生选择控件、原生确认框和文本机制；不拥有功能页面、交易规则或业务文案内容（架构边界见 [`ARCHITECTURE.md` §5.4、§7.2](../../../ARCHITECTURE.md)）。

## 2. 功能面

### Capabilities 与原生资源

| 面 | 版本 | 当前提供内容 |
|---|---:|---|
| `ui_components` | 1.22 | 通用组件与绘制/列表/导航接口；契约中的 `QolUiComponents` 由本插件实现（1.14 在旧尾槽后追加 3 槽：`capsule_text_button_create`、`decorated_text_button_create`、`main_menu_decorated_button_create`；容器 content rect/content draw 两槽已随要求 4 撤销删除）。此后追加可缩放图标按钮族与 `compact_capsule_text_button_create`；**1.19 追加 `forget_native`**：native 控件由外部（游戏控件树）销毁时只注销组件记录、不调用 `delete_control`；**1.20 追加锚定按钮**（`anchored_button_ensure/set_visible/release/draw`）：按 `QolUiRootKind` 每帧解析活挂载根、用 `click_binding_is_current` 证明句柄身份，失效时注销组件记录并重建；`unbind` 跳过 `native_externally_owned` 组件，不删除游戏拥有的 native。**1.21 追加任意原生控件锚定**（`anchored_control_ensure/set_visible/release/detach`）：多控件/自定义 `CO_PROC`/自带 data 的游戏树挂载，身份证明用 `control_proc_matches` + type/user_type/data 快照。**1.22 追加 `draw_rect`、`draw_rounded_rect`、`draw_circle`**：纯色 RGB565 填充/边框，alpha 为 0–100 百分比；圆角半径最多 256，圆形直径最多 512，不提供抗锯齿。 |
| `ui_panels` | 1.3 | 面板 provider 注册后的打开、关闭、状态/返回协作接口 |
| `ui_choice` | 1.1 | 原生选择框 open/active/close API |
| `ui_confirmation` | 1.1 | 单一 owner 原生 Yes/No 确认框 `open/active/close`；1.1 追加 `open_text`，正文使用显式字符串（`UIPopupMsg_CreateYesNo`），不依赖 TEXTDATABASE/TextLayer；自定义 panel 上仍由模块在帧内补绘原生外观 |
| `text_layer` | 1.0 | `MEMORYTEXT_GetText` hook 上的文本订阅/分派接口 |

提供版本见 `module/app/src/main/cpp/plugins/core.custom_ui/plugin.cpp`；`QolUiAdapter` 尾部追加：1.27 popup 活动状态、回调槽读取/条件替换、TEXTDATABASE Yes/No 创建与 owner-checked 关闭；1.28 overlay 作用域 `begin_overlay_scope/end_overlay_scope` 与绘制回调注册 `set_frame_overlay`；1.30 `call_ui_popup_msg_draw`（原生 `UIPopupMsg_Draw`）；1.31 `save_lcd`（主动 `GRP_SaveLCD` 快照）；1.32 `create_yes_no_text`（原始字符串 Yes/No 创建，type 固定 2）。1.29 的 raw 槽曾随 V2 移除并留空号，1.32 为本次重新引入的独立尾槽，序号不回退。这些槽是 provider 实现细节，消费者必须走 `ui_confirmation` capability，不得缓存或直接调用 callback trampoline。`ui_components` 契约保持 1.22，`ui.state` 保持 1.1。公共版本定义见 `contracts/ui.h`、`contracts/ui_confirmation.h`、`contracts/panels.h`、`contracts/ui_choice.h` 和 `contracts/text_layer.h`。本插件另声明四项必需资源：私有 `UIChoice_Init` entry hook、私有选择按钮 ptr slot、私有 `Scene_Draw_POPUP_SC_CHOICE` entry hook、共享 `MEMORYTEXT_GetText` entry hook；均为 required/fail-closed 且无 feature gate。`QolChoicePort` 1.1 尾追加 `draw` 原语（读 refresh flag / SaveLCD / 清 flag / GRPX Start·End / UIChoice_Draw），供选择框首帧去暗化 wrapper 使用。

### 原生确认框（`ui_confirmation`）

`UiConfirmationSpec` 接受 `text_id`、`display_type`、`format_arg`、`price`、确认/取消 callbacks 与消费者 `user` context。`price` 必须在 `0..INT32_MAX`，作为 `UIPopupMsg_CreateYesNoFromTextData` 的 `void* param` 数值编码传递；`user` 只保存在 `core.custom_ui` 的唯一活动状态中，绝不进入游戏 callback param。成功的 open 返回非零 `uint64_t` token；后续 active/close 必须携带该 token，wrong/stale token 返回错误且不操作弹窗。若 open 失败但清理未能完成、provider 仍保留 owner state，`out_token` 会返回非零 token，调用方可用它重试 `close(token)`；清理成功的失败 open 则返回 token 0。API 不携带扩展背包的 variant，也不实现价格、扣款或交易。

创建前检查全局 `UIPopupMsg_bOn` 和 provider 活动状态；已有原生弹窗或共享确认框时以 Busy 拒绝，不覆盖现有回调槽，也不把本 provider 的 trampoline 保存为旧 callback。callback trampoline 先读取 OK/Cancel pair；活动 popup 时只有两槽都仍属于本 provider，才条件恢复两槽并分发消费者 callback。活动状态下任一槽 foreign 时不写任何槽、不分发 consumer，并保留 closing owner。已确认 `bOn == 0` 时，只条件恢复仍匹配本 provider trampoline 的槽，foreign 槽保持不变，然后清理 owner state；`active(token)` 和新 `open()` 均可触发这项 inactive cleanup。成功分发前一次性清理 spec/context，并在锁外、主线程、最多调用一次；分发期间同步重开被 Busy 拒绝。

`close(token)` 与 disable/deactivate 的内部 owned-close 仅在两个 callback 槽都证明归本 provider 时调用原生 cancel/close 路径，不调用消费者取消 callback，也不以 `UIPopupMsg_Free` 代替关闭。native cancel trampoline 在关闭期间不清状态、不恢复槽、不分发 callback；外层只有在重新读取并确认 `UIPopupMsg_bOn == 0` 后才恢复仍双槽 owned 的 callbacks 并清 owner spec。close 失败或弹窗仍 active 时保留 token/spec、owned callback slots 和 closing gate，可由相同 token 或内部生命周期 close 重试；停用期间后续 trampoline 不分发业务 callback。旧的扩展背包 `create_confirmation/restore_confirmation` 专属路径与其交易逻辑保持独立、不迁移、不改动。

`user` 必须至少存活到确认/取消 callback 完成，或该 token 的 `close` 成功返回。价格 `0` 不绘制游戏价格栏；非零价格使用原生数值参数。调用者负责选择有效的 TEXTDATABASE 文本 ID，不应把 `user`、指针或代际值编码进 `price`。

`open_text`（capability 1.1）接受 `UiConfirmationSpecText`：首字段 `size`（必须 `>= sizeof`）、NUL 结尾且不超过 256 字节的 `message`、`display_type`、`price` 与同一组 callbacks/user。正文直接传给原生 `UIPopupMsg_CreateYesNo(text, len, 0, display_type, ok, cancel, param)`：第 3 参数 x2 转发给 CreateButtonControl 并固定 0；第 4 参数 x3 是 `i32DisplayType`（`0`=普通正文，`2`=价格布局），内部 i32Type 固定，无独立 type 参数，不经 TEXTDATABASE 或 `MEMORYTEXT_GetText`，因此无需 TextLayer。provider 在 open 时把 message 复制到内部静态 buffer，并保持到 `UIPopupMsg_bOn == 0` 且消费者 callback 栈退出后才清空；调用方可立即释放源字符串。长度、size、价格与 callback 校验失败返回 `kQolErrInvalidArgument`，绝不创建半成品弹窗。raw 与 TextData 共用同一 token/active/close、callback 槽恢复、Busy/ownership 与面板补绘语义。

**display_type 语义（反汇编事实，2026-10-22）**：`UIPopupMsg_Draw` 仅在 `i32DisplayType == 2` 时进入价格布局（先画价格标签，再在 `param > 0` 时画金额）；`display_type == 0` 为普通正文布局，完全无价格区。注意 `UIPopupMsg_CreateYesNoFromTextData` 的**第 3 原始参数**（现有 V1 调用中的 `format_arg=2`）才是 Draw 实际读取的 `i32DisplayType`，并同时影响文本格式化；其第 2 原始参数走按钮控件路径。因此纯确认框（无价格）应使用 `open_text` + `display_type=0`；带价格的 TextData 调用保持 `(text_id, 0, 2, ...)` 原参数。不要为隐藏金额向 param 写负值。`UIPopupMsg_Draw` 开头的 draw callback 槽非空时会绕过全部内建绘制，排查显示异常时先确认该槽归属。

示例（TextData）：`UiConfirmationSpec spec{9, 0, 2, price, on_confirm, on_cancel, context}; uint64_t token = 0; api->open(&spec, &token);`。
示例（raw 文本）：`UiConfirmationSpecText text{}; text.size = sizeof(text); std::snprintf(text.message, sizeof(text.message), "%s", "确认执行该操作？"); text.on_confirm = on_confirm; text.on_cancel = on_cancel; uint64_t token = 0; api->open_text(&text, &token);`。扩展背包当前 `create_confirmation(variant, price, ...)` 使用 text ID 9、display type 0、format arg 2；其 operation-specific callback template 与回调槽恢复仍由扩展背包专属端口管理，不因共享 API 改写。

#### V1 面板补绘（`ui_confirmation`）

V1 与 `open_text` 均创建游戏原生 `UIPopupMsg`（TextData/raw 路径、token/callback/close 语义一致）；补绘只依赖活动原生弹窗，因此两条创建路径共用同一机制。当打开时顶层活动 provider 有 `process` 回调且 overlay 作用域可用，`PopupHost::dispatch_process` 会 arm `frame_overlay` scope；宿主在 parent panel 自身 GRPX 帧的 `end_frame`（`GRPX_End` 之前）调用一次注册回调，回调仅在 `UIPopupMsg_bOn==1` 时调用 Adapter 1.30 `call_ui_popup_msg_draw()`，保留 100% 原生外观。成功绘制一次后才启用模态输入门禁；`bOn==0` 时结束 scope 并撤下门禁。补绘失败只记录日志，不改变 V1 token/close 语义。

输入语义：补绘成功且门禁启用后，F3（Return）只消费不下传 parent，由游戏 `UIPopupMsg_Process` 关闭弹窗；帧号绑定的 suppression latch 防止同帧双关。F4 与所有非触摸事件直接消费；触摸事件（`0x17/0x18/0x19`）也只消费，**不做模块 hit_test 分发**，原生按钮命中仍由游戏控件树处理。overlay 回调注册在宿主侧；注销失败进入 orphaned 状态，重启用时重试注册。

### 选择框背景、叠层与基础形状

`ui_choice` 只改写模块自有选项的 items/title/focus 并 push 游戏的 `F_PANEL_CHOICE_ENTER` PopupState；它不自行绘制背景。当前源码和符号登记没有找到 `Scene_Draw_POPUP_SC_CHOICE` 的绘制体，也没有证据定位调用方反馈的黑底来源、颜色或 alpha 参数，因此**不能安全地为原生选择框设置背景透明度**。`ui_components` 的形状 alpha 只作用于调用者主动绘制的矩形/圆角矩形/圆形，不会影响游戏原生 choice。

原生 choice 位于游戏 PopupState 栈顶，结构上可以覆盖底层原生页面；它不是 `ui_panels` 的自定义 panel 栈成员。现有资料不保证它与其他 provider 的自定义页面如何排序、是否遮罩输入，也没有嵌套页面真机证据；不得据此承诺任意自定义页面叠层或 z-order。`ui_confirmation` 使用另一套全局 `UIPopupMsg` 单例，活动原生弹窗时会 Busy 拒绝。**用户真机观察（2026-10-21）**：在自定义 panel 打开确认框时，`/api/ui/dialog` 显示 `type=popup, active=true`，输入被原生弹窗阻断，但弹窗视觉不可见；返回键会同时关闭确认框和 panel。同日只读设备 trace（68/68 渲染帧）证明根因是该状态下 `UIPopupMsg_Draw` 从未被调用（输入模态由 `UIPopupMsg_Process` 提供，panel 的 GRPX 帧照常渲染），并非 z-order 遮挡；见 `docs/refactor/verification/ui-state/20261022-dual-backend-confirmation.md`。该场景现由 V1 面板补绘处理：panel 帧内调用原生 `UIPopupMsg_Draw` 保留原版外观；真机视觉验收仍为 NOT_ACCEPTED。

`UiShapeDesc` 提供基础矩形、可调半径圆角矩形与圆形；调用方可设置逻辑尺寸、RGB565 填充/边框色、共同 alpha、边框宽度和圆角半径。alpha 为游戏 `GRPX_FillRectAlpha` 的 0–100 百分比；形状按互斥扫描线区域绘制，不重复合成边框/填充像素。栅格绘制不抗锯齿，圆角半径上限 256、圆形直径上限 512。

### Debug 专用入口

Debug 构建包含三个 POST 调试端点：`/api/debug/custom-ui/components`、`/api/debug/custom-ui/scroll`、`/api/debug/custom-ui/help`，分别驱动组件、原生滚动列表和帮助页探针。插件 descriptor 在 Debug 条件下声明 typed endpoint handlers；Host binder 按 descriptor lifecycle owner 接线。`routes.inc` 与 `core.api/routes.inc` 保存供同步/校验使用的 route facts，不由面板代码动态注册 HTTP handler。Debug 条件由 `core.custom_ui/CMakeLists.txt:11-19` 控制。

该构建还注册唯一的 debug `entry` feature `core.custom_ui.open_debug_components`，action label 为“打开”，callback 打开 `debug_components` 面板；另外的 `ui_tools` 是 Hidden toggle，不是入口项（`plugin.cpp:220-241`）。endpoint/feature 契约以路由元数据和 descriptor 为实现事实，API 公共契约以 [API 参考](../api-reference.md) 为准。

Debug Components 左栏的“基础形状”类别绘制矩形、圆角矩形、圆形和 alpha 示例；“原生对话框”类别提供原生选择框、无价格确认框、价格 100 确认框三个无业务副作用的检查入口；确认框在自定义面板上由模块补绘原生外观。确认/取消只更新面板状态，退出面板或停用时会关闭本 demo 自己打开的 dialog。

`PluginDescriptor` 通过 `http_endpoints` 声明这三个 Debug command routes（`plugin.cpp` 中的 `kHttpEndpoints` 与 descriptor 尾部）；各 Debug panel 注册函数只登记 UI panel provider 和 scheduler task，不另行向 `http_dispatch` 动态注册 HTTP route。

### 原生控件与面板绘制事实（2026-10-15）

以下为当前实现事实，供复核参考；本批已有 Host 定向测试与 Debug 构建证据，但**尚未部署、无手动/真机视觉验证，不能据此报告视觉 PASS**。

**Toggle（`game/access/ui_native_toggle.cpp:52-111`）**：按层绘制，每层源画布在基础 rect 内**居中**——`layer_x = x + (base.w − layer.w) * percent / 200`，纵向同理；`percent` 为统一缩放。按压层 `loc 7` 画布比基础画布每边多 8 px halo（262×103 对 246×87），若不按源画布居中而沿用 atlas 锚点，按压态会整体偏移。所有层统一使用基础层 `offset_x/offset_y`，不再取各层自身 offset。

**原生文本按钮（`game/access/ui_native_text_button.cpp:157-200,291-395`）**：按压态在同一源上叠加按压层——标题图 `loc 7` 的橙底白边胶囊，左右 cap 直接取两端，中段只平铺均匀橙色带；中段采样为 `sx = 100`（相对源 `[100,120)×[40,60)`：真实中央各行 RGBA 横向一致、alpha 0..10），Host 用例按 `sx 578 + 100 = 678` 断言。`sx = 30` 为**历史旧假设**（当时按 RGB 判断；旧『避开白色 dash bbox』只是 RGB 断言、不构成 alpha/可见性证据，也不改变历史测试结果）。不使用新填充色，也不调用全局 clip；按压层读取失败时按压绘制整体失败，正常态仍可绘制。同一 textbutton 类型的**两侧饰裁切由 rect 比例决定**（`height_percent <= width_percent` 或 rect 不小于源 405×77 时等比整体缩放含两侧饰，否则水平切片只压缩中间填充、保留圆角端），不是按 fontstyle 枚举分支。设置页 entry 动作按钮沿用 capsule 样式 `kUiTextButtonFontLarger` 原样（`plugins/settings/panels/settings.cpp:248-254`）。

**按压轮廓校准（2026-10-15 子集 A；脚本交付与用户安装已核对，视觉仍待验收）**：按压主体按**可见主体**对齐，而非整张带 halo 的画布。三层实际主体：**Title `loc 7` pressed** = 220×61；**Toggle normal / pressed** 均为 220×61；**UI_006 文本按钮 normal 裁剪主体** = 405×75。`75/61` 校准的是 **UI_006 normal 裁剪主体（75）与 Title `loc 7` pressed 主体（61）** 这两层，**Toggle 不参与 75 校准**（其两态已同为 61，见下条）（`ui_native_text_button.cpp:302-322`）。旧实现 `floor(floor(77*p/103)/10)*10` 把 103 画布压进 normal 守卫并向下量化到 10%，得到小一圈的轮廓；现改为 `round(75*p/61)`、不做 10% 量化，整按压画布宽度为 `normal_visible_width + 2*round(21*scale/100)`，中心取正常胶囊真实 body center，主菜单绘制 rect 分支不进入该算法；按压源画布最大 scale 由 75/61 比例导出为 492%，避免原本合法的大按钮按住时不可绘制。
**Toggle 本轮不改执行几何（用户已反馈开关表现正确；本轮无执行几何变化）**：`loc 7` 按压与 normal 的 alpha 主体均为 220×61，现有代码只用基底 scale 并把按压画布移 (-8,-8) 居中即已 body-to-body 重合；本轮只补准确注释与真实 alpha 边界回归测试，若按 262/246 或 103/87 放大反而会制造新的真实变大（`game/access/ui_native_toggle.cpp`；`tests/test_native_toggle_render.cpp`）。
**Phase 1C（要求 4 已撤销；源码已合流，撤销后未附新验证）**：保留双显式外观工厂 `capsule_text_button_create` / `decorated_text_button_create`（外观显式，不由 rect 比例推断；旧 `text_button_create` 比例自动分支与 `UiButtonDesc.style` 字体档语义保留）与 `main_menu_decorated_button_create`（真实菜单消费者仍是 settings）；Settings 全部 feature entry 统一走 capsule（保留 220×80 与 `kUiTextButtonFontLarger`）；Debug 文本按钮 6 枚（胶囊/装饰 × 可用/禁用/长文本）保留。**要求 4（容器/裁切/几何/编排）已撤销**并回到 `0ba1dad1^`＝`b3ecb441` 语义：`container_content_rect`／`container_draw_content`（含 `UiContainerContentDesc`／`QolUiContentDrawFn`）、Adapter 的 `get_clip_rect`、`game/access/ui_native_scroll.h` 的 `native_scroll_draw_clipped` 与 `F_UI_DRAW_SCROLLBAR_VMA`／`G_SGL_CLIP_RECT_GOT_VMA`／`fn_ui_draw_scrollbar`／`ui_clip_snapshot` 等新增符号全部删除；`QolUiAdapter::scroll_draw` 恢复直接调用原生 `fn_ctrl_scroll_draw`（`host/ui_adapter.cpp:684-689`）；`set_clip`／`reset_clip` 回到 `(x1,y1,x2,y2)` 与整屏复原语义。因此 `QolUiComponents` 1.14 现为**追加 3 槽**，`QolUiAdapter` 1.23 保持追加式布局：minor 21 的三槽、minor 22 的 UIDesc cursor/span 两槽，以及 minor 23 的 `draw_part_alpha`（`draw_part` 仍固定第七参为 0）。**主菜单按钮改为显式目标尺寸**：`UiMainMenuDecoratedButtonDesc` 为 `{text, font_style, font_size, width, height, on_click, user}`（不再有 `height_percent`），settings 传 **290×72**，工厂以原生控件中心为锚点直接使用该可见尺寸；**命中区仍等于原生 snapshot rect**（`paint_local` 按真实 control origin 相对计算），碰撞/几何拒绝时保留原按钮；字号 28 与按压动效保留。旧『272×64→272×96（`height_percent=150`）』『hit 为 normal/pressed 两态包围并集』『clip∩screen 保护 / rows·scrollbar 防泄漏 / Scroll A′ plumbing』等表述**均已废弃**，不得再作为当前实现证据。
历史注：本条在「子集 A」时曾写作『未纳入本批（双工厂/Debug 并列/menu 字号高度/容器 clip）』；Phase 1C 实现的双工厂与 menu 入口保留、menu 尺寸已改为显式 **290×72**，**容器/clip 部分已撤销**；视觉仍 NOT_ACCEPTED，撤销后未附新验证。`ui_native_close_button.cpp` 的 X 底板/整金属框按压与 `draw_back_button` 外框效果仍不在本阶段。

**旧主菜单替换入口（兼容路径）**：`text_button_draw` 只在绘制上使用 decorated 紧凑比例——`height = min(rect.h, rect.w × 48 / 220)` 并垂直居中，背景与标签共用该绘制 rect；原生菜单控件 rect、命中范围与 identity 判定不变（绑定/存活校验仍走 `main_menu_text_button_*` 原路径）。Settings 当前入口已迁移到上述显式 decorated 工厂。

**面板四角装饰**：`help_page` 左右两栏均改为 `draw_container_ornamented`（四角 ornament，`plugins/core.custom_ui/ui_components.cpp:1480,1601-1603`）；`draw_container` 仍保留给无装饰容器。Debug 双栏对照示例（`plugins/core.custom_ui/panels/debug_components.cpp:302-303`）本就用 ornamented，本次不改；外壳/Frames 对照示例同样不改。设置页两侧 pane 的同类改动见 [`settings.md`](settings.md) §4。

前轮历史参考证据（**非本批按压修复的构建/部署**）：Host 定向 7/7（`core_custom_ui_host_tests`、`ui_adapter_tests`、`native_toggle_render_tests`、`native_text_button_render_tests`、`core_ui_components_tests`、`settings_panel_widget_tests`、`settings_panel_model_tests`，日志 `.tmp/custom-ui-polish-20261015/final-ctest.log`）；`scripts/build-debug.sh --offline` 构建成功，产物 `output/inotia4_qol_lsposed_debug_2610150537_3cc7410c87df.apk`（SHA-256 `3cc7410c87df6871ac07c7b4626b5cec497e4689ef027108ba5de65c52255670`，`.tmp/custom-ui-polish-20261015/final-build-debug.log`、`final-apk.json`）。本题材为 **monster v31 简中**：`monster-atlas-evidence.json` 与 `monster-center-strip-evidence.json` 确认 `loc 7/9/f/10` metadata 与标题 atlas、`UI_006` 与 overhaul 对应一致（每项 sha 相同）；不据此扩到其他 variant。构建成功与素材核对**不等于真机或像素验收**，视觉结论仍为 **NOT_ACCEPTED**。（输出目录内其他 release 产物为既有文件，本批未重建、未改动，也未做 Release 运行。）
本批按压修复的当前证据：`.slim/deepwork/ui-craft-regression-20261015/pressed-fix.md` 记录的 **4/4**（`ui_adapter_tests`、`native_toggle_render_tests`、`native_text_button_render_tests`、`core_ui_components_tests`，日志 `.tmp/custom-ui-components-20261015/pressed/final-ctest.log`），以及主代理当前整树的 UI 相关 target 通过（`141/143`，红项与本批无关）；本批 Debug 构建成功（`output/inotia4_qol_lsposed_debug_2610151556_ed9081c4c54e.apk`，module SHA-256 `ed9081c4c54e4bd5e0829ae8749d58d6a70e5101a0428f131cb299e953d29fd2`），脚本打包 4/0 成功并由**用户手动安装**（主代理只读校验设备 base APK sha 与 canonical 一致、`running-module.log:33` identity=module；`.tmp/ui-craft-regression-20261015/deploy/identity-summary.json`）。**用户观察**：大小/位置**正确**；两横线间黄色块**已定位为真实 alpha 采样缺陷**（v29 的 Title/UI006 素材字节哈希与 v31 相同；中段 `[30,50)×[40,60)` RGB 单一但 alpha 为 2..82、共 80 种 RGBA，每片末 alpha 2 跳回起点 71 造成周期性黄块，分数缩放下的片间重叠为次要因素；候选 `[100,120)×[40,60)` 同 y 各行 RGBA 横向一致、alpha 0..10）。**Phase1B 已实施最小修复**：生产仅把中段采样 `sx 30→100`（`ui_native_text_button.cpp:343-348`），几何（75/61、492%、body center、halo、cap、rect/hitbox）与 Toggle 均不变；定向 Host **4/4 PASS**（`ui_adapter_tests`、`native_toggle_render_tests`、`native_text_button_render_tests`、`core_ui_components_tests`），并有独立 fixture（`tests/fixtures/native_text_button_pressed_rgba.inc`：v29 真实 PNG 506 像素逐点核对、旧 edge alpha 重置 69 负对照、透明白合法）。新候选包 `output/Inotia4_v1.3.2_monster_v29_lspatched.apk`（SHA-256 `6598fc5f919f71547db2b4c743793cdf925e4ee1c8cf9ac6ff3ffbddc473026d`，内嵌 module `cdeb2118fb00ace445038ca26c27025c97abdc5dfcd41a273ba6fe6121660ecc`）**尚未安装**，在机运行仍是先前已装包（module `ed9081c4…`），无新 VM。几何冻结。整体视觉仍 **NOT_ACCEPTED**；不得用前轮 7/7 或旧 APK 作为本批新源码验证。

前述按压与面板交付记录中的包名、SHA 和在机状态属于各自历史时点；脚本会覆盖同名集成包。当前选择修复候选以本小节末尾为准。

### 模块选项响应与原生旁路（2026-10-16，两条独立根因）

**现象**：原生选项框（含地图传送目的地）触屏有 pressed 特效但无结果。两条根因**互相独立**，不能用同一解释覆盖。

**用户政策（2026-10-16）**：**原版选项对话框不改动、绕过去；只接管模块插件自己的选项对话框。** 因此**不改动原生选项框本身**（只让原生旁路能调用原函数），模块只接管自己拥有的那部分；**原生透传只发生在 `inactive` 或明确 `foreign`**。

- **A（Host 侧，feature-mode `kPtrSlot` backup 断链）**：`dispatch_install` 的 `kPtrSlot` 分支原先把 `patch::install_claim` 的 `Outcome` 直接转成 `MechanismOutcome`，丢掉 `site.expected_value`（该值正是被替换的原函数入口）；`MechanismOutcome` 缺少原值字段，`install_other_step` 又把 `TxStep.backup` 写死为 null，于是 registry 不调 `on_resource_ready`，`g_button_original` 保持 null，原函数委托变成空操作。
  修复（Host）：`TxDeps::MechanismOutcome` 尾追加默认 null 的 `backup`；PtrSlot 成功回填 `site.expected_value`；引擎成功步透传到 `TxStep.backup`；不改 registry 生产 gate、不改公共 header。该 Host 修复**保留**，但其目的仅是让**原生旁路能真实调用原函数**，不改变 Native 业务语义或调用协议。**注意**：`world_teleport` 的**模块 active 成功分支不依赖 backup**，其目的地问题见 B，不可照抄同一原因。
- **B（模块侧，`UIChoice_Init` 覆写）**：v29 `Scene_Init_POPUP_SC_CHOICE` 按序调用 `UIChoice_Init` → `UIChoice_CreateControl`（两者是两个独立函数，中间无第三函数）；`CreateControl` 只读 count/items/title 等全局，是纯消费者。旧模块在 push 前写入 items/count/focus，被 `UIChoice_Init` 清写覆盖，导致 `text_owns` 失败、目的地无反应。
  修复（`plugins/core.custom_ui`，`choice_init_wrapper`，按函数名引用）：`original(control)` **之后**（Init 与 CreateControl 之间）恢复模块自有的 items/count/focus 及可选 title，**有/无 title 都要恢复**；限定为**进入 wrapper 之前已存在的 pending 且同一 generation**。判定顺序：**先 ownership 判定**——仅**明确 foreign** 才把控制权交回原函数；ownership **读取不明**时只报错，不猜、不写。
  已确认 **module owned** 时：成功、关闭、读取错误、索引错误、focus 错误**都不得运行原版剧情回调**；**选择关闭由原生 `UIChoice_ButtonListExe` 排队唯一一次 pop；模块只清自身状态、不重复 `close_popup`、不触发 `on_select`**（模块主动关闭 API 仍保留关闭能力，同 generation 仅一次）；失败只记原因并保活，等待用户下次点击。**原生只在 `inactive` 或明确 `foreign`（`text_owns=0`）时透传**；**ownership 未知/读取失败保持 module 路径、不写原生**。wrapper 先捕获 `original` 与 spec/generation/pending，再调原函数，随后锁内核对同 generation、锁外调用 port 写入；无标题 spec 同样恢复 items。无新 hook/resource/port/公共 ABI/调度器，回调签名保持 `void`，调用点不使用其返回值。
- **日志**：模块 blocked 用 `choice button module blocked reason=…`（键：`owner_unavailable`、`owner_read_failed`、`missing_module_fn`、`selected_index`、`index`、`focus`）；foreign 旁路用 `choice button foreign passthrough text_owns=0 …`；成功用 `choice button module <select|close> index=N`。旧 `fallback reason` 关键词已删除，`original=null` 警告仍在。**不记录玩家文本**。
- **证据与状态**：`native_choice_tests` **200 passed / 0 failed**（`.tmp/choice-dialog-fix-20261016/module-only/01-run.log`）；`hooks_transaction_tests` **543 checks 通过**，未重复运行无关测试或增加独立复核。`scripts/build-debug.sh --offline` 成功（7 秒），模块 `output/inotia4_qol_lsposed_debug_2610161506_a149d8807dd7.apk`，SHA-256 `a149d8807dd762d102ee362c7c394582a83ad8e2a32a033f3ab0c7cae64a81ab`；直接 `scripts/patch-apk.sh` 成功 **4 / 失败 0**，当前候选 `output/Inotia4_v1.3.2_monster_v29_lspatched.apk`，SHA-256 `c06a849123edfd5f43023cd0f83bf1b3e2a8315c1200181a7c02e42ad19e41db`。脚本日志在 `.tmp/choice-dialog-fix-20261016/`。**未安装或重启、未执行选项操作**，构建身份不是运行身份；选择 VM **NOT_ACCEPTED**，由用户亲自验证。3 条资源声明、capability 版本及发布版本均不变。

## 3. 使用的模块功能、依赖与消费者

- **Requires / optional**：`ui.state` 1.0 为可选依赖；Debug 构建对 `http.registry`/`http.dispatch` v1.1 的依赖也为可选，非 Debug descriptor 不声明 HTTP 依赖（`plugin.cpp:70-79,247-253`；版本见 `contracts/http.h:96-99`）。provider 缺失只使 Debug HTTP 探针无法接线，不影响共享 UI 功能；provider 恢复后按可选依赖生命周期重新接线。选择框向 `ui.state` 发布自定义 dialog snapshot；缺少该 capability 时 native choice 本身仍按本插件路径工作（`feature.cpp` 的 `ui_state_api`、`publish_dialog_snapshot`）。
- **Required port**：`QOL_PORT_CHOICE` 1.0，提供原生选择框状态/选项/标题/焦点/弹出等受控操作（`plugin.cpp:134-137`；版本见 `contracts/game_ports/ports.h:17,47`；接口见 `contracts/game_ports/choice_port.h:11-30`）。
- **Host SDK**：使用 Host 注入的 `sdk->ui` Adapter（当前契约 1.31，兼容布局按 minor/size 检查；minor 23 追加 `draw_part_alpha`；minor 27 popup 活动状态/回调槽/创建/关闭；minor 28 overlay scope 与绘制注册；minor 30 原生 `UIPopupMsg_Draw`；minor 31 主动 `GRP_SaveLCD`），以及 `panels`、`hooks.text_subscribe/run_text`、descriptor endpoint handlers、scheduler/log（`contracts/ui.h:3-6,15-19`；`plugin.cpp` 中的 `kHttpEndpoints`；`text_mechanism.cpp:27-46`）。
- **已核实消费者**：`ui_components` 与 `ui_panels` 由当前 `plugins/settings/` 设置页消费；`ui_panels` 还由 `save_backup`、`autosell` 使用；`ui_choice` 被 `world_teleport` 作为可选能力消费；`text_layer` 被 `custom_recipe`、`character_panel_i18n` 要求（`plugins/settings/plugin.cpp:32-38`、`settings/panels/settings.cpp:816-821`、`save_backup/plugin.cpp:31-35`、`autosell/plugin.cpp:196-204`、`world_teleport/plugin.cpp:46-61`、`custom_recipe/plugin.cpp:168-178`、`character_panel_i18n/plugin.cpp:33-36`）。
- **真实设置页边界**：页面实现位于 `plugins/settings/`，descriptor ID 为 `settings`，只要求 `ui_panels` 与 `ui_components`；`core.custom_ui/settings_contribution.cpp` 当前不注册设置项（`plugins/settings/plugin.cpp:19-20,32-38,238-255`；`core.custom_ui/settings_contribution.cpp:4-8`）。

## 4. 职责范围

- `core.custom_ui` 提供组件、面板/导航、原生选择框、单 owner 原生确认框和文本机制；具体页面、页面状态、交易与业务文案由各自插件拥有，设置页归注册 ID 为 `settings` 的 `plugins/settings/`（`ARCHITECTURE.md` §7.2）。
- `core.ui_state` 拥有游戏界面状态/操作；`core.custom_ui` 只通过可选 `ui.state` 发布自定义 dialog 状态，不接管该领域 owner（`feature.cpp` 的 `ui_state_api`、`publish_dialog_snapshot`；架构边界见 `ARCHITECTURE.md` §7.2）。
- UI Adapter/Game Adapter 承担底层游戏控件、绘制与资源原语；本插件通过 contracts/Host SDK 使用受控接口，不承载通用游戏语义（`contracts/ui.h:3-6,304-315`；`ARCHITECTURE.md` §4.3、§7.2）。
- Debug 面板与 endpoint 是 Debug 构建探针，不等同于功能页面或 release 对外业务接口（`CMakeLists.txt:11-19`；`plugin.cpp:74-79,220-241`）。

## 5. 对外提供与依赖影响

五项 capability 随普通插件启停发布/撤销。当前外部 consumers 如 §3 所列；`ui_choice` 的可选消费者 `world_teleport` 不把它作为 install 前提。停用时关闭选择框与确认框门禁，确认框仅关闭 callback 槽证明为本 provider 所有的原生弹窗；同时停用文本分派，由 Host binder 撤销该 descriptor 的 Debug routes，并解除面板/SDK 绑定。

## 6. 读写线程与生命周期机制

- **UI、选择框与确认框**：`contracts/ui.h` 明确 Host UI Adapter 接口仅可在游戏主线程调用；`UiChoiceSpec.on_select` 与 `UiConfirmationSpec` callbacks 均在主线程回调。确认框 provider 以 mutex 保护唯一 active spec、user context、generation 和关闭状态；native popup 读写经 minor 27 Adapter slots，callback 先核验并条件恢复槽位、清状态后锁外分发。
- **Debug HTTP 写路径**：descriptor command handler 只验证请求并写原子请求状态；UI 面板开关交由 scheduler task 执行。三个 Debug 面板都有该“HTTP 请求→调度任务→面板”路径（`panels/debug_components.cpp`；`debug_scroll.cpp`；`debug_help.cpp`）。
- **文本读路径**：wrapper 先调用原 `MEMORYTEXT_GetText`，若文本机制 active 再同步调用 Host `run_text` 分派，失败/空输出回退原文；订阅由 Host hooks API 登记/撤销（`text_mechanism.cpp:27-64`）。源码没有为该 hook 回调另行声明一个通用线程保证，本说明不把它扩写为 UI 主线程接口。
- **生命周期**：install 绑定 UI/面板/文本 API、确认框 Adapter 与 required choice port；enabled/disabled 分别打开/关闭 choice、confirmation 与 text gate，confirmation disabled/deactivate 时仅尝试关闭自身确认框；Debug 构建同步启停调试面板。deactivate 撤销 Debug 路由/面板、停用文本分派并清理绑定。

## 7. 文件地图

| 子职责 | 文件 | 关键事实 |
|---|---|---|
| descriptor、provides/requires/port、资源与生命周期 | `module/app/src/main/cpp/plugins/core.custom_ui/plugin.cpp` | ID/version/capabilities：21-79；resources/port：81-137；install/lifecycle：141-202；Debug entry：220-267 |
| 组件接口实现 | `module/app/src/main/cpp/plugins/core.custom_ui/ui_components.cpp`、`ui_components.h` | 组件对象/原生 adapter 绑定：`ui_components.cpp:20-78,124-130`；接口：`ui_components.h:13-45`；文本按钮/菜单 decorated 工厂：`645,862-872`；基础形状扫描线绘制：`ui_components.cpp:2183-2410`；help 双栏 ornament：`1480,1601-1603` |
| 原生 toggle 绘制 | `module/app/src/main/cpp/game/access/ui_native_toggle.cpp` | 分层源画布居中与 halo：`52-111` |
| 原生文本按钮绘制 | `module/app/src/main/cpp/game/access/ui_native_text_button.cpp` | 比例裁切/切片判定：`157-200`；按压几何/按压层：`291-395`（当前中段 `sx=100`；旧『避开烘焙 dash』属 RGB 断言，非 alpha 证据） |
| 原生选择框 | `module/app/src/main/cpp/plugins/core.custom_ui/feature.cpp`、`feature.h` | `choice_init_wrapper`、`choice_button_execute`、`native_choice_open/close`；锁状态、port 读写与 dialog publish |
| 文本共享机制 | `module/app/src/main/cpp/plugins/core.custom_ui/text_mechanism.cpp`、`text_mechanism.h` | subscribe/unsubscribe 与原文 fallback dispatch：`text_mechanism.cpp:27-64` |
| Debug panels/endpoints | `module/app/src/main/cpp/plugins/core.custom_ui/plugin.cpp`、`panels/debug_components.cpp`、`debug_scroll.cpp`、`debug_help.cpp` | descriptor endpoint facts/handlers 位于 plugin.cpp；各面板源码实现 command callback 与 scheduler task |
| 跨插件契约 | `module/app/src/main/cpp/contracts/ui.h`、`panels.h`、`ui_choice.h`、`text_layer.h`、`game_ports/choice_port.h` | adapter/components 版本与线程说明：`ui.h:3-19`；其余 capability ID/version 与接口见各文件开头定义 |
| 原生确认框能力 | `module/app/src/main/cpp/contracts/ui_confirmation.h`、`plugins/core.custom_ui/ui_confirmation.cpp` | 固定宽度 V1 spec、单活动 owner、条件 callback 槽恢复、原生 close path、V1 面板补绘与模态门禁 |
| 确认框 overlay 作用域 | `plugins/core.custom_ui/panels/frame_overlay.{h,cpp}`、`panels/host.{h,cpp}` | 一次性帧作用域（token+provider+generation）、嵌套 sentinel、宿主 `end_frame` 前单次补绘、F3/F4/event 模态门禁 |

| 当前设置页消费者 | `module/app/src/main/cpp/plugins/settings/plugin.cpp`、`plugins/settings/panels/settings.cpp` | descriptor requires：`plugin.cpp:32-38`；运行时 require ui_components：`settings.cpp:816-821` |


## 控件挂载归属与生命周期规则（强制）

游戏会随面板关闭/切页销毁原生子树，插件若把句柄跨帧保留，就会拿到失效指针；它的地址还可能被之后创建的控件复用（实测扩展背包页签复用了自动出售入口的地址）。因此**任何跨帧保留的控件句柄都必须能证明存活**。规则按 native 归属划分：

| 挂载方式 | native 由谁销毁 | 必须使用 |
|---|---|---|
| 自建 root（`panel_create`/`create_control(0)`），插件自己 `destroy` | 插件 | 常规组件 API（`panel_create`/`button_create`/…）；退出时 `destroy` |
| 借用自建原生 root（页面自建 `g_root`，先 destroy 子组件再删 root） | 插件 | 常规组件 API，并保证销毁顺序（先 `destroy` 子组件，再 `delete_control(root)`） |
| 游戏原生子树（袋容器、商店/合成器容器）中的单图标按钮 | 游戏 | `anchored_button_ensure` + `anchored_button_draw`；关闭态用 `anchored_button_set_visible(0)` |
| 游戏原生子树中的多控件/自定义 `CO_PROC`/自带 `control_data` | 游戏 | `anchored_control_ensure`；删除前用 `anchored_control_detach` 取回已证明存活的句柄 |
| 主菜单入口（更多游戏右侧齿轮） | 游戏 | `anchored_button_ensure/draw` + `QolUiRootKind::kMainMenuParent`（Adapter 用主菜单快照证明父控件存活） |

### 实现约束

1. **每帧重新解析挂载根**：用 `QolUiAdapter::find_root(kind)` / `main_menu_more_games_snapshot`，不得缓存 root 指针做跨帧判断。容器指针可能不变而子树已被重建，所以 root 比对不能单独作为存活判据。
2. **先指针、后字段**：校验顺序必须是「句柄在活父控件子链表内（纯指针比对）→ 再读 type/user_type/data/CO_PROC」。对可能已失效的句柄直接 `control_data`/`get_rect`/`set_rect` 会命中已释放内存。
3. **失效只注销记录**：`forget_native`（组件）或 `anchored_*_release`（原生控件）。**绝不 delete 未证明存活的句柄**；`anchored_control_detach` 只在 `out_live=1` 时交出可删除句柄。
4. **身份判据**：
   - 组件按钮（`ui_components` 创建）：Adapter `click_binding_is_current`（绑定元组 + ExecuteProc 仍是 adapter trampoline）。
   - 原生控件：创建时捕获 `type` + `user_type` + `control_data`，配合 `control_proc_matches`（`CO_PROC==+0x90`）。地址复用后这组字段必有一项不符。
5. **`NotReady` 不是死亡**：内存不可读/主线程不匹配时只表示无法证明，调用方必须保留记录，不得删除或改写控件。
6. **unbind 不删除游戏拥有的 native**：锚定层创建的组件标记 `native_externally_owned`，`ui_components_unbind` 跳过它们，避免对已销毁地址重复 `delete_control`。

### 当前使用方

- `autosell` 背包入口：`anchored_button_*`（`plugins/autosell/ui/panel.cpp`）。
- `extension_bag` 装备页/商店/合成器页签：`anchored_control_*`（`runtime/tabs_ui.cpp`、`ui/store_tabs.cpp`、`ui/mix.cpp`），descriptor 可选依赖 `ui_components`。
- 其余自建 root 面板（autosell 配置面板、save_backup、settings、debug 面板）：常规组件 API，无需锚定。
- 主菜单入口齿轮：`plugins/settings/panels/settings.cpp` 的 `settings_main_menu_more_games_draw`（原版按钮照常绘制 + 锚定齿轮 68×68、原按钮右侧 +10、垂直居中）。
- `main_menu_text_button_is_live` 仍保留为 Adapter 的菜单身份证明原语（当前生产入口不再使用替换路径）。

新增游戏树挂载时必须选择上表机制；不得再手写「root 指针相等即认为句柄有效」的判断。
