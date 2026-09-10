# P7 阶段 2：原版/扩展静态支持矩阵

历史归档（2026-09-08）：仅供追溯，不得作为当前实现依据；当前权威见 docs/development/features/extension-bag/

> 状态：STATIC-COMPLETE（机制选择和边界已冻结；不包含真机验证）
> 日期：2026-09-06
> 前置：阶段 0 `stage-0-static-inventory-audit.md`、阶段 1 `stage-1-physical-inventory-contract.md`
> 后续：阶段 3读取本文件裁决旁路；阶段 4读取本文件直接实现单函数 PoC；阶段 5验证生产者运行时矩阵。

## 1. 阶段目标与证据边界

本阶段只回答“哪个入口采用哪种机制”，不回答“游戏运行时所有调用者是否已经生效”。阶段 0/1已经冻结的 ABI、VMA、物理槽、任务袋和对象所有权结论直接复用，不在本阶段重复反汇编。

- Native Hook 只使用 LSPosed 官方 Native Hook API。
- 不引入 Dobby、ShadowHook 或手写 ARM64 trampoline。
- 原版路径优先调用 backup；扩展逻辑只承接无法由物理袋表达的查询、逻辑槽、payload、所有权和 UI 同步。
- `SaveItem`、保存辅助和生产者不做全局入口 Hook；不能用单一 caller 推导全局覆盖。
- `NetworkStore_AddItem` 仅保留历史审计，不属于当前 P7 扩展背包范围。
- 阶段 2不进行真机；线程、自动保存、失败回滚和生产者时序未知项转阶段 5/P6。

## 2. 冻结的函数矩阵

| 类别 | 原版入口 | 阶段 2决策 | 扩展边界 |
|---|---|---|---|
| 查询 | `FindItem` / `HaveItem` / `GetItemCount` | 原版优先 Hook 或等价统一分发 | 扩展逻辑 `6..10` 独立聚合，不伪造物理槽；原版任务袋 `5` 语义不改 |
| 物理槽反查 | `FindItemSlot` | 不新增 Hook，上层分发 | 只返回原版物理编码；扩展返回逻辑引用 |
| 容量/空袋 | `GetBagSize` / `GetEmptyBagSlot` / `IsEmptyBag` | 优先原版调用；`GetEmptyBagSlot` 不作统一扩展入口 | 普通目标 `0..4`；任务袋 `5` 不作为扩展入库目标 |
| 空槽门禁 | `IsHavingEmptySlot` | 原版优先并保留 `include_task_bag` 参数语义 | 扩展容量独立计算，不改写物理任务袋语义 |
| 保存槽辅助 | `GetEmptySaveSlotEx` 等 | 暂缓 Hook | 不传扩展袋号或扩展对象 |
| 入库 | `FindSaveSlot` / `SaveItem` / `SaveItemDirect` / `SaveItemOnEmpty` | 原版优先，按 caller 上层分发 | 原版袋 `0..4`；任务物品可留在原版袋 `5`；扩展对象不得直接传入 |
| 批量生产 | `SaveItemData` / `CreateItem` / `MakeItem` / `ProcessUnpack` | 上层分发，保留旁路 | caller 明确对象移交；失败回滚转阶段 5 |
| 删除/消费 | `RemoveItem` / `ConsumeItem` | 保留现有 Native Hook | 扩展对象逻辑删除/消费；原版对象调用 backup；禁止物理释放路径处理扩展对象 |
| 移动/拆堆 | `MoveItem` / `Divide` | 不新增全局 Hook | 保留 P4 三方向事务；任务袋 `5` 与扩展逻辑袋不得混用 |
| 使用/装备/镶嵌 | `UseItemEx` / `Equip*` / `PutJewel` | 优先原版效果函数；扩展对象受控物化和适配 | 扩展消费、材料删除、所有权和 UI 同步由扩展层承接 |
| 生产者 | 拾取、奖励、开箱、拆包、合成、商店、脱装备、使用结果 | 上层分发，不以单一 `SaveItem` 宣称全局覆盖 | 原版袋优先、扩展兜底；逐项转阶段 5 |
| 保存/序列化 | `SAVE_Save*` / `SAVE_Load*` | 暂缓新增 Hook | 保持 P6 协调器和 sidecar 契约 |

## 3. 实施约束

1. 扩展对象不得进入 `INVEN_RemoveItemDirect`、`ITEMPOOL_Free` 或只接受物理槽的保存链。
2. `GetEmptyBagSlot` 不得被改造成扩展统一入库入口。
3. 任务袋 `5` 仅保持原版查询/任务语义；普通扩展目标固定为 `0..4`。
4. 现有旁路在阶段 3完成迁移/退役裁决和后续运行时证据前不得删除。
5. 阶段 4实现必须读取本矩阵，不重新判断机制选择；只有新证据推翻某个具体函数的 ABI、所有权或旁路假设时，才做定向复核。

## 4. 阶段结论

- 查询、空槽、消费、删除、装备、宝石的入口决策已冻结。
- 入库、生产者、保存入口不做宽泛 Hook，保留 caller 级扩展分发。
- 阶段 3继续读取本文件裁决现有旁路。
- 阶段 4直接进入单函数 Hook/调用机制 PoC 和 Host 回归。
- 阶段 5验证拾取、任务奖励、开箱、拆包、合成、商店、装备和使用的真实调用链。

## 5. 验收记录

| 字段 | 内容 |
|---|---|
| Evidence | `control-plane.md`：`E-2026-09-06-04` |
| Source | 阶段 0/1文档、`game_symbols.h`、`symbol_registry.h`、`native_inventory_hook.cpp`、固定 ELF 反汇编 |
| Device | 未使用；阶段 2禁止真机 |
| Result | `STATIC-COMPLETE`；P7 Overall仍为 `NOT_ACCEPTED` |
