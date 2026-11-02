# `extension_bag` 目标架构（v3）

状态：**目标/设计稿，不代表当前实现已完成**。本文只描述所有权边界与目标形态；当前实现见
[`final-architecture.md`](final-architecture.md) 与 [`extension_bag` 参考](../../reference/plugins/extension_bag.md)。
文中出现的新 ABI/能力只表示目标所需，均需 Gate/versioning 决策，**不得写成已批准或已实现**。

## 1. 所有权总则

`core.item` 是物品游戏规则与动作编排的 owner；`extension_bag`（EB）是扩展袋数据、对象生命期与
UI 投影的 owner。两者不是同一层：

- **`core.item` 决定“做什么、是否允许、目标是谁”**：物品事实与类型、使用效果与前置规则、
  移动/合并/交换策略、商店购买与扩展袋卸下的原版优先接纳规则。
- **EB 决定“扩展袋数据如何被安全承载”**：sidecar/provider 数据原语、对象物化与 custody/use lease、
  UI/绘制/投影适配、扩展背包自身的装备状态转换。

EB **不保留普通游戏操作决策**。任何“该不该合并/交换”“该落到哪个原版袋”“使用是否满足前置”
的判断都属于 `core.item`；EB 只执行数据原语、维护自身状态不变量，并把失败原样上报。

## 2. 职责边界

### 2.1 `core.item` 持有

| 职责 | 说明 |
|---|---|
| 物品事实与类型 | `item.facts`/`item_type`、装备/宝石/强化卷/可用/售价等事实 |
| 使用效果与前置 | 是否可用、类别分派、冷却、礼盒/佣兵封印/开箱等效果与前置判断 |
| 移动/合并/交换策略 | 是否合并、交换哪两个对象、冲突裁决、跨空间所有权转移与编排 |
| 商店购买接纳规则 | 原版袋优先的接纳顺序与失败后的目标选择 |
| 扩展袋卸下接纳规则 | 原版袋优先的接收袋顺序，以及原版全满后的目标策略 |
| 原版/任务/C18 袋 | 原版 0–5 与 C18 追加袋的读写、装备/镶嵌、出售、入库 |
| 对外操作面 | HTTP、票据、库存快照与跨线程查询 |

### 2.2 EB 保留

| 职责 | 说明 |
|---|---|
| `QolBagSource` sidecar/provider 数据原语 | `count/info/item_at/for_each_item/export_item/import_item/remove/identify` 及后续版本化 provider mutation；只执行数据读写与不变量校验 |
| 对象物化/custody/use lease | descriptor → 原生对象、handle/ownership ledger、use lease 与延迟释放；lease 只管理对象生命期，不判断使用效果 |
| UI/绘制/投影适配 | 页签、布局、reflow、显示读取虚拟化、装备页/商店/合成/详情投影；投影是副作用适配器，不是数据源 |
| 扩展背包自身装备状态转换 | 袋作为装备的 marker、sidecar 的 types/capacities/equipment 状态、容量与持久化状态转换 |
| 扩展袋数据不变量 | 容量/槽数、payload 合法性、扩展槽 assignability、sidecar 原子提交 |

### 2.3 决策与状态归属

| 事项 | 决策 owner | EB 的角色 |
|---|---|---|
| 扩展袋间 merge/swap 是否发生、目标与冲突策略 | `core.item` | 只在校验数据不变量后原子提交 sidecar 状态 |
| 物品 use 的效果与前置 | `core.item` | `use` lease 只保证对象在调用期间存活、不可被复用 |
| 商店购买的原版优先接纳 | `core.item` | 提供 marker/sidecar/ownership/equipment 状态与扩展袋 assignability |
| 扩展袋卸下的原版优先接纳 | `core.item` | 执行 EB 自身装备状态转换，提供 marker/sidecar/ownership/equipment 状态 |
| 扩展槽是否可分配 | EB | 作为 provider 数据不变量拒绝非法目标 |
| 扩展对象物化/释放 | EB | 维护 custody 与 lease |

## 3. provider mutation 与 `to.slot=-1`

### 3.1 版本化 provider mutation

目标是让 `core.item` 把已决定的操作（如扩展袋间 merge/swap 的源、目标与语义）交给 EB，
EB 在一个**版本化 provider mutation capability** 内校验并原子提交 sidecar 状态：

- 一次 capability 调用只提交一次 sidecar 变更；失败不得留下半提交状态。
- EB 不保存“为什么合并/为什么交换”的策略，也不接管跨空间 journal。
- 该 capability 目前**不存在**；新增或扩展它需要 Gate/versioning 与契约评审，尚未批准、尚未实现。

**不设计通用事务框架**：不新增跨插件共享事务、undo log 或作业调度器。`core.item` 继续拥有动作编排，
EB 的提交只是扩展袋 sidecar 的本地原子操作。

### 3.2 `to.slot=-1` 不得替代 EB assignability

`core.item` 当前可以用 `to_slot=-1` 表示“由接收方选择空槽”，并通过 `item_at` 找到第一个空槽。
这只能观察“槽里没有物品”，**弱于** EB 的 assignability：

- EB assignability 同时要求槽位在该袋派生容量内，且 descriptor、原生对象、handle、
  active use 与 pending release 全部为空态。
- 因此 `to.slot=-1` 只是 `core.item` 的目标选择便利，不是对 EB 槽位的授权；更不得用它绕过
  EB 对容量外槽、在途对象或 use lease 的拒绝。
- 目标契约：EB provider mutation 始终以自己的 assignability 为准；自动选槽被拒时返回冲突，
  由 `core.item` 决定重试、改目标或中止。若 `core.item` 需要在不复制 EB 状态的前提下选择合法槽，
  契约需提供 placement/assignability 查询或返回实际落点；新增成员同样需要 Gate/versioning。

## 4. 线程模型

保持现有单线程状态与快照读取架构：

- `QolBagSource` 回调、provider mutation、UI/显示 hook 与绘制均在游戏主线程。
- 业务状态只在主线程读写；跨线程查询只读 `core.item` 已发布的不可变快照，不触碰 EB live State。
- EB 不提供 HTTP 查询或写命令；对外库存查询由 `core.item` 快照承担。
- `EntryGuard` 只承担同线程重入守卫，不作为跨线程互斥量。
- provider mutation 保持主线程同步契约；不引入 EB 后台任务或跨线程提交。

## 5. 投影架构

保持现有投影边界，投影只是 UI 副作用适配器：

- 装备页、商店、合成与详情页通过 `ControlItem_SetItem/GetItem`、`INVEN_GetBagSize` 虚拟化读取；
  **不写 `INVEN_pItem` 或容量字**。
- 绘制只读快照/mode，不改业务状态；临时 `current_bag` 的写入/恢复由投影控制器负责。
- 恢复时在抑制范围内调用原版刷新，让控件一次性读回真实槽表。
- 布局/来源/控件身份无法确认时 fail-closed，不替换非投影控件的物品。
- C18 追加袋的 child index 只用于 UI 命中，不进入扩展逻辑 ID；页签 reflow 与命中属于 EB UI 适配。
- 投影失败不得改变原版选择，也不得成为数据写入或所有权转移的入口。

## 6. 数据与持久化

- 扩展袋状态仍由 sidecar `extensionbags.items` v5 承载；`bagCount` 只由设置页修改。
- `types/capacities/items/bagCount/mode/selection` 与对象 custody/ledger 由 EB 维护；
  `persist_state_locked()` 只表示内存 mutation 被接受，不是持久化确认。`save.slot_data` capture
  仅提供待保存快照；只有完整 core.save 保存事务成功且收到对应成功 complete，才确认该 revision。
- 物化对象、handle 与 use lease 只在 provider/UI 需要原生对象时存在；descriptor 是扩展袋数据的权威。
- EB 不拥有跨空间 journal；跨空间移动与所有权转移由 `core.item` 负责。
- 原版/C18 袋始终以游戏数据为准；扩展袋以 sidecar 为准。

## 7. 当前未迁移

以下路径仍是**当前实现**，与本文目标不一致；列出它们是迁移输入，不代表目标已落地：

| 未迁移项 | 当前实现 | 目标归属 |
|---|---|---|
| EB `use` 效果/前置 | `api/api_impl.cpp` 的 `extension_bag_api_use_item_impl` 仍判断 `IsMercenarySeal`、礼盒/开箱、`fn_is_use` 与使用结果；`api/bag_source.cpp::source_use` 只是转调它 | 效果与前置归 `core.item`；EB 只保留 provider `use` 原语与对象 lease |
| 扩展袋间 merge/swap | `transaction/ext_ext.cpp` 仍实现扩展袋间合并/交换策略、本地 pending 与对象回滚 | 策略归 `core.item`；EB 只在版本化 provider mutation 中原子提交 sidecar |
| 商店购买接纳 gate | `ui/store_trade.cpp::store_buy_find_slot_gate` 仍执行原版优先查找与扩展袋接纳判断 | 接纳规则归 `core.item`；EB 保留 marker/sidecar/ownership/equipment 状态与 assignability |
| 扩展袋卸下目标策略 | `core/host_adapter.cpp::unequip_extension_bag_locked` 仍选择原版接收袋并在原版全满时收编到扩展袋 | 接收顺序/目标策略归 `core.item`；EB 保留自身装备状态转换与 marker/sidecar/ownership/equipment 状态 |

已迁移且与本目标一致的部分：现有 `QolBagSource` 数据原语（注册版本以插件参考为准）、显示读取虚拟化、
sidecar/对象物化、主线程回调与 `core.item` 快照查询；跨空间 journal/move 已部分归 `core.item`。

## 8. 迁移约束

1. 只移动职责，不顺带改变物品语义；每步以对应验收卡验证。
2. 不恢复 EB 的 HTTP、typed command、旧 provider 或旧式全库存 move。
3. 新 ABI/capability 先过 Gate/versioning，再谈实现；文档不得预支批准状态。
4. 不以通用事务框架替代 `core.item` 的动作编排，也不把 EB 的本地 pending 升级为跨插件 journal。
5. `to.slot=-1` 等自动目标只能作为 `core.item` 的选择输入，不能覆盖 EB 的 assignability。

## 9. 不变量

1. `core.item` 拥有物品规则、动作编排与目标策略；EB 不保留普通游戏操作决策。
2. EB 只保留 sidecar/provider 数据原语、对象物化/custody/use lease、UI/投影适配与自身装备状态转换。
3. 扩展袋 merge/swap 策略由 `core.item` 决定；EB 的 sidecar 提交必须原子。
4. `use` 效果/前置归 `core.item`；EB lease 只管理对象生命期。
5. 商店购买与扩展袋卸下的原版优先规则归 `core.item`；EB 保留 marker/sidecar/ownership/equipment 状态。
6. `to.slot=-1` 不替代 EB 更强的 assignability；容量外、在途或 lease 中的槽位必须被拒绝。
7. 状态只在主线程写；跨线程查询只读 `core.item` 快照。
8. 投影是副作用，失败不得改变原版选择，也不得写入数据。
9. 扩展逻辑 ID `6..6+active−1`；原版 0–5 与 C18 页序号命名空间不混用。
10. catalog 是数量/容量/槽数的唯一来源；布局/来源/事务歧义 fail-closed。

## 10. 验证与验收

- 本文是设计文档，不产生行为验收结论；未迁移项不得报告为已完成。
- 变更 owner：主代理 review diff，并运行 `check_arch`；必要时按 `docs/refactor/verification.md`
  的受影响验收卡补验证。
