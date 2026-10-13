# `core.custom_ui` 插件

## 1. 作用与 owner 定位

`core.custom_ui` 是普通核心共享 UI 工具/框架插件，注册 ID 为 `core.custom_ui`、版本 `0.1.1`。它拥有可复用 UI 组件、面板注册/导航、原生选择控件和文本机制；不拥有功能页面或其文案内容（`module/app/src/main/cpp/plugins/core.custom_ui/plugin.cpp:21-23,139-140`；架构边界见 [`ARCHITECTURE.md` §5.4、§7.2](../../../ARCHITECTURE.md)）。

## 2. 功能面

### Capabilities 与原生资源

| 面 | 版本 | 当前提供内容 |
|---|---:|---|
| `ui_components` | 1.13 | 通用组件与绘制/列表/导航接口；契约中的 `QolUiComponents` 由本插件实现 |
| `ui_panels` | 1.3 | 面板 provider 注册后的打开、关闭、状态/返回协作接口 |
| `ui_choice` | 1.1 | 原生选择框 open/active/close API |
| `text_layer` | 1.0 | `MEMORYTEXT_GetText` hook 上的文本订阅/分派接口 |

提供版本见 `module/app/src/main/cpp/plugins/core.custom_ui/plugin.cpp:27-67`；公共版本定义见 `contracts/ui.h:15-19`、`contracts/panels.h:16-18`、`contracts/ui_choice.h:21-23`、`contracts/text_layer.h:10-12`。本插件另声明三项必需资源：私有 `UIChoice_Init` entry hook、私有选择按钮 ptr slot、共享 `MEMORYTEXT_GetText` entry hook；均为 required/fail-closed 且无 feature gate（`plugin.cpp:81-132`）。

### Debug 专用入口

Debug 构建包含三个 POST 调试端点：`/api/debug/custom-ui/components`、`/api/debug/custom-ui/scroll`、`/api/debug/custom-ui/help`，分别驱动组件、原生滚动列表和帮助页探针。插件 descriptor 在 Debug 条件下声明 typed endpoint handlers；Host binder 按 descriptor lifecycle owner 接线。`routes.inc` 与 `core.api/routes.inc` 保存供同步/校验使用的 route facts，不由面板代码动态注册 HTTP handler。Debug 条件由 `core.custom_ui/CMakeLists.txt:11-19` 控制。

该构建还注册唯一的 debug `entry` feature `core.custom_ui.open_debug_components`，action label 为“打开”，callback 打开 `debug_components` 面板；另外的 `ui_tools` 是 Hidden toggle，不是入口项（`plugin.cpp:220-241`）。endpoint/feature 契约以路由元数据和 descriptor 为实现事实，API 公共契约以 [API 参考](../api-reference.md) 为准。

`PluginDescriptor` 通过 `http_endpoints` 声明这三个 Debug command routes（`plugin.cpp` 中的 `kHttpEndpoints` 与 descriptor 尾部）；各 Debug panel 注册函数只登记 UI panel provider 和 scheduler task，不另行向 `http_dispatch` 动态注册 HTTP route。

## 3. 使用的模块功能、依赖与消费者

- **Requires / optional**：`ui.state` 1.0 为可选依赖；Debug 构建对 `http.registry`/`http.dispatch` v1.1 的依赖也为可选，非 Debug descriptor 不声明 HTTP 依赖（`plugin.cpp:70-79,247-253`；版本见 `contracts/http.h:96-99`）。provider 缺失只使 Debug HTTP 探针无法接线，不影响共享 UI 功能；provider 恢复后按可选依赖生命周期重新接线。选择框向 `ui.state` 发布自定义 dialog snapshot；缺少该 capability 时 native choice 本身仍按本插件路径工作（`feature.cpp:70-99`）。
- **Required port**：`QOL_PORT_CHOICE` 1.0，提供原生选择框状态/选项/标题/焦点/弹出等受控操作（`plugin.cpp:134-137`；版本见 `contracts/game_ports/ports.h:17,47`；接口见 `contracts/game_ports/choice_port.h:11-30`）。
- **Host SDK**：使用 Host 注入的 `sdk->ui` Adapter（契约版本 1.20；它不是本插件提供的 capability），以及 `panels`、`hooks.text_subscribe/run_text`、descriptor endpoint handlers、scheduler/log（`contracts/ui.h:3-6,15-19`；`plugin.cpp` 中的 `kHttpEndpoints`；`text_mechanism.cpp:27-46`）。
- **已核实消费者**：`ui_components` 与 `ui_panels` 由当前 `plugins/settings/` 设置页消费；`ui_panels` 还由 `save_backup`、`autosell` 使用；`ui_choice` 被 `world_teleport` 作为可选能力消费；`text_layer` 被 `custom_recipe`、`character_panel_i18n` 要求（`plugins/settings/plugin.cpp:32-38`、`settings/panels/settings.cpp:816-821`、`save_backup/plugin.cpp:31-35`、`autosell/plugin.cpp:196-204`、`world_teleport/plugin.cpp:46-61`、`custom_recipe/plugin.cpp:168-178`、`character_panel_i18n/plugin.cpp:33-36`）。
- **真实设置页边界**：页面实现位于 `plugins/settings/`，descriptor ID 为 `settings`，只要求 `ui_panels` 与 `ui_components`；`core.custom_ui/settings_contribution.cpp` 当前不注册设置项（`plugins/settings/plugin.cpp:19-20,32-38,238-255`；`core.custom_ui/settings_contribution.cpp:4-8`）。

## 4. 职责范围

- `core.custom_ui` 提供组件、面板/导航、游戏原生选择框和文本机制；具体页面、页面状态与业务文案由各自插件拥有，设置页归注册 ID 为 `settings` 的 `plugins/settings/`（`plugin.cpp:139-140`；`ARCHITECTURE.md` §7.2）。
- `core.ui_state` 拥有游戏界面状态/操作；`core.custom_ui` 只通过可选 `ui.state` 发布自定义 dialog 状态，不接管该领域 owner（`feature.cpp:70-99`；架构边界见 `ARCHITECTURE.md` §7.2）。
- UI Adapter/Game Adapter 承担底层游戏控件、绘制与资源原语；本插件通过 contracts/Host SDK 使用受控接口，不承载通用游戏语义（`contracts/ui.h:3-6,304-315`；`ARCHITECTURE.md` §4.3、§7.2）。
- Debug 面板与 endpoint 是 Debug 构建探针，不等同于功能页面或 release 对外业务接口（`CMakeLists.txt:11-19`；`plugin.cpp:74-79,220-241`）。

## 5. 对外提供与依赖影响

四项 capability 随普通插件启停发布/撤销。当前外部 consumers 如 §3 所列；`ui_choice` 的可选消费者 `world_teleport` 不把它作为 install 前提。停用时关闭选择控件门禁、停用文本分派，由 Host binder 撤销该 descriptor 的 Debug routes，并解除面板/SDK 绑定；组件或面板能力不因此变成具体页面的 owner（`plugin.cpp:166-202`）。

## 6. 读写线程与生命周期机制

- **UI 与选择控件**：`contracts/ui.h` 明确 Host UI Adapter 接口仅可在游戏主线程调用；`UiChoiceSpec.on_select` 约定在主线程回调。选择操作使用 mutex 保护 active/spec/generation 并持有选项/标题副本，底层 read/write 通过 `QOL_PORT_CHOICE`；port contract 本身未另行规定所有函数调用的线程上下文（`contracts/ui.h:3-6`；`contracts/ui_choice.h:25-34`；`contracts/game_ports/choice_port.h:11-30`；`feature.cpp:23-46,130-221,223-307`）。
- **Debug HTTP 写路径**：descriptor command handler 只验证请求并写原子请求状态；UI 面板开关交由 scheduler task 执行。三个 Debug 面板都有该“HTTP 请求→调度任务→面板”路径（`panels/debug_components.cpp`；`debug_scroll.cpp`；`debug_help.cpp`）。
- **文本读路径**：wrapper 先调用原 `MEMORYTEXT_GetText`，若文本机制 active 再同步调用 Host `run_text` 分派，失败/空输出回退原文；订阅由 Host hooks API 登记/撤销（`text_mechanism.cpp:27-64`）。源码没有为该 hook 回调另行声明一个通用线程保证，本说明不把它扩写为 UI 主线程接口。
- **生命周期**：install 绑定 UI/面板/文本 API 并验证 required choice port；enabled/disabled 分别打开/关闭 choice 与 text gate，Debug 构建同步启停调试面板；deactivate 关闭 choice、撤销 Debug 路由/面板、停用 text dispatch 并清理绑定（`plugin.cpp:141-180,182-202`）。

## 7. 文件地图

| 子职责 | 文件 | 关键事实 |
|---|---|---|
| descriptor、provides/requires/port、资源与生命周期 | `module/app/src/main/cpp/plugins/core.custom_ui/plugin.cpp` | ID/version/capabilities：21-79；resources/port：81-137；install/lifecycle：141-202；Debug entry：220-267 |
| 组件接口实现 | `module/app/src/main/cpp/plugins/core.custom_ui/ui_components.cpp`、`ui_components.h` | 组件对象/原生 adapter 绑定：`ui_components.cpp:20-78,124-130`；接口：`ui_components.h:13-45` |
| 原生选择框 | `module/app/src/main/cpp/plugins/core.custom_ui/feature.cpp`、`feature.h` | 锁状态、port 读写、dialog publish：`feature.cpp:23-46,70-99,130-221,223-334` |
| 文本共享机制 | `module/app/src/main/cpp/plugins/core.custom_ui/text_mechanism.cpp`、`text_mechanism.h` | subscribe/unsubscribe 与原文 fallback dispatch：`text_mechanism.cpp:27-64` |
| Debug panels/endpoints | `module/app/src/main/cpp/plugins/core.custom_ui/plugin.cpp`、`panels/debug_components.cpp`、`debug_scroll.cpp`、`debug_help.cpp` | descriptor endpoint facts/handlers 位于 plugin.cpp；各面板源码实现 command callback 与 scheduler task |
| 跨插件契约 | `module/app/src/main/cpp/contracts/ui.h`、`panels.h`、`ui_choice.h`、`text_layer.h`、`game_ports/choice_port.h` | adapter/components 版本与线程说明：`ui.h:3-19`；其余 capability ID/version 与接口见各文件开头定义 |
| 当前设置页消费者 | `module/app/src/main/cpp/plugins/settings/plugin.cpp`、`plugins/settings/panels/settings.cpp` | descriptor requires：`plugin.cpp:32-38`；运行时 require ui_components：`settings.cpp:816-821` |
