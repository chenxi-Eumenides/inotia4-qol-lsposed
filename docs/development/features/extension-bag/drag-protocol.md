# 扩展背包 P5：原生拖动协议与三方向路径验收

> **状态**：已完成，`ACCEPTED`（2026-09-02）。本文件是 P5 的唯一实施与验收计划；P4/P5 已完成归档，P7 或整体发布仍未通过。
>
> **项目根目录**：`/home/chenxi-zqs/Code/opencode-workspace/projects/inotia4-qol-lsposed`
>
> **权威关系**：阶段状态、设备、证据格式和 ADR 以 `control-plane.md` 为准；本文件将其中 P5 拆为可实施的工作包。代码结构以 `../../architecture.md` 为准，API 以 `../../../reference/api-reference.md` 为准。本文不替代 P7 的持久化协议。

## 1. P5 要解决的主要任务

1. **先证实、后接通原生拖动**：查明 `UIEquip_InvenItemControlEventProc` 的 0x81/0x10 事件与 TouchHandle 生命周期。历史 handoff 所称“0x81 返回 1 建 moving”只是待复验的证据，不能直接当 ABI。
2. **只保留一个拖动所有者和一个提交入口**：以 generation 绑定的 `ExtensionDragSession` 收敛按下、移动、目标解析、取消和 release；每次 release 最多派发一次既有 P4 事务。
3. **完成三方向真实输入**：按扩展→扩展、原版→扩展、扩展→原版严格串行实现并验收；扩展源的 0x81 建立路径覆盖前、后三者中的扩展→扩展和扩展→原版。
4. **把失败和跨视图处理变成可验证协议**：覆盖满目标、合并/不合并、任务袋 5、非法目标、取消、stale 事件、F3、切换/退出视图和受限的进程内 Load/插入失败。
5. **提供可审计证据**：补齐纯模型 host 测试、结构检查、结构化日志和唯一真机物理触摸矩阵；debug 能力只能辅助诊断或受限注入，不能伪造拖动验收。

## 2. 已知基线、范围与非目标

### 2.1 已证实的代码基线

| 区域 | 当前事实 | P5 含义 |
|---|---|---|
| 原生 proc | 模块源码没有 0x81 的显式处理；`ui_equip_inven_item_proc_wrapper` 仅拦截既有事件，其他事件透传原版 proc。0x81 仅见于 P3 handoff 的历史事件表与 G-8 卡点，P5 同一设备观测未出现该事件。 | P5 采用已实证的 `0x10 → result=1 → MOVING_CTRL` 路径，不把未观察到的 0x81 假定为 ABI；后续不得新增 0x81 猜测逻辑。 |
| 面板输入 | `virtual_bag_event` 在扩展网格命中时由 `g_extension_touch_capture` 处理 0x17/0x18/0x19；旧 `ExtensionDrag` 有条件地参与该路径。 | 旧路径是实现基线而非验收协议。新路由接通前不得与原生路由双活，退役前需用行为对照证明。 |
| 控件与投影 | 扩展槽是投影到原版窗口的 `ControlItem`；投影槽按下已旁路模块捕获并委托原版 TouchHandle；仅验证为投影子控件的 native moving 跨帧保留，其他 stale moving 仍清理，所有权窗口同步识别该借用。 | 已验证拖动建立和动画所需的 `0x10 → result=1 → MOVING_CTRL`；仍需 source identity、release/ownership 矩阵和三方向验收。标签绝不能因 0x81/0x10 返回 1 而成为拖动源。 |
| 当前 drop 入口 | 现存路径包含投影 item 的 0x02、袋 drop 的 0x04/`INVEN_SaveItemOnEmpty` gate，以及面板 release。 | P5.6 必须对每个 release 指定唯一的入口/return；禁止顺序敏感的隐式去重。 |
| P4 移动核心 | `move_original_to_extension_slot_locked`、`move_original_to_extension_locked`、`move_extension_to_original_locked`、`move_extension_to_extension_locked` 已接入 P4 的 payload、所有权、五态事务与隔离。 | P5 只能解析输入并调用这些既有事务入口，不能重写或旁路事务模型。 |

### 2.2 非目标与明确边界

- 不在 P5 实现或验收 `extensionbags.journal` 的 stage 0/1/2 正式落盘、原版/sidecar 保存协调、杀进程/重启/切档重放、全部保存调用点或 ADR-009；这些均归 P7。
- 不把 API 移动、debug 注入、坐标脚本或 host 测试记为真实拖放成功。除启动协议弹窗的既有 `(420,280)` 例外外，P5 真机拖动必须由用户在 `192.168.3.54:5555` 物理触摸。
- 不在本次文档变更中修改 `api-reference.md`、`environment.md`、`backlog.md` 或源码。P5.0 必须登记其冲突并在对应责任工作中修正。
- 不新增裸 VMA、未知 TouchState 写法、第二个移动状态机、第二个 pending/journal 格式，或发布构建可用的隐藏写入端点。

## 3. P4 继承契约

以下约束由主控文档的 P4 归档节定义，P5 的每个实现和测试都必须消费它们：

| 领域 | 不变量 |
|---|---|
| 袋与槽 | 原版事务袋只为 `0..4`；任务袋 `5` 从源、目标、投影、恢复、回滚、扫描和自动投递中排除；扩展 API/日志袋为 `6..10`，内部状态袋为 `0..4`，两者不得混用。 |
| payload | orig→ext 唯一使用 `SAVE_SaveItem` 完整 payload；ext→orig 唯一使用 `SAVE_LoadItem`，其 out 先置空，返回/out/consumed/重序列化字节比较必须全部通过。payload 长度为 `19..255`，禁止近似重建。 |
| 所有权 | `module-owned`、`borrowed-for-view`、`inventory-owned`、`released` 四态唯一所有者；视图恢复撤销全部引用后才归还；原版接管成功后同锁 handover，模块永久不再访问该对象。 |
| 事务 | 唯一五态 `prepared → pending-recorded → logical-state-updated → original-state-updated → committed`；ext→ext 无 original 阶段、不调 `INVEN_MoveItem`；失败先还原逻辑/UI，再释放仍属模块的临时对象。 |
| 隔离 | 仅使用七个既有 reason；非法/坏数据保留原始袋槽、transactionId、generation 与字节，作为只读诊断，绝不静默清除或变成替代 Item。 |
| 持久化 | `PendingTransfer` 是内存 `durable=false` 记录，只与 `JournalRecord` v1 字段同构；P5 不承诺跨进程恢复。 |

## 4. P5.0：前置证据与文档治理

**目标**：把 P5 的输入事实固定下来，避免用过期文档、旧 APK 或错误设备解释新行为。

**必须完成**：

1. 在任何源码或真机动作前记录 `git status --short`、HEAD、APK 身份、包名、版本、唯一设备与日志起点；不得把历史“工作树干净”作为当前证据。
2. 以主控文档为扩展背包验收裁决面：它与 `environment.md`/`backlog.md` 的设备或触摸描述冲突时，先按主控的唯一设备与用户真实拖放规则执行，并建立回写任务。
3. 建立事实修复清单：`api-reference.md` 的“unsaved journal”措辞应改为内存 `durable=false`；固定容量表述、历史包名以及 `environment.md` 的章节引用均需由其责任文档后续校正。本工作包只登记，不在本次计划文档中越权修改。
4. 确认 P4 归档中的五个未就绪项已进入本文件：目标满/数量上限、拖放合并、任务袋真实拒绝、0x81 取消、每路径 Load/插入失败矩阵。

**退出条件**：当前身份、设备、文档差异和 P4 移交清单均有记录；尚未发送任何协议改动或把 API/debug 结论写成真实拖放通过。

## 5. P5.1：0x81/0x10 与 TouchHandle 只观测取证

**目标**：建立可复核的事件事实，而不是立即“修复”拖动。

**观测内容**：

| 维度 | 每次按下/移动/release/取消必须记录 |
|---|---|
| 事件 | event 值、发生顺序、proc 返回值、param 是否存在及仅已知字段的安全摘要。 |
| 控件 | source/target 控件类型、`data[0]` 所属域、UserType、父链身份、slot 映射结果；不持久化裸指针。 |
| TouchHandle | `MOVING_CTRL`、drop source、release 坐标的前后可观察状态，以及引用何时安装/撤销。 |
| 行为 | 原版物品与投影扩展物品各自的按下、移动、release、取消结果；0x81 与 0x10 的角色差异必须由同一 APK/设备复验裁决。 |

**禁止项**：不得复制未知 TouchState 字段、伪造原版 proc 参数、基于历史表硬编码“0x81 必须返回 1”，也不得让未验证的控件或事件进入原生拖动/事务路径。

**退出条件**：形成带源码/构建/设备身份的协议证据；明确原版 proc 的可安全委托条件，或证明必须使用最小 adapter。若证据冲突或触发 UAF/重复提交，停止在 P5.1，不进入实现。

### 5.1.1 P5.1 debug observer 闸门与取证规程

P5.1 observer 仅在 `__android_log_is_loggable(ANDROID_LOG_DEBUG, "Inotia4VirtBag",
ANDROID_LOG_INFO)` 为真时运行。必须先以闸门关闭状态安装并启动 APK；闸门开启只用于单个
证据会话，不能作为默认运行配置。

1. 每次会话开始前登记设备 API level（必须 `>= 30`）、`ro.debuggable`、`log.tag`、
   `persist.log.tag`、`log.tag.Inotia4VirtBag` 与 `persist.log.tag.Inotia4VirtBag`。四个
   log 属性必须为空，才能将安装前状态解释为 observer 默认关闭；`persist.log.tag.*`
   优先于 runtime 链，不能遗漏。
2. 开启时先执行
   `adb -s 192.168.3.54:5555 shell setprop log.tag.Inotia4VirtBag DEBUG`，并逐字读取
   同一属性确认值为 `DEBUG`。属性键严格区分大小写，必须使用代码中的
   `Inotia4VirtBag`，不得尝试全大写变体。回读失败时中止取证；首次物理触摸未产生
   `p5obs` 行时，当前证据行无效，不得把没有日志解释为事件不存在。
3. 每个有效会话的第一项动作必须是一次物理触摸并出现 `p5obs` 行；随后才记录原版 source
   与投影 source 的按下、移动、release、取消。0x10 与 panel 0x19 已按 100ms 采样，0x81
   不限频，读取时不得将日志行数直接解释为事件总数。
4. 会话开始至结束不得改动任一 `log.tag*` 属性。中途关闭会留下只有 pre、没有 post 的
   token；该 token 的结果只能登记为 `unknown`，不能作返回值或生命周期结论。
5. 会话结束立即拉取文件日志与 logcat，并以
   `adb -s 192.168.3.54:5555 shell setprop log.tag.Inotia4VirtBag ""` 恢复闸门关闭，
   回读 runtime 和 persist tag 属性均为空；每轮使用独立 `E-YYYY-MM-DD-NN`，关联源码、
   APK SHA-256、设备、属性值、日志起点和用户物理触摸结论。

## 6. P5.2：唯一 `ExtensionDragSession` 与协议 adapter

**目标**：用可验证的状态机代替多条隐式输入路径。

### 6.1 运行时状态机

```text
Idle → Pressed → NativeMoving → TargetResolved → TransactionInFlight
                                              ↘ Rejected
NativeMoving / TargetResolved → Cancelled
TransactionInFlight → Committed | Rejected
```

- session 至少记录 protocol version、单调 token、view generation/identity、逻辑 source bag/slot、press/release 序列与 terminal cause；不保存 native 指针。
- 每个 terminal 状态只清理本 session 的 moving 标记、控件引用和 transient 诊断；只有 `TransactionInFlight` 可创建 P4 transactionId。
- 同 token 的重复 release、stale generation、已取消 session 或未知事件均为幂等无操作；绝不重新定位源、补发事务或回收 `inventory-owned` 对象。

### 6.2 选型闸门

1. **安全委托原版 proc**：只有 P5.1 证明参数、所有权与 TouchHandle 字段均满足原版前置条件，且 session 全周期可持有有效借用引用时才能采用。
2. **最小 adapter**：若原版 proc 拒绝借用对象，adapter 只能构造已经实证的返回语义并交给 session；它不得仿写未知原版状态机或让扩展标签成为拖动源。
3. 两种方案必须二选一。`g_extension_touch_capture`/旧 `ExtensionDrag`、投影 0x02、0x04、面板 release 与低层 gate 不得同时拥有同一 release 的提交权。

**生命周期前置修复已完成第一步**：draw-end 不再无差别清 `MOVING_CTRL`；仅经当前投影控件身份验证的 native moving 跨帧保留，`object_touch_window_active_locked()` 同步将其视为活动借用，其他 stale moving 仍清理。完整 session token、source identity 和 release ownership 矩阵仍是 P5 完成前置条件。

## 7. P5.3：实时目标解析与路由去重

**目标**：以真实控件身份决定路径，消除硬编码坐标和多入口竞态。

- source 与 target 必须经控件树、UserType、已证实的 slot 映射和运行时容量确认；仅以手工父链计算的已验证绝对位置辅助命中，不直调已知存在 ABI 风险的 `GetAbsoluteRect`。
- `original_bag_button_index` 的固定坐标判断不得成为最终原版袋目标依据；目标解析要返回原版袋 `0..4`、扩展逻辑袋/槽、扩展标签、非法目标或取消。
- 一旦目标是任务袋 `5`、无效槽、容量外槽或过期 view generation，session 在 payload、Load、ledger 或事务前拒绝。
- 为 0x02、0x04、面板 release、`INVEN_SaveItemOnEmpty` gate 建立 dispatch ownership 表。每一个 release 只能有一个 owner 和一个 return；模块自身直接调用底层入库时必须保持不重入 gate 的既有约定。

**退出条件**：host 可验证 target classification 与 event 去重；结构检查能说明所有真实 release 都经过唯一 dispatch；没有再依赖“某路径先 reset moving 因而恰好去重”。

## 8. 三方向实施工作包

### P5.4：扩展→扩展（第一条真机路径）

1. 先用原版对照实验记录同一逻辑袋内空槽重排、合并、非合并占用目标和交换的真实语义。当前 `move_extension_to_extension_locked` 对同袋行为的限制不能被猜测性绕过。
2. 根据证据选择明确的同袋规则：同一逻辑背包内同类可堆叠物品允许合并，非合并交换或拒绝均须有原版对照和独立纯模型断言；若需扩展事务模型，必须保留“逻辑-only、无 original 阶段”的 P4 不变量。
3. 跨扩展袋目标遵循容量、物品类别与整堆语义，禁止跨袋合并；原版→扩展同样属于跨域移动，不合并；满且不可合并时不创建 transactionId，源/目标/投影不变。

**通过条件**：空槽、合并、非合并、满目标、同袋语义、非法槽、取消和重复 release 均有单变量证据；无 `INVEN_MoveItem`、无 journal 写入、无 pending 残留。

### P5.5：原版→扩展（第二条真机路径）

1. 原版物品必须继续由原版 TouchHandle 建立拖动；P5 不为它另建扩展 source session。
2. 扩展标签 target 必须区分两种语义：背包装备物品的既有装备 drop，与普通原版物品移入扩展逻辑袋的 P4 orig→ext 事务。分类依据须来自控件/物品实证，不能凭 event 码单独判断。
3. 只通过既有 Save→逻辑更新→原版源实态复核的事务入口移入扩展袋；原版源删除不得只信返回值。

**通过条件**：正常、空/满目标、合并/不合并、任务袋源拒绝、取消和标签装备语义不回归分别留证；不得因为处理移动而让标签成为可拖动 source 或重复执行面板 release。

### P5.6：扩展→原版（第三条真机路径）

1. 由 P5.2 的扩展 source session 经 P5.3 的原版目标解析进入唯一 dispatch；验证 0x04、投影 0x02、release 与 `save_item_on_empty_gate` 的职责，而非并行触发它们。
2. 仅对原版袋 `0..4` 调用既有 `move_extension_to_original_locked`。`SAVE_LoadItem`、ledger 登记、`INVEN_SaveItemOnEmpty`、同锁 handover 和逻辑源清理必须保持 P4 顺序。
3. 目标满、插入失败或 slot-not-found 时，扩展源 payload 与逻辑状态保持，临时 module-owned 对象只处理一次，必要时写既有隔离记录。

**通过条件**：正常落空槽、合适的自动落位、目标满、任务袋目标拒绝、Load/插入失败、取消与 stale release 的每条证据均证明不重复、不丢失、不留下源槽残影或错误所有权。

## 9. P5.7：取消、跨视图与旧路径退役

- F3、切换原版/扩展袋、回主菜单、投影 restore、view generation 改变和不属于 0x17/0x18/0x19 的中断事件，均必须使 session 进入 `Cancelled`，撤销引用并且不创建新事务。
- 已进入 `TransactionInFlight` 的失败必须交给 P4 `txn_abort_locked`/隔离流程，不能由 UI 层自行清源或猜测回滚。
- 旧 capture/`ExtensionDrag` 路由只能在新路由对照通过、未出现双 dispatch 且其激活条件/行为已证实后退役；“有历史引用”或“当前难以复现”均不是删除依据。
- 退出后审计应表明 `outstanding_borrows=0`，不存在模块持有的 moving 控件引用；`inventory-owned` 从不由 cleanup 触碰。

## 10. P5.8：测试、静态检查与受限故障注入

### 10.1 host 与静态检查

在 `feature/extension_bag/model/virtual_bag_state.h` 的纯状态层新增可测试的 session token、view generation、target classification、单次 dispatch、terminal 幂等与同袋语义决策函数；`tests/test_host.cpp` 覆盖：

- 每个 session 状态转移、非法跳转、重复 release、stale generation 与取消；
- 任务袋 5、外部/内部袋号混用、无效槽、容量外槽和目标解析拒绝；
- 三方向调度只能选一个既有 P4 事务入口，ext→ext 永不引用原版移动；
- P4 payload、五态、所有权和隔离回归继续通过。

结构检查还须确认：没有新增裸 VMA；`extension_tab_item_proc` 不因 0x81/0x10 返回 1；`MOVING_CTRL` 清理受 session 生命周期保护；每个 release 路径恰好有一个 dispatcher。

### 10.2 故障注入边界

若 P5 需要触发 Load/插入失败，它必须是单次、可撤销、debug 构建专用、文档化且发布构建排除的测试 harness；通过既有只读诊断报告触发/结果。它不得成为正式移动 API、隐藏发布写入入口或持久化故障替身。

| 进程内 P5 故障 | P5 的预期 |
|---|---|
| 无效 payload/Load 失败 | 不近似重建；隔离原始字节；扩展源保持。 |
| 原版插入失败/找不到目标槽 | 临时对象按所有权规则处理一次；源、投影和 pending 恢复。 |
| 满/不可合并/容量外/任务袋 | `prepared` 前拒绝；无 payload/所有权/事务变更。 |
| 取消、过期或重复事件 | 不创建第二事务；只清该 session 的 transient 引用。 |

以下项目明确**不属于 P5**：原版保存失败、sidecar 写失败、journal stage 0/1/2、杀进程、重启、切档、跨进程隔离落盘和恢复裁决；全部转 P7。

## 11. P5.9：唯一真机串行证据矩阵

每一行都使用独立 `E-YYYY-MM-DD-NN`，包含主控 §5.1 的源码/APK/设备/配置、bag/projection、ownership/persistence、before/action/after、日志和用户结论。用户只确认物理触摸结果；执行代理记录 API/日志/host 结论。

| 顺序 | 单一变量 | 最低通过证据 |
|---:|---|---|
| 1 | P5.1 协议观测 | 原版 source 与扩展投影 source 的按下/移动/release/取消事件序列、返回值、moving 生命周期；不改行为。 |
| 2 | ext→ext 基本移动 | 扩展 source 到扩展空槽：token、hit target、P4 transaction 阶段、payload 和两端状态一致。 |
| 3 | ext→ext 边界 | 合并、非合并、满、同袋语义、取消、非法槽；每次只变一个条件。 |
| 4 | orig→ext 基本移动 | 原版真实 source 移入扩展目标；与“拖放背包装备到标签”分别对照。 |
| 5 | orig→ext 边界 | 满/合并/非合并、任务袋源、取消、标签语义不回归。 |
| 6 | ext→orig 基本移动 | 扩展 source 到原版 `0..4` 的真实落点，入库 handover 后模块不可访问。 |
| 7 | ext→orig 边界 | 满、任务袋目标、Load/插入失败 harness、取消/stale release；源保留或按事务提交。 |
| 8 | 跨视图/退出 | 拖动期 F3、切袋、投影 restore、回主菜单；terminal cleanup、零借用、无重复提交。 |
| 9 | 回归 | P4 host、P5 host、native 构建、符号/结构检查和三路径已通过证据均保留。 |

任一步失败，停止在该行，仅修复该路径；不得同时改变协议、事务、构建或设备条件。

### 11.1 P5 关闭结论（2026-09-02）

- `E-2026-09-02-01`/`02`：扩展→扩展基本空槽及扩展标签目标真实拖动通过。
- `E-2026-09-02-03`：满目标、非合并占用、空源、非法槽和任务袋目标 API 边界通过；Host/session 覆盖重复 release 幂等。
- `E-2026-09-02-04`/`05`/`06`：用户确认三方向真实拖动、同袋合并、跨袋不合并和非法目标取消通过。
- P5.1 的同身份观测确认 `0x10 → result=1 → MOVING_CTRL` 及 source/drop 生命周期；未观察到的 `0x81` 不作为运行时前置条件。
- P5.7–P5.9 的 session 清理、Host 回归、结构/构建回归沿上述实现和证据关闭；P5 不包含保存协调、跨进程恢复或 P8 发布条件。

**结论**：P5 已完成（`ACCEPTED`）。扩展源装备后的短暂原版背包闪现仍是独立的非阻断 UI 回归，不改变 P5 拖动事务结论；P7/P8 继续按主控文档执行。

## 12. 文件职责与退出门槛

| 文件 | P5 允许的变更 | 明确禁止 |
|---|---|---|
| `game_ui_virtbag.cpp` | session 生命周期、控件目标解析、单一拖动 dispatch、既有 P4 移动入口调用、日志与视图清理。 | 第二套事务/pending、猜测 ABI、裸 VMA、用坐标硬编码代替最终 target identity。 |
| `game_patch.cpp` | 仅在 P5.1 证实后调整 proc wrapper/gate 的唯一 owner。 | 多 route 重复提交、把未知 event 当 0x81 契约。 |
| `virtual_bag_state.h` / `tests/test_host.cpp` | 纯 session、分类、幂等、同袋语义与回归测试。 | native 指针、真实控件/库存断言、P7 持久化写入。 |
| `game_access.*` / `game_symbols.h` / `symbol_registry.h` | 逆向已经验证的 ABI 注册与类型化包装。 | 在调用点新增裸偏移或复制未验证 TouchState 结构。 |
| debug 测试代码 | 受限单次故障 harness 与只读诊断。 | 发布构建写入入口、正式移动 API 或持久化模拟。 |

P5 已满足以下关闭条件：同一身份下的实证 `0x10` 拖动协议与 `0x81` 未出现事实；单一 session/routing 无双 dispatch；三方向按顺序通过物理触摸矩阵；P4 不变量与任务袋 sentinel 无回归；host/构建/结构检查通过；P7 项明确保留为未完成。因此 P5 标记为 `ACCEPTED`，P7/P8 与 Overall 仍保持各自未完成状态。
