# 日志审计批次 1：bridge / infra / game-access 清单

逐条基于 `logging-standards.md` 的等级、正文和频率判据；行号为当前源码行号。

## 现有日志

### `module/app/src/main/cpp/bridge/native/gamebridge.cpp`

| file:line | domain | level_now | message 摘要 | event_class | level_target | disposition | rationale |
|---|---|---|---|---|---|---|---|
| gamebridge.cpp:65 | core | E | callback destructor attach 失败 | JNI 清理失败 | W | demote | 只影响释放全局引用，宿主与其他能力继续运行。 |
| gamebridge.cpp:113 | core | E | config receipt JNI 操作异常 | JNI 单次回调失败 | W | augment | 单次通知失败不使模块不可用；补 `op`、`reason`。 |
| gamebridge.cpp:126 | core | E | receipt callback context 过期 | 回调生命周期异常 | W | augment | 当前通知无法执行但进程继续；补请求/订阅标识及 `reason`。 |
| gamebridge.cpp:134 | core | E | receipt callback attach 失败 | JNI 单次回调失败 | W | augment | 仅丢弃此次回执；补 `op`、`reason`。 |
| gamebridge.cpp:145 | core | E | receipt JSON 分配失败 | JNI 单次回调失败 | W | augment | 本次回执失败而非全局不可用；补 request id 与 `reason`。 |
| gamebridge.cpp:282 | core | E | nativeInit assembly failed | 初始化装配失败 | E | augment | 装配失败使 native 功能不可用；改结构化 `fail op=native_init reason=assembly_failed`。 |
| gamebridge.cpp:345 | core | E | receipt callback context 分配失败 | 单次订阅失败 | W | demote | 本次订阅失败但模块仍运行。 |
| gamebridge.cpp:370 | core | E | receipt callback registration allocation failed | 单次订阅失败 | W | augment | 失败局限此订阅；补 `op`、`reason`。 |
| gamebridge.cpp:381 | core | E | receipt observer registration allocation failed | 单次订阅失败 | W | augment | 不影响其他模块能力；补 request id 与 `reason`。 |
| gamebridge.cpp:385 | core | E | receipt observer registration failed | 单次订阅失败 | W | augment | 通知订阅失败可绕过；补 `op`、`reason`，异常不能改变等级。 |

### `module/app/src/main/cpp/bridge/native/gamebridge_data.cpp`

| file:line | domain | level_now | message 摘要 | event_class | level_target | disposition | rationale |
|---|---|---|---|---|---|---|---|
| gamebridge_data.cpp:54 | core | E | health callback destructor attach 失败 | JNI 清理失败 | W | demote | 只影响释放全局引用，health/native 仍运行。 |
| gamebridge_data.cpp:71 | core | E | module health JNI 操作异常 | JNI 单次回调失败 | W | augment | 单次状态通知失敗；补 `op`、`reason`。 |
| gamebridge_data.cpp:91 | core | E | health callback thread attach 失败 | JNI 单次回调失败 | W | augment | 仅当前通知失败；补 `op`、`reason`。 |
| gamebridge_data.cpp:101 | core | E | health callback JSON allocation failed | JNI 单次回调失败 | W | augment | 本次通知数据构造失败而非模块失效；补 `reason`。 |

### `module/app/src/main/cpp/bridge/native/gamebridge_public_export.cpp`

| file:line | domain | level_now | message 摘要 | event_class | level_target | disposition | rationale |
|---|---|---|---|---|---|---|---|
| gamebridge_public_export.cpp:52 | platform | E | public export skipped: jni env unavailable | 单次导出调用失败 | W | demote | 当前调用返回 false，后续调用可重试，非模块整体不可用。 |
| gamebridge_public_export.cpp:64 | platform | E | public export failed: java exception | 单次导出调用失败 | W | augment | 单次调用失败且异常已清除；补 `op`、`reason`。 |
| gamebridge_public_export.cpp:82 | platform | E | bridge register method lookup failed | 可选 bridge 注册失败 | W | augment | Download 导出链路不可用但游戏继续；补失败方法 `symbol` 与 `reason`。 |
| gamebridge_public_export.cpp:86 | platform | E | bridge register method missing | 可选方法查找失败 | W | augment | 仅公共导出功能不可用；补缺失 `symbol` 与 `reason`。 |
| gamebridge_public_export.cpp:91 | platform | E | bridge register global ref failed | 可选 bridge 注册失败 | W | augment | 仅公共导出功能不可用；补 `reason`。 |

### `module/app/src/main/cpp/infra/frame/transition_dispatch.cpp`

| file:line | domain | level_now | message 摘要 | event_class | level_target | disposition | rationale |
|---|---|---|---|---|---|---|---|
| transition_dispatch.cpp:72 | platform | I | logic-pre consumer registered | 生命周期注册成功 | I | augment | 有限次状态变化应保留；补 `consumer=` 对象字段并使用结构化动词。 |
| transition_dispatch.cpp:112 | platform | W | transition timeout (cancelled) | 可取消等待超时 | W | augment | 单次等待取消且游戏继续；补 transition/操作标识及 `reason=cancelled`。 |

### `module/app/src/main/cpp/game/access/ui_components.cpp`

| file:line | domain | level_now | message 摘要 | event_class | level_target | disposition | rationale |
|---|---|---|---|---|---|---|---|
| ui_components.cpp:170 | ui | I | container frame first result=ok | 首次容器帧结果 | D | demote | 仅调试构建的一次诊断，不是用户态状态变更；成功结果用于定位应为 debug。 |
| ui_components.cpp:172 | ui | W | container frame first result=failed stage | 容器帧失败 | W | augment | 游戏可继续但该操作失败；补 `op`、`reason`，并避免自然语言正文。 |

### `module/app/src/main/cpp/game/access/ptr_slots.cpp`

| file:line | domain | level_now | message 摘要 | event_class | level_target | disposition | rationale |
|---|---|---|---|---|---|---|---|
| ptr_slots.cpp:110 | platform | W | desc button target rejected: data not writable | 指针结构校验拒绝 | W | augment | 仅拒绝该目标且继续运行；正文已带 index/reason，改为结构化 `reject` 并补对象字段。 |
| ptr_slots.cpp:118 | platform | W | desc button target rejected: original not executable | 指针结构校验拒绝 | W | augment | 仅跳过该目标；已有 index/reason，改为结构化 `reject`。 |

### `module/app/src/main/cpp/game/access/symbols.cpp`

| file:line | domain | level_now | message 摘要 | event_class | level_target | disposition | rationale |
|---|---|---|---|---|---|---|---|
| symbols.cpp:246 | core | W | hook entry follow refused: invalid target | hook 目标拒绝 | W | augment | 有限次降级为原入口；正文 reason 应使用 token `target_not_distinct_or_executable`。 |
| symbols.cpp:261 | core | W | hook entry follow refused: stub first instruction | hook 目标拒绝 | W | augment | 降级使用原入口；已有 symbol/address，reason 改为 token。 |
| symbols.cpp:271 | core | I | hook entry followed | hook 解析状态变化 | I | augment | 有限次成功状态应保留；改为 `bind hook=... entry=... target=...`。 |
| symbols.cpp:288 | core | W | hook entry resolve deferred: game base not ready | 启动期可重试查找 | W | augment | game base 未就绪属于可重试失败；补 `op`、`reason` 并保证不在循环无区分输出。 |
| symbols.cpp:297 | core | E | hook entry resolve failed: empty elf export | 必需符号查找失败 | W | augment | 此函数返回 0，仅对应 hook/功能不可用；补 `symbol`、`reason`，单项失败非全模块失效。 |
| symbols.cpp:304 | core | E | hook entry resolve failed: export not found | 必需符号查找失败 | W | augment | 对应 hook 不可用而游戏继续；带 export 与 reason，改结构化正文。 |
| symbols.cpp:312 | core | E | hook entry resolve failed: address not executable | 必需符号结构不符 | W | augment | 失败范围是该入口；保留 address，补 expected/actual 或明确 reason。 |
| symbols.cpp:321 | core | E | hook entry resolve failed: hookable check | 必需符号查找失败 | W | augment | 单项入口无法安装而模块继续；补 `symbol`、`reason`。 |
| symbols.cpp:332 | core | E | hook entry resolve failed: symbol not executable | 必需符号结构不符 | W | augment | 单项入口失败；补 `reason` 并规整结构化字段。 |
| symbols.cpp:341 | core | E | hook entry resolve failed: symbol not registered | 符号表查找失败 | W | augment | 仅相关 hook 不可用；补 `reason`，非整体能力失效。 |

### `module/app/src/main/cpp/game/access/item_handle_registry.cpp`

| file:line | domain | level_now | message 摘要 | event_class | level_target | disposition | rationale |
|---|---|---|---|---|---|---|---|
| item_handle_registry.cpp:68 | platform | E | item handle table full capacity | 基础设施容量耗尽 | E | augment | registry 无法继续分配 handle，能力不可用符合 E；补 `reason=capacity_exhausted` 和调用对象/provider 标识。 |

### `module/app/src/main/cpp/game/access/world.cpp`

| file:line | domain | level_now | message 摘要 | event_class | level_target | disposition | rationale |
|---|---|---|---|---|---|---|---|
| world.cpp:435 | api | D | replan wait-retry ok dirs | 导航重规划状态 | D | keep | 帧任务中的调试状态，已用 debug seam 门控。 |
| world.cpp:442 | api | D | replan wait timeout coordinates | 导航等待超时 | D | keep | 逐帧等待分支仅最终超时输出，诊断级且已门控。 |
| world.cpp:462 | api | D | reached nearest, waiting | 导航状态迁移 | D | keep | 状态迁移调试信息，非 INFO，受 debug 门控。 |
| world.cpp:478 | api | D | replan abort target/replan | 导航重规划终止 | D | keep | 失败路径诊断信息，debug 等级正确。 |
| world.cpp:489 | api | D | replan hit wall position/direction/count | 导航碰撞重规划 | D | keep | 对象位置、方向和次数可定位碰撞，debug 等级正确。 |
| world.cpp:506 | api | D | replan current path result | 导航重规划结果 | D | keep | 有路径长度/是否找到/最近点，逐帧状态使用 debug 合规。 |
| world.cpp:512 | api | D | replan fail terminate position | 导航重规划失败 | D | keep | 内部排障细节，debug 等级且门控。 |

## 缺失日志建议

| file + 函数 | 事件 | 目标等级 | 必填字段 | 理由 |
|---|---|---|---|---|
| `bridge/native/gamebridge.cpp` + `Java_com_inotia4_qol_NativeBridge_nativeInit` | `bridge_init()` 成功且初始化能力就绪 | I | `op=native_init result=ready` | 当前成功路径无里程碑；启动后无法区分初始化成功与尚未调用/早期拒绝。只在成功分支记录一次。 |
| `bridge/native/gamebridge.cpp` + `Java_com_inotia4_qol_NativeBridge_nativeInit` | `assembly_init()` 成功 | I | `op=assembly_init result=ready` | 当前仅 assembly 失败输出 E；成功里程碑缺失，且该生命周期事件有限次。 |
| `bridge/native/gamebridge_public_export.cpp` + `register_bridge` | 三个公共导出方法和全局引用全部注册就绪 | I | `op=public_export_bridge result=ready methods=3` | 当前只有失败日志；无法确认可选导出桥接是否成功注册。 |

## 统计

- 现有日志：43 条。
- disposition：`keep` 7，`demote` 5，`promote` 0，`delete` 0，`merge` 0，`augment` 31。
- 补充建议：3 条。

## Top 发现

1. bridge 订阅回调及公共导出的一次性失败大多记为 E，但调用失败不使游戏或模块整体停止，应降为 W。
2. public export 的 JNI env unavailable 是当前调用可失败、后续调用可重试的情形，应为 W，不是不可恢复 E。
3. `nativeInit` 的 assembly 失败确实使 native bootstrap 不可用，E 合理；正文当前为自然语言且无结构化 `reason`。
4. `symbols.cpp` 多处单符号/单 hook 解析失败记 E；按单项能力失败、游戏可继续应为 W。
5. `world.cpp` 七条 seam 日志均位于导航帧任务逻辑，但全是 DEBUG；没有逐帧 INFO，频率等级合规。 |

## 审核裁决

以下为批准修改的源码位置（文件均相对 `module/app/src/main/cpp/`）：

- E→W，正文不动：`bridge/native/gamebridge.cpp:65,345`、`bridge/native/gamebridge_data.cpp:54`、`bridge/native/gamebridge_public_export.cpp:52`。
- E→W 并追加结构化字段：`bridge/native/gamebridge.cpp:113,126,134,145,370,381,385`、`bridge/native/gamebridge_data.cpp:71,91,101`、`bridge/native/gamebridge_public_export.cpp:64,82,86,91`。
- 保留 E 并结构化：`bridge/native/gamebridge.cpp:282`、`game/access/item_handle_registry.cpp:68`。
- I→D：`game/access/ui_components.cpp:170`。
- 结构化且等级不变：`infra/frame/transition_dispatch.cpp:72,112`、`game/access/symbols.cpp:246,261,271,297,304,312,321,332,341`。`game/access/ui_components.cpp:172`、`game/access/ptr_slots.cpp:110,118`、`game/access/symbols.cpp:288` 保持原样。
- 新增 INFO：`gamebridge.cpp` nativeInit 成功时 `op=native_init result=ready`；`gamebridge_public_export.cpp` 注册成功时 `op=public_export_bridge result=ready methods=3`。
- 不新增 `assembly_init` INFO：`host/assembly.cpp:878` 已输出 `assembly: initialized=true ...`，且 `assembly_init()` 从 `nativeInit` 成功路径同步调用；新增会重复记录同一里程碑。

以上未列出的所有现有日志均裁定为 `keep`，不采纳原清单其余 augment 建议；不改对应正文、等级或控制流。
