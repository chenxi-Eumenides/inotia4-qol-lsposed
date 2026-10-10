# `core.world` 插件

## 1. 作用与架构定位

`core.world` 是世界领域的官方 provider/owner：发布当前世界事实，并拥有世界移动、交互命令。它按 `ARCHITECTURE.md` 的领域拆分规则通过 capability、Host SDK 和 Game Host port 协作；不属于 Loader，也不直接依赖 Game Adapter 私有实现。

## 2. 功能面

### HTTP 路由

`plugins/core.api/routes.inc` 中 owner 为 `core.world` 的路由，与 `plugin.cpp` 声明式 handler 一致：

| 方法 | 路径 | 用途 |
|---|---|---|
| GET | `/api/world/map/id` | 当前地图 ID 与可用名称 |
| GET | `/api/world/map/exits` | 当前地图出口 |
| GET | `/api/world/map/units` | 当前单位 |
| GET | `/api/world/map/enemies` | 当前敌人 |
| GET | `/api/world/map/interactives` | 当前可交互对象 |
| GET | `/api/world/map/drops` | 当前掉落 |
| GET | `/api/world/map/distance` | 玩家到目标的导航距离与最近可达点 |
| POST | `/api/world/movement/move_to` | 移动到目标瓦片 |
| POST | `/api/world/movement/walk_dir` | 按方向行走 |
| POST | `/api/world/movement/stop_move` | 停止移动 |
| POST | `/api/world/movement/interact_with` | 执行当前交互 |
| POST | `/api/op/movement/teleport` | 越权门控下的直接传送命令 |

正式参数、状态码、响应和票据契约见 [API 参考 §7.3、§7.9–§7.10](../api-reference.md)。静态地图资料 `/api/world/maps/*` 由 `core.data` 持有，不属于 `core.world`。

### Capabilities 与 provider 面

- 提供 `world.facts`（版本由 `contracts/world_facts.h` 定义），包含 state、game-state gate、地图记录数、当前地图 ID/名称、玩家朝向和 tutorial state 读取接口。
- 无可选 capability 提供；不另行提供静态地图数据。

### 注册 feature

| feature id | 默认 | 分类 | 类型 |
|---|---:|---|---|
| `core.world.provider` | 1 | Hidden | toggle |

provider 为常驻需求，用 default true 的 hidden feature 表达；`enable`（Host 独占 lifecycle 期望）与 feature 由 Host 管理，插件不读写。

## 3. 依赖前置与消费者

- 必需 capability：`http.registry`、`http.dispatch`。
- 必需 Game Host ports：`QOL_PORT_WORLD`、`QOL_PORT_WORLD_WRITE`。
- Host SDK 使用快照存储、帧调度、typed command、typed ticket result handler 注册/注销及日志接口。
- 运行时消费者包括 `core.quest`（world facts）以及世界移动/传送等功能插件；精确依赖以各自 descriptor 为准。HTTP 客户端经 Kotlin API 服务调用注册路由。

## 4. 职责边界

- 拥有当前地图实时事实、7 条世界查询及 5 条世界写命令的 owner 语义。
- 不拥有静态地图表/地图文件查询（`core.data`）、角色/队伍状态（`core.character`）、物品/金钱（`core.item`）、传送功能策略（功能插件）或 HTTP socket/通用 router（Kotlin host 与 `core.api`）。
- Kotlin Controller 负责参数接收与传输协调；native 插件整理快照、查询响应和 owner 命令；Game Adapter/Host port 承担低层游戏读写。

## 5. 核心实现流程

- **读/快照**：帧任务每 15 帧读取 world port；先判断是否处于 world 态，再读取单位、地图 ID/名称、出口、敌人、交互对象、掉落与导航网格。全部成功后一次发布不可变 `Snapshot<WorldSnapshot>`；不完整读取保留上一快照并周期性警告。HTTP query 只读该快照，不在请求线程访问游戏状态。
- **导航查询**：距离 handler 校验 `tx/ty` 整数，使用快照中的导航阻挡数据调用共享 `contracts/nav_bfs.h` 算法。
- **写/命令/票据**：HTTP handler 解析并校验 body，构造固定宽度 payload 后投递 typed command 并立即返回 ticket。结果 handler 在主线程调用 `QOL_PORT_WORLD_WRITE`；移动/行走/交互结果按兼容形状转换，票据保存终态结果。传送路由由 dispatch 的 op gate 控制。
- **生命周期**：install 校验 SDK、port ABI/成员并注册结果 handler；enable 初始化快照并注册 15 帧任务；disable/deactivate 停任务、销毁快照并注销结果 handler/清除 port 引用。路由注册生命周期由插件 descriptor/宿主管理。

## 6. 文件地图

| 子职责 | 文件 | 职责 |
|---|---|---|
| provider 与 HTTP | `module/app/src/main/cpp/plugins/core.world/plugin.cpp` | descriptor、capability、快照、HTTP query/command 与生命周期 |
| 插件声明 | `module/app/src/main/cpp/plugins/core.world/plugin.h` | 插件入口声明 |
| 快照 SDK | `module/app/src/main/cpp/plugins/core.world/snapshot_sdk.h` | Host SDK 快照包装 |
| 对外契约 | `module/app/src/main/cpp/contracts/world_facts.h`、`game_ports/world_port.h`、`game_ports/world_write_port.h` | facts capability 与低层读写端口类型 |
| 导航算法 | `module/app/src/main/cpp/contracts/nav_bfs.h` | 无状态 BFS 导航实现 |

接口规格：[HTTP API 参考](../api-reference.md)；游戏 UI 状态事实另见[游戏 UI 参考](../game/ui.md)。
