# 扩展背包 P4：原版物品对象与逻辑背包事务桥接

> **状态**：实施中（2026-08-31）。P4.1 事务域校验与 P4.2 payload 桥接已实现并通过只读补丁审查（Oracle 批准）与 host 测试；真机最小闭环证据 `E-2026-08-31-04`/`E-2026-08-31-05` 已登记（空槽双向字节保真、合并身份规则、任务袋拒绝）。目标满/数量上限/取消/插入失败的真机注入无 API 表达路径，登记「验收路径未就绪」转 P5；P4.3–P4.5 未实施，P4 整体 `NOT_ACCEPTED`。
> **命名空间**：本文的 P4 仅指 `docs/extension-bag-control-plane.md` 的“原版物品对象与逻辑背包事务桥接”，不指 `docs/refactor-plan.md` 或 `docs/backlog.md` 中同名的其他阶段。
> **权威边界**：阶段状态、范围和验收顺序以控制面为准；代码结构以 `architecture.md` 为准；sidecar 容器以 `docs/module-save-store.md` 为准；API 以 `docs/api-reference.md` 为准。本文件只把这些既定要求拆成可实施、可审查的 P4 工作包，不改变 ADR-001 至 ADR-009。

## 1. 目标、完成定义与非目标

### 1.1 P4 的目标

把现有的原型移动代码收敛为一个唯一的、可回滚的原版对象与扩展逻辑状态桥接。完成后：

1. 原版→扩展只以 `SAVE_SaveItem` 取得完整 payload；扩展→原版只以 `SAVE_LoadItem` 重建对象。
2. 任一物品在 `module-owned`、`borrowed-for-view`、`inventory-owned`、`released` 四态中只有一个逻辑所有者。
3. 三条移动路径共享一个事务入口和统一失败语义；扩展→扩展不调用 `INVEN_MoveItem`。
4. 原版任务袋索引 `5` 在参数、事务记录、恢复和回滚层全部被拒绝。
5. 已知坏 payload、反序列化失败和入库失败会被隔离并可只读诊断，绝不按 `category/count` 构造近似物品，绝不静默丢弃。

P4 的“任意失败可恢复”限于**单个进程存活期间的一笔事务**：源、目标、逻辑状态和投影/UI 恢复到可证明的一致状态。它不等价于进程崩溃、切档或保存期间断电后的恢复承诺。

### 1.2 P4 完成所需的结果

- 所有跨包移动经唯一事务协调入口；不再并存 `UnsavedCrossMove`、临时 `PendingTransfer` 回滚和各路径自定义回滚三套语义。
- 成功路径能对完整 payload 做字节级保真验证，覆盖装备属性、附魔、宝石孔、稀有度与堆叠数量。
- 成功入原版库存后，对象在同一锁保护区内移交为 `inventory-owned`；模块不再读、写、释放该指针。
- 原型期的“不实际 `ITEMPOOL_Free`”仅能在控件、TouchState 和描述面板引用仍未撤销时暂存；P4 必须先证明所有借用引用均已归还，才可替换为真实释放。
- host 证明纯模型和确定性回滚；native 对象、原版库存和控件投影由真机最小闭环证明。两类证据不得互相替代。

### 1.3 明确不在 P4 内

| 事项 | 所属阶段 | P4 的责任 |
|---|---|---|
| 0x81 扩展拖动建立、hit-test、真实拖放和取消体验 | P5 | 提供可复用事务契约；不实现或验收交互路径。 |
| 三方向逐路径真机矩阵、保存失败/进程中断全矩阵 | P5 | 不改变 P5 的路径级验收范围。 |
| 所有显式/自动保存调用点审计与收口 | P7 | P4 不得绕开未来协调器。 |
| `extensionbags.journal` 的正式写入、stage 0/1/2 持久化、跨进程恢复 | P7 | P4 只保证内存事务模型和字段与 journal v1 兼容。 |
| ADR-009 存档面板是否隐式保存 | P7 | 不作产品语义决定。 |
| 自动入库、自动装备、掉落和装备行为 | P6 | 仅维持任务袋排除不变量。 |
| v2/v3、`virtualbags.items` 或旧格式迁移 | P8/ADR-005 收尾 | P4 不新增兼容分支。 |

## 2. 当前源码基线与必须收敛的原型

本节记录 2026-08-31 的已读代码事实，不表示任何 P4 能力已完成。行号用于定位，后续改动时以符号名复核。

| 区域 | 当前事实 | P4 缺口或动作 |
|---|---|---|
| `game_symbols.h:241-243` | `SAVE_SaveItem(uint8_t*, void*)` 使用 u8 长度前缀，返回总长度，最大 255；`SAVE_LoadItem(const uint8_t*, void**, int*)` 失败返回 0，且不会清空 `*out`。 | 形成唯一包装层；Load 调用前必须把 out 指针置空。 |
| `game_ui_virtbag.cpp:690-709` | `serialize_item_payload_locked()` 已执行长度、尾部零填充和实际长度校验。 | 保留为唯一 Save 包装入口，不能在别处复制或截断 payload。 |
| `move_original_to_extension_slot_locked()` | 已拒绝源袋 5，并做 payload 序列化、内存 pending、目标写入和原版源删除。`persist_state_locked(true)` 在非显式保存窗口不落盘。 | 接入统一事务；不得把该调用的成功视为已保存或可跨进程恢复。 |
| `move_extension_to_original_locked()` | 已使用 `SAVE_LoadItem` 与 `INVEN_SaveItemOnEmpty`，但 payload 为零时以 `fn_create_item(category, ...)` 降级创建，且成功后未 `handover_to_inventory`。 | 删除近似创建；成功入库后执行所有权移交。 |
| `materialize_module_item_locked()` 与装备回滚 | `SAVE_LoadItem` 失败后仍存在按 category/count 重建的降级路径。 | 改为隔离 + 只读诊断；不得生成替代对象。 |
| `UnsavedCrossMove` / `rollback_unsaved_cross_moves_locked()` | 仅内存；回滚时会弹出失败记录，失败 payload 无法保留。 | 退役，不能与统一事务并存。 |
| `recover_pending_transaction_locked()` | 只消费内存 `PendingTransfer`。 | 仅保留为 P4 进程内恢复或改由统一入口实现；不得宣称重启恢复。 |
| `ownership_ledger.h` | 四态、generation handle 与 audit 已存在；生产侧已有 `allocate`、`borrow_for_view`、`release`，但没有 `return_from_view`、`handover_to_inventory`。 | 补齐状态转移与引用撤销；审计不能只检查 `balanced`。 |
| `defer_item_free_locked()` | 当前为 no-op，以避免 P3 控件/TouchState 残留指针的 UAF。 | P4 以借用归还证据替换它；不得直接恢复 Free 而不先撤销所有视图引用。 |
| `virtual_bag_state.h` | `JournalRecord` 和 `journal_recovery_action()` 已定义；`recovery_action()` 与 `valid_journal_record()` 只拒绝 `>= 6`，会放行任务袋 5。 | 在事务域验证层改为显式允许原版袋 `0..4`；坏数据不得由 `normalize()` 静默清空。 |
| `ExtensionBagJournal.kt` | 已有 v1 `read/write/clear/nextTransactionId/parsePayload` 辅助，但无生产调用。 | P4 不直接接线持久化；P7 消费该格式和辅助。 |
| `ExtensionBagUiBridge.kt` | `parseState()` 会把不安全 payload 或 payloadless item 降为 `ItemData(0,0)`。 | 需要支持隔离结果，不能把这类记录转成正常空物品或静默丢失。 |

## 3. 不变量与编号域

### 3.1 编号与输入验证

| 名称 | 合法范围 | 禁止行为 |
|---|---:|---|
| 原版可参与扩展事务的袋 | `0..4` | 不接受 `5`、负数或大于 5 的原版袋。 |
| 原版任务袋 | `5` | 不作为源、目标、投影窗口、恢复目标、回滚目标、匹配扫描或自动投递目的地。 |
| 扩展逻辑袋（API/日志） | `6..10` | 不把该值直接写入 native 的内部袋字段。 |
| 扩展内部袋（状态/sidecar） | `0..4` | 不把内部 0..4 当作原版袋编号。 |
| 槽位 | 由 `derive_capacity(BagType)` 决定 | 不以固定 16 格绕开容量判断。 |

入口、`PendingTransfer`/`JournalRecord` 解析、恢复裁决、回滚记录和任何目标匹配扫描必须调用同一域验证函数。验证失败的记录进入隔离，不得尝试“最接近”的袋或槽。

### 3.2 payload 与 native 对象

1. 逻辑 `Item`、sidecar、pending、journal 和隔离记录只存数据，不存 native 指针。
2. `SAVE_SaveItem` 成功的长度必须在 `kPayloadHeaderSize..255`，并通过现有尾部零检查；越界、截断、空结果或校验失败拒绝整笔事务，源不变。
3. `SAVE_LoadItem` 包装必须：初始化 `void* item = nullptr`；检查返回值、输出指针非空和 consumed 长度恰好等于 payload 长度；任何失败均只释放本次临时对象（若有），不能修改源逻辑物品。
4. 禁止在 payload 缺失、Load 失败或字段不一致时调用 `fn_create_item(category, ...)`、改写数量或用缓存字段拼装替代物品。
5. native 对象只能在本次操作窗口、`g_virtual_bag_mtx` 保护的明确状态中使用。模块不得跨保存、退出、切档或下一次投影保留裸所有权指针。

### 3.3 所有权状态机

| 转移 | 唯一转移方 | 前置条件 | 成功后 | 失败处理 |
|---|---|---|---|---|
| `module-owned → borrowed-for-view` | 投影安装 | ledger handle 有效、控件尚未持有该对象 | 控件仅借用；ledger 记录 borrow | 不安装控件，维持 module-owned。 |
| `borrowed-for-view → module-owned` | 投影恢复/退出 | 所有 `ControlItem`、TouchState、描述面板和快照引用均已清空或恢复 | 调用 `return_from_view`，模块可释放或继续使用 | 保持借用状态并中止进一步 Free；记录诊断。 |
| `module-owned → inventory-owned` | 扩展→原版入库提交点 | Load 成功、原版插入成功、指针确由原版库存接管 | 在同一锁范围调用 `handover_to_inventory`，handle 立即失效 | 插入失败时仅归还临时对象，源扩展逻辑物品不变。 |
| `module-owned → released` | 模块清理 | 无借用引用、未移交库存 | `ITEMPOOL_Free` 一次，ledger `release` 一次 | 释放失败必须记录并阻断，不得二次释放。 |

任何实际 `ITEMPOOL_Free` 前必须完成“所有视图引用已撤销”的审计。每轮测试在视图外应满足 `outstanding_borrows == 0`；重复循环后分配、释放、移交和 live 总数不得单向增长。`inventory-owned` 对象不得通过 ledger 异常或回滚逻辑再被模块释放。

## 4. 统一事务模型与 P7 的持久化边界

### 4.1 P4 的进程内五态

所有三方向操作都从同一协调入口创建不可变的事务快照：事务 ID、方向、源/目标袋槽、源和目标 payload/数量快照、目标容量与合并判定、投影快照、所有权 handle。状态严格单向前进，失败只允许进入明确的回滚/隔离分支。

| P4 状态 | 所做事实 | 回滚要求 | 与 journal v1 的关系 |
|---|---|---|---|
| `prepared` | 通过编号、容量、payload 和所有权前置校验，收集不可变快照。 | 无状态变更。 | P7 stage 0 前的输入。 |
| `pending-recorded` | 写入进程内 pending，标注 transactionId。 | 删除 pending，释放本次临时资源。 | 与 stage 0 `prepared` 字段同构，但不代表落盘。 |
| `logical-state-updated` | 仅按方向更新扩展逻辑 source/target，并重新物化/恢复投影。 | 用快照还原 source、target、计数与 UI。 | P7 将基于相同快照写 journal。 |
| `original-state-updated` | 仅在涉及原版的一侧确认删除或入库，并以原版可观察实态复核。 | 若原版动作未成功，回滚逻辑状态；若原版已接管，不得释放该对象。 | P7 stage 1 `original_saved` 的前置业务事实。 |
| `committed` | 清理进程内 pending、释放或移交对象、完成投影同步。 | 不做猜测性逆转。 | P7 committed sidecar 与清 journal 后才是跨进程提交。 |

`original-state-updated` 对扩展→扩展恒为“不适用”：该路径只走 `prepared → pending-recorded → logical-state-updated → committed`，不调用 `INVEN_MoveItem`，也不创建 prepare journal。

### 4.2 P4 与 P7 的硬边界

- `persist_state_locked()` 在非显式保存窗口不落盘。因此它的返回值只能表示本地调用结果，不能推进事务到“已保存”、不能清除可恢复信息、不能成为跨进程恢复证据。
- P4 不直接调用 `ExtensionBagJournal.write/clear`，不实现 `extensionbags.journal` 的 stage 0/1/2 落盘，不审计全部原版保存入口，也不在重启后宣称恢复一笔未落盘事务。
- P4 必须让事务快照可以无损转换为既有 `JournalRecord` v1：保留 transactionId、generation、方向、源/目标袋槽、payload、sourcePayload 和 stage 所需身份信息；不得新建不兼容格式。
- P7 的唯一正式顺序保持不变：独立落盘 stage 0 → 原版变更和原版保存 → stage 1 → 提交 committed sidecar → 清 journal。世界实态探针优先于可能滞后的 stage；非法 journal `kDiscard` 隔离并告警。
- P4 发现 `PendingTransfer`、journal 记录或 sidecar 项含任务袋 5、缺 payload 或字段非法时，只产生隔离和诊断；不尝试重放或推断目标。P7 决定跨进程隔离记录的持久化时机。

### 4.3 三条路径的提交顺序

**原版→扩展**：验证原版源袋为 0..4 → `SAVE_SaveItem` 取得完整 payload → 预判目标合并/空槽且建立双端快照 → pending → 更新扩展逻辑目标并验证投影 → 删除原版源并以原版实态复核 → 提交并清理临时模块对象。原版源删除失败时必须恢复目标逻辑项、投影和 pending；不能只相信删除函数返回值。

**扩展→原版**：验证目标原版袋为 0..4、源 payload 有效 → `SAVE_LoadItem` 到临时对象 → ledger 分配/登记临时模块所有权；登记失败立即 `ITEMPOOL_Free` → 原版 `INVEN_SaveItemOnEmpty` 成功后，在同锁内 `handover_to_inventory` → 清扩展源逻辑项并恢复投影 → 提交。Load、登记或插入任一步失败时，扩展源和其 payload 原样保留；不得清源、不得创建近似物品。

**扩展→扩展**：验证两个扩展袋均为内部 0..4 和有效槽位 → 建立 source/target 快照 → pending → 单纯更新逻辑项、合并数量和投影 → 提交。不得调用 `INVEN_MoveItem`、原版保存函数或 prepare journal；失败仅由逻辑快照恢复。

## 5. P4 工作包

### P4.1 事务域与任务袋 5 前置门禁

**目标**：先消除任务袋 5 在模型与解析层的放行风险，之后才允许任何事务入口消费外部输入。

**具体要求**：

- 在 `virtual_bag_state.h` 建立唯一的原版事务袋验证函数，明确只接受 `0..4`。
- `PendingTransfer`、`JournalRecord`、`valid_journal_record()`、`recovery_action()`、`journal_recovery_action()` 的输入和回滚目标统一使用它；不能以 `>= 6` 作为唯一上界判断。
- API 换算层、触摸/投放路由、投影安装、源/目标扫描和 `UnsavedCrossMove` 的替代机制在调用事务前验证编号域。
- 非法记录进入隔离原因 `invalid_transaction_domain`，日志至少有 transactionId（如有）、源/目标袋槽和拒绝原因；不访问原版任务袋。

**禁止项**：不得通过“当前 UI 无法选中 5”推导安全性；不得让恢复代码成为验证例外。

**退出条件**：host 覆盖源=5、目标=5、恢复记录=5、回滚记录=5、逻辑 ID=11 等非法输入；真机对已有 API、投放、投影拒绝路径分别留下索引 5 sentinel 前后不变的证据。

### P4.2 完整 payload 桥接原语

**目标**：把 Save/Load 的专有调用约束封装为唯一、可审查、可失败的桥接原语。

**具体要求**：

- 保留并复用 `serialize_item_payload_locked()` 的长度与尾部检查；所有原版→扩展调用点只能经该入口。
- 为 `SAVE_LoadItem` 建立对应的受管重建入口，强制 out 初始化、返回值/consumed/指针校验和失败时单次对象归还。
- 把 payloadless、长度不符、非零尾部、Load 失败、Load 后 consumed 不匹配都归为不可移动的隔离事件。
- 删除 `move_extension_to_original_locked()`、`materialize_module_item_locked()`、装备回滚和其他路径中的 `fn_create_item(category, ...)` 近似重建；不允许后续再写同类降级分支。
- 原版→扩展→原版保真检查必须直接比较源和重建前后的完整 payload 字节；日志可记录长度和确定性摘要用于定位，但摘要不得替代测试中的字节级比较。

**依赖**：P4.1 已完成。

**退出条件**：host 覆盖 1/99/999 堆叠、附魔/宝石孔/稀有度样本、255 字节边界、空/截断/超长 payload；真机最小闭环在原版库存中验证复杂物品往返后完整 payload 不变。

### P4.3 所有权账本真实接线与释放收敛

**目标**：使 ledger 成为实际 native 对象唯一所有权的记录，而非只在 host 中平衡的计数器。

**具体要求**：

- 物化后分配 handle；投影安装/恢复必须成对调用 `borrow_for_view`/`return_from_view`。
- 扩展→原版插入成功后立即 `handover_to_inventory`；handle 无效后禁止模块通过数组、快照或错误分支访问该指针。
- `SAVE_LoadItem` 成功但 ledger 分配失败时立即 `ITEMPOOL_Free`；入库失败只释放临时对象，逻辑 source 保持不变。
- 在控件树、TouchState、描述面板、投影快照全部撤销后，逐步用真实 `ITEMPOOL_Free` 替换 `defer_item_free_locked()` no-op。每一步须先有 UAF 回归证据，不能以一次未崩溃替代引用审计。
- 日志输出 allocation、release、handover、live、outstanding_borrows 和 transactionId；日志不得输出 native 指针到 sidecar 或长期状态。

**依赖**：P4.1、P4.2。

**退出条件**：host 覆盖合法/重复/过期 generation handle；真机重复进入、切换、退出、失败移动与入库后，在视图外借用为零，循环计数无增长，且无 UAF、重复 Free 或模块释放 inventory-owned 对象。

### P4.4 唯一进程内事务入口

**目标**：将散落的 pending、回滚、投影恢复和所有权处理收敛为第 4 节的五态事务。

**具体要求**：

- 三条移动函数仅负责构造方向参数并调用统一协调入口；状态推进、快照、提交和回滚不得在各路径复制。
- `PendingTransfer` 仅作为本进程 pending 记录，并与 `JournalRecord` v1 字段一一映射；日志明确 `durable=false`。
- 退役 `UnsavedCrossMove` 及 `rollback_unsaved_cross_moves_locked()`；不存在两套跨包回滚日志或按失败次数弹出记录的语义。
- 所有失败分支先还原逻辑状态和 UI/投影，再释放仍由模块拥有的临时对象；已移交原版库存的对象不参与模块回滚。
- 扩展→扩展使用纯逻辑快照，不创建 prepare journal，不调用原版库存移动。
- 锁内不调用 `op_ok()` 或其他可能重入同一把锁的 API 辅助；遵守 `docs/backlog.md` 的锁内死锁审计约束。

**依赖**：P4.1 至 P4.3。

**退出条件**：结构搜索确认移动路径不存在 `UnsavedCrossMove` 和近似创建回退；host 对每个状态边界注入失败，断言 source、target、projection、pending 和 ledger 审计满足预期。

### P4.5 失败隔离与只读诊断

**目标**：把不可安全重建、不可安全恢复的数据保留为可审计记录，而不是创建假物品、置空或下一次保存覆盖。

**最小隔离记录**：

| 字段 | 要求 |
|---|---|
| `recordId`、`observedAt`、`reason` | 标识和明确失败分类。 |
| `transactionId`、`generation`、`direction`、`phase` | 若可取得则原样保留，支持与 P7 journal 对照。 |
| `source`、`target` | 袋/槽原始数值；非法数值不得正则化。 |
| `payload`、`sourcePayload` | 原始字节或受限的原始编码；不能保存 native 指针。 |
| `payloadLength`、完整性诊断 | 区分空、截断、长度不符、Load 失败、插入失败、域非法。 |
| `gameIdentity`、只读状态 | 与 sidecar 身份绑定；业务 API 不得将其当正常 Item 操作。 |

**具体要求**：

- `normalize()` 和 Kotlin `parseState()` 遇到无效 payload 不得默默转空槽、`ItemData(0,0)` 或自动回写；改为产生隔离结果并使原始记录保留只读诊断。
- 隔离的 schema 可以由 P4 定义并在内存中暴露给开发期状态读取；其 sidecar section 写入时机、CRC 故障恢复和跨进程 replay 由 P7 协调器决定。
- 插入失败和 Load 失败都必须保留源逻辑物品；如果原始字节无法安全进入逻辑 Item，则保留隔离字节，不构造可移动替代项。
- `kDiscard` 的非法 journal 行为为隔离并告警，不得猜测重放。

**依赖**：P4.1、P4.2、P4.4。

**退出条件**：host 覆盖每个失败 reason；状态读取能显示只读诊断；真机强制一次 Load 或入库失败后证明没有近似物品、没有丢失源项、没有任务袋触碰。

### P4.6 验证、证据与移交

**目标**：在不侵入 P5/P7 范围的前提下，建立 P4 通过所需的 host 与真机最小证据闭环。

**具体要求**：

- host 只验证纯域校验、状态转移、journal v1 兼容映射、隔离、字节保真和 ledger 审计；不得据此宣布真实库存或控件安全。
- 真机唯一使用控制面指定设备 `192.168.3.54`，一项一变量，按控制面 §5.1 当场登记 APK/源码身份、操作前后状态、日志与用户结论。
- P4 使用已登记的 `/api/item/inventory/move_item` 和只读状态端点进行最小事务驱动；若 API 不能表达真实拖动取消，记录“真实交互验收路径未就绪”，转交 P5，不把其标记为已通过。
- P4 只执行每方向成功一次、每方向失败回滚一次、任务袋拒绝、所有权计数和 payload 保真最小闭环。保存调用点、stage 间杀进程和重启重放矩阵交给 P7。

## 6. 失败遏制矩阵

| 故障 | 预期行为 | 必须保持/恢复 | 可观测证据 |
|---|---|---|---|
| `SAVE_SaveItem` 返回非法长度、截断或校验失败 | 拒绝事务，不写目标。 | 原版源、扩展目标、pending、投影均不变。 | transactionId、payload 长度、失败 reason。 |
| 目标无容量、满且不可合并、超过数量上限 | 在 prepared 阶段拒绝。 | 双端逻辑状态和原版状态不变。 | 容量、目标快照、拒绝原因。 |
| `SAVE_LoadItem` 失败或 consumed 不匹配 | 隔离 payload，不创建近似物品。 | 扩展源、原版目标、sidecar 内存状态不变。 | 隔离 recordId、reason、payload 长度。 |
| ledger 分配失败 | 立即归还仅本次临时对象，拒绝事务。 | 扩展源和原版目标不变。 | allocation/release 计数和 transactionId。 |
| `INVEN_SaveItemOnEmpty` 失败 | 归还临时 module-owned 对象，保留扩展源。 | source、target、投影、ledger 一致。 | 插入结果、release、source payload 摘要。 |
| 原版源删除后复核失败 | 按原版实际状态裁决并回滚逻辑目标；不只信返回值。 | 不产生重复或丢失。 | 原版源探针、逻辑双端快照。 |
| 投影安装/恢复失败 | 中止提交，先恢复快照；有残留借用时禁止 Free。 | 控件、TouchState、描述面板和 ledger 借用一致。 | projection 状态、outstanding_borrows。 |
| 含任务袋 5 的输入或恢复记录 | 拒绝并隔离，不读取/写入任务袋。 | 任务袋 sentinel 不变。 | 原始袋值、拒绝 reason、sentinel 前后比对。 |
| pending/journal 结构或 payload 损坏 | 隔离并只读诊断；不得 normalize 成空项。 | 已提交逻辑状态不被猜测性覆盖。 | recordId、解析错误、原始字节长度。 |

`extensionbags.journal` 写入失败、原版保存失败、stage 0/1/2 间进程中断、切档和重启重放是 P7 的持久化矩阵；P4 文档不得把这些未执行的用例写成通过。

## 7. 测试与验收矩阵

### 7.1 host 必测项

1. 事务域：原版 `0..4`、任务袋 5、负数、上界越界、扩展逻辑 `6..10` 与内部 `0..4` 的双向换算。
2. 五态：三方向每个状态边界的成功和失败；扩展→扩展不得引用任何原版移动符号。
3. payload：完整字节往返、1/99/999、复杂装备样本、255 字节边界、空/截断/尾部非零/Load consumed 不匹配。
4. 所有权：借用归还、入库移交、重复 Free、过期 generation handle、插入失败释放、视图外借用为零。
5. 隔离：所有 `reason`、非法 journal `kDiscard`、`normalize()` 与 Kotlin 解析不会清除或替换原始坏记录。
6. 事务兼容：从 P4 pending 快照构造的 `JournalRecord` v1 经现有纯函数裁决时字段无损；此测试不调用持久化写入。

### 7.2 真机最小闭环与 P5 移交

| 控制面测试目标 | P4 最小证据 | P5 追加责任 |
|---|---|---|
| 空槽 | ✅ 已登记 `E-2026-08-31-04`：原版→扩展与扩展→原版各两笔 API 驱动成功（×18 药水、背包（小）），双端字节保真。 | 三条真实输入路径逐项验收。 |
| 目标满、不可合并、数量上限 | 🔶 不可合并/身份规则已登记（不同 payload 不合并、独立落位）；目标满与数量上限的真机注入「验收路径未就绪」（API 无法表达；ext→orig 为整堆语义，`count` 不参与），host 已覆盖纯逻辑。 | 原版 UI 的满包反馈与路径矩阵。 |
| 可合并 | 🔶 身份规则已登记（同上）；正向合并由 host 纯逻辑覆盖（`mergeable_items`），API 无同 payload 堆叠样本。 | 拖放合并体验。 |
| 非法目标 | ✅ 已登记 `E-2026-08-31-05`：任务袋 5 双向 `task bag excluded`，sentinel 无写入。 | 真实投放到任务袋的拒绝。 |
| 取消 | 🔶 提交前可控失败注入「验收路径未就绪」，host 已覆盖快照恢复断言。 | 0x81 拖动中的真实取消和跨视图取消。 |
| 插入失败 | 🔶 `INVEN_SaveItemOnEmpty` 故障注入「验收路径未就绪」，host 已覆盖「源保留、临时对象只释放一次」断言。 | 每条用户路径的故障注入矩阵。 |

> 登记说明（2026-08-31）：扩展移动 API 当前为整堆语义——**暂时实现**（用户决策：引擎路径可实现按数量移动，暂不实施）；native 签名无 count；orig→ext 成功后按 P2 设计安装扩展视图并锁移动，视图退出为真实触摸边界；`/api/debug/extension_bag/*` 为开发期视图/诊断端点。

每条真机证据必须使用唯一 `E-YYYY-MM-DD-NN`，并按控制面 §5.1 记录源码提交、`git status --short`、APK/设备身份、输入、前后 Bag/Projection/Ownership/Persistence 状态、原生日志和用户结论。没有完整字段的历史证据不能支撑 P4 通过。

## 8. 文件职责矩阵

| 文件 | P4 允许的职责变更 | 明确禁止 |
|---|---|---|
| `module/app/src/main/cpp/virtual_bag_state.h` | 事务域验证、事务状态/快照、隔离模型、journal v1 兼容映射、纯函数测试接口。 | 更改 journal v1 协议或让 bag 5 合法化。 |
| `module/app/src/main/cpp/ownership_ledger.h` | 使用既有四态完成真实借用归还、移交和释放审计。 | 以裸指针或布尔标志取代 generation handle。 |
| `module/app/src/main/cpp/game_ui_virtbag.cpp` | 统一三方向协调入口、Save/Load 包装调用、投影引用撤销、临时对象生命周期、隔离接线。 | 近似创建、并存 `UnsavedCrossMove`、把 persist 成功当落盘、调用 ext→ext 的 `INVEN_MoveItem`。 |
| `module/app/src/main/cpp/game_access.*` / `game_symbols.h` | 仅在现有 ABI 不足且经逆向证实时补充已验证符号包装。 | 在 UI/事务代码中新增裸 VMA 或复制偏移。 |
| `module/app/src/main/java/com/inotia4/qol/ExtensionBagUiBridge.kt` | 解析结果携带隔离信息，避免坏 payload 降级为空 Item。 | 旧格式自动迁移、把隔离项当正常可移动 Item。 |
| `module/app/src/main/java/com/inotia4/qol/ExtensionBagJournal.kt` | P4 可验证其 v1 读写辅助和字段兼容。 | 在 P4 把所有移动或保存入口直接接到正式落盘协调器。 |
| `module/app/src/main/cpp/tests/test_host.cpp` | 新增纯模型、生命周期、隔离、域验证和兼容映射测试。 | 以 host 断言替代真机 native/UI/库存结论。 |

实现必须符合 `architecture.md`：符号和 ABI 留在 data 层，解析留在 `game_inventory.*` / `game_save.*` 域，patch 逻辑不得向 Java 抛 native 异常。若当前文件布局不足，先写出最小职责变更并审查，不以 P4 为理由做未经验证的大规模重组。

## 9. P4 退出门槛与移交清单

只有以下全部满足，控制面才可登记 P4 结果：

1. P4.1 至 P4.5 的代码审查和 host 测试通过；原型的近似创建、双回滚机制和不受控 no-op 释放均已删除或被有证据的真实生命周期替代。
2. 所有跨包路径共享事务域验证和统一入口；任务袋 5 的源、目标、恢复、回滚和 sentinel 证据完整。
3. 所有权审计在成功、失败、退出和重复循环场景中满足唯一所有者、视图外零借用和无增长计数。
4. P4 真机最小闭环已按 §7.2 登记；host、日志或单次 API 成功没有被误写为 P5/P7 通过。
5. P7 交接明确列出：P4 事务快照到 journal v1 的映射、尚未落盘的 pending 不具恢复承诺、所有原版保存入口需要收口、stage 边界故障注入和重启恢复待验收。

移交 P5 时，三条移动和 0x81 拖动必须复用本文件的编号域、payload、所有权、事务和隔离契约。移交 P7 时，协调器必须复用本文件的 journal v1 映射而不是另建交易格式；P7 完成前不得把任意 P4 内存 pending 宣称为崩溃恢复。

## 10. 审查参考（非本地权威）

以下材料只用于代码审查时交叉验证原则，不覆盖或修改本项目 ADR：C++ Core Guidelines I.11/R.1/R.23/CP.20/E.16（显式所有权、RAII 与不失败释放）、Stroustrup 的强异常安全“先构建后提交”、LevelDB 对坏 WAL 记录的报告与跳过、SQLite atomic commit 的 journal-before-data 和独立提交点、PostgreSQL 2PC 的持久 transaction ID。与控制面或 sidecar 契约出现差异时，始终以本仓库权威文档为准。
