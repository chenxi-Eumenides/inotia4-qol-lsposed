# Slot data 契约规格（P1）

本文规定 `core.save` 存档数据收口的契约与后续批次验收边界。P1 仅定义头文件、编译期断言和规格，生产不接线，不表示后续行为已经实现。目标分层以根目录 `ARCHITECTURE.md` 为准。

## 1. 分层与权责

| 所有者 | 职责 |
|---|---|
| 功能插件 | 自己 payload 的编解码、校验、默认状态与恢复规则 |
| core.save | 槽位生命周期、参与者登记、加载/保存顺序、协调日志语义与操作准入 |
| loader/store | 容器编解码、原子替换、锁、last-good 与文件 IO |
| Host / Adapter | Host 注入验证身份与转接；Adapter 提供受控原版转换、保存及 save-file evidence |

**「谁可以写、何时写」由领域 owner 收口，「字节怎样可靠落盘」继续作为机制。** 宿主不决定领域时序，功能插件不自行操作容器。

## 2. ABI 与预算

契约位于 `contracts/owner_identity.h`、`contracts/slot_data.h`、`contracts/slot_data_admin.h`，使用 `qol::contracts` 命名空间与 `Qol` 前缀。ABI 头沿用 `QolAbiHeader { uint32_t size; uint16_t major; uint16_t minor; }`；`QolResult` 来自 `contracts/result.h`。接口演进只允许尾部扩展并通过 size 协商，不改变既有前缀。

`save.slot_data` 与 `save.slot_admin` 初始契约版本均为 1.0。owner、注册句柄、request、admin token、operation 与 generation 均为 `uint64_t`；声明的无效 token/handle/request/operation 为 0。跨边界只用固定宽度 POD、函数表及回调期间字节视图，不公开 STL、JSON、文件路径或游戏指针；`user` 是消费者回调上下文，不是游戏对象。

`QolSlotSession` 包含 slot/state/epoch，状态为 unavailable/activating/ready/closing，每次进档（含同槽重入）epoch 递增。`QolSlotDataContext` 包含 session/reason/transaction，reason 为 enter/catch_up/requested/save/recovery，非保存回调 transaction 为 0。

沿用 `infra/store.cpp` 的上限：section 名 **64 字节**、单 payload **1 MiB**、容器 **4 MiB**。P1 不提高预算。

## 3. 读写策略枚举

读和写为两个独立维度，两策略都按槽隔离，区别只在读写触发方式。

- `QolSlotDataReadPolicy::kImmediate = 0`：消费者决定何时 request_load，可在注册后或 session 激活通知中请求。
- `QolSlotDataReadPolicy::kSessionBound = 1`：core.save 在 session 就绪时自动加载；注册自动申请当前 session 补加载。
- `QolSlotDataWritePolicy::kImmediate = 0`：请求修改即落盘，不等待原版保存。
- `QolSlotDataWritePolicy::kSlotBound = 1`：只标 dirty，由 core.save 在原版保存事务成功后统一提交。

| 读 | 写 | 语义 |
|---|---|---|
| Immediate | Immediate | 消费者自行请求加载，修改请求独立落盘 |
| Immediate | SlotBound | 消费者自行请求加载，修改随原版成功保存统一提交 |
| SessionBound | Immediate | session 自动加载，修改请求独立落盘；autosell 使用此组合，规则修改无需等待原版保存 |
| SessionBound | SlotBound | session 自动加载，修改随原版成功保存统一提交；EB 使用此组合，背包 payload 与原版保存协调 |

注册只复制规格，不在注册调用栈加载或回调；`out_initial_request` 表示自动补加载请求，不需要时为无效 request。所有消费者回调在游戏主线程执行。Immediate 请求在主线程且无事务占用时当次执行，其他线程投递主线程返回 request；「立刻」是不等待下一次原版读写存档，不是允许后台线程直接操作游戏。

读写请求仅接受注册 handle 与 expected_epoch，不接受 slot/section；只能操作绑定 section 与当前 session。epoch 不匹配拒绝，防止旧面板写入新档。load 的字节仅回调期间有效；capture 在单次调用完成序列化，不通过两次调用先测长度再写重复业务 prepare。missing 由消费者生成默认状态；IO 不可用、损坏、版本不支持必须可区分，不得全部伪装为 missing。SlotBound 写完成为 staged，不是 durable。

## 4. 加载顺序冻结

所有入口——API 读档、API 建档、原版读档按钮、原版新档按钮——统一进入 begin_transition：

```text
→ core.save begin_transition(reason, target)
→ closing 旧 session；撤销旧 epoch 的操作准入
→ prepare_detach：EB 撤投影、释放缓存、清按钮
→ 清理成功才执行原版转换
→ RenderPre 观测世界就绪
→ core.save 确认 target/current_slot、生成新 epoch
→ 新档先 reset；读档先 ensure；检查未完成 journal
→ kSessionBound 读自动加载；kImmediate 读发激活通知并执行其加载请求
→ 发布 session.ready；消费者才恢复扫描/物品操作
```

`prepare_detach` 幂等且可拒绝，拒绝时阻止原版转换。消费者的 `session_ready` 通知先于同轮 SessionBound load；Immediate 消费者在通知中自行 request_load。消费者通知不是全局 session.ready 的提前发布，未加载不得扫描/操作。

core.save 的确认阶段必须位于旧回调与 `kEventSaveEnter` 派发之前，不能仅作为事件订阅者依赖订阅顺序。依据 `patch/save_enter.cpp` 的 `save_enter_tick`：旧回调在事件派发之前执行，事件订阅顺序不能保证预清与确认门禁。

## 5. 保存事务顺序

1. 主线程固定 session 与参与者集合，执行 prepare/capture，复制不可变 payload。
2. core.save 写唯一 prepare journal；预算或写入失败时不调用原版保存，通知已 prepare 的消费者取消。
3. 调受控原版保存入口；明确失败时不提交正式 sections，处理 journal 清理。
4. 原版成功后持久化 original_confirmed journal，再用一次 replace_sections 替换全部捕获 sections、删 journal。
5. 确认正式容器已提交后通知消费者清 dirty/pending。恢复重试只用捕获字节，不重新序列化当前运行时。

`complete` 只做幂等内存记账，不再执行 section IO。outcome 区分 committed、staged、original-save-failed、pending-recovery、degraded、cancelled；不能用取消通知假装已成功的原版保存被撤销。

## 6. journal v2 格式

保留 section 名 `module.save.journal`，section 版本 **2**。由 core.save 私有二进制编码，不是 JSON/Base64：

```text
schema=2
tx_id, slot, session_epoch, stage
原版成功后的 save-file evidence
entries[{owner_id, section_name, section_version, payload_length, payload}]
```

journal 上限 **1 MiB**，正式数据与 journal 同存时仍须满足 **4 MiB** 容器上限。事务前预检，超预算即拒绝开始事务，不调用原版保存。

## 7. 失败状态与迁移

| 状态 | 精确定义与准入 |
|---|---|
| commit-pending | 本进程已确认原版返回成功，但正式 sidecar 尚未确认提交；保留捕获字节/journal，禁止同槽新保存、换档及导入覆盖，不得调用 abort 假装原版已撤销 |
| recovery-retrying | 重复提交同一 tx 的不可变 payload，不再次调用原版保存，不重新 capture；最多 3 次后续帧重试，失败转 degraded 并保留证据 |
| degraded | 重试耗尽、恢复证据不足、版本未知或存储损坏；停止该槽依赖数据的写操作，需要显式恢复/管理处理，不能回退空状态继续 |

### 冻结限制：重启后的非严格跨文件原子协议

重启后仅对 original_confirmed 且 Adapter 提供的当前 save-file evidence 匹配的记录自动重试。prepared 阶段存在「原版已写、确认标记未写」窗口，不能自动 commit 或 abort：保留原记录并降级。此限制是非严格跨文件原子协议的真实边界，**属于冻结限制，不是待办**。

### v1 迁移规则

`ModuleSaveCoordinator.kt` 的 v1 是 JSON、stage=0、entries 含 Base64。只读识别，不自动提升为 v2、不自动重放、不删除证据。有未决 v1 journal 的槽进入恢复降级；无 journal 的旧存档直接兼容，功能 section 格式不变。v2 开始后禁止产生 v1，单事务不混用两个协调器。

## 8. 身份约束两层

Host 注入并验证 owner token，仅领域 owner core.save 获得 `module-saves` backend。token 的签发属于 Host 装配期，generation 是 owner 生命周期代次，停用后递增使旧 token 失效；授权由装配期注册的 token 决定，不按 plugin_id 字符串比较或白名单特判名称。当前 PluginContext 只有自报可引用的 plugin_id，仅在 spec 加 owner 字符串不足以落实身份约束，必须注入凭据。验证能力由 Host 持有，插件凭据只读。

core.save 注册时验证归属，后续仅接受 handle/epoch，内部保留 journal 名与命名空间。`host/host_sdk.cpp` 的任意名称入口最终保留 ABI 布局但拒绝旧 section write/replace/remove；迁移窗口只保留明确列出的未迁移入口，最终撤销。**不能只加新接口却让旧入口继续可写。** P1 不接线；P2..P5 按批次迁移消费者，不以本文推定旧入口已封堵。

## 9. 注销生命周期

provider 先关闭消费者准入并拒绝新请求，等待已有回调与捕获中的事务释放引用；unregister 非阻塞，不阻塞游戏主线程。句柄含注册代次，注销完成后不可复用。

**provider 保证：unregister 返回的 request 一旦 query 报终态，其后不再触发任何消费者回调。** query 线程安全，消费者据此释放 user 与回调资源；不能在在途引用未释放时销毁上下文。停用与注销保留磁盘 section，不自动删除、不隐式保存。

## 10. 容器管理接口

`save.slot_admin` 不是普通参与者接口，读容器、导入、删除、回滚走此面。open 仅授予宿主装配明确授权的 owner，core.save 内部直接管理建档/reset。begin 接受 admin、slot、op、expected_generation 并返回 operation；存在 pending 保存事务则拒绝。

operation 跨完整 dat/sidecar/warehouse 备份事务持有，但不是跨调用持有 mutex。导入当前已加载槽默认拒绝，要求先退主菜单，防止旧运行时状态下一次保存覆盖导入内容。end 只能在完整备份操作成功或完整回滚结束后调用，不能 sidecar 写完就提前调用；committed=0 表示完整回滚结束，两者都会递增槽 generation 并失效运行时 epoch。

管理结果必须区分 unchanged / primary_applied / fully_applied，不能只用 bool 判断回滚需求：`infra/store.cpp` 的 import_container 可以 primary 已成功、last-good 失败而返回 IO error。未知功能 sections 原样保留，导入的未决 journal 不盲目重放。

## 11. P1..P5 批次表

串行执行，前一批验证完成才进入下一批。

| 批 | 内容 | 触及 VM 卡 |
|---|---|---|
| P1 | 契约冻结，生产不接线（本批） | 无行为变化，不需要真机 |
| P2 | owner token / 受限 backend / provider 骨架、session 状态机、fake backend 验证注销与补加载；生产仍走旧路径 | 重验 VM-K5-06h；新增 Host 身份越权、注销在途、epoch 用例 |
| P3 | core.save 协调与 journal v2、转换桥、SAVE_Save 入口资源、save Adapter、旧 module_save；EB callbacks/runtime；EB Kotlin/JNI 薄层 | 重验 VM-K4-04b、VM-K5-06d、VM-K5-06i |
| P4 | autosell 接入 SessionBound 读 + Immediate 写，移除其存档事件加载与任意 section IO | 重验 VM-44 |
| P5 | save_backup admin 接入、锁范围收窄、导入/删除/回滚；Host/JNI 旧入口权限封堵；旧注册生产链删除 | 重验 VM-K4-04b、VM-K5-06i |

旧 `contracts/module_save_port.h` 协议 P1/P2 不动；P3 退役 EB 生产注册，可留兼容转发但不能保留第二套参与者顺序；P5 关闭旧入口。既有 VM 卡位置见 `docs/refactor/verification.md`。

## 12. 四张新卡验收要点

### VM-SD-01 多槽/同槽重入

两槽 EB 与 autosell 数据独立；同槽退出重入 epoch 增长；旧面板/旧 request 拒绝。API 与原版按钮、新档均先预清后加载，不存在重复加载或旧缓存复用。

### VM-SD-02 当前档启停补加载

已进档启用、deactivate→activate 都加载当前 epoch；未加载不得扫描/操作。在途注销不释放 callback/user，磁盘 section 保留；provider 撤销与恢复后无旧函数表使用。

### VM-SD-03 联合提交失败恢复

prepare 失败不调用原版保存；原版失败不写正式 sections；原版成功 sidecar 失败进入 pending。同 tx 重试不重调原版、不重新 capture；分别验证 prepared、confirmed、正式提交后三个崩溃窗口；未知或 v1 证据降级而非空状态放行。

### VM-SD-04 建档/导入后失效

建档 reset 在消费者加载前发生，覆盖原版建档；导入当前槽被拒绝。成功导入与完整回滚后 generation/epoch 正确失效，删除同步清 sidecar；primary 已改但 last-good 失败能够回滚并保留证据，未知 sections 保持。

## 13. 取证门与约束声明

P3 开工前必须补证，不能推断：

1. 各变体原版/自动保存是否完整经过 SAVE_Save。符号已登记于 `game/symbols/symbol_registry.h`；`extension_bag_render.inc` 的 SAVE_SaveInventory hook 仅撤投影，不构成完整事务。
2. 原版新档 wrapper（`patch/save_enter.cpp`）如何由 Adapter 提供可靠目标槽。必须由 Adapter 提供，不能插件读裸地址。

插件间仅通过 contracts capability，core.save 不 include infra/store.h；EB/autosell/save_backup 不 include core.save 私有文件。宿主只做验证/转接，领域时序留在 core.save。不新增裸 VMA/偏移，不改模块或插件版本号，HTTP 路径/方法/成功响应与 NativeBridge external 方法名/签名冻结。

P1 仅编译期/布局期断言，不写 fake 行为测试、不接线宿主/SDK/插件/Kotlin/生产 CMake 依赖。验证包括 diff 检查、相关 host tests（含新增 target）、check_arch、check_http_routes（冻结路由不漂移）、项目唯一 debug 构建入口 scripts/build-debug.sh。当前路由检查输出 controller/api-reference/native frozen 均为 146 条，重复、遗漏、额外路由均为 0；任务描述的 143 条与当前基线计数不同，本批不修改路由。P2..P5 触及行为部分必须取得真机 VM 日志，缺证据仍为 **NOT_ACCEPTED**，Host 测试或构建不能替代。
