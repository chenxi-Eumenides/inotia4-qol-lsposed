# `core.quest` 插件

## 1. 作用与架构定位

`core.quest` 是任务领域的官方 owner，读取活动/已完成任务事实并拥有退出任务的操作命令。它消费 world facts 以判断游戏状态，使用 quest Game Host port 访问任务域；这符合 `ARCHITECTURE.md` 的领域语义由 provider 插件承载原则。

## 2. 功能面

### HTTP 路由与实际处理器

| 方法 | 路径 | 当前处理 |
|---|---|---|
| GET | `/api/quest` | `core.quest` native query handler，组合 active/details/completed |
| GET | `/api/quest/active` | `core.quest` native query handler |
| GET | `/api/quest/details` | `core.quest` native query handler |
| GET | `/api/quest/completed` | `core.quest` native query handler |
| POST | `/api/quest/quit_quest` | `core.quest` native command handler，返回 ticket |
| GET | `/api/quest/{id}` | routes.inc owner 标为 `core.quest`，但当前不在 `core.quest` native descriptor handler 列表；实际由 Kotlin `QuestController` 处理，未由该插件实现 native detail handler |
| POST | `/api/op/quest/accept`、`/api/op/quest/complete` | routes owner 为 `core.quest` 的 Kotlin 占位处理，当前不是 native descriptor handler |

路由归属以 `routes.inc` 为准，实际实现/handler 以 descriptor 与 Kotlin 控制器为准；不可把 route map 中的 owner 标记推断成该插件已有实现。正式契约见 [API 参考 §5、§7.9–§7.10](../api-reference.md)。

### Capability 面

- 无 `provides` capability。
- 必需 capability：`world.facts`、`http.registry`、`http.dispatch`。
- 必需 port：`QOL_PORT_QUEST`（descriptor 要求 1.1）。

## 3. 依赖前置与消费者

- install 还要求 `PluginContext` 的 Game Host 与 Host SDK 可用；quest port 提供任务读取/片段、静态数据就绪状态及写入接口。
- 使用 scheduler、snapshot、commands、ticket result 与 capability require 面；消费 `world.facts` 的 state 判定当前是否处于游戏世界。
- 对外消费者主要是 HTTP 客户端经 Kotlin `QuestController`/native query bridge；插件不向其他插件发布任务 capability。

## 4. 职责边界

- 拥有 native 任务快照查询、活动/详情/完成列表 JSON 组装、退出任务命令。
- 不拥有角色状态、world 状态事实（由 `core.world` 提供）、静态任务文本资源底层读取（quest port/Game Adapter 适配）或 Kotlin HTTP/socket。
- `GET /api/quest/{id}` 与两个 `/api/op/quest/*` 虽由 routes 标记归 `core.quest`，但目前处理职责在 Kotlin/占位层，不能描述为插件 native 已实现功能。

## 5. 核心实现流程

- **读/快照**：每 15 帧经 `world.facts` 确认 state，再从 quest port 读取活动槽 ID/状态与完成任务 ID；静态片段未就绪时跳过发布。快照只保留任务 ID/状态数组。HTTP 查询按 endpoint 获取任务 JSON fragment 并在请求响应阶段组合，避免缓存重复静态文本。
- **写/命令/票据**：`quit_quest` 在 HTTP 线程严格解析 JSON 中唯一的非负整数 `quest_id`，提交 typed command 并立即回 ticket；主线程 result handler 调 quest write port，写入操作结果至 ticket，业务错误按失败终态处理。内部 `post_set_state` 通过 registered typed handler 将设置状态命令投递到主线程。
- **生命周期**：install 校验 quest port、world facts capability、快照及 command/ticket handlers，开启 gate 并启动 resync/snapshot 任务；enable 重启 gate 与任务；disable/deactivate 关闭 gate、取消任务、撤销 handler、销毁快照并清除引用。

## 6. 文件地图

| 子职责 | 文件 | 职责 |
|---|---|---|
| owner 实现 | `module/app/src/main/cpp/plugins/core.quest/plugin.cpp` | descriptor、查询快照、HTTP handler、命令票据及生命周期 |
| 任务 feature 状态 | `module/app/src/main/cpp/plugins/core.quest/feature.h`、`feature.cpp` | gate/resync 与任务 feature 辅助状态 |
| 快照声明 | `module/app/src/main/cpp/plugins/core.quest/snapshot_sdk.h` | Host SDK snapshot 包装 |
| Game Host 契约 | `module/app/src/main/cpp/contracts/game_ports/quest_port.h` | 任务事实、JSON fragment 与写操作端口 |
| World 依赖契约 | `module/app/src/main/cpp/contracts/world_facts.h` | 被消费的 world facts capability |
| Kotlin 路由 | `module/app/src/main/java/com/inotia4/qol/api/controller/QuestController.kt` | Kotlin 侧 quest 路由；包含未由 native descriptor 接管的处理路径 |

接口规格：[HTTP API 参考](../api-reference.md)。
