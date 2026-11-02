# `core.character` 收敛方案（v1）

状态：已实施。实施口径见本文件；插件事实以 [`docs/reference/plugins/core.character.md`](../../reference/plugins/core.character.md)
为准，验收登记见 [`docs/refactor/verification.md`](../verification.md)。

## 1. 结论与范围

`core.character` 是纯转发型领域 owner：无自有业务状态、无持久化、无 UI 控件、无互斥量，读写全部经 Host SDK、
owner 端口与 Game Host 端口完成。`plugin-architecture-guide.md` 中收益最大的部分（业务状态 / 事务 / 持久化 /
控件解耦）在本插件没有对应问题，因此**不做 L0–L6 整体分层重构**，只收敛四类真实偏差。

决策依据（用户裁决）：

1. HTTP API 契约可以变更，不考虑迁移与兼容层。
2. 校验/判定放在哪个线程，按"是否存在跨线程风险"决定：无风险留在 HTTP 线程，有风险移到主线程。

## 2. 决策记录

| # | 决策 | 理由 |
|---|---|---|
| D1 | 删除 HTTP 线程的 `add_stat` 剩余能力点预判，业务判定只留主线程 | 该判定读的是查询快照，最长滞后 15 帧（≈250ms），与主线程判定构成两份真相：快照陈旧时会误拒合法请求。参数语法/范围校验留在 HTTP 线程 |
| D2 | 路由绑定表 + owner 元数据必须完整覆盖 `routes.inc` 声明，编译期计数断言 + 运行期 fail-closed | 原实现在 `initialize_http_routes` 中匹配失败即 `continue`，漏配表现为"路由静默消失"，无日志无断言 |
| D3 | 合并同文件内重复的 path 整数解析器 | `parse_path_int` 与 `char_path_int` 逻辑逐字相同；此前 ITM-07 加固时两处分别修改，属复制漂移风险 |
| D4 | 查询快照改只读视图，取消每次请求的整块拷贝 | 快照结构 115KB；原实现每次 GET 在栈上零初始化 + memcpy 一份，`add_stat` 分支另有一份 |
| D5 | `character.state` 保持 JSON 文本输出 | `contracts/character_state.h` 已规定 owner 组装 JSON 投影、其他 owner 只能过滤重排；改 typed 属跨插件契约变更，收益不抵成本 |
| D6 | 插件内不引入共享 JSON 解析器替代 | `ARCHITECTURE.md` §3.2 禁止插件间 include；`core.item/http/json_lite`、`core.data/json`、`core.api/json_field` 都是同型自备件。收口需要 Host SDK 提供 typed body 解析，属模块级议题 |

## 3. 线程归属审计

`ARCHITECTURE.md` §7 要求：query 只读快照与不可变配置，command 只解析为 owner 命令并立即返回票据，操作在主线程执行。

| 入口 | 线程 | 访问的数据 | 结论 |
|---|---|---|---|
| `query_handler` 62 条分支段（15 条 GET） | HTTP | Host 快照 + 自有 JSON 切片 | 无风险，保留 HTTP 线程 |
| `char_write_command` 参数解析 | HTTP | 请求体（私有内存）+ 路径参数 | 无风险，保留 HTTP 线程 |
| ~~`kAddStat` 剩余点数预判~~ | ~~HTTP~~ | ~~查询快照里的 `status_point`~~ | **有风险**：业务状态判定，已删除（D1） |
| `char_write_result` 全部分支 | 主线程（票据结果处理器） | owner state port、HTTP 读面、`item.state` 能力 | 保持主线程 |
| `update_snapshot` / `frame_tick` | 主线程帧任务 | HTTP 读面四类 JSON | 保持主线程 |
| `plugin_install` / `on_enabled` / `on_disabled` / `deactivate` | 装配线程 | SDK、端口、快照句柄 | 保持 |

判定规则（供后续同类收敛复用）：**只读不可变数据 = 可留 HTTP 线程；读可变/业务状态 = 必须主线程。**
参数语法、范围、类型校验属于"请求自身的性质"，留在 HTTP 线程不构成跨线程风险。

## 4. 实施方案

### D1 写面判定单点化

- 删除 `plugin.cpp` `char_write_command` 中 `kAddStat` 分支的快照读取与 `requested > remaining` 预判，只保留
  `char_parse_points` 的语法/范围校验。
- 主线程 `char_write_result` 的 `kAddStat` 分支保持唯一权威：读 `QOL_PORT_CHARACTER_STATE` 的 party 投影，判定
  `member_present`、`kQolCharacterStateHasStatusPoint` 与 `requested > status_point`。
- 契约变更：超出剩余点数的请求由"HTTP 400 `no status point`"改为"HTTP 200 `{"ok":true,"ticket":N}` +
  票据终态 `{"ok":false,"error":"no status point"}`"。与 `api-reference` §7.10 "POST 先校验参数、业务结果由票据表达"
  的口径一致。

### D2 路由表 drift 防护

- 三个声明源职责固定：`kRoutes`（35 条，HTTP 线程绑定表，含 `Query` / `QolCharacterWriteKind`）、
  `routes.inc`（41 条 declarative owner 事实，含 6 条 Kotlin 占位元数据）、`kHttpRouteMetadata`（6 条）。
- 编译期断言：`|kRoutes| + |kHttpRouteMetadata| == |kRouteFacts|`。
- 运行期：`initialize_http_routes()` 返回 `bool`，先完成全部匹配再填充；任一绑定缺失、facts 数量不符或重复匹配
  返回 false。`plugin_install` 调用失败时记录 error 日志并返回非 0，宿主按安装失败处理（fail-closed，不注册半张路由表）。
- `routes.inc` 文本格式不变：`plugins/core.api/routes.inc` 是 `check_http_routes.py` 的聚合事实源，插件内文件保持同构。

### D3 解析器合并

- 删除 `parse_path_int`，全部调用点改用 `char_path_int`；保留位数 ≤10、逐位 INT32 溢出守卫与既有失败语义
  （query 侧 400/404，command 侧 400 `<field> required`）。

### D4 快照只读视图

- `snapshot_sdk.h` 在 `Snapshot<T>` 上增加 `const T*` + `QolSnapshotToken` 的 RAII 只读视图（禁止拷贝），析构即
  `snapshot_release`；删除已无调用点的 `read()`。
- `query_handler` 改用视图，所有早退路径由守卫释放；响应体仍是同步 memcpy，指针不跨作用域。

## 5. 被否决的方案

| 方案 | 否决理由 |
|---|---|
| L0–L6 文件级分层（对齐 `core.item`） | `core.item` 的分层驱动力是袋源注册、事务回滚、物品句柄代际、脏纪元快照重建；本插件没有自有状态与事务，拆分只增加文件与调试路径，行为收益为零 |
| `character.state` 改 typed POD | 跨插件契约变更（`contracts/character_state.h` + `core.item` 等消费者），且 JSON 投影是 owner 语义的稳定出口 |
| 引入共享 JSON 解析器 | 违反插件隔离规则；正确收口点是 Host SDK 的 typed body 解析，需模块级立项 |
| 抓取 `core.api/routes.inc` 作为唯一路由源 | 属插件外改动，且该文件是 Kotlin controller / API 参考 / native 三方核对基线，需独立批次 |

## 6. 验收门

本批**不做真机验证**（多代理共享一台设备，禁止并发占用）。可执行证据：

| 门 | 命令/口径 | 状态 |
|---|---|---|
| 层依赖 | `uv run python scripts/maintenance/check_layers.py` | 通过（违规 0，新增 0） |
| 架构基线 | `uv run python scripts/maintenance/check_arch.py` | 通过（layers/offsets/jni/resources 全绿） |
| 路由集合 | `uv run python scripts/maintenance/check_http_routes.py` | 通过（137/137，重复 0；`core.character` 41 不变） |
| 语法检查 | `clang++ -std=c++17 -fsyntax-only -I module/app/src/main/cpp plugins/core.character/plugin.cpp` | 通过 |
| Host 用例 | `test_character_facts_dispatch` / `test_character_facts_plugin` 新增与修正断言 | 已改写断言，**未运行**（用户口径：只跑静态检查） |
| 真机 | 明确跳过 | 不得据此宣告插件验收通过 |

未运行 Host CTest 与 Android 构建是本批的已知证据缺口：不得以静态检查替代行为验收，插件整体状态仍为
`NOT_ACCEPTED`，沿用 `docs/refactor/verification.md` §5 与 `docs/refactor/issues.md` 的欠账登记。

## 7. 影响面

| 面 | 变化 |
|---|---|
| HTTP API | `POST /api/character/grow/{role}/add_stat` 超点数由 400 改为 200 + 票据失败（唯一对外行为变更） |
| 路由数/owner/门控 | 不变（35 handler + 6 元数据 = 41） |
| 能力面 | `character.facts`、`character.state` 不变 |
| 端口/ABI | 不变 |
| 插件外 | 仅测试与文档；`contracts/`、`host/`、`core.api`、Kotlin 未改动 |

## 8. 后续观察项

1. 查询快照为 15 帧节拍，写后读己写窗口未在契约中声明（CHR-11）。
2. 6 条 Kotlin 占位路由的 `op_gated` 失效（CHR-03），本批不涉及。
3. 快照结构 115KB 且按请求投影组装响应；当前只读路径已无整块拷贝，如需进一步降低内存占用应改为
   "元数据快照 + 分页查询"，属独立批次。
