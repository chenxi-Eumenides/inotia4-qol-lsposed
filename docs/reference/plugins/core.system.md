# `core.system` 插件

## 1. 作用与架构定位

`core.system` 是系统信息 query provider，仅聚合系统/运行状态查询，不承担通用系统管理或业务领域逻辑。它从 Game Host system port 取得定型 JSON，通过 Host SDK 快照与 HTTP capability 协作，保持 Kotlin transport、native owner 和 JNI bridge 的职责边界。

## 2. 功能面

### HTTP 路由

routes.inc 与 descriptor 均将以下五个 GET handler 归于 `core.system`：

| 路径 | 内容 |
|---|---|
| `/api/system/snapshot` | 系统聚合快照 |
| `/api/system/game` | 游戏状态聚合 |
| `/api/system/info` | 版本、身份及存档相关系统信息 |
| `/api/system/game_frame` | 游戏帧信息 |
| `/api/system/events` | 系统事件 JSON |

正式响应定义见 [API 参考 §6、§7.9](../api-reference.md)。诸如 `/api/system/tables/*`、`/api/system/help` 等静态资料路由由 `core.data` 拥有；`/api/system/features`、tickets、routes 等由 `core.api` 持有，不属于本插件。

### Capability 面

- 不提供跨插件业务 capability。
- 必需 capability：`http.registry`、`http.dispatch`。
- 必需 `QOL_PORT_SYSTEM`（descriptor 要求 ABI 1.1）。

## 3. 依赖前置与消费者

- Game Host 注入 system port，其 `body_json` 读取 snapshot/game/info/frame 类型的只读投影，`events_json` 提供事件查询体。
- Host SDK 使用 snapshot 存储、帧调度与日志接口；该插件没有写命令、票据 handler 或业务写 port。
- HTTP 客户端经 Kotlin `GameController`/`EventsController` 和 service/query bridge 消费。Kotlin 侧负责 socket、请求生命周期、响应写回；native 插件只处理 owner query 数据。

## 4. 职责边界

- 拥有五条系统信息只读 HTTP query handler 和其快照读取/响应路由。
- 不拥有静态数据查询（`core.data`）、HTTP registry/全局 feature 状态与 tickets（`core.api`）、角色/库存/任务事实或写操作。
- Kotlin host 维持 AndServer、解析及传输；JNI/NativeBridge 仅作为既有窄桥接入口；native `core.system` 以 `QOL_PORT_SYSTEM` 获取数据并序列化响应，不建立 socket、不承担通用 HTTP router。

## 5. 核心实现流程

- **读/快照**：每 15 帧通过 system port 读取 snapshot、game、info、frame 四类 JSON，校验缓冲容量后发布 `Snapshot<SystemSnapshot>`。snapshot/frame 是核心可用数据；若 game/info 身份尚未下发，可对应暂不可用而不阻止其余数据发布。核心读取失败保留上一份完整快照并告警。
- **事件查询**：`/api/system/events` 经 port 的 `events_json` 读取并直接填充 HTTP response buffer，不经过四类快照缓存。
- **写/命令/票据**：无 native 写路由、owner command 或 ticket result handler。
- **生命周期**：install 校验 SDK、system port ABI 与 `body_json/events_json`；enable 初始化快照并启动 15 帧任务；disable/deactivate 停任务、销毁快照、清除端口引用。路由随 descriptor/宿主 owner 生命周期注册和撤销。

## 6. 文件地图

| 子职责 | 文件 | 职责 |
|---|---|---|
| system query owner | `module/app/src/main/cpp/plugins/core.system/plugin.cpp` | descriptor、system 快照读取、五条 query handler 与生命周期 |
| 插件声明 | `module/app/src/main/cpp/plugins/core.system/plugin.h` | 插件入口声明 |
| 快照封装 | `module/app/src/main/cpp/plugins/core.system/snapshot_sdk.h` | Host SDK 快照接口包装 |
| Game Host 契约 | `module/app/src/main/cpp/contracts/game_ports/system_port.h` | system JSON 与 events 读取端口 |
| Kotlin query 层 | `module/app/src/main/java/com/inotia4/qol/api/controller/GameController.kt`、`EventsController.kt` | Kotlin 请求解析与 query service 调用 |
| JNI bridge | `module/app/src/main/java/com/inotia4/qol/NativeBridge.kt` | 既有 native query 桥接签名；不承载业务聚合 |

接口规格：[HTTP API 参考](../api-reference.md)。
