# `core.character` 插件

> **权威范围**：本文件是 `core.character` 的设计、架构、能力与职责的唯一权威文档。
> HTTP 端点的请求/响应字段、错误文案与票据契约以 [`api-reference.md`](../api-reference.md) §4、§7.9–§7.10、§8 为准；
> 本文件说明这些端点由谁实现、在哪个线程执行、依赖什么、边界在哪。
> 收敛决策与取舍见 [`refactor/core-character/architecture.md`](../../refactor/core-character/architecture.md)。

## 1. 定位

`core.character` 是角色、队伍、佣兵、战斗与成长领域的官方 owner/provider：对外发布角色事实与状态投影两类
capability，并在本插件内实现 35 条 HTTP 端点的 native handler。

| 维度 | 事实 |
|---|---|
| 插件形态 | 官方核心 provider（`core.<domain>`），可启停、可被依赖、读写一体 |
| 对外面 | capability `character.facts` 1.0、`character.state` 1.0；35 条 handler 路由 + 6 条元数据路由 |
| 进程内位置 | `module/app/src/main/cpp/plugins/core.character/`，静态库 `qol_plugin_core.character` |
| 线程模型 | 查询读不可变快照（HTTP 线程）；写命令投递到游戏主线程执行并回写票据 |
| 非目标 | HTTP 监听/通用传输（Kotlin host + `core.api`）、物品域（`core.item`）、世界/移动（`core.world`）、静态表（`core.data`）、游戏低层地址与 ABI（Game Adapter / Game Host） |

## 2. 内部结构与依赖

### 2.1 编译单元

插件由 2 个编译源（`plugin.cpp`、`json_slice.cpp`）、3 个头（`plugin.h`、`json_slice.h`、`snapshot_sdk.h`）
和 1 份路由声明（`routes.inc`）组成；`plugin.cpp` 的匿名命名空间内共享 9 个文件级状态。

| 文件 | 行数量级 | 职责 |
|---|---:|---|
| `plugin.cpp` | ~2230 | 路由表、能力实现、快照、JSON 解析/切片、查询 handler、写 handler、主线程结果处理器、生命周期、descriptor |
| `json_slice.h/.cpp` | ~280 | 对已缓存 JSON 做顶层键/数组下标切片，原样返回字节，不重组字段 |
| `snapshot_sdk.h` | ~95 | Host SDK 快照包装 `Snapshot<T>` 与 RAII 只读视图 `ReadView` |
| `routes.inc` | 41 行 | declarative 路由事实（owner / op 门控 / lease），由宏展开为 `kRouteFacts` |

**不采用 L0–L6 文件级分层**：本插件没有自有业务状态、事务、持久化与 UI 控件，读写全部是端口转发；
分层的收益来自 `extension_bag` / `core.item` 那类「状态 + 事务 + 控件」混居场景。判据与取舍记录在
`refactor/core-character/architecture.md` §5。

### 2.2 依赖方向

- 只 include `contracts/` 与自身目录（`check_layers` 的 `plugin_boundary` 规则），不依赖其他插件源码、
  `game/`、`infra/`、`hooks/`、`patch/`、`host/`。
- 4 个**私有端口**（Host 侧校验：非 `core.character` 声明即安装失败）：

| 端口 | 版本 | 提供方 | 用途 |
|---|---|---|---|
| `QOL_PORT_CHARACTER` | 1.0 | `game/adapter_ports/character_port.cpp` | 角色对象 `type`、`is_active_player_group`（事实面） |
| `QOL_PORT_CHARACTER_HTTP` | 1.0 | `game/adapter_ports/character_http_port.cpp` | 按 kind 生成 player/party/skills/mercenaries JSON（仅主线程帧任务调用） |
| `QOL_PORT_CHARACTER_WRITE_OPS` | 1.0 | `game/adapter_ports/character_write_ops_port.cpp` | 20 个 typed 低层写原语，失败回填 `op_err` 文案 |
| `QOL_PORT_CHARACTER_STATE` | 1.0 | `host/character_state_port.cpp` | 固定宽度 POD 状态：party / player / skills |

- 3 个可选能力依赖：
  - `item.state`：`withdraw` 的装备入库差量需要富化物品数据；能力缺失只省略 `inventory` 段，不阻止插件启用。
  - `http.registry` / `http.dispatch`：路由注册与分发；缺失时路由不可用，但插件主体仍安装。
- Host SDK 面：日志、能力查找（`capabilities.require`）、帧调度（`scheduler.add/remove`）、typed command
  （`commands.post_typed`）、票据结果注册（`ticket_result.register/unregister_typed_result_handler`）、
  快照（`snapshot_create/publish/acquire/release/destroy`）。

## 3. 能力面（provides）

| Capability | 版本 | 入口 | 语义 |
|---|---|---|---|
| `character.facts` | 1.0 | `CharacterFactsApi.type` / `is_active_player_group` | 对调用方持有的角色对象问询类型与「是否当前出战组」；插件只把指针转发给端口，不保存、不跨帧 |
| `character.state` | 1.0 | `CharacterStateApi.write_state_json(kind, out, capacity, out_size)` | 由 owner 组装的版本化 JSON 投影：`kParty` / `kPlayer` / `kSkills` |

`character.state` 的契约约束（`contracts/character_state.h`）：

- JSON 由 owner 组装，跨领域消费者只能过滤或重排，不得重组字段。
- 缓冲由调用方提供；容量不足返回 `kQolErrBlocked`（`character state buffer too small`）。
- 未进入游戏时返回 `{"error":"not in game"}` 作为正常结果体，而不是错误码。
- `party` 为 3 槽数组（空槽 `null`），`skills` 为 3 槽数组，`player` 为
  `money/map_id/x/y/active_quest/leader_slot/party_count` 对象；字段级 schema 见 API 参考 §4。

**消费者**

| 消费者 | 关系 | 用途 |
|---|---|---|
| `push_all` | `requires character.facts` | 判定出战组成员 |
| `world_tuning` | `requires character.facts` | 判定角色类型/出战组 |
| `special_equip` | `optional_requires character.facts` | 装备逻辑的角色判定，缺失时降级 |
| `core.item` | `optional_requires character.state` | 装备族写结果的 `player.main_stats` 回读（`http/write_results.cpp`） |
| host 装配（`host/assembly.cpp`） | 运行时 `require` | legacy 角色 JSON 构造；插件撤销后 require 返回 nullptr 并逐字回退 legacy |

## 4. HTTP 面

### 4.1 路由清单（35 条 handler + 6 条元数据 = 41）

**GET（15 条，query handler，读快照）**

| 组 | 路由 |
|---|---|
| 佣兵 | `/api/character/mercenary`、`/mercenary/list`、`/mercenary/{slot}` |
| 队伍 | `/api/character/party`、`/party/count`、`/leader`、`/party/{slot}`、`/party/{slot}/id`、`/api/character/party/{slot}/name`、`/party/{slot}/level`、`/party/{slot}/status`、`/party/{slot}/stats`、`/party/{slot}/equipment`、`/party/{slot}/equipment/{equip_slot}`、`/party/{slot}/skills` |

**POST（20 条，command handler）**

| 组 | 路由 | 主线程写入原语 |
|---|---|---|
| 成长 | `/api/character/grow/add_skill`、`/grow/{role}/add_stat`、`/grow/reset_stat`、`/grow/reset_skill` | `learn_action`、`add_stat`、`stat_reset`、`skill_reset` |
| 战斗 | `/api/character/combat/{role}/set_auto_attack`、`/cast_skill`、`/attack_target`、`/stop_combat`、`/combat/switch_player` | `set_auto_attack`、`cast_skill`、`attack_target`、`stop_combat`、`switch_player` |
| 队伍 | `/api/character/party/include`、`/exclude`、`/discharge`、`/withdraw` | `party_include`、`party_exclude`、`party_discharge`、`party_withdraw` |
| OP | `/api/op/character/{role}/experience`、`/hp`、`/level`、`/mp`、`/set_attr`、`/status-point`、`/api/op/party/swap` | `set_experience`、`set_hp`、`set_level`、`set_mp`、`set_attr`、`set_status_point`、`party_swap` |

全部 41 条 `op_gated` 取值见 `routes.inc`：13 条为 1（7 条 handler 路由 + 6 条 Kotlin 占位元数据），其余为 0；
`lease_required` 一律为 1（受存档槽操作租约保护）。

### 4.2 路由表三源与 fail-closed

| 声明源 | 内容 | 角色 |
|---|---|---|
| `kRoutes[]` | 35 条：method + path + `Query` 或 `QolCharacterWriteKind` 绑定 | HTTP 线程绑定表（唯一可执行来源） |
| `routes.inc` | 41 条：owner / `op_gated` / `lease_required` | declarative 事实，宏展开为 `kRouteFacts` |
| `kHttpRouteMetadata[]` | 6 条 Kotlin 占位路由 | 仅声明 owner 元数据，无 native handler |

一致性由两侧保证：

1. **编译期**：`static_assert(|kRoutes| + |kHttpRouteMetadata| == |kRouteFacts|)`。
2. **运行期**：`initialize_http_routes()` 先完成全部校验再填充 descriptor——每条绑定必须唯一匹配 facts、
   每条元数据必须唯一匹配 facts、facts 必须被全覆盖；任一不符返回 false，`plugin_install` 记
   `[core.character] http route table mismatch` 并安装失败，不注册半张路由表。重复调用幂等。

### 4.3 占位与归属例外

- 6 条 metadata 路由（`skill-point`、`skill-level`、`heal`、`rest`、`revive`、`hate`）由 Kotlin 返回 `501`，
  owner 归属本插件但无 native handler；其 `op_gated=1` 当前不可观测（CHR-03）。
- `POST /api/character/combat/{role}/set_skill_usage` 不属于本插件：由 `core.feature` 托管并保持 `501`。

### 4.4 错误语义分层

| 层 | 表现 | 范围 |
|---|---|---|
| HTTP 层 | `400` | 路径参数非法、body 非 JSON/非对象、字段缺失/类型非法/超出允许区间 |
| HTTP 层 | `404 not found` | 快照内无该槽/成员/装备/技能 |
| HTTP 层 | `503` | 快照未就绪（首次读取前或被销毁）、owner 停用（dispatch 层文案 `http owner inactive`） |
| 票据层 | `state=failed` + result `{"ok":false,"error":"<op_err>"}` | 一切依赖游戏状态的业务失败，包括 `add_stat` 剩余能力点不足 |

判据：**请求自身的性质**（格式、类型、范围）在 HTTP 线程拒绝；**世界状态相关判定**只在主线程执行。
`add_stat` 的点数判定是后者的典型：HTTP 层照常受理并返回票据，失败以票据终态表达。

## 5. 读写机制

### 5.1 查询路径：帧任务 + 不可变快照

| 环节 | 行为 |
|---|---|
| 节拍 | `scheduler.add(point=1, frame_tick, nullptr, interval=15, count=0)`，≈4Hz，替代每请求直读游戏内存 |
| 采集 | 每轮依次取 player(512B)、party(64KiB)、skills(16KiB)、mercenaries(32KiB) JSON；**四类全部成功**才发布整轮快照 |
| 状态 | `kUnavailable`（未发布/已销毁）、`kNotInGame`（任一类为 `{"error":...}`）、`kReady` |
| 失败 | 保留上一快照并限速 WARN（`snapshot read failed; keeping last snapshot`），清标志后下一次成功恢复 |
| 发布 | `Snapshot<CharacterSnapshot>::publish`（宿主持有 `shared_ptr<const vector<uint8_t>>` 副本，发布即快照替换） |
| 读取 | `Snapshot::ReadView`（RAII，持有 `QolSnapshotToken`）直接读宿主缓冲，不做整块拷贝；指针只在 handler 作用域内使用 |

快照结构约 115KB（`state` + 四段 size/char 缓冲），常驻一个版本；ReadView 消除的是每请求的栈拷贝与零初始化，
不减少常驻内存。`Snapshot<T>` 要求 payload 平凡可复制，且对齐不超过 default-new 对齐（宿主用
`std::vector<uint8_t>` 持有字节副本）。

查询 handler 的分工：

- 从快照取原样子串直接透传（`mercenary`、`party`、`party/{slot}`、`equipment`、`skills` 等）。
- 用 `json_slice`（`json_member` / `json_element` / `json_array_size` / `json_int_member`）做顶层取值，
  用 `append_int_field` 组装小对象（`status`）；缺失或负值字段按既有语义跳过。
- 组合型端点：`party/count` 取 `player.party_count`；`leader` 用 `player.leader_slot` 索引 party；
  `{slot}/id` 取 `class_idx` 并附带 `class_name`；`{slot}/status` 合并成员字段与 `skill_points`。

### 5.2 写路径：HTTP 解析 → 票据 → 主线程执行

```
HTTP 线程                                 主线程（command drain）
─────────                                 ──────────────────────
路径/body 解析与范围校验                  char_write_result(ctx, payload, size, out, capacity)
  ↓ 96B CharWritePayload（≤128B 上限）      ↓ 按 kind 分派 g_character_write_ops 对应原语
commands.post_typed(kCharWriteHandlerId)    ↓ 失败：op_err 文案 → {"ok":false,"error":...}
  ↓                                       ↓ 成功：按操作读取 state/HTTP 读面，组装瘦身 result
{"ok":true,"ticket":N}
```

- **载荷**：`CharWritePayload{kind, i0, i1, i2, i64, point_count, point_idx[8], point_amt[8]}`。单点 kind 用
  `i0..i2/i64`；批量 kind（`set_attr` / `add_stat`）用点集，逐点下发，首次失败即停止、已执行点不回滚。
- **判定位置**：`add_stat` 在主线程读 `QOL_PORT_CHARACTER_STATE` 的 party 投影，校验 `member_present`、
  `HasStatusPoint` 与 `requested <= status_point`；HTTP 线程不再用快照预判（快照最长滞后 250ms）。
- **结果组装**（按操作瘦身，字段级 schema 见 API 参考 §7.10）：

| kind 组 | result 形状 |
|---|---|
| `set_hp` / `set_mp` / `set_level` / `set_experience` / `set_status_point` / `party_swap` | `{"ok":true}` |
| `set_attr` | `{"ok":true,"set":[{"attr":idx,"value":v}…]}` |
| `add_stat` | `{"ok":true,"applied":[{"attr":idx}…]}`，状态前后可读时追加 `role{main_stats,status_point}` |
| `set_auto_attack` | `{"ok":true,"role":{"role":r,"auto_attack":<受理值>}}`（回显，不读回） |
| `cast_skill` | `{"ok":true}`；技能读面可用时追加 `role.active_skill_id` |
| `attack_target` | `{"ok":true}`；读面能确认 `target_slot`+`combat` 时才追加（当前生产投影不含该字段，实际只回 `ok`） |
| `switch_player` | `{"ok":true}`；状态可读时追加 `player.leader_slot` |
| `stop_combat` | `{"ok":true,"role":{"role":r,"combat":false}}` |
| `learn_action` | `{"ok":true,"skills":[{action_id,level}],"skill_points":n}`；不可读时退化为 `{"ok":true}` |
| `stat_reset` | `role{main_stats[5],status_point}`；不可读时 `{"ok":true}` |
| `skill_reset` | `{"ok":true,"skill_points":n}`；不可读时 `{"ok":true}` |
| `party_include` / `exclude` / `discharge` | `{"ok":true,"party":[{"slot":s,"member":obj 或 null}…]}`（前后差分）；基线不可读时 `{"ok":true}` |
| `party_withdraw` | `{"ok":true,"equipment":{"mercenary_slot":m,"slot":e,"item":null}}` + 可选 `inventory` 差分 |

- **结果上限**：handler 返回真实长度；单结果超过票据上限（16 KiB）时由票据层替换为合成摘要
  `{"ok":true/false,"truncated":true,"reason":"result_limit_exceeded"}`，不返回截断前缀（API 参考 §7.10）。
- **依赖富化**：`withdraw` 通过 `item.state` 能力在写入前后各取一次 enriched inventory 并做槽位差分；
  能力缺失或解析失败时省略 `inventory` 段，不伪造变化。

### 5.3 解析器

写面 body 使用插件自备的最小 JSON 解析器（`char_json_*`），语义对齐迁移前的 Android `JSONObject.optXxx`：
数字按 `Double.parseDouble` 解析、向零截断并饱和窄化到 int32/int64，负号/指数/前后空白按原语义处理。
`char_parse_points` 对 `attrs`/`stats` 的对象键接受属性名或 `0..4` 索引，并要求值为正整数（加点）或
`0..255`（置值）。自备解析器是插件隔离规则的结果（不得跨插件 include），不是可选项。

## 6. 生命周期

| 阶段 | 行为 |
|---|---|
| descriptor 读取 | `plugin_descriptor()` 触发路由表初始化（忽略失败），返回常量 descriptor |
| `install` | 路由表校验 → 结构/ABI/版本 → 4 端口签名校验（含 20 个写原语非空）→ 快照句柄创建并发布空快照 → 注册 typed 结果处理器；任一失败清空全局并返回 1 |
| `on_enabled` | 重新确保快照句柄、发布空快照、注册 15 帧任务；调度失败只记日志（无重试通路） |
| `on_disabled` | 停止帧任务、销毁快照（查询返回 503） |
| `deactivate` | 停任务、销毁快照、注销结果处理器、清空端口引用 |
| `fill_status` | `out.enabled = g_port != nullptr`；**已知缺陷**：installed_disabled 仍报 `enabled=true`，且 host 当前无调用点（ENG-04） |

Feature：`core.character.provider`，`default=1`，Hidden，toggle，回调为空操作。provider 是常驻需求，
用 hidden+default true 表达；`enable` 由 Host 独占管理，插件不读写。

## 7. 职责边界

**拥有**

- 角色、队伍、佣兵、技能与玩家状态投影（`character.state`）与角色事实（`character.facts`）。
- 上述 35 条端点的参数校验、载荷组装、结果瘦身与票据结果处理。
- 20 个 typed 写命令的语义分派（成长/战斗/队伍/OP），以及写后差量结果的组装口径。

**不拥有**

| 面 | 归属 |
|---|---|
| HTTP 监听、路由解析、op 门禁、票据存储与上限 | Kotlin host + `core.api`（dispatch/registry） |
| 物品事实、物品读写、背包/商店 | `core.item` |
| 世界地图、移动、传送 | `core.world` |
| 存档槽、存档数据与备份 | `core.save` / `save_backup` |
| 静态表、文本与名称解析 | `core.data` |
| 游戏地址、符号、ABI、低层读写 | Game Adapter / Game Host 端口 |
| 界面状态、面板与控件 | `core.ui_state` / `core.custom_ui` |

**消费者与级联**：停用本插件时，`push_all`、`world_tuning` 按必需依赖进入 degraded；`special_equip`、
`core.item` 的可选能力边失效并降级；host legacy 角色 JSON 构造回退到旧实现；35 条 handler 路由在 dispatch
层返回 `503`，6 条元数据路由仍由 Kotlin 响应。

## 8. 已知限制与开放项

| 项 | 影响 | 状态 |
|---|---|---|
| CHR-03：6 条 Kotlin 占位路由 `op_gated=1` 失效 | 501 抛于 op 门禁之前，op 开/关不可区分 | 开放 |
| CHR-11：写后读己写窗口 | GET 最长 250ms 内返回旧快照，契约未声明 | 开放 |
| 快照常驻约 115KB、每 15 帧重建四类 JSON | 内存与主线程周期成本固定；换页/切图瞬间可能读到上一轮 | 设计现状 |
| 自备 JSON 解析器与 `json_slice` | 与 `core.item`/`core.data`/`core.api` 同型重复；插件隔离规则下无法共享 | 已知成本 |
| 单结果 16 KiB 上限 | 队伍满配时差量较大，可能触发合成摘要，调用方需按 `truncated` 处理 | 契约现状 |
| `fill_status` 语义 | installed_disabled 报 enabled；host 无调用点 | ENG-04 |
| 调度失败无重试 | `on_enabled` 中 `scheduler.add` 失败仅告警 | 与 core.world WLD-02 同型 |

已收敛（本批，`fc71ff02`）：`add_stat` 双判定（CHR-12）、路由表静默漏配（CHR-13）、查询快照整块拷贝与
重复 path 解析器（CHR-14）。

## 9. 文件地图

| 子职责 | 文件:位置 | 说明 |
|---|---|---|
| 路由声明 | `routes.inc` | 41 条 owner/门控/lease 事实 |
| 路由绑定与初始化 | `plugin.cpp` `kRoutes` / `initialize_http_routes` | 35 条绑定、三源一致性校验、fail-closed |
| 能力实现 | `plugin.cpp` `kCharacterFactsApi` / `write_character_state_json` | facts 转发 + state JSON 投影 |
| 快照 | `plugin.cpp` `CharacterSnapshot` / `update_snapshot` / `frame_tick`；`snapshot_sdk.h` | 三态快照、15 帧节拍、RAII 只读视图 |
| JSON 切片 | `json_slice.h/.cpp` | 顶层键/下标切片，原样字节 |
| body 解析 | `plugin.cpp` `char_json_*` / `char_parse_points` | 迁移前 `JSONObject.optXxx` 语义 |
| 查询 handler | `plugin.cpp` `query_handler` | 15 条 GET 的快照切片与组合 |
| 写 handler | `plugin.cpp` `char_write_command` | 20 条 POST 的校验与 typed 投递 |
| 结果处理器 | `plugin.cpp` `char_write_result` | 主线程原语分派、判定与结果瘦身 |
| 生命周期/descriptor | `plugin.cpp` `plugin_install` … `plugin_descriptor` | 端口绑定、快照与结果处理器注册、feature |
| 契约 | `contracts/character_facts.h`、`character_state.h`、`game_ports/character_*.h` | 能力与端口 ABI |
| Kotlin 入口 | `CharacterController.kt`、`PartyController.kt`、`CombatController.kt`、`OpController.kt` | 路由声明与原生 dispatch 转发 |

## 10. 验证与证据

| 面 | 位置 |
|---|---|
| Host 用例 | `tests/test_character_facts_plugin.cpp`（descriptor/能力/生命周期/结果）、`test_character_facts_dispatch.cpp`（15 GET + 20 POST 接缝）、`test_character_json_slice.cpp`、`test_character_http_port.cpp` |
| 静态门 | `check_layers.py`、`check_arch.py`、`check_http_routes.py`（`core.character` 41 条必须与冻结集一致） |
| 问题台账 | `refactor/issues.md` §3（CHR-*） |
| 收敛决策 | `refactor/core-character/architecture.md` |
| 真机卡 | `refactor/verification.md`（VM-K5-04e、VM-K5-08h 等），当前仍有 `NOT_ACCEPTED` 欠账 |

当前批次仅完成静态检查（`check_layers` / `check_arch` / `check_http_routes` / `clang -fsyntax-only`），
Host CTest 与真机未运行；本文件不构成验收通过结论。
