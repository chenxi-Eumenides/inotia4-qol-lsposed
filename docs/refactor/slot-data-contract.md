# Slot data 契约与机制规格

本文规定 `core.save` 存档数据收口的契约、机制与后续批次验收边界。P1 契约、P2 provider 骨架与 P3a Host 机制已完成；journal v2、恢复状态容器与 dat evidence 已有纯逻辑实现，**尚未接入生产保存协调、原版转换或 hook**。共享 store 的 strict 修复已影响旧生产读写入口。当前实现及准确验收范围见 §14；目标分层以根目录 `ARCHITECTURE.md` 为准。

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

### 2.1 owner-bound 预算预约与显式凭据（P3a）

`QolOwnerStoreApi` 既有六项及 `QolStoreApi` 前缀不变，只尾部追加 strict inspect、preflight、reserve/release、带凭据 write/remove/replace 与 set_referenced。跨边界 POD 定义在 `contracts/owner_store.h`，均有 size 字段；reserve 和所有候选预检在单次 store 锁内完成，**生命周期跨调用但不跨调用持 mutex**。

`QolOwnerStoreReservation` 包含 `size / slot / id / generation / owner / encoded_bytes / retain / reserved / revision`：

- id 为进程内不复用的唯一预约标识；owner 为当前宿主授权 token，不是历史 journal 的 owner_id。
- generation 为槽容器代次；成功持久化提交后更新调用方凭据的 generation 和 revision。
- revision 是独立的预约修订号。set_referenced 实际改变引用状态、owner_rebound 迁移持有者时递增 revision，**不冒用磁盘 generation**。写入、降引用与释放均验证当前 id/slot/owner/generation/revision/预算，旧副本拒绝。
- retain 是 store 的权威引用状态；只通过 set_referenced 变更，不能从可复制凭据的 retain 字段撤销保护。set_referenced 返回更新后的凭据。
- 已释放 id 的重复释放返回成功或 NotFound，不影响同槽新预约。当前有效预约的旧 generation/revision 不能通过幂等收尾释放新状态。

预约期间 read_section 放行；其他无凭据写请求（**包括同 owner**）返回 Busy。显式凭据是事务身份，不能由 owner token、线程、TLS 或写入顺序推断。owner-bound 无凭据写入/删除 `module.save.journal` 返回 Blocked；迁移窗口的旧 SDK/JNI 入口尚未全面撤销，但共享 store 的槽预约同样阻止它们消耗预算或覆盖 journal。

候选以 `QolStoreSection` 替换集、删除集和可选 journal 表达，每个候选都基于同一当前容器，不能把候选累加当作最终结果。preflight 调实际 encoder 并输出各候选精确长度；reserve 只在全部预检通过后建立租约，预算为候选最大编码长度。容器开销为：

```text
C(S) = 21 + Σ(11 + name_bytes + payload_bytes)
```

固定 21B 包含尾 CRC；每 section 的 11B 包含名字长度、版本、payload 长度与 payload CRC。正式数据与 prepared/confirmed journal 同存、最终删 journal 提交三个候选都须各自 ≤4 MiB。

prepared 写失败且无在途引用时释放；原版调用前通过 set_referenced 登记引用，不靠重复写 journal 改变租约状态。事务取消或原版明确失败返回 Release 动作；commit-pending、recovery-retrying、degraded 返回 Hold，原版已经成功时不能以 abort 撤销事实。无引用租约随 owner 停用释放；有引用租约保留，Host 重新签发受限 backend 所属恢复身份后迁移 owner 并更新 revision。当前 backend 只授权 core.save，不能把消费者离线当作捕获字节或恢复证据失效。**P3a 只实现机制与纯状态动作，生产执行这些动作留 P3b。**

### 2.2 strict 共享容器读取与证据保留（P3a）

inspect 区分 `Absent / PrimaryValid / FallbackUsed / Corrupt / IoFailed`。共享 load/read/ensure 路径都不再把 primary 与 last-good 双损坏折叠成 NotFound：损坏文件保存为 `*.corrupt.<ms>`，未决 `*.unresolved-corrupt` marker 防止 quarantine 移走原路径后、重复检查或重新初始化时被误当新槽。ensure 遇 Corrupt/IO 失败必须返回失败，不能写空容器；正常 primary 读面不变。

显式 reset/delete 先确认旧 last-good 和 primary 的删除，再完成 reset 写入，最后清除未决 marker。unlink 只有成功或 ENOENT 可以继续，其余返回 IO error；marker 清理失败也不能假报成功。失败保留 marker，quarantine 证据不批量删除。该流程可能部分应用，**不是跨多个文件的全有或全无事务**；没有父目录 fsync 时不承诺掉电原子保证。

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

生产接线的结算要求：HTTP/API 票据只有在正式 sidecar 提交确认后才 done；commit-pending 保持 pending。游戏 SAVE_Save 的 int 返回可保持原版结果。**这不是 P3a 已接入的行为**，原版保存/协调器/票据桥留 P3b。

## 6. journal v2 格式

保留 section 名 `module.save.journal`，section 版本 **2**。由 core.save 私有二进制编码，不是 JSON/Base64：

```text
schema=2
tx_id, slot, session_epoch, stage
原版成功后的 save-file evidence
entries[{owner_id, section_name, section_version, payload_length, payload}]
```

journal 上限 **1 MiB**，正式数据与 journal 同存时仍须满足 **4 MiB** 容器上限。事务前预检，超预算即拒绝开始事务，不调用原版保存。

### 6.1 v2 字节布局

全部为大端、紧凑编码，无 padding，不直接 dump C++ struct。固定头 48B：

| 偏移 | 字节 | 字段 |
|---:|---:|---|
| 0 | 4 | magic ASCII SDJ2 |
| 4 | 2 | schema=2 |
| 6 | 2 | header_bytes=48 |
| 8 | 4 | total_bytes（含尾 CRC） |
| 12 | 4 | stage：0 prepared，1 original_confirmed |
| 16 | 8 | 非零 tx_id |
| 24 | 4 | slot（0..2） |
| 28 | 4 | entry_count |
| 32 | 8 | session_epoch |
| 40 | 4 | evidence_kind：0 none，1 raw-dat-sha256 |
| 44 | 4 | evidence_bytes=40 |

紧接固定预留 40B：`file_length:u64 + sha256[32]`。prepared 的 kind=0 且 evidence 全零；confirmed 的 kind=1 且 SHA-256 非零。固定预留保证确认记录在原版成功后不突然变大。

每 entry 为 `owner_id:u64 | name_length:u16 | section_version:u16 | payload_length:u32 | name | payload`；尾部为 IEEE CRC32（反射多项式 0xEDB88320，初值与最终异或均 0xFFFFFFFF）：

```text
J = 48 + 40 + Σ(16 + name_bytes + payload_bytes) + 4
```

解码拒绝未知 magic/schema/stage、错误 total_bytes/CRC、截断/尾随、槽不匹配、零 tx_id、非法名字/版本、重复 section、保留 `module.save` 命名空间条目及不合法 evidence。名字 1..64、单 payload 与总 journal 均 ≤1 MiB，读取长度先验证 `length <= remaining` 再分配。同一 owner 的多个不同 section 合法。owner_id 仅记来源身份，恢复授权使用 core.save 当前凭据，不能把持久化 owner_id 当作重启后的有效 token。

### 6.2 原始 dat evidence 与路径绑定

`game/access/save_file_evidence.cpp` 只读调用方**明确指定**的 dat 路径，输出原字节长度及 SHA-256，不写原版文件、不扫描目录取首项、不回退到 mtime 或解密明文。读取前后核对同一 fd 的 dev/inode、长度、mtime/ctime 纳秒，结束时 stat(path) 必须与 fd 的 dev/inode 一致；读取中同名原子替换或 IO 失败时返回失败并保持 out 不变。时间仅检测变化，不作为存档身份。

此读函数只约束读取窗口，**不能保证 evidence 取得之后到正式 commit 之间路径不变**。应用目录注入、可靠目标槽、平台桥以及覆盖该窗口的槽准入均留 P3b。

## 7. 失败状态与迁移

| 状态 | 精确定义与准入 |
|---|---|
| commit-pending | 本进程已确认原版返回成功，但正式 sidecar 尚未确认提交；保留捕获字节/journal，禁止同槽新保存、换档及导入覆盖，不得调用 abort 假装原版已撤销 |
| recovery-retrying | 重复提交同一 tx 的不可变 payload，不再次调用原版保存，不重新 capture；最多 3 次后续帧重试，失败转 degraded 并保留证据 |
| degraded | 重试耗尽、恢复证据不足、版本未知或存储损坏；停止该槽依赖数据的写操作，需要显式恢复/管理处理，不能回退空状态继续 |

### 冻结限制：重启后的非严格跨文件原子协议

重启后仅对 original_confirmed 且 Adapter 提供的当前 save-file evidence 匹配的记录自动重试。prepared 阶段存在「原版已写、确认标记未写」窗口，不能自动 commit 或 abort：保留原记录并降级。此限制是非严格跨文件原子协议的真实边界，**属于冻结限制，不是待办**。

### v1 迁移规则

`ModuleSaveCoordinator.kt` 的 v1 是有界 UTF-8 JSON，键为 `transactionId / slot / stage / sections`，stage=0，sections 每项含 name/version/payload（Base64.NO_WRAP），不是 entries。先按 section.version 分派：v1 识别成功返回 LegacyUnresolved，失败返回 CorruptLegacy；两者均终止自动恢复，不 replace/remove/reset/重放/删除证据。version=2 必须校验 SDJ2/schema，不能因首字节像 JSON 而降格为 v1。有未决 v1 journal 的槽进入恢复降级；无 journal 的旧存档直接兼容，功能 section 格式不变。v2 生产接线后禁止产生 v1，单事务不混用两个协调器；P3a 不替换生产旧协调器。

## 8. 身份约束两层

Host 按真实注册身份向 PluginContext.owner_token 注入凭据，仅领域 owner core.save 获得 `module-saves` backend。签发/query 属 Host 私有面；插件可见 verify 仅验证已持有的 token，不授予签发或枚举能力。owner 生命周期 generation 与槽容器 generation、预约 revision、session epoch 各自独立；停用使旧 token 失效。授权由装配期 owner↔命名空间绑定决定，不按 plugin_id 字符串特判。只在 spec 加 owner 字符串或引用 plugin_id 不足以落实身份约束。

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
| P1（完成） | 契约冻结，生产不接线 | 无行为变化，不需要真机 |
| P2（完成） | owner token / 受限 backend / provider 骨架、session 状态机、fake backend 验证注销与补加载；生产功能仍走旧路径 | VM-K5-06h 装配/名册范围；Host 身份、注销、epoch 用例 |
| P3a（完成） | strict 共享 store、真实编码容量预检、显式 generation/revision 预约；journal v2/v1 识别、恢复状态容器、原 dat evidence；不接生产协调 | VM-K4-04b、VM-K5-06i 受影响正常读面/同槽进档范围；损坏与预约由 Host 覆盖 |
| P3b/P3c（实现完成，真机部分验收） | core.save 生产协调/journal v2、转换桥、SAVE_Save 入口资源、save Adapter、旧 module_save 参与者编排退役；EB callbacks/runtime、Kotlin/JNI 薄层及 evidence→commit 槽准入。生产切换见 `730ff84d`，后续静态修复与面板状态修复见 `26b19fb8` 及后续 P3c 修复提交 | 已执行 VM-SD-01/02/04 的限定范围、VM-SD-03 happy path、VM-B01..04 API 子项及 monster 保存 happy path；崩溃恢复窗口、旧 epoch 句柄设备级拒绝、overhaul 和 hook 结构日志仍欠。详细范围见 `docs/refactor/verification/slot-data/p3c-20261008/verification-log.md`；整批仍 `NOT_ACCEPTED` |
| P4（实现完成，VM-44 部分真机验证；NOT_ACCEPTED） | autosell 接入 SessionBound 读 + Immediate 写，移除其存档事件加载与任意 section IO；修复 request revision、pending intent、失败重试与注销保留 | VM-44 已观察 `sold=3`、`failed=16`；仍欠 EB bag6/7 RemoveFailed 的独立归因及真实 Provider 并发/注销时序证据；见 `docs/refactor/verification/slot-data/p4-20261008/verification-log.md` |
| P5 | save_backup admin 接入、锁范围收窄、导入/删除/回滚；Host/JNI 旧入口权限封堵；旧注册生产链删除 | 重验 VM-K4-04b、VM-K5-06i |

旧 `contracts/module_save_port.h` 协议 P1/P2 不动；P3 退役 EB 生产注册，可留兼容转发但不能保留第二套参与者顺序；P5 关闭旧入口。既有 VM 卡位置见 `docs/refactor/verification.md`。

## 12. 四张新卡验收要点

以下四卡是生产接线后的计划验收，**均不因 P3a Host 或正常读面证据而标记已完成**。

### VM-SD-01 多槽/同槽重入

两槽 EB 与 autosell 数据独立；同槽退出重入 epoch 增长；旧面板/旧 request 拒绝。API 与原版按钮、新档均先预清后加载，不存在重复加载或旧缓存复用。

### VM-SD-02 当前档启停补加载

已进档启用、deactivate→activate 都加载当前 epoch；未加载不得扫描/操作。在途注销不释放 callback/user，磁盘 section 保留；provider 撤销与恢复后无旧函数表使用。

### VM-SD-03 联合提交失败恢复

prepare 失败不调用原版保存；原版失败不写正式 sections；原版成功 sidecar 失败进入 pending。同 tx 重试不重调原版、不重新 capture；分别验证 prepared、confirmed、正式提交后三个崩溃窗口；未知或 v1 证据降级而非空状态放行。

### VM-SD-04 建档/导入后失效

建档 reset 在消费者加载前发生，覆盖原版建档；导入当前槽被拒绝。成功导入与完整回滚后 generation/epoch 正确失效，删除同步清 sidecar；primary 已改但 last-good 失败能够回滚并保留证据，未知 sections 保持。

## 13. P3c 实测边界与约束声明

P3c 已把协调器与转换接入生产，但**符号存在或 hook 安装成功不等于所有游戏保存路径都已证明覆盖**。P3c 真机记录见 `docs/refactor/verification/slot-data/p3c-20261008/verification-log.md`；整批仍 `NOT_ACCEPTED`，未覆盖项在 `docs/refactor/verification.md` §5 保持登记。

1. original v1.3.2 与 monster v31 的 `SAVE_Save` 正常保存 happy path 已真机验证，原版返回后 sidecar 按同事务提交；monster guard 有成功写盘行为侧证据及静态 trampoline 复审。但所有自动保存/隐藏下层写盘路径未穷举，设备无入口解析/B 目标/backup 结构日志；不得据此宣称所有存档写入均被事务捕获。
2. API enter/create 使用请求目标槽并通过 slot-data 生命周期；original v1.3.2 slot1 create 已由 API 自动 skip 剧情后 ticket `done`、EB missing/default 不继承旧槽。各变体所有新档按钮/取消选角路径仍需逐卡证据；目标槽只能由受控 Adapter 提供，不得由插件读裸地址。
3. VM-SD-03 的 prepared/confirmed/final crash/restart 三窗口尚未真机验证；Host 故障测试不能替代设备恢复日志。VM-SD-04 的导入/完整回滚留 P5。

插件间仅通过 contracts capability，core.save 不 include infra/store.h；EB/autosell/save_backup 不 include core.save 私有文件。宿主只做验证/转接，领域时序留在 core.save。不新增裸 VMA/偏移，不改模块或插件版本号，HTTP 路径/方法/成功响应与 NativeBridge external 方法名/签名冻结。

P1 仅编译期/布局期断言，不写 fake 行为测试、不接线宿主/SDK/插件/Kotlin/生产 CMake 依赖。验证包括 diff 检查、相关 host tests（含新增 target）、check_arch、check_http_routes（冻结路由不漂移）、项目唯一 debug 构建入口 scripts/build-debug.sh。当前路由检查输出 controller/api-reference/native frozen 均为 146 条，重复、遗漏、额外路由均为 0；任务描述的 143 条与当前基线计数不同，本批不修改路由。P2..P5 触及行为部分必须取得真机 VM 日志，缺证据仍为 **NOT_ACCEPTED**，Host 测试或构建不能替代。

## 14. P3a 当前实现与验收边界

P3a 已完成 Host 机制、编解码及相应测试，oracle 三项阻断复审通过。有效验证为 7/7 targets：save_journal_tests、infra_store_tests、slot_data_contract_tests、slot_data_provider_tests、save_facts_plugin_tests、host_tests、host_adapter_tests；包含旧凭据释放/降引用拒绝、reset/delete 确定性 IO 注入与读取中同名原子替换。新增架构违规 0，HTTP 冻结集合 146 条不漂移。

有效最终 Debug 模块 SHA-256：`466f895b62089ca0c81918bee8ccb529c5a397c496194b3f1affc415d32ebe90`。真机仅原版 1.3.2/槽 0：同签名 LSPatch install-r 后实际加载该 SHA；正常 world 下 5 个扩展袋、33 件物品的 88 份完整 raw body 前后相同，无归一化；24 个存档目录文件与 11 个模块目录文件 SHA 不变，原配置逐字恢复、最终 main_menu。新增 WARN 与崩溃匹配均 0；既有 optional-cycle 与 P_EXT_SAVE_REVIVE 延迟安装告警不计作消失。

此证据只补 VM-K4-04b、VM-K5-06i 的**受影响正常读面及同槽进档**，不是完整旧卡重验。不覆盖设备双损坏注入、原版 save、创建/导入/删除/回滚、provider 级联或 journal v2 生产联合提交。SAV-01 已修复；新机制、纯状态动作与生产协调之间的接线仍留 P3b。可提交证据与复现说明见 `docs/refactor/verification/slot-data/p3a-20261007/README.md`；保护备份和完整采集缓存仅本地保留，不入源码提交。
