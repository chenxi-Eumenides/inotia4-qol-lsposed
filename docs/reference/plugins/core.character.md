# `core.character` 插件

## 1. 作用与架构定位

`core.character` 是角色、队伍和佣兵领域的官方 provider/owner，发布角色 facts/state，并实现角色成长、队伍与战斗相关 API 命令。它遵循 `ARCHITECTURE.md` 的窄领域 owner 模型，通过能力、Host SDK 与 Game Host ports 协作。

## 2. 功能面

### HTTP 路由

**GET 查询**：

- `/api/character/mercenary`、`/api/character/mercenary/list`、`/api/character/mercenary/{slot}`
- `/api/character/party`、`/api/character/party/count`、`/api/character/leader`
- `/api/character/party/{slot}`、`/api/character/party/{slot}/id`、`/api/character/party/{slot}/name`、`/api/character/party/{slot}/level`、`/api/character/party/{slot}/status`、`/api/character/party/{slot}/stats`、`/api/character/party/{slot}/equipment`、`/api/character/party/{slot}/equipment/{equip_slot}`、`/api/character/party/{slot}/skills`

**POST 写命令**：

- 成长：`/api/character/grow/add_skill`、`/api/character/grow/{role}/add_stat`、`/api/character/grow/reset_stat`、`/api/character/grow/reset_skill`
- 战斗：`/api/character/combat/{role}/set_auto_attack`、`/api/character/combat/switch_player`、`/api/character/combat/{role}/cast_skill`、`/api/character/combat/{role}/attack_target`、`/api/character/combat/{role}/stop_combat`
- 队伍：`/api/character/party/include`、`/api/character/party/exclude`、`/api/character/party/discharge`、`/api/character/party/withdraw`
- OP：`/api/op/character/{role}/experience`、`/api/op/character/{role}/hp`、`/api/op/character/{role}/level`、`/api/op/character/{role}/mp`、`/api/op/character/{role}/set_attr`、`/api/op/character/{role}/status-point`、`/api/op/party/swap`
- OP 战斗：`/api/op/combat/{role}/heal`、`/api/op/combat/{role}/rest`、`/api/op/combat/{role}/revive`、`/api/op/combat/{role}/hate`

`/api/character/combat/{role}/set_skill_usage`、`/api/op/character/{role}/skill-point`、`/api/op/character/{role}/skill-level` 是 owner 元数据归本插件但当前由 Kotlin 占位响应，不在 native descriptor handler 中。特别是 `set_skill_usage` 保持 501；以上区分以 routes 与真实 handler 为准。

正式路径参数、响应和错误契约见 [API 参考 §4、§7.9–§7.10](../api-reference.md)。

### Capabilities

- 提供 `character.facts` 的角色类型与 active-player-group 等事实查询。
- 提供 `character.state`：按 kind 输出 party、player、skills 状态 JSON 投影。
- 必需 `http.registry`、`http.dispatch`；可选 `item.state`，用于角色装备物品等投影所需的物品富化。

## 3. 依赖前置与消费者

- 必需 ports：`QOL_PORT_CHARACTER`、`QOL_PORT_CHARACTER_HTTP`、`QOL_PORT_CHARACTER_WRITE_OPS`、`QOL_PORT_CHARACTER_STATE`。
- Host SDK 使用统一不可变快照、调度器、能力查找、typed command、ticket result 注册/注销与日志。
- 角色事实/状态由物品相关 owner 与其他实际消费者经 capability 访问；可选 `item.state` 只用于所需的物品数据富化。Kotlin Character/Party/Combat/Op controllers 是 HTTP 消费入口，不是领域 owner。

## 4. 职责边界

- 拥有角色、队伍、佣兵及 skills/player/party 状态投影，以及实现中的成长、队伍与战斗命令。
- 不拥有物品事实和物品操作（`core.item`）、世界地图/移动（`core.world`）、静态表数据、HTTP 监听/通用 JSON 传输（Kotlin host/`core.api`）或游戏低层地址/ABI（Game Adapter）。
- Kotlin 可做传输所需参数处理；owner 负责 native typed command、结果与兼容状态投影。低层读写由 Game Host port 实现。

## 5. 核心实现流程

- **读/快照**：每 15 帧通过 `QOL_PORT_CHARACTER_HTTP` 读取 player、party、skills、mercenaries JSON。四项都可用才整体发布 `Snapshot<CharacterSnapshot>`；未进入游戏时发布 not-in-game 状态；读失败保留上一快照并告警。HTTP handler 从快照切片、筛选成员或组装小型响应。
- **Capability state**：`character.state` 的 party/player/skills 输出通过 state port 读取 POD，再由 owner 序列化为版本化 JSON 投影；不将游戏指针或低层对象暴露给消费者。
- **写/命令/票据**：POST handler 校验路径/body 并组装有界 POD payload，通过 Host SDK 投递 typed command，立即返回 ticket。主线程 result handler 按 `QolCharacterWriteKind` 调用写 ops port，必要时读取 owner state 生成瘦身结果并存入票据。op 路由的门控由 API dispatch 层按 routes 声明执行。
- **生命周期**：install 校验 SDK 与四个 ports、初始化快照和 handler；enable 启动快照帧任务；disable/deactivate 停任务、销毁快照、注销 handler 并清空依赖引用。descriptor 的 optional item.state 缺失不阻止插件主体启用。

## 6. 文件地图

| 子职责 | 文件 | 职责 |
|---|---|---|
| 主要实现 | `module/app/src/main/cpp/plugins/core.character/plugin.cpp` | descriptor、能力、快照、query/command handler 与结果处理 |
| JSON 切片 | `module/app/src/main/cpp/plugins/core.character/json_slice.h`、`json_slice.cpp` | 对已缓存 JSON 执行字段/数组切片读取 |
| 插件与快照 | `module/app/src/main/cpp/plugins/core.character/plugin.h`、`snapshot_sdk.h` | 入口声明与 Host SDK 快照包装 |
| 状态/事实契约 | `contracts/character_facts.h`、`contracts/character_state.h` | capability API 与 state 投影类型 |
| Game Host 契约 | `contracts/game_ports/character_http_port.h`、`character_state_port.h`、`character_write_ops_port.h` | HTTP JSON 读面、typed state 与写操作端口 |
| Kotlin 入口 | `module/app/src/main/java/com/inotia4/qol/api/controller/CharacterController.kt`、`PartyController.kt`、`CombatController.kt`、`OpController.kt` | 路由解析和 service/bridge 调用 |

接口规格：[HTTP API 参考](../api-reference.md)。
