# `core.custom_ui` 插件

`core.custom_ui` 是普通核心共享 UI 工具/框架插件：注册 ID `core.custom_ui`、版本 `0.1.1`。它拥有可复用 UI 组件、面板注册与导航、游戏原生选择框、单 owner 原生确认框、文本机制与共享形状绘制；不拥有功能页面、游戏业务语义、交易规则或业务文案（边界见 [`ARCHITECTURE.md` §5.4、§7.2](../../../ARCHITECTURE.md)）。

本文件是该插件的设计、架构、能力与职责的唯一权威说明。公共 API 的完整 schema 见 [`api-reference.md`](../api-reference.md)；验收状态见 [`refactor/verification.md`](../../refactor/verification.md)，本文件不登记验收结论。

## 1. 对外能力

### 1.1 Capability

| capability | 版本 | 内容 |
|---|---:|---|
| `ui_components` | 2.23 | `QolUiComponents`：原生控件/组件创建与销毁、文本与列表、滚动列表、帮助页、导航/箭头/图标/文本按钮工厂、锚定控件、容器与形状绘制；Major 2 起唯一容器入口为 `draw_container_styled`（minor 23 槽） |
| `ui_panels` | 1.3 | `UiPanelsApi`：`open/close/active/block_reason/deactivate_owner/back`，逻辑栈深 4 |
| `ui_choice` | 1.1 | `UiChoiceApi`：原生 UICHOICE 的 `open/active/close`；1.1 尾追加稳定 `dialog_type_id` |
| `ui_confirmation` | 1.1 | `UiConfirmationApi`：单 owner Yes/No 原生弹窗的 `open/active/close`；1.1 尾追加 `open_text`（显式正文） |
| `text_layer` | 1.0 | `TextLayerApi`：`MEMORYTEXT_GetText` 单一 owner 上的文本订阅/撤销 |

版本常量位置：`contracts/ui.h:16-19`、`contracts/panels.h:17-18`、`contracts/ui_choice.h:22-23`、`contracts/ui_confirmation.h:12-13`、`contracts/text_layer.h:11-12`。发布点在 `plugin.cpp:48-75`。

### 1.2 Host 注入的 Adapter

`QolUiAdapter` 由 Host 实现，当前契约 2.34（`contracts/ui.h:17`；Major 2 为容器 API 统一的 ABI break），按 major/minor/size 兼容检查，只允许尾追加。本插件使用的槽位分组：

| minor | 槽位组 | 用途 |
|---:|---|---|
| 基线 13 | 控件创建/销毁、矩形、文本、绘制、命中、根解析、点击绑定、主线程判定 | 组件与面板基础能力 |
| 21–23 | 原生文本按钮 layout/draw、滚动绘制、`draw_part_alpha` | 原生控件渲染与部分绘制 |
| 25–26 | 锚定身份证明（`click_binding_is_current`、`control_proc_matches`） | 游戏控件树挂载 |
| 27 | popup 活动状态、回调槽读取/条件替换、TEXTDATABASE Yes/No 创建与 owner-checked 关闭 | 原生确认框 |
| 28 | `begin_overlay_scope/end_overlay_scope`、`set_frame_overlay` | 面板帧内补绘 |
| 30–32 | `call_ui_popup_msg_draw`、`save_lcd`、`create_yes_no_text` | 确认框外观与原始文本创建 |
| 33 | `use_control_event_on` | 原生控件事件位恢复 |
| 34 | `draw_container_frame` | 连续原生纹理容器边框与单次内缩底色 |

插件不得缓存 Adapter 指针跨 bind 生命周期使用，也不得直接调用确认框 callback trampoline（只能走 `ui_confirmation` capability）。

### 1.3 资源与 port

四条声明式资源（`plugin.cpp:92-158`），全部常驻、`fail-closed`、可物理还原：

| id | 机制 | 策略 | 原因 |
|---|---|---|---|
| `hook:UIChoice_Init` | entry hook | 私有/必需 | `UIChoice_Init` 会重置 items/count/focus，模块数据必须在其后写回 |
| `ptrslot:g_uichoice_button_list_exe_got` | ptr slot | 私有/必需 | 截获选择按钮 ExecuteProc 以驱动模块回调 |
| `hook:Scene_Draw_POPUP_SC_CHOICE` | entry hook | 私有/必需 | 模块 choice 首帧保存画面并跳过游戏压暗 |
| `MEMORYTEXT_GetText`（`kResourceIdGetText`） | entry hook | 共享/必需 | 全游戏唯一文本热点，单一 owner + dispatch 分派 |

`QOL_ENABLE_DEVICE_PROBE` 构建下 `hook:UIChoice_Init` 与 `hook:Scene_Draw_POPUP_SC_CHOICE` 两条改用 `elf-export:` 严格导出名解析（`plugin.cpp:95-99,127-131`）。

必需 port：`QOL_PORT_CHOICE` 1.1（`contracts/game_ports/ports.h:51`），提供原生选择框的读/写/焦点/标题/关闭与 `draw` 原语。缺 port 时 install 直接失败，宿主不启用、资源不安装（`plugin.cpp:214-219`）。

### 1.4 依赖

- 可选 capability：`ui.state` 1.2（`plugin.cpp:78-79`）。缺 provider 只影响自定义 dialog 快照发布，选择框本身仍工作。
- Debug 构建追加可选 `http.registry`/`http.dispatch`（`plugin.cpp:80-85`）；非 Debug descriptor 不声明。

## 2. 架构

### 2.1 内部分层

| 层 | 职责 | 文件 |
|---|---|---|
| 契约 | POD 接口与版本 | `contracts/*.h`（外部） |
| 描述符与生命周期 | provides/requires/资源/port/feature/启停 | `plugin.cpp` |
| 状态与机制 | 组件注册表、选择框状态机、确认框 owner 状态机、文本分派 | `ui_components.cpp`、`feature.cpp`、`ui_confirmation.cpp`、`text_mechanism.cpp` |
| 纯绘制 | 形状扫描线栅格化（无全局状态，adapter 参数化） | `ui_shapes.cpp` |
| 原生纹理容器绘制 | loc metadata 校验、受限纹理切片、单次 interior fill | `game/access/ui_components.cpp`、`ui_container_frame.h`、`ui_container_frame_geometry.h` |
| 编排 | 面板逻辑栈、PopupState 宿主、帧补绘作用域 | `panels/stack.cpp`、`panels/host.cpp`、`panels/frame_overlay.cpp` |
| 探针 | Debug 面板与 command handler | `panels/debug_components.cpp`、`debug_scroll.cpp`、`debug_help.cpp`（仅 Debug 编译） |

依赖方向：出插件只依赖 `contracts/`；插件内部无 include 环。跨模块调用只有两条窄缝：

1. 组件 → 面板栈：`ui_components.cpp` include `panels/stack.h`，通过函数指针探针 `ui_panels_set_callback_probe`（`panels/stack.h:33-36`）告知「当前是否在组件回调内」，栈据此延迟导航（`panels/stack.cpp:242`）。
2. 确认框 ↔ PopupState 宿主：`ui_confirmation.cpp:681` 查询 `ui_panels_custom_overlay_scope_identity`；`panels/host.cpp:780-795` 提供该查询，并在 `dispatch_process`（`:623`、`:656`）arm overlay scope；`panels/host.cpp` 反向调用确认框模态门禁。两侧头文件不互相 include。

### 2.2 状态与线程

- UI Adapter 全部接口仅允许游戏主线程调用（`contracts/ui.h:3-6`）；组件回调和确认框回调都在主线程。
- 组件表（`ui_components.cpp:95`）与根树只由主线程访问；`unbind` 在回调栈内（`g_callback_depth != 0`）时推迟到安全点（`ui_components.cpp:2563-2584`）。
- 两个互斥量保护跨线程可见状态，不保护绘制或 UI 调用：
  - `feature.cpp:24`：选择框状态机，17 个锁点。所有 `port` 调用在锁外，锁内只做同 generation 快照与状态迁移。
  - `ui_confirmation.cpp:18`：确认框 owner/token 与 overlay 作用域，48 个锁点。adapter 调用与消费者 callback 在锁外，callback 最多分发一次。
- Debug HTTP 线程只写原子请求状态，面板开关由 scheduler 任务在主线程执行（`panels/debug_components.cpp:968-1017` 同型路径）。
- 文本 hook 回调先调用原函数；机制 active 时同步调用 `hooks.run_text` 分派，失败/空输出回退原文（`text_mechanism.cpp:53-64`）。

### 2.3 生命周期

| 阶段 | 行为 |
|---|---|
| install | 绑定 UI/面板/文本/确认框；要求 choice port；Debug 注册三个探针面板；任一步失败逆序回滚（`plugin.cpp:201-243`） |
| enabled | 打开 choice 与确认框门禁、注册内置帧补绘、启用文本分派与 Debug 面板（`plugin.cpp:263-281`） |
| disabled | 关闭 choice/确认框门禁与文本分派；确认框只关闭槽位证明归本 provider 的弹窗；Debug 面板置停用（`plugin.cpp:283-293`） |
| deactivate | 撤销 Debug 面板、停用文本、解绑面板/确认框/组件、释放 port 与 SDK（`plugin.cpp:245-261`） |

组件 `unbind` 不删除游戏拥有的 native：锚定组件标记 `native_externally_owned`，跳过 `delete_control`（`ui_components.cpp:2563-2584`）。

## 3. 机制

### 3.1 组件与原生控件

- 组件记录 `Component`（`ui_components.cpp:47-93`）持有 kind、native 句柄、rect、文本/回调、子节点、行、帮助项与原生归属位；注册表以 native 句柄为 key（`:95`）。
- 创建走 `create_component`（`:171-257`）：先校验 adapter/主线程/父组件/矩形，再调 adapter `create_control`，最后入表挂树。
- 销毁分两类：自建控件走 `destroy`/`flush_pending_destroy`（`:584-628`），回调栈内延迟到安全点；游戏拥有的控件走 `forget_native`，只注销记录。
- 触摸分发 `dispatch_touch`（`:422`）只在根矩形内命中子组件，不穿透原生控件。
- 文本按钮工厂：`text_button_create_impl` 承载校验与创建（`:692`），遗留入口 `text_button_create`（`:766`）；`capsule_`/`decorated_`/`compact_capsule_`/`main_menu_decorated_` 为显式外观工厂（`:1549-1637`）。遗留 `text_button_draw`（`:906`）仍按 decorated 紧凑比例绘制，`height = min(rect.h, rect.w × 48 / 220)`（`:965`）。
- 图标/箭头/导航按钮：`icon_button_*`（`:1096-1214`）、`arrow_button_*`（`:1003-1095`）、`nav_button_*`（`:1640-1733`）。导航按钮点击一律延迟请求弹栈顶，`on_click` 只是附加通知。

### 3.2 面板注册与导航

`ui_panels` 只编排 provider，不解释面板语义；逻辑栈深 4（`panels/stack.h:27`）。语义约束集中在 `panels/stack.h:6-21`：open 幂等、失败关栈顶重试一次、每帧剔除 stale 栈顶、延迟关闭不提前弹栈记录。常驻帧任务 `ui_panels_frame_pump`（`panels/stack.cpp:214`）驱动宿主重试；`ui_panels_request_back`（`:311`）可在原生事件遍历内安全调用。宿主 `PopupHost` 是唯一 PopupState owner（`panels/host.h:36-50`），close 管线停滞超过 120 帧时强制收尾（`host.h:50`、`host.cpp:421-446`）。

### 3.3 控件挂载归属（强制）

游戏会随面板关闭/切页销毁原生子树，跨帧句柄必须能证明存活：

| 挂载方式 | native 由谁销毁 | 必须使用 |
|---|---|---|
| 自建 root（`panel_create`/`create_control(0)`） | 插件 | 常规组件 API，退出时 `destroy` |
| 借用自建 root | 插件 | 常规组件 API，先销毁子组件再删 root |
| 游戏原生子树中的单图标按钮 | 游戏 | `anchored_button_ensure/draw`，关闭态 `set_visible(0)` |
| 游戏原生子树中的多控件/自定义 `CO_PROC`/自带 data | 游戏 | `anchored_control_ensure`，删除前 `anchored_control_detach` 取回已证明存活的句柄 |
| 主菜单入口（右侧齿轮） | 游戏 | `anchored_button_*` + `QolUiRootKind::kMainMenuParent` |

实现约束：

1. 每次 `anchored_*_ensure` 调用重新解析挂载根，不缓存 root 做跨调用判断（容器地址可能不变而子树已重建）；调用方可降低 ensure 频率，但不得跨调用缓存 root。
2. 先指针、后字段：先在活父控件子链表内做纯指针比对，再读 type/user_type/data/`CO_PROC`。
3. 失效只注销记录：`forget_native` 或 `anchored_*_release`；绝不 delete 未证明存活的句柄。
4. 身份判据：组件按钮用 `click_binding_is_current`；原生控件用创建时 `type` + `user_type` + `control_data` 快照配合 `control_proc_matches`。
5. `NotReady` 只表示无法证明，调用方必须保留记录。
6. `unbind` 跳过 `native_externally_owned` 组件，不删除游戏拥有的 native。

当前使用方：`autosell` 背包入口（`anchored_button_*`）、`extension_bag` 页签（`anchored_control_*`）、`settings` 主菜单齿轮。

### 3.4 原生选择框（`ui_choice`）

- 判定顺序：先 ownership。仅明确 foreign（`text_owns=0`）才把控制权交回原函数；ownership 读取失败/未知时报错并保持模块路径，不写原生、不关弹窗。
- 已确认 module owned 后，成功、关闭、读取错误、索引错误、focus 错误都不得运行原版剧情回调；关闭由原生 `UIChoice_ButtonListExe` 排队唯一一次 pop，模块不追加 `close_popup`，只清自身状态；失败只记原因并保活，等待下次点击。
- `choice_init_wrapper`（`feature.cpp:189`）：先调用原 `UIChoice_Init`，在 `UIChoice_CreateControl` 之前恢复模块 items/count/focus（有/无标题都恢复）；只恢复进入 wrapper 前已存在且同 generation 的 pending；port 调用一律在锁外。
- 首帧绘制 `choice_draw_wrapper`（`feature.cpp:459`）保存原始画面并跳过游戏压暗，避免深色面板下全黑。
- 同时向 `ui.state` 发布 dialog 快照（sequence/generation/type_id/title/options），关闭时发布 clear（`feature.cpp:153-186`）。
- 日志键固定：`owner_unavailable`、`owner_read_failed`、`selected_index`、`index`、`focus`；foreign 旁路记 `choice button foreign passthrough text_owns=0`；成功记 `choice button module <select|close> index=N`。不记录玩家文本。

### 3.5 原生确认框（`ui_confirmation`）

- 单 owner。open 前检查全局 `UIPopupMsg_bOn` 与本 provider 状态；已有弹窗或共享确认框时以 Busy 拒绝，不覆盖现有回调槽。
- token 语义：成功返回非零 token；失败通常返回 0，但清理未完成且仍保留 owner 状态时返回可重试 token，调用方用同一 token 调 `close`。
- callback 槽：只有 OK/Cancel 两槽都仍属本 provider 时才条件恢复并分发；任一槽 foreign 时不写槽、不分发，保留 closing owner。分发前一次性清理 spec/context，锁外、主线程、最多一次；分发期间同步重开被 Busy 拒绝。
- close：仅两槽都证明归属时调用原生 cancel/close，不调用消费者 cancel，不以 `UIPopupMsg_Free` 代替关闭；失败或弹窗仍 active 时保留 token/spec/门禁，可重试。
- `open_text`（capability 1.1）：`size` 校验、NUL 结尾且 ≤256 字节，provider 复制正文并保持到 `bOn==0` 且 callback 栈退出；`price>0` → 原生 `i32DisplayType=2`（价格布局），`price==0` → `0`（普通正文）；不依赖 TEXTDATABASE 或 TextLayer。
- V1 面板补绘：打开时若顶层 provider 有 `process` 且 overlay 可用，arm 一次性帧作用域；宿主在 `end_frame`（`GRPX_End` 之前）调用一次 `call_ui_popup_msg_draw`（`ui_confirmation.cpp:203`、`panels/host.cpp:623-657`）。成功绘制一次后才启用模态门禁；失败只记日志，不改 token/close 语义。
- 输入门禁（`ui_confirmation.cpp:925-973`）：F3 只消费不下传；F4 与所有非触摸事件消费；触摸 `0x17/0x18/0x19` 也消费但不做模块 hit_test；帧号 suppression latch 防同帧双关。
- 反汇编事实：`UIPopupMsg_Draw` 仅在 `i32DisplayType == 2` 进入价格布局；`UIPopupMsg_Draw` 开头 draw callback 槽非空会绕过全部内建绘制，排查显示异常时先确认槽归属。

### 3.6 形状与容器

- `ui_shapes` 三个入口：`ui_shape_draw_rect`、`ui_shape_draw_rounded_rect`、`ui_shape_draw_circle`（`ui_shapes.cpp:108/131/228`），adapter 显式传参，无全局状态。
- 形状语义：RGB565 填充/边框；alpha 为 `GRPX_FillRectAlpha` 的 0–100 百分比；圆角半径上限 256、圆直径上限 512；按互斥扫描线绘制，不重复合成边框/填充像素；不抗锯齿；`border_width==0` 走纯填充，`radius==0` 走整矩形。
- 容器：`draw_container_styled(const UiContainerStyleDesc*)`（组件能力 2.23）经 Adapter 2.34 的 `draw_container_frame(UiRect,uint32_t,uint32_t)` 绘制 unit `0x0f` 原生纹理边框与统一可配置内底；颜色为 RGB565、alpha 为 0–100、`ornamented` 仅接受 0/1。旧 `draw_container`、`draw_container_ornamented` 组件入口与 Adapter `draw_border_rect` 槽已随 ui_components/Adapter **Major 2 移除**，不再可用；迁移前它们走原生 `UI_DrawBorderRect`、ornamented 另叠加四角装饰图，现由 styled 的 `ornamented=1` 在同一入口内叠加。profile 仅由运行时 unit `0x0f` 的 loc0/loc1/四角 source metadata 精确匹配选择；不检查版本名、系列或 `GameVariant.known`，unknown 变体只要 metadata 命中且所需资源/符号/内存有效也走相同 profile，metadata 不匹配或绘制前置条件不可用时 fail-closed。静态资源与 ABI 审计覆盖 variant table 的 33 项：除明确排除且 loc metadata 不同的 Original v1.4.0 外，其余 32 项与共同 UI_003 profile 一致；C18 v586 有设备视觉验收，Monster v29 尚无设备视觉验收。横竖直边仅绘制 source `(2,2,w,9)` 与 `(2,29,9,h)` 的外侧阴影/金线区（tile 上限 508/383）；下边以 flip selector 1、右边 selector 0。四角不画整片：按实测每行 inner-inset 仅保留外侧 source run，内侧烘焙黑底不绘；剩余 aperture 按各角测量轮廓和直边 9px inset 生成互不重叠的 RGB565/alpha 填充 spans，每个像素只合成一次。alpha 0 时不填充但仍绘制边框装饰；48×48 的圆角 aperture 仍按 profile 处理，不强行缩成空内底。`ornamented=1` 时再叠加原有四角装饰。
- 开关：`switch_draw`（`:2142`）用轨道 + 滑块纯矩形自绘，颜色取统一 token，不依赖原生控件或图集。

### 3.7 文本机制（`text_layer`）

独占 `MEMORYTEXT_GetText` 共享资源，`text_api_subscribe/unsubscribe`（`text_mechanism.cpp:27/43`）提供注册；wrapper 先调用原函数，机制 active 时同步 `hooks.run_text` 分派，失败或空输出回退原文（`:53-64`）。phase/kind/scope 数值单一声明在 `contracts/text_layer.h`，实现侧用 `static_assert` 对齐。该 hook 不声明通用线程保证。

### 3.8 Debug 探针

- 三个 POST command：`/api/debug/custom-ui/components`、`/scroll`、`/help`（`plugin.cpp:165-177`），只在 Debug descriptor 声明；另注册 Hidden entry `core.custom_ui.open_debug_components`（`plugin.cpp:328-332`）。
- 面板只登记 provider 与 scheduler task，不动态注册 HTTP route；HTTP 线程只写原子请求状态。
- 编译门：`panels/debug_components.cpp`、`debug_scroll.cpp`、`debug_help.cpp` 只在 `CMAKE_BUILD_TYPE STREQUAL "Debug"` 参与编译（`CMakeLists.txt:13-20`），Release 不包含这 1679 行。

## 4. 渲染与几何事实（防回归）

| 项 | 事实 |
|---|---|
| 原生 Toggle | 按层绘制，每层源画布在基础矩形内居中：`layer_x = x + (base.w − layer.w) × percent / 200`；按压层 `loc 7` 画布每边多 8 px halo；所有层统一用基础层 offset；两态可见主体均为 220×61 |
| 文本按钮按压 | 按压轮廓按可见主体校准：normal 可见主体 405×75 与 Title `loc 7` pressed 220×61 导出 `round(75×p/61)`，不做 10% 量化；按压画布宽 = normal 可见宽 + 2×`round(21×scale/100)`；最大 scale 492% |
| 按压中段采样 | `sx = 100`（源 `[100,120)×[40,60)`：各行 RGBA 横向一致、alpha 0..10）；`sx = 30` 是旧假设，会带出周期性黄块 |
| 两端饰裁切 | 由 rect 比例决定：`height_percent <= width_percent` 或 rect 不小于源 405×77 时整体等比缩放，否则水平切片只压缩中段 |
| 主菜单 decorated | 显式目标尺寸 290×72，字号 28；命中区等于原生 snapshot rect；工厂以原生控件中心为锚点 |
| 设置页入口 | feature entry 统一 capsule 外观 + `kUiTextButtonFontLarger` |
| 圆角/圆形 | 圆角按 doubled-coordinate 求像素中心 inset；边框与填充按互斥行区间绘制 |

## 5. 消费者

| capability | 必需消费者 | 可选消费者 |
|---|---|---|
| `ui_components` | `custom_map`、`save_backup`、`settings` | `extension_bag`；`autosell`（描述符只声明 `ui_panels`，`ui_components` 由面板运行时 require） |
| `ui_panels` | `custom_map`、`save_backup`、`settings` | `autosell` |
| `ui_choice` | — | `world_teleport` |
| `ui_confirmation` | `custom_map`、`save_backup` | — |
| `text_layer` | `custom_recipe`、`character_panel_i18n` | — |

消费者归属以各自 descriptor 的 requires/optional 为最终依据；本表只列当前已核实的消费方。

## 6. 职责边界

- 机制归本插件，内容归消费者：页面、页面状态、交易规则、文案都不在本插件（`ARCHITECTURE.md` §5.4/§7.2）。
- `core.ui_state` 是游戏界面状态 owner；本插件只通过可选 `ui.state` 发布自定义 dialog 快照，不接管该领域。
- 底层游戏控件、绘制与资源原语由 UI Adapter / Game Adapter 承担；插件内不出现裸 VMA/偏移（`check_offsets` 0 违规）。
- 确认框只呈现原生 UI 并分发确认/取消，不实现调用方价格、扣款或交易规则；扩展背包既有专属确认路径保持独立。
- Debug 面板与端点只是 Debug 探针，不等同 release 对外接口。

## 7. 已知限制

- 原生 choice 背景透明度无实现：未找到 `Scene_Draw_POPUP_SC_CHOICE` 绘制体与黑底来源，不得承诺背景 alpha；形状 alpha 只作用于模块主动绘制。
- 原生 choice 与其他自定义面板的 z-order、输入遮挡没有真机证据，不得承诺任意叠层。
- 确认框在自定义 panel 上依赖模块帧内补绘；原生 `UIPopupMsg` 本身不覆盖 panel。
- 跨帧句柄必须按 §3.3 证明存活；`NotReady` 不等于控件已死亡。
- 文本 hook 回调不声明通用线程保证，不得当作 UI 主线程接口使用。

## 8. 文件地图

| 子职责 | 文件 | 关键位置 |
|---|---|---|
| descriptor、资源、port、生命周期、Debug 入口 | `plugin.cpp` | id/version `:24-25`；provides `:48-75`；optional `:77-86`；资源 `:92-158`；port `:160-163`；Debug endpoints `:165-177`；install/deactivate `:201-293`；features/descriptor `:324-363` |
| 组件注册表与生命周期 | `ui_components.cpp` | `Component` `:47-93`；注册表 `:95`；创建/销毁 `:171-257,584-628`；触摸分发 `:422`；文本按钮工厂 `:692,1549-1637`；图标/箭头/导航 `:1003-1214,1640-1733`；锚定按钮 `:1214-1365`；锚定控件 `:1366-1540`；容器 `:2057-2114`；形状委托 `:2175-2188`；槽位表 `:2463`；bind/unbind `:2547-2584` |
| 形状绘制 | `ui_shapes.h`、`ui_shapes.cpp` | 3 个入口 `ui_shapes.cpp:108/131/228`；半径/直径上限 `:26-27` |
| 共享几何与 adapter 判定 | `ui_geometry.h`、`ui_adapter_check.h` | `rect_within_int32`；`kUiAdapterBaseMinor`、`ui_adapter_ready`、`ui_adapter_on_main_thread` |
| 面板逻辑栈与导航 | `panels/stack.h`、`panels/stack.cpp` | 栈深与语义 `stack.h:27,6-21`；open `stack.cpp:239`；帧泵 `:214`；request_back `:311`；block_reason `:368` |
| PopupState 宿主 | `panels/host.h`、`panels/host.cpp` | 单一 owner `host.h:36-50`；dispatch_process `host.cpp:623`；overlay scope `:656`；身份查询 `:780-795`；停滞逃生 `:421-446` |
| 帧补绘作用域 | `panels/frame_overlay.h`、`panels/frame_overlay.cpp` | 一次性 token+provider+generation 作用域，嵌套 sentinel |
| 原生选择框 | `feature.h`、`feature.cpp` | 纯规则 `feature.h:22-47`；Init wrapper `:189`；按钮执行 `:270`；open/active/close `:365-457`；首帧绘制 `:459`；ui.state 发布 `:153-186` |
| 原生确认框 | `ui_confirmation.h`、`ui_confirmation.cpp` | 补绘 `:203`；close `:384`；分发 `:440`；open/open_text `:554-737`；补绘注册 `:905`；模态门禁 `:925-973` |
| 文本机制 | `text_mechanism.h`、`text_mechanism.cpp` | subscribe/unsubscribe `:27/43`；wrapper `:53-64` |
| Debug 面板 | `panels/debug_components.cpp`、`debug_scroll.cpp`、`debug_help.cpp` | 仅 Debug 编译；components 另含形状/原生对话框示例；路由事实见 `routes.inc` |
| 设置页贡献 | `settings_contribution.h`、`settings_contribution.cpp` | 当前不注册设置项；设置页实现在 `plugins/settings/` |
| 跨插件契约 | `contracts/ui.h`、`panels.h`、`ui_choice.h`、`ui_confirmation.h`、`text_layer.h`、`game_ports/choice_port.h` | 版本常量与 POD 接口 |
| 构建与能力开关 | `CMakeLists.txt` | Debug 源清单 `:13-20`；`QOL_DEBUG_COMPONENTS` `:23-25` |

## 9. 演进约束

- capability 变更只允许尾追加 minor；`QolUiAdapter` 只允许尾追加槽位，消费者按 major/minor/size 检查，不得重排或复用槽位。
- 新增游戏控件树挂载必须先选 §3.3 的机制，不得手写「root 指针相等即有效」。
- 内部拆分保持纯移动：绘制参数、几何、调用顺序、错误码与能力版本逐字不变；本轮形状抽离只移动实现，槽位签名与委托保持不变。
- 不套用 `extension_bag` 的 L0–L6 全量分层：本插件没有持久化、事务与业务用例层；可复用的准则只有依赖方向、主线程契约、句柄+身份证明与 fail-closed。方案与理由见 [`refactor/custom-ui/refactor-plan.md`](../../refactor/custom-ui/refactor-plan.md)。
- Debug 面板不得进入 Release 源列表；`plugin.cpp` 对它们的引用必须保持在 `QOL_DEBUG_COMPONENTS` 条件块内。
