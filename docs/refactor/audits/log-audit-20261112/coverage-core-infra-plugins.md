# Core 基础设施插件日志覆盖审计

审计口径：依据 `docs/reference/logging-standards.md` §1–6；只评估关键生命周期/操作节点，不要求逐帧状态日志。`present`=有可用日志覆盖，`weak`=有覆盖但等级、正文、事件/生命周期不完整，`missing`=关键节点没有日志。等级遵循失败后果；逐帧采样仅建议 `D`。

## core.api

入口/描述符：`module/app/src/main/cpp/plugins/core.api/plugin.cpp:258-307`；路由命令：`312-350`；路由元数据见 `owned_routes.inc`，能力包括 HTTP registry/dispatch 与 API 服务开关。

| 功能 / 生命周期节点 | 证据 | 结论 | 审计判断 |
|---|---|---|---|
| API registry/dispatch 提供与启停 | `plugin.cpp:258-280` | missing | 安装、停用均无生命周期日志；成功启停是有限状态变化，应 `I`。install 缺 SDK 返回失败且无日志，可导致能力不可用，应 `E`。 |
| API 服务配置校验、配置拒绝 | `plugin.cpp:235-255` | missing | 非法地址/端口返回 `QolResult`，无 `W`；仅关键拒绝建议记录 endpoint/配置键及 reason，不记配置原文。 |
| 生命周期 HTTP 请求：解析/拒绝/入队 | `plugin.cpp:312-350` | missing | 请求解析拒绝、未知 action、队列拒绝均无日志；单次拒绝/失败应 `W`，带 op/对象 id/reason。成功入队和命令完成当前均无事件日志；仅成功状态变化记录 `I`，不要把每个轮询/请求都当状态变化。 |

## core.data

入口/描述符：`module/app/src/main/cpp/plugins/core.data/plugin.cpp:568-610`；静态数据导入：`612-668`；提供 `static_data` API（地图名、出口、信息、tile）及 HTTP 查询端点。

| 功能 / 生命周期节点 | 证据 | 结论 | 审计判断 |
|---|---|---|---|
| 静态数据服务启用/停用、API 提供 | `plugin.cpp:568-610` | missing | 安装/停用无日志；成功生命周期变化应 `I`，安装失败若阻断服务则 `E`。 |
| 导入开始、单资源导入成功 | `plugin.cpp:612-639` | missing | 成功无须逐文件刷 `I`；可在整批导入完成后记录一次 `I`（文件数/字节数），不记录内容。非法路径、超限、未开始导入等拒绝无 `W`。 |
| 导入完成、校验失败与回退 | `plugin.cpp:641-668` | missing | 数量/字节不匹配会清 staging 并令目录不可用、清缓存；失败原因没有记录。操作失败但进程可继续通常 `W`；若无有效静态目录且服务能力整体不可用，最终状态按影响定 `E`。当前无 fallback 成功事件。 |
| 静态地图读取、解析/容量失败 | `plugin.cpp:124-230,476-560` | missing | 缺目录/资源属于可选路径用 `D`；存在但格式不符、输出容量不足且请求失败用 `W`。避免每个查询重复报同一底层故障。 |

## core.feature

入口/描述符：`module/app/src/main/cpp/plugins/core.feature/plugin.cpp:159-239`；资源回调与功能定义：`176-208`。功能为兼容路由占位和屏蔽暴击特效。

| 功能 / 生命周期节点 | 证据 | 结论 | 审计判断 |
|---|---|---|---|
| 插件启用/停用 | `plugin.cpp:159-174` | missing | 安装/停用状态变化无日志；建议 `I`，安装失败（含 hook/resource 不可用）`W`。 |
| 屏蔽暴击特效开关绑定/变更 | `plugin.cpp:191-208` | missing | 开关改变实际行为但无日志；仅值变化时 `I feature=... enabled=...`。兼容占位回调无业务状态，不建议补日志。 |
| 资源绑定与缺失 | `plugin.cpp:176-188` | missing | backup 绑定无成功日志；关键资源缺失会让该功能不可用，应 `W hook/resource=... reason=...`；不要记录逐帧拦截/暴击分支，除排障外只用受门控 `D`。 |

## core.character

入口/描述符：`module/app/src/main/cpp/plugins/core.character/plugin.cpp:2126-2304`；快照更新/失败：`486-547`；读写处理：`592-...`、`1227-1428`。

| 功能 / 生命周期节点 | 证据 | 结论 | 审计判断 |
|---|---|---|---|
| 路由表校验、安装/停用/启停 | `plugin.cpp:2126-2254` | weak | 路由表不匹配有日志 `2127`，成功 install/deactivate/enable/disable 有日志 `2214,2221,2248,2254`。但不匹配日志等级 `2` 应核对为可继续的 `W`，正文缺少固定 verb/结构；其余状态变化正文结构不一致。 |
| 角色快照读取与降级 | `plugin.cpp:493-543` | weak | 读取失败仅首次 `W`，保留上次快照，恢复后重置标记；没有恢复/回退成功事件，失败 reason 过于笼统。补充失败来源区分并在恢复时只记一次 `I fallback=previous_snapshot`（确有降级时）。不记录逐帧成功读取。 |
| HTTP 角色快照查询 | `plugin.cpp:592-...` | weak | 未就绪/不在游戏等有响应拒绝，但无日志；可选未进游戏属于正常情形，不应 `W`。仅能力不可用或结构读取失败时由快照层记录一次，避免 handler 重复。 |
| 角色状态写操作与结果 | `plugin.cpp:1227-1428,1756-...` | missing | 入参拒绝、命令提交失败、主线程写失败/成功无日志；拒绝或单次失败 `W`（带 op/role/reason），实际写入成功且状态变更 `I`。 |

## core.quest

入口/描述符：`module/app/src/main/cpp/plugins/core.quest/plugin.cpp:558-700`；快照读取：`153-210`；写命令：`430-485`。

| 功能 / 生命周期节点 | 证据 | 结论 | 审计判断 |
|---|---|---|---|
| 安装/启停/停用与快照调度 | `plugin.cpp:558-643` | weak | install 有成功 `I`（599）；启用失败有未就绪日志（625）但缺 sched add 失败日志，disable/deactivate 无日志。补充关键生命周期变化与 scheduler 失败 `W`。 |
| 任务快照读取/发布 | `plugin.cpp:153-210` | missing | 周期读取/失败路径无日志，接口不可用会继续返回旧/不可用快照；首次失败、持续失败和恢复均无明确记录。只在状态连续失败开始/恢复时记录 `W`/`I`，不要逐帧记录快照。 |
| 任务查询及不在游戏/未就绪 | `plugin.cpp:235-...` | missing | 读请求通过错误响应拒绝但无日志。正常不在游戏不需要 `W`；快照不可用由所有者单点记录。 |
| 退出任务命令提交/执行 | `plugin.cpp:430-485` | missing | 参数拒绝、队列拒绝、实际写失败和成功均无日志；失败 `W`，完成状态改变 `I`。 |

## core.system

入口/描述符：`module/app/src/main/cpp/plugins/core.system/plugin.cpp:237-328`；快照与读失败：`69-130,157-193`。

| 功能 / 生命周期节点 | 证据 | 结论 | 审计判断 |
|---|---|---|---|
| 安装/启停/停用 | `plugin.cpp:245-280` | present | install/deactivate/enable/disable 均有记录。正文 `snapshot unavailable`、`snapshot scheduler unavailable`缺 verb、对象与 reason；应结构化。 |
| system/game/info/frame 快照读取 | `plugin.cpp:76-126` | weak | 读取失败有一次性 `W` 且保留旧值，恢复清除告警状态；正文未说明具体操作对象/哪类 snapshot，恢复无日志。只需在首次失败及恢复时各记一次，绝不逐帧输出成功。 |
| API 查询与单次事件读取 | `plugin.cpp:157-193` | weak | snapshot 未就绪有 HTTP 503，但没有入口日志；events 读失败会调用同一告警。可选未就绪窗口不必另记；持续数据能力失败应由 owner 的一次性 `W` 覆盖，避免重复。 |

## core.ui_state

入口/描述符：`module/app/src/main/cpp/plugins/core.ui_state/plugin.cpp:1194-1266`；状态读写/注册/发布：`111-213,682-840`；native 快照 tick：`237-267`。

| 功能 / 生命周期节点 | 证据 | 结论 | 审计判断 |
|---|---|---|---|
| UI 状态插件启停/快照调度 | `plugin.cpp:1150-1225` | missing | 没有日志。启停/调度建立或不可用属于有限生命周期事件，分别建议 `I`/`W`。 |
| native UI 状态快照采集、失效/恢复 | `plugin.cpp:237-267` | missing | 读端口缺失或读取失败会令快照失效，但无日志；若连续失败影响 UI 状态能力，首次 `W`，恢复 `I`。成功逐帧采样不得记 `I`，调试细节仅 `D` 且门控。 |
| 自定义 panel/dialog 与外部 UI 状态写入 | `plugin.cpp:111-210,822-840` | missing | 参数非法、sequence/generation 冲突均返回错误，无日志；拒绝用 `W` 并带 kind/reason，避免记录用户文本。只有确实改变可见状态才建议 `I`，高频发布不可逐次 info。 |
| UI 状态读取、注册表与 HTTP 写操作 | `plugin.cpp:327-840,853-1105` | missing | 状态查询/注册、对话选择及面板操作缺少拒绝/执行结果日志。失败按单次操作 `W`；仅成功且实际状态改变 `I`。 |

## core.world

入口/描述符：`module/app/src/main/cpp/plugins/core.world/plugin.cpp:682-788`；快照失败：`132-180`；查询/写入：`331-...`,`491-602`。

| 功能 / 生命周期节点 | 证据 | 结论 | 审计判断 |
|---|---|---|---|
| 安装/启停/停用及快照调度 | `plugin.cpp:701-740` | weak | 生命周期有日志，但正文为自然语言/带插件名，不符合 `verb key=value`；快照初始化/调度失败覆盖存在。成功启停应保留并结构化。 |
| 世界/地图状态快照读取 | `plugin.cpp:172-252` | weak | 每累计 64 次读取失败才记一次 `W`，保留上一份完整快照；能抑制高频但丢失首次失败时机，无法区分失败源，恢复无日志。改为错误状态边沿的一次 `W`，恢复/降级回退成功一次 `I`；不记每帧正常采样。 |
| 世界状态查询及可选对象缺失 | `plugin.cpp:331-...` | weak | 快照不可用返回 503，未单独记日志；不在游戏已有正常状态，不应作为 `W`。由采集 owner 记录实质失败即可，避免重复。 |
| 世界写入（移动/交互/传送） | `plugin.cpp:491-602` | missing | 参数拒绝、命令队列失败、执行期失败/成功均无日志；拒绝与单次失败 `W`，执行成功且状态改变 `I`。 |

## 优先补全项

1. `core.ui_state`：为 native 状态快照失效/恢复、自定义 UI 发布拒绝及成功状态变化建立边沿日志；不记录逐帧成功采样。
2. `core.data`：补齐静态数据导入失败与整批完成/目录不可用状态，区分可恢复拒绝 `W` 与能力整体不可用 `E`。
3. `core.api`：记录服务能力启停、配置拒绝和 lifecycle 命令拒绝/入队结果；请求细节不重复记，非法输入为 `W`。
4. `core.feature`：补插件生命周期、功能开关状态变化和资源绑定失败日志；拦截热路径只允许受门控 `D`。
5. `core.quest`：补快照连续失败/恢复与命令失败/成功日志，修正缺失的 disable/deactivate 生命周期覆盖。
6. `core.world`：将自然语言生命周期日志结构化，快照改为失败/恢复边沿记录并补写操作结果。
7. `core.character`：补写命令结果覆盖，改善快照失败原因与恢复事件；现有 `log_message` 调用逐条遵循标准正文结构。
8. `core.system`：保留现有生命周期与快照失败记录，补恢复事件并结构化未就绪/调度失败正文。
