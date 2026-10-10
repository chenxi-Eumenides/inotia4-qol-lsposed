# `core.ui_state` 插件

## 1. 作用与 owner 定位

`core.ui_state` 拥有游戏原生 screen、panel、dialog 的状态读取与对应 UI 操作，包括面板开关、回主菜单、NPC 交互和对话选项选择。它是游戏自身 UI 状态领域 owner；不等同于 `core.custom_ui` 的共享自定义页面/组件服务。

## 2. 功能

### HTTP 路由

`routes.inc` 将以下 GET/POST 路由标记为 `core.ui_state`：

| 方法 | 路径 | 当前功能面 |
|---|---|---|
| GET | `/api/ui` | 当前 UI 聚合状态 |
| GET | `/api/ui/screen` | 主线程快照中的原生游戏 screen；不被自定义 panel/dialog 改写 |
| GET | `/api/ui/panel` | 自定义 PanelProvider 栈顶或原生 panel |
| GET | `/api/ui/dialog` | 自定义 UiChoice dialog 或原生 DialogContent |
| POST | `/api/ui/go_main_menu` | 返回主菜单 |
| POST | `/api/ui/close_panel` | 关闭当前原生面板 |
| POST | `/api/ui/open_panel` | 打开指定原生面板 |
| POST | `/api/ui/start_interact` | 发起 NPC 交互 |
| POST | `/api/ui/dialog/select` | 选择对话选项 |

native `core.ui_state` descriptor 登记三条 GET 查询和五条 POST 写 handler；路径、方法及既有响应字段不变。`GET /api/ui` 仍走 Kotlin `gamestateJson()` legacy 聚合，不由这些 query handler 接管。协议、响应字段和状态名称见 [API 参考 UI 章节](../api-reference.md)。

### Capability 与 ports

- provides：`ui.state` v1.0，接收自定义 panel/dialog 的有序状态快照。
- 必需 capability：`http.registry` 与 `http.dispatch`。
- 必需 ports：`QOL_PORT_CHOICE_WRITE`、`QOL_PORT_UI_WRITE`、私有的 `QOL_PORT_UI_STATE_READ`。
- `core.custom_ui` 仅 optional-requires `ui.state`；`core.ui_state` 不依赖或引用 `core.custom_ui`。

### 注册 feature

| feature id | 默认 | 分类 | 类型 |
|---|---:|---|---|
| `core.ui_state.provider` | 1 | Hidden | toggle |

provider 为常驻需求，用 default true 的 hidden feature 表达；`enable`（Host 独占 lifecycle 期望）与 feature 由 Host 管理，插件不读写。

## 3. 依赖前置与消费者

- Host SDK 提供 typed commands、ticket result handler、HTTP 路由注册/dispatch、调度与日志面；Game Adapter 注入 choice/UI write 及 UI state read ports。
- HTTP 客户端经 Kotlin UI controllers/service 使用查询和写路由；其他 native 功能通过 UI state 读取结果或 owner 写操作协作。
- 游戏自身面板栈的查询与操作面属于本插件；自定义面板 provider stack 和组件能力属于 `core.custom_ui`。

## 4. 职责边界

- 拥有游戏原生 UI 当前态及相应 owner 操作；Game Adapter 在游戏主线程读取原生 screen/panel/dialog，插件把读取结果保存为 HTTP 查询快照。
- 接收 `core.custom_ui` 完整有序 PanelProvider 栈快照；栈顶映射为 `panel_<panel_id>`，仅影响 `/api/ui/panel`，不覆盖原生 `/api/ui/screen`。
- 接收带 generation 的自定义 dialog 快照；稳定 `UiChoiceSpec.dialog_type_id` 只替换原生 `DialogContent.type`，原生 `text/speaker/displayed/options` 等字段保持原 JSON；迟到的旧序列或旧 generation 不会清除新状态。没有合法顶层 `type` 时 fail-closed，不重建响应。
- `core.custom_ui` 负责可复用组件、面板 provider stack 和文本机制；Kotlin 负责 HTTP transport/JVM 同意页分支；Game Adapter/Host 负责低层 PopupState 与 UI 原语适配。

## 5. 核心实现链

- **状态读取**：Game Adapter UI state read port 在主线程按 PopupState 原语与 wrapper alias 生成原生 screen/panel/dialog 快照；HTTP handlers 只复制/合并已发布快照，不在线程池直读游戏对象。自定义 panel/dialog 快照以版本化可选 capability 单向发布。
- **写/命令/票据**：HTTP 写 handler 在请求线程解析窄 payload 并提交 typed command，返回 ticket；主线程 result handler 分别调用 choice write 或 UI write port，生成对应结果。五个 POST 路由包含 dialog select、start interact、open/close panel、go main menu。
- **线程/缓存**：UI 有副作用的动作只由 owner 命令在游戏主线程执行；Game Adapter 周期性在主线程采集原生状态，HTTP 查询线程仅读锁保护的副本。自定义 panel/dialog 仅通过 `ui.state` 可选 capability 发布深拷贝快照。
- **生命周期**：install 校验两个 write ports、UI state read port 与 Host SDK ticket result 面，注册两个 typed result handlers 并启动主线程 snapshot task；插件 provides `ui.state` v1.0，无声明式资源。deactivate 注销 handlers、移除 snapshot task 并清理 snapshot/SDK/port 引用；enable/disable 回调无额外 UI 写操作。

## 6. 文件地图

| 子职责 | 文件 | 职责 |
|---|---|---|
| owner handlers/snapshot | `module/app/src/main/cpp/plugins/core.ui_state/plugin.cpp` | descriptor、三条 GET + 五条 POST handlers、`ui.state` capability、typed command/ticket result 与 snapshot 生命周期 |
| 插件声明 | `module/app/src/main/cpp/plugins/core.ui_state/plugin.h` | 插件入口声明 |
| capability/读写端口契约 | `module/app/src/main/cpp/contracts/ui.h`、`game_ports/choice_write_port.h`、`ui_write_port.h` | `ui.state` 快照、原生 UI 读取及对话/panel 写操作 |
| PopupState 原语 | `module/app/src/main/cpp/hooks/popup_state.h`、`popup_state.cpp` | PopupState entry 解析、占用/恢复原语 |
| wrapper alias | `module/app/src/main/cpp/infra/popup_enter_alias.h`、`module/app/src/main/cpp/infra/popup_enter_alias.cpp` | wrapper enter 与原始游戏 entry 的身份关联 |
| 游戏 UI 事实/写适配 | `module/app/src/main/cpp/game/access/ui_state.cpp`、`game/state/ui_http.h`、`game/adapter_ports/ui_write_port.cpp` | 原生 UI 状态主线程快照与 owner 写端口的低层适配 |
| Kotlin UI API | `module/app/src/main/java/com/inotia4/qol/api/controller/UiController.kt`、`UiActionController.kt` | UI 查询/操作的 HTTP 接入与传输协调 |
