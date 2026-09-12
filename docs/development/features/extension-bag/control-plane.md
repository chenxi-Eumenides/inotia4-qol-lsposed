# 扩展背包控制面（Hub）

> **状态**：CURRENT（Hub）
>
> 本 Hub 只裁决扩展背包的范围、目标、阶段状态、未解决问题、文档路由和重大决策索引。
> 规则、结构、函数接入、交互、存档和验收分别由七册中的对应权威册维护；Hub 不复制实现正文。
>
> **事实基线**：commit `19dbc77`。问题 B 仍为“未解决+已布防”；当前确认使用
> `UIEquip_OKConfrimUseItem@0xb8478` 函数级 Hook、apply 分支已实现并真机验证通过、
> 问题 A 已修复、物理守卫已部署。

## §1 范围与目标

### 1.1 当前目标

扩展背包当前目标是：在不扩大原版物理背包和原版存档结构的前提下，维护五个扩展逻辑袋，
复用原版背包窗口、控件、物品效果和保存生命周期，并让扩展物品在查询、操作、拖动、入库和
持久化边界上具有可追踪的逻辑身份。

当前工作重点是 P7 全局库存接入：按对象身份统一查询、数量、消费、删除、装备、镶嵌、移动
和入库路径；同时逐 caller 审计生产者、失败释放、任务袋隔离和真机证据。P7 仍未整体验收。

S2 数量编码契约已批准并进入开发：可堆叠数量固定为 `128a+b` 高低位布局（总量
`0..999`，无版本标识、无迁移代码，模式经 `effective_clamp`/`effective_view_count`
影响运行时上限与关闭态视图）与关闭态语义（决策 b：低 7 位视图、`a` 保留）；子阶段
清单见 §3.5。S2 开发不改变 P7 与 Overall 的现有口径。

### 1.2 最终目标

最终交付必须同时满足以下边界：

1. 原版物理袋仍为 `0..5`，扩展逻辑袋对外为 `6..10`；不伪造额外物理袋。
2. 原版 UI、原版物品对象、原版效果函数和原版保存格式继续工作；扩展状态只在 sidecar
   保存，且以原版完整保存成功作为提交边界。
3. 扩展物品的 descriptor、payload、对象、handle、generation、ownership 和投影状态可
   互相核对；失败、取消、切档、重启和进程中断均有明确结论。
4. 容量、堆叠、三方向移动、自动入库、自动装备和满包提示共享同一逻辑契约，并由验收册
   给出逐操作证据。
5. 最终发布构建只保留批准的正式操作面，不以 debug 注入端点代替真实功能。

结构与依赖不要在本 Hub 展开，直接阅读 [`runtime-architecture.md`](runtime-architecture.md)。

### 1.3 非目标

- 不接管原版任务袋 `5` 的扩展移动、投影、恢复、回滚、自动入库或自动装备；任务袋仍由
  原版任务语义负责，原版只读扫描是否允许由具体函数契约决定。
- 不把扩展逻辑袋编码成原版物理槽，不改造 `FindItemSlot` 的既有 ABI，不让扩展对象进入
  原版物理释放或保存路径。
- 不为“统一”而 Hook 全部库存函数，不把单个 API、单个 caller 或 Host 通过写成全局覆盖。
- 不在 Hub 维护 VMA、逐函数矩阵、拖动状态机、sidecar 字段或 Host 测试断言；这些内容
  分别归属规则册、架构册、库存册、拖动册、存档册和验收册。

### 1.4 决策卡格式

涉及范围、阶段或跨册裁定时，使用下列格式登记，不在卡片中复制实现细节：

```text
决策卡：D-xx
日期：YYYY-MM-DD
问题：一句话描述冲突或选择
结论：一句话可执行裁定
影响域：规则 / 架构 / 库存 / 交互 / 存档 / 验收
证据：R-xx + host 测试名或真机用例号；无锚则写“无锚-待补”
未决：仍需验证的边界；无则写“无”
权威出处：七册文件 §节
```

卡片只记录“做什么和依据是什么”；代码解释、回滚顺序和日志格式必须回到对应分册。

## §2 权威地图

### 2.1 七册职责与权威域

| 册 | 文件 | 唯一职责 | 不负责的内容 |
|---|---|---|---|
| Hub | `control-plane.md` | 范围、最终目标、阶段状态、问题登记、路由、决策索引 | 实现细节和操作契约正文 |
| 规则册 | `rulebook.md` | 袋号、锁纪律、Hook 铁律、共享状态写者、`R-01..R-55` | 阶段结论和逐操作验收 |
| 架构册 | `runtime-architecture.md` | 代码层级、依赖、原版链与模块链、选型、替代方案、未知限制 | 规则编号和状态总览 |
| 库存册 | `inventory-integration-decision-plan.md` | 原版库存/物品函数逐函数 hook、旁路、保留和理由 | 拖动 session 与 sidecar 时序 |
| 拖动册 | `drag-protocol.md` | press/move/release、session、投影、drop owner、三方向事务和问题 B 取证 | 全局 Hook 清单和保存容器 |
| 存档册 | `module-save-store.md` | 保存 callsite、participant、sidecar、双 journal、恢复/回滚 | UI 事件和函数接入矩阵 |
| 验收册 | `verification-matrix.md` | 操作契约卡、Host 锚点、真机用例和验收结果 | 设计裁定与实现归属 |

### 2.2 改动路由：用户视角

每类工作先读 Hub，再读下列清单；每份清单最多三项，未列出的实现细节不得自行补写到 Hub。

#### 修 bug

1. `rulebook.md`：全量阅读，确认受影响的 `R-xx`、锁和对象所有权。
2. 受影响操作的 `verification-matrix.md` 契约卡：确认保持证据。
3. 按故障边界补读 `drag-protocol.md`、`module-save-store.md` 或 `inventory-integration-decision-plan.md`。

#### 增加操作

1. `runtime-architecture.md`：确认原版链、模块链和允许的接入层。
2. `inventory-integration-decision-plan.md`：确认函数处置、caller 覆盖和非目标边界。
3. `verification-matrix.md`：新增或更新对应操作契约卡及真机用例。

#### 修改 Hook

1. `rulebook.md`：Hook 铁律、original-first、三态返回和安装回滚。
2. `inventory-integration-decision-plan.md`：函数矩阵与 Hook/旁路裁定。
3. `runtime-architecture.md`：机制优先级、依赖方向和已知替代方案。

#### 修改存档

1. `module-save-store.md`：完整保存协调器、sidecar 和双 journal 语义。
2. `rulebook.md`：保存相关锁、状态写者和持久化禁止事项。
3. `verification-matrix.md`：保存成功、失败、切档、重启和恢复用例。

#### 编写验收

1. `verification-matrix.md`：操作契约卡和真机用例库是验收唯一正文。
2. `rulebook.md`：列出受影响的 `R-xx` 与可 grep 的证据锚点。
3. 本 Hub §3–§4：确认阶段状态和开放问题，不在 Hub 新写操作步骤。

## §3 阶段状态

### 3.1 P1–P7 状态表

| 阶段 | 当前状态 | 已保留的有效结论 | 仍不能据此推出 |
|---|---|---|---|
| P1 | ✅ 已完成 | 逻辑袋模型、容量派生、payload、所有权账本和 sidecar 原型已建立。 | 最终 v4-only 清理、所有替换/解除超容边界和全局运行时验收；当前桥接仍兼容读取 v2/v3/legacy，详见架构册 §5.3。 |
| P2 | ✅ 已完成 | 原版窗口投影 install/restore、容量快照和任务袋 sentinel 已形成阶段闭环。 | P3 控件行为、P5 拖动安全或问题 B 已通过。 |
| P3 | ✅ 已完成 | 原版控件树、标签、装备/解除、选中和全满边界已有真机记录；拖动转入 P5。 | 所有库存 caller、拖动异常和全局生产者已覆盖。 |
| P4 | ✅ 已完成并归档 | 原版对象与逻辑状态的进程内事务桥接、唯一事务入口、失败隔离和 ownership seam 已收口。 | 物理拖动的所有风险、跨进程恢复或问题 B 已解决。 |
| P5 | ✅ 核心路径已完成 | 三方向拖动、同袋合并、跨袋不合并、取消、session 清理和失败隔离已有阶段证据。 | 扩展↔扩展交换导致的问题 B；不得把逻辑提交等同于物理安全。 |
| P6 | ✅ 主保存协调已完成 | 保存 callsite→core coordinator→participant 的正常成功/失败边界已形成；sidecar 双 journal 语义已分立。 | 进程中断、coordinator 自动恢复和异常组合仍未全部关闭；详见存档册缺口。 |
| P7 | 🟡 进行中 | 阶段 0–3 静态审计完成，阶段 4 dispatcher/installer seam 已 Host 验证；线 A+B 与商店复制购买数量位段修复已完成；sidecar 读档已与运行时 clamp 分离，Host 已覆盖双向跨配置 canonical 数量保持；本轮 S2 四项修复（R-52 MakeItem 回写撤销、R-53 SaveItem 漏斗合并前置、R-54 ext→orig 宿主容量/显示袋守卫、R-55 原版背包详情出售 canonical 接管）与 R-51 位读重定向已落地，随 debug APK `52c5471e` 构建。本轮追加：R-56 `INVEN_RemoveItemData` 物理不足从扩展袋按类别补扣（H-21 扩展桥接，合成药水/宝石孔/混沌/传说扣料自动生效）、R-57 扩展视图原版袋列高亮遮蔽（商店/合成器袋列绘制 wrapper）、合成器（UIMix）扩展袋宿主（页签/投影/状态门控）、R-58 三个 UI 宿主页签挂载由扩展背包开关门控（背包/商店/合成器 install 入口门控 + 关闭态清除）。本轮追加多版本兼容：R-59 monster 物品数量上界由模块按特征码动态放行（`apply_monster_item_count_compat`，`cmp w8,#0x62`→`#0x7F`）、R-60 save gate 改为按符号 hook `SAVE_SaveInventory` 函数入口（不依赖调用点原指令字，修复 monster 上 `bridge_init` 失败导致注入链中止）。本轮追加拖放回归修复：R-61 扩展视图 `clear_stale_projected_moving_locked` 空闲态不再清原版 `MOVING_CTRL`（此前打断原版触摸状态机、`0x18` 释放事件不再产生、拖动几件后失效）、R-62 `ownership::kLedgerCapacity` 32→128 且 `handover_to_inventory` 交接即回收终态槽（此前池耗尽 → `destination materialize failed` → 拖动无结果）。 | 待真机双态（99/999）、VM-19 购买数量、VM-30 跨配置读档及 VM-22/VM-23/VM-37..VM-42 缺陷回归，以及 LSPosed 生产安装、阶段 5 生产者真机矩阵、全局库存覆盖和 P7 Overall。 |

### 3.2 当前总闸门

- Overall 仍为 **`NOT_ACCEPTED`**；P7 尚未完成，P8 最终回归尚未开始。
- P7 当前允许动作是继续逐函数/函数组验证，并按验收册建立操作契约；不得重复已经关闭的
  P1–P6 核心工作，也不得用 Host、Debug 构建或单次 API 成功替代真机证据。
- 阶段状态只在有证据 ID、源码快照和明确范围时前进；失败记录不得被后续成功记录静默覆盖。

### 3.3 当前已确认状态

1. 文档事实基线为 commit `19dbc77`；`UIEquip_OKConfrimUseItem@0xb8478` 使用
   LSPosed Native Hook 和 original-first 路径，apply 分支已真机验证通过。
2. 视图切换收尾刷新按当前投影袋身份处理，结构性说明见架构册，规则见规则册。
3. 当前防御包括 `0x18` 单一 drop owner、`moveMergeEnabled` 与扩展源保护解耦、
   物理 `0..5 × 16` 快照 pre/post 与恢复日志；这些防御不构成问题 B 修复证明。
4. 吞掉 `0x18` 的五个出口执行原版等价 moving/selected 清理，事件 `0x08` 和 selected
   reset 均放锁后执行；VM-28 收尾变体正常。
5. H-16 承担刷新后投影，moving 六条件保留门和 stale moving flags 清理已落地；原版四象限
   VM-B01～B04 正常。
6. 页签优先解析、触摸窗口延迟释放队列和 H-16/定点 sync 刷新职责已由真机确认；扩展源→空
   页签装备、扩展源→其它页签移动、原版→页签对照和同袋 apply 分支均正常。apply 分支覆盖
   宝石镶嵌与强化卷轴强化，失败为 `Blocked` 且不降级 swap；Overall 仍保持 `NOT_ACCEPTED`。
7. 任务袋 `5` 作为当前原版来源时，页签切换到扩展袋使用合法原版投影宿主 `0..4`；任务袋
   仍不是扩展物品操作源/目标或投影窗口。实现与验收归 `verification-matrix.md` 的 VM-31，
   Overall 仍保持 `NOT_ACCEPTED`。
8. 持锁原版回调已统一经 TLS original-only 查询分支；跨域原版删除先放锁调用、重新加锁后
   复核源槽为空（规则 R-44；受控入口清单见架构册 §5.4）。自死锁修复已随 debug APK
   `b4a66375c14f` 部署到真机；VM-15 的 API 跨域移动回归提交与回滚均保持 health 可达；
   VM-15 拖拽路径（orig→ext 源槽后置条件 + 无卡死）真机复测待用户执行，Overall 仍保持
   `NOT_ACCEPTED`。
9. 本轮 S2 缺陷修复已随 debug APK `52c5471e` 构建（构建产物身份，不代表真机通过）：
   R-51 出售/拆堆直接位读重定向统一 getter（5 条；装备页详情结算点 `0x1261c4` 不在表内）；R-52 `ITEMSYSTEM_MakeItem` 数量回写撤销（arg2 非数量）；R-53 `INVEN_SaveItem`
   漏斗入库前先并入扩展同类堆；R-54 ext→orig 目标袋为投影宿主时恢复真实容量且不切换
   显示袋；R-55 原版背包详情出售由 H-23 按 canonical 全量接管。对应 Host 断言已落地，
   行为断言待真机 VM-37..VM-41，Overall 仍保持 `NOT_ACCEPTED`。
10. 佣兵徽章使用（R-63）：新增第 16 个常驻 Native Hook H-24
   `UIEquip_ButtonUseMercenarySealExe@0xb8144`，扩展侧经 `extension_use_mercenary_seal`
   按原版顺序 `UIDesc_SetOff → SAVE_IsOK → IsEmptyManagerSlot → MERCENARYSYSTEM_MakeMercenary@0x119658`
   接管，消耗经 H-04 `INVEN_RemoveItem` 承接；API `data_op_use_item` /
   `extension_bag_api_use_item_impl` 对 `ITEMSYSTEM_IsMercenarySeal`（category 42..50 / 928..933）
   豁免 `fn_is_use` 并走 `MakeMercenary`。VM-47（扩展袋 UI）2026-09-12 真机确认通过
   （日志 `extension mercenary-seal handled bag=0 slot=8 used=1`）；VM-48（API）待补。
   Overall 仍保持 `NOT_ACCEPTED`。

### 3.4 阶段证据索引

阶段表只给状态，不复制证据块；需要核对时按下列入口读取原始记录：

| 范围 | 当前证据入口 | 使用限制 |
|---|---|---|
| P1–P3 | 对应阶段证据 ID、阶段册和现行分册记录 | 只支持已列范围的阶段结论，不支持 P7 全局覆盖。 |
| P4 | `E-2026-08-31-04` 至 `E-2026-09-01-01` 及 P4 归档记录 | 只证明进程内事务桥接与失败隔离。 |
| P5 | `E-2026-09-02-01` 至 `E-2026-09-02-06` 与拖动册 §7 | 问题 B 独立于 P5 核心路径，不能被“拖动通过”覆盖。 |
| P6 | 存档册 §3、§4 和既有保存闭环记录 | 正常保存协调已闭合，自动恢复缺口仍按存档册登记。 |
| P7 | 阶段 0–3 静态记录、阶段 4 Host seam 记录和验收册 | 未有 LSPosed/生产者真机证据前，不得推进为 Overall 通过。 |

阶段证据发生冲突时，保留较严格的失败或未知结论；不能以较新的文档日期自动替换较弱证据。
证据 ID、构建身份和真机用例的完整字段由验收册维护，Hub 只保存阶段指针和裁决结果。

### 3.5 S2 数量编码子阶段（开发中）

S2 子阶段编号独立于全局 P1–P7；各子阶段在真机证据落地前不得推进状态。

| 子阶段 | 内容 | 当前状态 |
|---|---|---|
| S2-P0 | 契约冻结：规则 R-45..R-49、架构 hook 分层（§3.7）、存档数量语义（存档册 §6.4）、API 字段语义 | 🟡 文档已落地；S2 专属 VM 卡 VM-32..VM-41 已登记（缺陷修复 VM-37..VM-41 待取证） |
| S2-P1 | ~~sidecar `encoding_version` 字段与 S1→S2 迁移~~ 已裁撤：无版本标识、统一 S2、旧字段宽容忽略（R-48） | 已裁撤 |
| S2-P2 | 读侧统一 getter hook：`ITEM_GetCumulateCount@0x106094` | 🟡 代码已落地（Host `test_get_cumulate_count_s2`）；真机证据待取证 |
| S2-P3 | 写侧调用方类别门控与 `128a+b` 进位/借位 | 🟡 代码已落地（H-18..H-21 + 模块写点，Host codec/门控断言）；真机证据待取证 |
| S2-P4 | ~~原版袋内 `+0x10` 一次性迁移扫描~~ 已裁撤：无迁移代码，`+0x10` 由读侧 hook 与写侧调用方级写点按 S2 读写 | 已裁撤 |
| S2-P5 | 关闭态（决策 b：低 7 位视图、`a` 保留）语义落地 | 🟡 代码已落地（`stack_codec::effective_*` 模式感知层，Host `test_stack_codec_effective_mode`/`test_virtual_bag_mode_aware_ops`）；真机证据 `VM-36` 待取证 |
| S2-P6 | S2 真机回归：补齐 S2 专属 VM 卡并验收 | 未开始 |

S2 完成（含 S2-P6 验收）不改变 Overall `NOT_ACCEPTED` 口径；S2 各子阶段的验收证据按
验收册操作契约卡登记，Hub 不复制证据正文。

## §4 未解决问题登记

### 4.1 问题 B：扩展↔扩展 swap 的原版同号槽丢失

| 字段 | 登记 |
|---|---|
| 症状 | 扩展袋 0 的 a↔b 双非空交换看似成功后，原版袋 0 同号 b 格物品消失；切换视图、重新打开和保存后仍消失，表现为物理库存损失并被保存固化。 |
| 当前防御 | drop 三态门、swap rollback、handle/generation/ownership 校验、`moveMergeEnabled` 解耦、`0x18` 单一 owner、物理快照守卫和 pre/post 插桩。 |
| 当前判定 | **难解搁置（未解决+已布防）**。防线降低破坏面，但用户复测仍复现；不得写成“已修复”或“已定位根因”。 |
| 取证 | 按 [`verification-matrix.md §4.1`](verification-matrix.md#41-vm-12-复现步骤) 保留 pre/post digest、mutation、rollback、projection sync 及同一时间窗的事务状态。 |
| 证据缺口 | H3/H4 复测尚未采集完整 `orig_event pre/post` 与 `ERROR physical inventory mutation`；是否触发、首个 digest 改变点均未知。 |
| 规则关联 | `R-20`（物理快照守卫）；复现和取证以 `verification-matrix.md` 的 VM-12/H3/H4 为准。 |

### 4.2 两条当前 backlog 项

- **扩展物品全局裸指针反查 generation 化**：`render` 仍有裸指针反查窗口，见
  [`backlog.md`](../../planning/backlog.md) 的扩展背包待办条目。
- **确认使用 token mismatch 后 active 槽滞留观察**：先裁决不释放未知对象的隔离/清理策略，
  见 [`backlog.md`](../../planning/backlog.md) 的扩展背包待办条目。

### 4.3 审计缺口

- **a）已实现、真机验证通过**：同一扩展逻辑袋内的扩展宝石/强化卷轴→扩展装备走
  `UIEquip_IsApplyStuff@0xb8d4c`、`SAVE_IsOK@0x128c14`、`UIEquip_ApplyStuff@0xb8df8`；
  原版 `PutJewel`/`EnchantItem` 分支和既有 `ConsumeItem` Hook 承接材料消费，失败为
  `Blocked` 且不降级 swap。对应 `S-03`/`VM-10`，提交 `19dbc77`。
- **b）已接入、真机验证通过**：扩展 apply 材料拖到装备槽使用
  `UIEquip_EquipControlEventProc@0xb8f7c` 第 15 个 Native Hook；宝石和强化卷轴均进入
  token/session 校验，失败 Blocked，不降级原版。源码以
  `native_inventory_hook.cpp:equip_control_event_proc_wrapper` 为准。
- **c）当前缺口**：`data_op_equip` 拒绝逻辑袋 6..10，与 UI 详情装备路径口径不一致；该缺口
  不影响已验证的同袋 apply 分支。出处：审计报告。

## §5 决策记录索引

以下只保留重大裁定的一行索引；详细理由、替代方案和实现边界以出处册为准。

| 日期 | 裁定 | 出处 |
|---|---|---|
| 2026-08-26 | 不扩大原版物理袋数量，扩展逻辑袋投影到原版窗口。 | `runtime-architecture.md` §3.4 |
| 2026-08-26 | 原版任务袋 `5` 不接管扩展事务；扩展普通目标限于原版 `0..4`。 | `rulebook.md` §1.1；`inventory-integration-decision-plan.md` §4.2 |
| 2026-08-26 | sidecar 只保存序列化 payload，不保存 native 指针；原版保存成功后才提交。 | `module-save-store.md` §1、§5 |
| 2026-08-27 | 保存采用 participant 协调器，不把 `SAVE_*` 函数本体整体 Hook 化。 | `module-save-store.md` §2–§3；`runtime-architecture.md` §3.5 |
| 2026-08-27 | 扩展数据/移动并入原版库存 API；视图控制能力与正式操作面分开。 | `runtime-architecture.md` §1.5；`inventory-integration-decision-plan.md` §3 |
| 2026-09-06 | 确认使用由全局 OK 槽劫持改为 `0xb8478` 函数级 LSPosed Hook，original-first。 | `runtime-architecture.md` §3.1；`inventory-integration-decision-plan.md` §3.2 |
| 2026-09-06 | `FindItemSlot` 保持原版物理 ABI，不做全局扩展接入；扩展在上层按逻辑身份分流。 | `runtime-architecture.md` §3.2；`inventory-integration-decision-plan.md` §2.1、§3.2 |
| 2026-09-06 | `moveMergeEnabled` 只控制原版合并语义，与扩展源保护和扩展事务解耦。 | `drag-protocol.md` §1.1、§3.3 |
| 2026-09-06 | 投影 `0x18` 采用单一 drop owner；扩展事务成功或拒绝后均阻止原版二次移动。 | `drag-protocol.md` §2.2–§4 |
| 2026-09-06 | `module.save.journal` 与 `extensionbags.journal` 双 journal 分立，不按名称合并。 | `module-save-store.md` §4 |
| 2026-09-06 | 确认使用采用 token/generation/owner 机制，active 与 pending-release 对象不得猜测性释放。 | `inventory-integration-decision-plan.md` §6；`rulebook.md` §4 |
| 2026-09-06 | 双非空 ext↔ext swap 是原子事务，descriptor、object、hash、handle、generation 和 ownership 成组交换。 | `drag-protocol.md` §2.4；`rulebook.md` §5 R-16 |
| 2026-09-08 | 当前代码保留 H-13 `SaveItem` wrapper；禁止扩大到 Direct/Data/所有保存辅助，caller 覆盖仍未验收。 | `inventory-integration-decision-plan.md` §2.2、§3.2；`runtime-architecture.md` §3.6 |
| 2026-09-08 | 正式库存操作面使用 `/api/item/inventory*`；`/api/debug/extension_bag/*` 仅作视图控制开发面。 | `verification-matrix.md` §1.1；`docs/reference/api-reference.md` |
| 2026-09-08 | `INVEN_MoveItem` 函数层接管采用方案 A：常驻 Native Hook 仅拦扩展身份源；原版对象全量 original-first backup 并取证。方案 B 因四参不足以还原扩展意图，且与 `0x18` owner 存在双提交风险而不采用。 | `inventory-integration-decision-plan.md` §2.2；`drag-protocol.md` §2.5、§4；`verification-matrix.md` VM-27 |
| 2026-09-08 | `INVEN_MoveItem` guard 的身份、view/session、caller 和物理槽摘要必须在锁内采集，调用 backup 前放锁；扩展命中记录 ERROR 并跳过 backup，问题 B 仍保持“未解决+已布防”。 | `rulebook.md` R-31；`native_inventory_hook.cpp:226-273`；`verification-matrix.md` VM-27 |
| 2026-09-09 | 第 15 个 Native Hook 接入 `UIEquip_EquipControlEventProc@0xb8f7c`：扩展 apply 材料源（宝石/强化卷轴）通过 descriptor/generation/session 与 token 校验后放锁调用原版 proc，`PutJewel`/`EnchantItem`/`ConsumeItem` 链承接镶嵌、强化和消费；非扩展源/非 apply 材料 original-first，校验失败 Blocked。 | `rulebook.md` R-02、R-09、R-15、R-16、R-31；`inventory-integration-decision-plan.md` §2.3/§2.4；`verification-matrix.md` VM-10 |
| 2026-09-10 | apply 分支落地：同一扩展逻辑袋内宝石/强化卷轴→装备经 `UIEquip_IsApplyStuff`、`SAVE_IsOK` 和原版 `UIEquip_ApplyStuff` 路由，`PutJewel`/`EnchantItem` 与 `ConsumeItem` 承接成功消费；失败为 Blocked，不降级 swap；真机验证通过。 | 提交 `19dbc77`；`rulebook.md` R-02、R-09、R-15、R-16、R-31；`verification-matrix.md` S-03、VM-10 |
| 当前 | 刷新使用 H-16 函数级关卡：原版一次刷新锁内 trampoline 后覆盖投影，模块内部走 raw-original dispatcher，restore 期间抑制投影；moving 门和定点 sync 负责无刷新事务。 | `rulebook.md` R-32；`drag-protocol.md` §2.5、§2.7；`verification-matrix.md` VM-B |
| 当前 | draw-end 不写帧级 projection；H-16 保留 post-projection，无刷新事务使用定点 `sync_projected_slot`；moving 仅六条件全真时保留，失效 moving 清理 TouchState 与控件 flags。 | `rulebook.md` R-34；`drag-protocol.md` §2.7；`verification-matrix.md` VM-B01～VM-B04 |
| 当前 | 页签优先解析、触摸窗口延迟释放队列、H-16/定点 sync 刷新职责和同袋 apply 分支已由真机确认；扩展源→空页签装备、扩展源→其它页签移动、原版→页签对照及宝石/强化卷轴→装备均正常。 | `rulebook.md` R-15、R-34～R-36；`drag-protocol.md` §2.2.1、§2.9、§4、§5；`verification-matrix.md` S-03、S-05、VM-10、VM-29、VM-B01～B04 |
| 2026-09-10 | 任务袋 `5` 作为当前来源允许切换到扩展页签；投影前选择原版 `0..4` 宿主并保持任务袋不进入投影/事务目标，扩展→任务袋仍只允许视图切换。 | `rulebook.md` R-19、R-26、R-27、R-30；`drag-protocol.md` §2.2.1；`verification-matrix.md` VM-31、VM-B04 |
| 当前 | 线 A+B 的数量路径已复核；`ITEM_IsRealEquip/ITEM_IsRealBroken` 四处非数量 patch 已撤销，`UIStore_BuyItem+0x1a8` 保持；当前待真机双态（99/999）及装备显示/详情回归。 | `runtime-architecture.md` §2.1.1；`rulebook.md` R-38、R-43；`verification-matrix.md` VM-30 |
| 2026-09-10 | sidecar 读档的 canonical 数量固定为 `0..999` 语义；不按当前堆叠配置截断已有值，可堆叠 payload 仅在与 descriptor 不一致时同步，越界值收敛到 999 并记录日志。 | `rulebook.md` R-38、R-40、R-41；`module-save-store.md` §6.1；`verification-matrix.md` VM-30；Host `test_virtual_bag_json_count_clamp` |
| 当前 | 持锁原版删除/刷新统一使用 raw-original dispatcher 与 TLS original-only 查询；删除后重新读取物理槽确认清空后才提交跨域事务。 | `rulebook.md` R-18、R-44；`verification-matrix.md` VM-15、VM-B01～B04；Host `test_original_only_queries` |
| 当前 | S2 数量编码（统一布局，无版本标识）：可堆叠数量按 `a=bits22-24`+`b=bits25-31`（`count=128a+b`，业务上限 999）读写，布局层 `s2_read_count`/`s2_write_count` 与模式无关，运行时操作面统一经模式感知层 `effective_read_count`/`effective_write_count`/`effective_clamp`/`effective_view_count`；读侧统一 `ITEM_GetCumulateCount@0x106094` getter hook（模式视图解码），写侧调用方级门控+进位，不 hook `UTIL_SetBitValue`（无物品指针无法判类别）；关闭态（决策 b）所有读写与操作按低 7 位视图（上限 99）、`a` 保留不动、重开读回完整 canonical；sidecar 不携带版本字段，历史 `encodingVersion` 宽容忽略，无迁移代码。子阶段见 §3.5。 | `rulebook.md` R-45..R-49；`runtime-architecture.md` §3.7；`module-save-store.md` §6.4；`api-reference.md` §7.6；Hub §3.5 |
| 当前 | S2 缺陷修复（debug APK `52c5471e`）：R-51 出售/拆堆直接位读重定向统一 getter（5 条）；R-52 `ITEMSYSTEM_MakeItem` 数量回写撤销（arg2 非数量）；R-53 `INVEN_SaveItem` 漏斗入库前先并入扩展同类堆；R-54 ext→orig 宿主容量字 RAII 恢复与显示袋守卫；R-55 原版背包详情出售由 H-23 按 canonical 全量接管（预演只回填金额）。装备页详情结算点 `0x1261c4` 重定向已回退不入表。 | `rulebook.md` R-51..R-55；`runtime-architecture.md` §3.7；`verification-matrix.md` VM-37..VM-41；Hub §3.3 |
| 2026-09-11 | 合成器（UIMix）扩展袋支持：新增 `extension_bag_mix.inc` 宿主（ENTER/F3 回调 + draw_end BL patch + 自建页签 + 网格投影，state!=0 才挂载）；R-56 H-21 `INVEN_RemoveItemData` 物理不足按 category 从扩展袋补扣（合成药水/宝石孔/混沌/传说扣料自动生效，宝石强化走 H-05）；R-57 扩展视图原版袋列高亮遮蔽（商店/合成器袋列绘制 wrapper，扩展页既有）。 | `rulebook.md` R-56、R-57；`runtime-architecture.md` §2.1、§2.5.1、§2.6；`verification-matrix.md` VM-22、VM-23；Host `test_remove_data_extension_shortfall` |

### 5.1 决策索引使用规则

- 索引中的日期是裁定日期，不代表该裁定覆盖了所有运行时 caller。
- “已采用机制”只表示设计或代码路径已有依据；是否通过真机，必须看验收册对应操作卡。
- “双 journal 分立”不表示普通内存 pending 已经逐笔落盘；持久化事实以存档册当前实现为准。
- “原子 swap”只描述扩展逻辑事务的提交/回滚边界，不得作为问题 B 的物理安全证明。
- 新裁定若改变既有索引，必须保留既有条目的出处和冲突说明。
- 决策卡与 `R-xx` 是交叉引用，不是新的权威域；同一规则只在规则册保留正文。
- 若出处册正在并行修改，Hub 只引用稳定的文件名和章节，不复制未冻结段落。
- 发现跨册口径不一致时，先在 Hub 或交接记录登记，再由权威册所有者修订正文。
- 任何“完成”字样都必须带范围，例如“P6 主保存协调已完成”，不能省略未关闭的异常边界。

### 5.2 经验记录（思考，非事实）

> 本节是经验与思考，不属于事实章；每条不构成新的权威裁定，落地时必须回对应分册按
> §1.4 决策卡格式登记。背景：orig→ext 事务持锁调用原版删除导致同线程自死锁（R-44）。

1. 设计前提「原版函数不会回调我们的 Hook」不成立：原版函数（如 `INVEN_RemoveItemDirect`
   经 `PLAYER_UpdateShortcut` 触发 `GetItemCount`）可能同步回调已 Hook 地址；任何持锁
   原版调用都必须默认 Hook 可重入。
2. 约束只写在代码注释里不足以约束实现：注释会被绕过或随重构失效；需要机械校验兜底
   （持锁区间 `fn_*` 原版调用按 R-44 受控入口白名单扫描 + Debug 断言），当前尚未建立，
   属待办方向。
3. Host 测试只能验证 original-only 直通 seam 的纯逻辑（`test_original_only_queries`），
   无法复现原版内部回调链的重入；此类死锁风险必须真机复现并按
   [`build-and-deploy.md` §3.2](../../../guides/build-and-deploy.md) 取线程栈，Host 通过
   不构成无死锁证据。
4. 涉及锁的行为面提交前，先把「卡死取证流程」固化到部署文档；下一次同类问题可直接按
   步骤取线程栈，缩短定位时间。
5. 被证伪并回退的旧方案（原版详情出售结算点重定向）：曾计划把 `0x1261c4` 的
   `0x1261fc`/`0x126208` 内联读重定向到 `ITEM_GetCumulateCount`（getter）。该方案在
   真机暴露缺陷——原版紧随其后的 clamp 是 `N=(b∈[1,99])?b:1`，重定向只改了读指令、
   没有改后续 clamp，于是 getter 返回的 canonical `>99` 会被 clamp 成 `1` 并触发
   异常/卡死；且该点与商店/拆堆读点的寄存器约定不同（物品指针在 `x21`）。结论：该点
   重定向已回退、不入 `g_stack_getter_redirect_patches`，改由 H-23
   `UIEquip_OKDestroyItem` 在回调层整体接管（R-55）。教训：重定向单个读点前必须
   连同其后的 clamp/分支一并建模，不能只替换 `bl` 目标。

## §6 阅读顺序与维护规则

### 6.1 新代理进场顺序

1. 先读本 Hub，确认 Overall、当前阶段、开放问题和下一允许动作。
2. 再按 §2 的用户视角路由读取规则册与受影响分册；涉及操作的工作必须再读对应验收契约卡。
3. 修改前核对当前工作树、基线 commit、源码锚点和现有证据；完成后只更新所属权威域。

### 6.2 维护契约

- 权威域不可重复：状态只改 Hub；不变式只改规则册；结构只改架构册；函数决策只改库存册；
  交互只改拖动册；持久化只改存档册；验收只改验收册。
- 扩展背包改动说明必须引用受影响的 `R-xx`，并为每条列出保持证据：Host 测试名与文件行，
  或真机用例号；没有证据必须写“无锚-待补”。若影响操作流程，还必须引用操作契约卡。
- 源码行号不是永久 ID；代码变更后必须重新核对链接、文件:行和可 grep 锚点。问题 B 的“已布防”
  不得被 Host 事务通过或 UI 现象改写为“已修复”。
- `verification-matrix.md` 正在并行编写；本 Hub 直接按该文件名引用，不在此复制未冻结的验收卡。

### 6.3 文档事实规则

1. 七册事实章只写当前状态、当前证据、当前限制和当前未决项。
2. 描述此前状态、变更过程、取代关系、防御时间线或失败过程的内容不进入事实章；需要保留
   的决策理由放在独立的决策/经验章，并明确标注为思考。
3. 纯历史过程只保留在 `docs/history/` 归档区；七册不复制归档内容，也不建立历史迁移清单。
4. 更新事实章时按当前源码和当前证据核对文件:行；无法从当前材料确认的内容登记为当前未决或
   证据缺口，不用历史材料补写当前事实。

### 6.4 最小维护检查

每次更新本 Hub 前必须回答以下问题：

1. 这次变化是否属于状态、开放问题、路由或决策索引；若不是，应修改对应分册而不是 Hub。
2. 是否保留了“未真机验证不等于完成”和“问题 B 未解决+已布防”的边界文字。
3. 新增裁定是否有日期、权威出处、`R-xx` 和保持证据；缺失项必须标为“无锚-待补”。
4. 是否把非当前权威材料写入事实章，或把分册结论复制成第二份正文。
5. 修改后执行 `git diff --check`，并确认本次变更未产生 Hub 之外的文件修改。

### 6.5 审查模板强制项

任何代码审查请求必须附带一节 **“意外影响走查”**，不得只回答修复闭环。该节至少包含：

1. 按 diff 触及面列出全部入口清单：event、proc、Hook 安装/卸载、原版 backup、扩展
   分流、物理槽读写、投影刷新、保存与生产者 caller；未触及项也要明确写“未触及”。
2. 审查者对清单逐项确认：非扩展物品的原版路径、输入语义、成功/失败返回、物理槽和
   原版副作用均为零变化；对应项必须引用 B-xx/R-xx 与 VM-B/受影响 VM 卡。
3. 明确列出当前新增或改变的例外条件（例如 C1 只拒绝扩展 source、H4 只拦扩展身份），
   并确认不会把原版对象误判为扩展对象。
4. 给出真机日志证据或无法执行原因；缺少行为面证据时结论只能是 `NOT_ACCEPTED`。

真机回归必须同时完成“意外影响走查”：修复闭环和原版基线行为均需逐入口确认。
