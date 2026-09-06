# 扩展背包全局库存接入检查与开发方案

> 文档状态：CURRENT，替代 `native-inventory-hook-development.md` 作为 P7 开发方案
> 适用范围：扩展背包、堆叠上限与游戏原版库存函数的全局接入决策
> 最后核对：2026-09-06
> 当前结论：P7 `NOT_ACCEPTED`；阶段 0–3 的静态可证明内容已闭合，阶段 4已完成首个 `INVEN_FindItem` 单函数 PoC，剩余函数与运行时未知项继续按阶段门禁推进。阶段 4–7 尚未整体完成验收；本方案不以“已存在代码”或“已登记 ABI”视为功能完成。
> 阶段 0 审计记录：`stage-0-static-inventory-audit.md`（生产者、所有权/回滚、副作用矩阵已收敛；14 项运行时未知项转后续真机验证）。

## 0.1 阶段结论索引

阶段 0–3 已完成的结论不需要重新盘点，统一从以下位置读取：

| 阶段 | 权威结论 | 验收记录 |
|---|---|---|
| 阶段 0 | 本方案 §3 阶段 0；函数/调用者、ABI、所有权和副作用静态矩阵 | `control-plane.md` 的 `E-2026-09-06-02` |
| 阶段 1 | 本方案 §3 阶段 1；物理袋 `0..5`、任务袋 `5`、普通目标 `0..4`、扩展逻辑袋 `6..10` | `stage-1-physical-inventory-contract.md`；`control-plane.md` 的 `E-2026-09-06-03` |
| 阶段 2 | 本方案 §3 阶段 2及 §4矩阵；必须 Hook、原版优先、上层分发、暂缓项 | `control-plane.md` 的 `E-2026-09-06-04` |
| 阶段 3 | 本方案 §3 阶段 3；旁路保留/迁移/退役候选、所有权和生产者静态矩阵 | `control-plane.md` 的 `E-2026-09-06-05`、`E-2026-09-06-06`、`E-2026-09-06-07` |

阶段 4只读取上述冻结结论，除非新的实现证据直接推翻某个具体函数的 ABI、所有权或旁路假设，才对该函数做定向复核。

## 1. 目标与核心判断

本方案不把“所有函数都 Hook”作为目标。每个原版函数必须分别回答以下问题：

1. 原版游戏是否通过它完成扩展背包需要覆盖的行为？
2. 直接调用原版函数、读写内存、PtrHook、指令 patch 或业务分发器是否已足够？
3. 入口 Hook 是否能覆盖所有调用者，还是只覆盖一个 API/按钮路径？
4. Hook 后能否保持原版物理槽、任务袋、对象所有权、堆叠上限和保存时机？
5. 当前扩展旁路能否被删除，还是必须继续保留作为安全边界？

### 1.1 决策原则

- 默认顺序固定为：直接读写内存 → 调用原版函数 → PtrHook → 指令 patch → LSPosed Native Hook API。
- Native Hook 只使用 LSPosed 官方 Native Hook API：`native_init` 初始化入口及 Native API 提供的 hook/unhook 函数指针（官方当前字段为 `hookFunc`/`unhookFunc`，本项目适配字段为 `hook_func`/`unhook_func`）；不引入或使用 Dobby、ShadowHook 或手写 ARM64 trampoline。
- 只有需要拦截、改写参数、替换返回对象或保护原版对象释放时，才使用 Hook。
- 扩展逻辑袋不能伪装成原版物理 bag/slot；原版物理槽编码只代表 `INVEN_pItem` 中的真实槽。
- 原版袋 `0..4` 是可入库物理袋，任务袋 `5` 不能被扩展逻辑占用。
- `GetEmptyBagSlot` 一类函数若能成为拾取、任务奖励、开箱、商店或合成入库的共同入口，应优先评估；但必须先证明所有调用者都遵守其返回值契约。
- 原版优先：凡原版函数已经覆盖业务效果、规则判断或消费时机，扩展路径应优先物化对象并调用原版函数；上层只负责逻辑槽门禁、物化、结果归属和状态同步。
- 扩展最小适配：`ConsumeItem`/`RemoveItem` 的扩展分支是原版函数的扩展后端，不属于可删除的重复业务逻辑；它负责 payload、逻辑槽、所有权和投影 UI，而不是重新实现物品效果。
- 只有在原版函数无法表达扩展逻辑槽、无法指定扩展对象、会写入错误物理槽/释放对象，或确实存在独立的上层事务时，才允许绕过原版函数使用上层自定义实现。
- 现有旁路不得在原版入口接入前删除；新入口完成调用链、所有权、回滚和真机证据后，才可逐项退役。退役目标是删除重复的原版业务模拟，而不是删除扩展适配层。
- 扩展保存暂缓。P6 已有保存协调器和 sidecar 机制，P7 不得在未完成冲突分析前 Hook 保存入口或改写保存时机。

## 2. 当前实现基线

### 2.1 已存在的实现机制

| 机制 | 当前实现 | 覆盖范围 | 不能据此推出 |
|---|---|---|---|
| 扩展逻辑状态/事务 | `feature/extension_bag/game_ui_virtbag.*`、`extension_bag_transaction.inc` | 扩展槽、三方向移动、payload、所有权、任务袋拒绝 | 所有游戏生产者自动支持扩展 |
| 原版物品入口 Hook | `feature/patch/native_inventory_hook.cpp` | `FindItem`、`HaveItem`、`GetItemCount`、`ConsumeItem`、`RemoveItem`、装备入口、宝石入口、`IsHavingEmptySlot` | 未 Hook 的拾取/奖励/生产者调用链 |
| 原版函数指针调用 | `game_access.*` | 查询、装备、入库、保存等已登记函数 | 调用者会自动把扩展对象当作原版对象 |
| 堆叠上限 patch | `feature/patch/game_patch.*` | 已登记的 42 个堆叠上限 patch 点 | 扩展逻辑袋和原版生产者已统一 |
| P6 保存协调 | `core/native/module_save.*`、sidecar | 原版保存成功后的扩展状态提交 | P7 可以直接接管 `SAVE_*` 或 `INVEN_*` 保存函数 |
| 原版绕行的扩展操作 | `extension_bag_api_impl.inc`、`extension_bag_transaction.inc`、装备/绘制/生命周期文件 | API、扩展视图、跨袋移动、扩展装备 | 可以立即删除；必须先完成等价原版调用链验证 |

### 2.2 当前必须保留的安全边界

- `INVEN_RemoveItemDirect` 只处理真实原版物理槽和 `ITEMPOOL_Free`，不能作为扩展入口。
- 扩展对象只由 `g_module_objects` 和逻辑槽映射管理，不交给原版对象池释放。
- 当前扩展保存入口、sidecar 提交和 pending 恢复属于 P6；在 P7 保存冲突检查完成前保持原样。
- 任务袋 `5` 的 sentinel、容量和内容必须在所有新入口前后不变。

## 3. 分阶段检查方案

> **前期验证边界**：阶段 0–4 禁止直接进行真机验证。前期只允许使用原版
> `libgame.so` 汇编/符号表、已有开发文档、当前源码、Host 测试和静态脚本获取结论。
> 真机只用于后续已经收敛的单项方案验证，不能用真机试错替代 ABI、调用链或旁路分析。

### 阶段 0：建立函数与调用者清单（仅静态分析）

**输出**：函数清单、动态符号/VMA、ABI、直接调用点、间接调用点、返回值和副作用。

- 从 `game_symbols.h`、`symbol_registry.h`、`libgame.so` 符号表生成候选函数表。
- 对每个函数反汇编入口、所有 `BL` 调用点、返回值使用点和错误分支。
- 标记调用线程：游戏主线程、HTTP worker、FrameTask、UI callback。
- 标记是否读写 `INVEN_pItem`、是否创建/释放 item、是否触发刷新、是否触发保存。

### 阶段 1：定义原版物理库存契约

**输出**：不带扩展逻辑的原版行为基线；不进行真机操作。

- 固定 bag `0..5` 的真实用途、容量、空槽编码、任务袋限制。
- 固定 item 指针、物理槽编码、类别、数量、堆叠身份和对象释放关系。
- 对每个函数记录成功/失败/边界/空指针/满包返回值。
- 从原版汇编、已有文档和当前代码重建移动、拾取、奖励、任务物品、开箱、合成、商店和保存的调用链。
- 对无法由静态证据确定、且不影响 ABI/物理契约定义的行为标记为“待阶段 5真机验证”；保存崩溃、进程中断和 sidecar 一致性归 P6，不在阶段 0重复定义。

### 阶段 2：建立扩展支持矩阵（静态决策）

**输出**：每个函数的“必须 Hook / 不应 Hook / 需要上层分发 / 暂缓”结论。

- **查询类**：原版优先，扩展 fallback 或聚合；不得伪造物理槽。
- **入库类**：原版袋优先，满包后转扩展事务；重点检查 `GetEmptyBagSlot` 是否是统一入口。
- **对象类**：扩展对象只能在扩展事务中创建、借用、删除和释放。
- **堆叠类**：区分原版 999 上限 patch 与扩展逻辑数量/身份合并规则。
- **生产者类**：根据汇编和代码逐一追踪拾取、任务、奖励、开箱、拆包、合成、商店购买，不能用 `GetItemCount` 代替；网络商店不属于当前扩展背包接入范围；静态无法闭合的路径留到阶段 5。

**阶段 2静态决策结果（2026-09-06）**：以下决策只冻结机制和边界，不宣称生产者运行时覆盖；未知线程、回滚、自动保存和任务奖励落点统一转阶段 5/P6。

**阶段 2状态：`STATIC-COMPLETE`。** 该状态只表示支持矩阵和机制选择已冻结；阶段 3旁路冲突分析现已 `STATIC-COMPLETE`，阶段 4最小 PoC、阶段 5生产者真机验证、阶段 6旁路退役和 P8最终回归仍未完成。

| 类别 | 原版入口 | 阶段 2决策 | 扩展边界 |
|---|---|---|---|
| 查询 | `FindItem` / `HaveItem` / `GetItemCount` | 必须保留原版优先 Hook 或等价统一分发 | 原版物理查询可含任务袋 `5`；扩展逻辑 `6..10` 仅通过独立聚合，不伪造物理槽 |
| 物理槽反查 | `FindItemSlot` | 不应 Hook；上层分发 | 只返回原版物理编码，扩展返回逻辑引用；任务袋 `5` 仅作原版查询结果 |
| 容量/空袋 | `GetBagSize` / `GetEmptyBagSlot` / `IsEmptyBag` | 优先原版调用；`GetEmptyBagSlot` 不作为统一扩展入口 | 普通目标 `0..4`；任务袋 `5` 不成为扩展入库目标 |
| 空槽门禁 | `IsHavingEmptySlot` / `CalculateEmptySlotCountForSave` | `IsHavingEmptySlot` 保留原版优先并按参数分流；保存计算暂缓 | `include_task_bag` 的任务袋语义不改写；扩展容量独立计算 |
| 保存槽辅助 | `GetEmptySaveSlotEx` / `GetNeededSaveSlotEx` / `GetCumulateSaveSlotEx` / `CheckSaveInNotEmptySlot` | 暂缓 Hook，保留原版 ABI | 不向这些函数传扩展袋号或扩展对象；失败输出细节留保存审计 |
| 入库 | `FindSaveSlot` / `SaveItem` / `SaveItemDirect` / `SaveItemOnEmpty` | 原版优先但不做全局入口 Hook；按 caller 上层分发 | `FindSaveSlot` 普通物品为 `0..4`、任务物品可为 `5`；`SaveItemOnEmpty` 只用于已校验的普通目标 `0..4` |
| 批量生产 | `SaveItemData` / `CreateItem` / `MakeItem` / `ProcessUnpack` | 上层分发，保留旁路 | 创建对象所有权由 caller 明确移交；失败回滚和产物落点留阶段 5 |
| 删除/消费 | `RemoveItem` / `ConsumeItem` | 已有 Native Hook 保留 | 扩展对象走逻辑删除/消费；原版对象调用 backup；禁止 `RemoveItemDirect`/`ITEMPOOL_Free` 处理扩展对象 |
| 移动/拆堆 | `MoveItem` / `Divide` | 不新增全局 Hook；保留 P4事务和上层分发 | 普通原版路径调用原版；扩展三方向事务独立；任务袋 `5` 和扩展逻辑袋不得混用 |
| 使用/装备/镶嵌 | `UseItemEx` / `CanEquip` / `Equip*` / `PutJewel` / `EnchantItem` | 优先原版效果函数；扩展对象走受控物化与适配 | 扩展消费、装备、材料删除和 UI同步由上层承接；失败原子性留阶段 5 |
| 生产者 | 拾取、任务奖励、开箱、拆包、合成、商店、脱装备、普通使用结果 | 上层分发；不以单一 `SaveItem` caller 宣称全局覆盖 | 原版袋优先、扩展兜底；NetworkStore 保留原版并排除当前 P7 |
| 保存/序列化 | `SAVE_Save*` / `SAVE_Load*` | 暂缓新增 Hook | P6协调器和 sidecar 契约不改 |

### 阶段 3：检查现有旁路能否被原版入口替代（静态冲突分析）

**输出**：保留、迁移、退役三类旁路清单。

**阶段 3当前状态：`STATIC-COMPLETE`。** 静态审计已闭合确定性旁路、API 创建物品失败泄漏、商店扣款/入库顺序、pending 恢复对象链和 LSPosed `native_init` 导出约束；生产者/保存 caller 已形成完整静态矩阵，现有 8 个 Hook 不覆盖它们是有意的上层分发决策。合成部分提交回滚、生产者运行时行为和保存一致性明确转阶段 5/P6；本阶段不删除旁路，也不批准 P7 Overall。

- 对每个旁路记录当前入口、绕过的原版函数、原因、覆盖调用者和失败回滚。
- 如果新 Hook 覆盖所有调用者且能提供同等事务语义，先建立对照日志，再删除旁路。

#### 阶段 3旁路裁决（2026-09-06）

| 分类 | 入口 | 裁决与边界 |
|---|---|---|
| 保留 | `extension_bag_transaction.inc` 三方向事务、`extension_bag_api_impl.inc` 扩展分发、`module_save.cpp` P6 协调、扩展 UI 投影/拖动 | 原版物理槽无法表达逻辑袋 `6..10`、payload、五态事务和 sidecar；任务袋 `5` 继续排除；不得以 API 成功或 Hook 安装替代真机证据 |
| 保留 | `native_inventory_hook.cpp` 的 `consume_item_wrapper`、`remove_item_wrapper`、装备/宝石适配 | 扩展对象不得进入 `RemoveItemDirect`、`ITEMPOOL_Free` 或原版保存路径；这些 Hook 只证明已登记入口，不证明生产者全局覆盖 |
| 迁移 | `find_item_wrapper`、`have_item_wrapper`、`get_item_count_wrapper`、`is_having_empty_slot_wrapper` | 维持原版优先、扩展 fallback；不把扩展对象伪造成物理槽，不把 `GetEmptyBagSlot` 变成扩展入口，并保持 `include_task_bag` 语义 |
| 迁移 | `data_op_use_item`、`data_op_equip`、`data_op_jewel`、`data_op_enchant` 的重复原版业务模拟 | 效果/规则优先调用原版；扩展物化、逻辑状态、消费、payload、所有权和 UI 同步继续由扩展层承接。已修复 `data_op_equip` 直接调用 `fn_equip_item` 和 `data_op_jewel` 直接调用 `fn_remove_item_direct` 的扩展对象旁路 |
| 暂缓 | 拾取、任务奖励、开箱、拆包、合成、商店、脱装备、快捷键和原版保存子调用 | 每个生产者单独核对 caller、context、对象所有权、失败释放、任务袋门禁和持久化；不能由单一 `SaveItem` 或现有 8 个 Hook 宣称覆盖 |
| 退役候选 | 扩展 API 中重复的原版效果/扣数量模拟、旧 v2/v3 持久化兼容分支 | 只有全部 caller 覆盖、原版/扩展成功失败回归、对象审计、P6 保存边界和真机证据齐全后，才由阶段 6逐项退役；当前不删除 |

**阶段 3转交项**：①投影宝石 Direct remove、直接 equip、pending ext→orig ledger 对称性、API 创建物品失败释放、商店扣款/入库失败路径和 `native_init` 导出约束已完成静态修复；②`INVEN_SaveItem*`、`RemoveItemData`、生产者及保存 caller 已完成静态盘点，但 caller context、线程、堆叠释放和运行时覆盖转阶段 5/P6；③合成部分提交回滚没有安全、静态可证明的逆操作，保留现有旁路并转阶段 5/P6，不添加猜测性回滚。

#### 生产者 caller 覆盖矩阵（阶段 3/阶段 5前置）

| 生产者 | 已确认原版链 | 当前裁决 | 未闭合项 |
|---|---|---|---|
| 拾取 | `CHAR_PickItemAll` → `NOTIFIER_Add` → `CHAR_ActivePickupEvent` → `INVEN_SaveItem` | 原版优先，caller 级扩展兜底 | 掉落对象/通知节点的失败释放、线程与满包行为 |
| 任务奖励 | `QUESTSYSTEM_ApplyReward`；部分分支可见 `ITEMSYSTEM_CreateItem` → `INVEN_SaveItem` | 任务物品保持原版任务袋 `5`；普通奖励才允许原版失败后扩展兜底 | 全部 reward 分支、类别到袋域映射、失败回滚 |
| 开箱 | `ITEMSYSTEM_OpenItemBox` 内部调用 `INVEN_SaveItem`；模块 `data_op_use_item` 另行消费输入 | 不 Hook；保留上层分发 | 输入消费时机、多个产物的部分成功回滚、产物落点 |
| 拆包 | `ITEMSYSTEM_ProcessUnpack`；已知 caller 为 `CHAR_UseItemEx` | 逐产物原版优先，失败后再扩展 | 前序产物回滚、输入消费和第三参数语义 |
| 合成 | `MIXSYSTEM_MakeItem` 有 5 个原版 caller；模块 `game_patch_craft.inc` 自建保存/材料删除链 | 保留旁路，逐 caller 审计 | 产物保存失败释放、材料删除前后校验和扣款返回值检查已补；部分提交回滚、5 个 caller 分支差异仍未闭合 |
| 商店 | `UIStore_*`/`DEALSYSTEM_*`；模块路径 `FindSaveSlot` → `SaveItem` → 扣款 | 原版物理袋 `0..4` 优先；NetworkStore 排除当前 P7 | 商品对象借用、扣款顺序、失败回滚和堆叠释放 |
| 脱装备 | `CHAR_UnequipItemToInven*`；模块路径直接调用 `fn_unequip` | 原版优先；扩展装备走受控物化入库 | 全部 caller、满包行为、ext→orig pending 恢复回归 |
| 使用结果 | `CHAR_UseItemEx`、`CHAR_ProcessShortcut` 及事件/强化/制造/地图/任务结果；`ITEMSYSTEM_CreateItem` 有多处 caller | 原版效果优先，扩展对象不得进入 `ITEMPOOL_Free` | 创建→保存→消费所有权链、间接 caller、失败释放 |

**矩阵结论**：当前 8 个 Native Hook（查询、消费、删除、装备、镶嵌、空槽）不覆盖上述生产者创建/保存 caller，这是已登记的上层分发裁决；逐 caller 静态证据已完成，但不得用其替代阶段 5真机验收。合成旁路已增加失败检测，但仍不是完整原子事务，转阶段 5/P6验证。
- 如果原版函数会写入真实物理槽、释放对象或依赖 UI 上下文，则保留上层扩展事务，不强行回归原版。
- API 端点不能作为“所有游戏内调用者已覆盖”的证据。

### 阶段 4：最小 PoC 与 Host 回归（仍禁止真机）

**输出**：单函数 Hook/调用机制 PoC 和 ABI 证据。

- 一次只接入一个函数或一个严格同源函数组。
- 原版物品路径必须逐项走 backup 或原版函数，返回值不变。
- 扩展物品路径必须通过 Host 测试验证逻辑槽、payload、数量、对象借用和释放计数。
- 安装失败必须完整回滚已安装 Hook；递归、并发和重复安装必须有日志。
- 阶段结束时只形成候选实现方案，不宣布游戏内功能完成。

### 阶段 5：生产者调用链真机验证（静态结论之后）

**输出**：从来源到库存的端到端矩阵。

只有阶段 0–4 已经完成 ABI、调用链和旁路决策后，才逐项验证：拾取、任务物品、任务奖励、开箱、拆包、合成、商店购买、装备和使用。网络商店不纳入本轮扩展背包验收；其 P6 保存记录和原版运行代码保持不变。每项记录：

- 来源函数和目标入库函数；
- 原版未满、原版已满、扩展有空槽、全部满；
- 可堆叠同身份、可堆叠不同 payload、不可堆叠；
- 失败时源对象、目标槽、数量和所有权是否恢复；
- 是否触发堆叠上限 patch、UI 刷新和保存。

### 阶段 6：旁路迁移与退役

**输出**：每个被替代旁路的删除/保留决定。

只有满足以下条件才允许删除现有旁路：

1. 所有已知直接 `BL` 和间接调用者均覆盖；
2. 原版与扩展两条路径均有真机成功/失败证据；
3. 任务袋 sentinel、物理槽和对象释放无变化；
4. 堆叠上限和 payload 合并规则不回归；
5. P6 保存协调没有重复提交、提前提交或丢失窗口；
6. 删除旁路后 Host、Debug、真机回归全部通过。

### 阶段 7：最终验收与文档冻结

**输出**：最终函数列表、Hook 决策、旁路状态、真机证据和 P7 结论。

- 运行 `git diff --check`、相关 Host tests、`scripts/maintenance/check_symbols.py`、`scripts/build-debug.sh`。
- 在 `192.168.3.54:5555` 完成扩展开关、原版回归、生产者矩阵、堆叠边界和重启回归。
- 未完成真机证据的函数标记为“待真机验证”，不能标记完成。

## 4. 最终函数决策清单

状态含义：**已实现**=已有代码但仍可能缺真机覆盖；**部分实现**=ABI/局部路径已有；**未实现**=没有可接受的扩展支持；**暂缓**=有意不改，等待冲突评估。

决策含义：**必须 Hook**=普通函数入口拦截是必要机制；**优先原版调用**=直接调用原版函数即可；**上层分发**=不应 Hook，必须在业务入口或逻辑事务处理；**保留原版**=扩展不能伪装该函数契约。

### 4.1 查询与容量

| 函数 | ABI 状态 | 当前实现 | 决策 | 扩展/堆叠影响范围 | 旁路处理 |
|---|---|---|---|---|---|
| `INVEN_FindItem` | 已确认 `void*(int32_t)` | 已实现 Hook | 必须 Hook 或等价统一分发 | 使用、快捷键、材料检查、装备查找的扩展 fallback；原版优先 | 保留逻辑查询层，逐步统一调用 |
| `INVEN_HaveItem` | 已确认 `int(int32_t)` | 已实现 Hook | 必须覆盖所有直接调用 | 配方、任务、使用前检查的类别存在性 | 现有 Hook 保留，补调用链验收 |
| `INVEN_GetItemCount` | 已确认 `int(int32_t)` | 已实现 Hook | 必须覆盖所有直接调用 | 配方、消费、生产者数量；原版数量+扩展数量 | 不能单独替代材料槽/入库支持 |
| `INVEN_FindItemSlot` | 已确认 `int(void*,int8_t*)` | ABI/原版路径已实现，扩展物理槽不支持 | 保留原版契约；上层分发 | 只识别原版物理对象；扩展返回逻辑 bag/slot，不伪造 `out_slot` | 扩展对象继续走逻辑映射 |
| `INVEN_GetBagSize` | 已确认 `int(int32_t)` | 原版调用已实现 | 优先原版调用；不伪装 | 原版容量基线；扩展容量走独立 `derive_capacity` | 现有扩展逻辑容量保留 |
| `INVEN_GetEmptyBagSlot` | 已确认 `int()` | 仅原版解析 | **不应 Hook；仅作原版普通袋预检** | 固定返回普通袋 `0..4`，不覆盖任务袋 `5` 或扩展逻辑袋 | 保留现有生产者旁路，不把它升级为统一扩展入口 |
| `INVEN_IsEmptyBag` | 已确认 `int(int32_t)` | 原版解析；扩展逻辑独立判断未统一 | 上层分发，必要时 Hook | 满包提示、入库前预检；扩展袋应独立统计 | 保留 `virtual_bag_has_empty_slots` |
| `INVEN_IsHavingEmptySlot` | 已确认 `int(int32_t,int32_t)` | 已实现 Hook | 必须 Hook 或统一调用点适配 | 原版空槽优先，扩展空槽 fallback；任务袋排除 | 保留逻辑空槽接口 |
| `INVEN_CalculateEmptySlotCountForSave` | 已确认 `int(int32_t,int32_t)` | ABI 已登记，扩展支持暂缓 | 暂缓 | 只影响保存所需槽计算，可能与 P6 冲突 | 不改保存协调器 |
| `INVEN_GetEmptySaveSlotEx` | 已确认 5 参数 | ABI 已登记，扩展支持暂缓 | 暂缓 | 保存/入库槽编码，可能影响 P6 | 不改 |
| `INVEN_GetNeededSaveSlotEx` | 已确认 7 参数 | ABI 已登记，扩展支持暂缓 | 暂缓 | 保存/堆叠/空槽组合，可能影响 P6 | 不改 |

**重点结论**：`INVEN_GetEmptyBagSlot` 不是“已确认就立即 Hook”。必须先阶段 0/1 找出所有调用者。如果拾取、任务奖励、开箱、拆包、合成和商店购买均通过它或同一底层入库链，则它是高价值统一接入点；如果调用者随后直接写 `INVEN_pItem`、依赖 UI 上下文或使用不同的入库函数，则只能逐个适配，不能仅靠它宣称全局支持。

### 4.2 堆叠、移动、删除与对象

| 函数 | ABI 状态 | 当前实现 | 决策 | 扩展/堆叠影响范围 | 旁路处理 |
|---|---|---|---|---|---|
| `INVEN_GetCumulateSaveSlotEx` | 已确认 5 参数 | 仅 ABI 登记 | 暂缓随保存；非保存堆叠另行验证 | 原版堆叠槽选择、堆叠上限和合并身份 | 保留扩展事务 |
| `INVEN_MoveItem` | 已登记 4 参数，调用语义仍需完整冻结 | 扩展三方向事务已有实现 | 优先保留事务；不立即 Hook | 移动、合并、payload、任务袋和 999 上限 | 新入口验证后再评估替代 |
| `ITEMSYSTEM_Divide` | 已确认 `void*(void*,int32_t)` | 扩展拆堆未完成 | 上层分发/必要时 Hook | 拆堆数量、返回对象所有权、物理槽落位 | 不让返回对象直接进入原版物理槽 |
| `INVEN_RemoveItem` | 已确认 `int(void*)` | 已实现 Hook | 必须 Hook | 扩展整槽删除、原版 backup 删除 | 保留对象识别和逻辑事务 |
| `INVEN_RemoveItemDirect` | 已确认 `int(int,int)`，返回值不可信 | 原版原语已登记 | **禁止作为扩展入口** | 原版物理释放、`ITEMPOOL_Free` | 扩展对象不得进入 |
| `INVEN_RemoveItemData` | 已确认 `int(int32_t category,int32_t count)` | 未实现扩展接管 | 上层分发；仅限原版物理数据回滚 | 数据删除、数量和物理释放边界 | 保留现有安全删除路径，扩展对象禁止进入 |
| `INVEN_ConsumeItem` | 已确认 `void(void*)` | 已实现 Hook | 必须 Hook | 堆叠减 1、不可堆叠整槽删除、堆叠上限回归 | 保留逻辑消费事务 |

### 4.3 入库、创建与生产者

| 函数/函数族 | ABI/状态 | 当前实现 | 决策 | 影响范围 | 旁路处理 |
|---|---|---|---|---|---|
| `INVEN_FindSaveSlot` | 已确认 `int(void*,int8_t*)`；返回是否找到并通过输出指针写入物理槽编码 | 原版/扩展事务已有部分使用；API 添加和商店购买已修正调用语义 | **上层分发；仅作原版物理预检** | 普通物品为 `0..4`，任务物品可由原版类别路径使用 `5` | 暂不删除跨袋事务，不把输出槽改造成扩展槽 |
| `INVEN_SaveItem` | 已登记，完整 ABI/调用者待查 | 原版调用存在 | **优先原版调用；不做全局入口 Hook** | 拾取、奖励、开箱、合成、商店入库的 caller 逐项确认；context/所有权未知 | 保留生产者旁路，扩展对象不得直接传入 |
| `INVEN_SaveItemDirect` | 已登记 | 原版底层入库 | 保留原版原语，不直接接扩展 | 指定物理槽写入、堆叠上限 | 扩展使用逻辑事务 |
| `INVEN_SaveItemOnEmpty` | 已登记 `int(void*,int)` | 跨包移动使用 | **优先原版调用；不新增 Hook** | 纯首空槽扫描；调用前目标必须限定原版普通袋 `0..4` | 保留扩展→原版事务，任务袋 `5` 和扩展对象均在入口排除 |
| `INVEN_SaveItemData` | 已确认 `int(int32_t category,int32_t count)`；内部按数量创建并调用 `INVEN_SaveItem`，失败调用 `RemoveItemData` 回滚 | 未实现扩展接管 | 上层分发/暂缓；不作为扩展对象入口 | 数据写入、数量和物理回滚 | 不改 P6 |
| `INVEN_CheckSaveInNotEmptySlot` | ABI 未完成 | 未实现 | 原版校验原语；不伪造扩展槽 | 入库前槽状态 | 保留扩展预检 |
| `ITEMSYSTEM_CreateItem` / `ITEMSYSTEM_MakeItem` | ABI 已登记 | API/部分生产路径使用 | 优先原版创建，接管返回对象所有权 | 任务奖励、开箱、合成、掉落产物 | 创建后必须明确进入原版还是扩展事务 |
| `ITEMSYSTEM_ProcessUnpack` | ABI 未完成 | 未实现 | **上层分发；不 Hook** | 拆包产物、数量和入库时序待阶段 5 | 暂不删除旁路 |
| `MAPITEMSYSTEM_CreateItem` / `CHAR_PickItemAll` | 部分 ABI/调用链已登记 | 掉落路径未完成 | **上层分发；不 Hook** | 拾取掉落实体到原版/扩展背包，时序待阶段 5 | 当前 `nav_pick_items` 等旁路保留 |
| `NetworkStore_AddItem` | ABI/调用链已有历史审计 | 不纳入当前扩展背包实现 | 保留原版；移出 P7 验收 | 网络商店原版入库和保存 | 保留 P6 保存协调与原版运行代码，不做扩展分流 |

### 4.4 物品语义、使用、装备与镶嵌

| 函数/函数族 | 当前实现 | 决策 | 扩展/堆叠影响范围 | 现有旁路 |
|---|---|---|---|---|
| `ITEM_GetCumulateCount` | 原版函数已调用；扩展逻辑独立统计 | 优先原版调用；扩展对象走逻辑属性 | 数量读取、999 上限判断 | 保留扩展 `Item.count` |
| `ITEM_GetPrice` / `ITEM_GetBuyPrice` / 卖价族 | API/静态表部分已有 | 优先原版调用或 category 静态逻辑 | 商店价格，不直接决定入库 | 保留商店上下文适配 |
| `ITEM_GetAbilityLevel` / `ITEM_GetRarity` / Damage/Defense/Magic/Name | 部分已实现 | 优先原版调用，必要时受控物化 | 装备展示、限制和语义判断 | 不把物化对象交给原版释放 |
| `ITEMDATABASE_IsUse`、`ITEMSYSTEM_Is*`、`CanPutJewel` | 部分登记/调用 | 优先原版函数或静态表，不默认 Hook | 使用、开箱、封印、宝石材料判定 | 保留 API/事务门禁 |
| `CHAR_UseItemEx` / `CHAR_ProcessShortcut` | 使用链与快捷键调用点已确认，扩展消费部分已接入 | **优先原版调用**；仅必要直接 BL 才做调用点适配 | 药水、卷轴、技能书、快捷键；成功消费由 `ConsumeItem` 扩展分支同步 | 保留逻辑槽门禁、物化、`FindItem` Hook 和扩展消费适配；删除重复的上层效果/扣数量实现 |
| `CHAR_CanEquipItem` / `CHAR_FindEquipSlot` | 原版解析/扩展装备事务部分完成 | 优先原版调用；扩展对象走事务 | 装备槽、限制、容量和对象所有权 | 保留扩展装备事务 |
| `CHAR_EquipItem` / `CHAR_EquipItemFromInven*` | 入口部分已 Hook/适配 | 必须覆盖扩展装备对象，但不一定继续扩大入口 Hook | 装备、源槽清理、UI 投影 | 真机覆盖后再评估旁路退役 |
| `CHAR_UnequipItemToInven*` | 扩展事务已有部分 | 上层分发；先验证原版空槽入口 | 脱装备入库、满包和扩展目标 | 保留扩展事务 |
| `ITEMSYSTEM_PutJewel` / `ITEMSYSTEM_EnchantItem` | 宝石入口已接入；强化未完整 | **优先原版效果函数**；成功后的材料消费/删除走 `ConsumeItem`/`RemoveItem` 扩展适配 | 宝石消耗、强化材料、堆叠数量 | 保留逻辑槽、payload、所有权和投影同步；不重复模拟原版效果 |

### 4.6 使用与消费的原版优先矩阵

本节是 4.4 中“优先原版调用”的具体执行规则。目标不是让扩展对象伪装成原版物理对象，而是让原版函数继续决定效果、规则和消费时机，再由扩展适配层承接扩展状态。

| 路径 | 原版优先动作 | 必须保留的上层职责 | 当前决策 |
|---|---|---|---|
| 普通使用（药水、技能书、配方书、增益等） | 扩展逻辑槽校验后物化 native item，调用 `CHAR_UseItemEx`；成功消费由 `INVEN_ConsumeItem` Hook 接管 | 物化、扩展对象识别、payload/逻辑槽同步、对象所有权、投影刷新 | 优先迁移到原版；删除重复扣数量逻辑 |
| 强化卷轴 | 调用原版强化函数；成功后调用 `INVEN_ConsumeItem` | 扩展卷轴识别和消费适配 | 优先原版；不得在上层重新实现强化效果 |
| 宝石 | 调用原版 `ITEMSYSTEM_PutJewel`；成功后通过 `INVEN_RemoveItem` 适配删除扩展宝石 | 扩展宝石删除、所有权和 UI 同步 | 优先原版；禁止 `RemoveItemDirect` 处理扩展对象 |
| 堆叠消费 | 由原版消费时机触发 `ConsumeItem`；扩展分支更新逻辑数量和 payload | 数量回写、序列化失败回滚、dirty/persist、投影刷新 | 原版决定时机，扩展适配承接状态 |
| 不可堆叠消费 | 原版可能从 `ConsumeItem` 进入 `RemoveItem`；扩展分支必须在 Hook 中截断并执行逻辑槽删除 | 清槽、对象释放、所有权账本、UI 刷新 | 原版触发，扩展适配执行删除；禁止进入物理释放链 |
| 骰子 | 复用原版掷骰/属性效果函数；仅在静态或真机证据确认原版不消费时保留一次消费调用 | pending 状态、一次性消费、投影刷新 | 原版优先，消费时机待证；禁止 accept 阶段重复消费 |
| 解封/开箱 | 优先调用原版效果函数，但必须确认其输入是指定物品还是 category，以及产物进入哪个库存 | 扩展源对象消费、产物归属、满包/回滚、投影刷新 | 调用链未闭合前保留上层适配，不得直接删除 |

统一模型：

```text
扩展逻辑槽 → 物化 native item → 原版效果函数
                         → ConsumeItem/RemoveItem
                         → 扩展状态适配（payload/所有权/UI）
```

以下情况才允许绕过原版效果函数：原版函数只接受无法绑定到扩展对象的 category、强制写入原版物理槽、直接释放扩展对象，或其事务契约无法表达扩展产物归属。绕过时上层只补齐缺失边界，不复制原版已经具备的效果规则。

### 4.5 合成、商店、掉落、存档与 UI

| 函数/函数族 | 当前实现 | 决策 | 影响范围 | 状态 |
|---|---|---|---|---|
| `MIXSYSTEM_CheckMixture` / `GetStuff*` | 合成链部分登记 | 先完成材料查询调用链 | 扩展材料是否可被配方识别 | 未完成 |
| `MIXSYSTEM_UseStuff` / `MIXSYSTEM_MakeItem` | ABI/部分实现 | 上层分发；产物进入统一入库策略 | 材料消耗、产物创建、堆叠 | 未完成 |
| `UIStore_*` / `DEALSYSTEM_*` | 商店购买/卖出部分已有 | 先确认商品槽与背包入库是否同源 | 购买、出售、buyback、价格和所有权 | 部分实现 |
| `MAPITEMSYSTEM_*` / `CHAR_PickItemAll` | 掉落实体和生成链未完整 | 先冻结掉落对象与入库调用链 | 拾取、范围拾取、堆叠、满包 | 未完成 |
| `SAVE_SaveInventory` / `SAVE_Save` / `SAVE_LoadInventory` | P6 保存协调已完成 | **暂缓新增 Hook** | sidecar、原版保存、自动保存、重启恢复 | 已完成；存档面板语义另由 ADR-009 决定 |
| `SAVE_SaveItem` / `SAVE_LoadItem` | 原版序列化函数已登记 | 暂缓扩展接管 | payload 与原版记录格式 | 暂缓 |
| `UIEquip_*` / `UIMix_*` / `UIStore_*` | 扩展 UI 投影和部分控件适配存在 | 优先 PtrHook/上层逻辑，不默认 Native Hook | 点击、绘制、选中、说明和输入 | 部分实现 |

## 5. 旁路迁移策略

### 5.1 可以被统一原版入口替代的候选

优先检查以下候选，而不是直接删除：

1. `INVEN_GetEmptyBagSlot`：若它是所有“寻找首个入库槽”的共同入口，可统一原版袋优先和扩展 fallback。
2. `INVEN_SaveItem` / `INVEN_SaveItemOnEmpty`：若创建者都通过这些函数提交物品，可统一所有权移交和满包处理。
3. `INVEN_FindItem` / `HaveItem` / `GetItemCount`：已具备统一查询价值，但不能代替生产者接入。
4. `INVEN_IsHavingEmptySlot`：可统一“是否还能放入 N 件”的门禁，但不能替代实际入库事务。
5. `INVEN_ConsumeItem` / `RemoveItem`：已具备统一扩展对象消费和删除价值；应作为原版消费时机的扩展后端，而不是被上层重复调用或被扩展业务逻辑替代。

### 5.2 必须保留的旁路

- 扩展→扩展移动：原版没有逻辑槽和 payload 事务契约。
- 扩展对象装备/脱装备事务：除非原版函数能接受借用对象且不释放/写入错误物理槽。
- 扩展对象删除与对象所有权：不能交给 `RemoveItemDirect`/`ITEMPOOL_Free`。
- 扩展 UI 投影、选中、拖动和任务袋门禁：原版控件树不认识逻辑袋。
- P6 sidecar 保存协调：在保存冲突审计完成前不得被 P7 Hook 替代。

### 5.3 使用/消费旁路的迁移规则

- 普通 `CHAR_UseItemEx` 路径：保留扩展逻辑槽校验和物化，删除任何成功后的重复扣数量/清槽；由 `ConsumeItem` Hook 的扩展分支完成状态提交。
- 强化和宝石路径：保留原版效果函数；扩展材料分别通过 `ConsumeItem` 或 `RemoveItem` 适配，不在上层复制原版效果。
- 骰子、解封和开箱：在确认原版函数是否消费源对象、是否按指定对象操作、产物是否进入正确库存前，不得删除现有上层消费或产物适配。
- 任何扩展对象不得进入 `INVEN_RemoveItemDirect`、`ITEMPOOL_Free` 或只接受物理槽的原版写入链。
- “可退役”只表示移除重复的上层原版业务模拟；`extension_bag_consume_native_item`、`extension_bag_remove_native_item` 及其 payload/所有权/UI 子流程不因原版 Hook 存在而退役。

### 5.4 退役判定

任何旁路退役必须单独提交检查记录，至少包含：旧入口、新入口、全部调用者、原版回归、扩展回归、堆叠上限、任务袋 sentinel、对象审计、保存边界和真机结果。没有完整证据时，旁路继续保留。

## 6. 实施顺序

1. **先做阶段 0/1**：只依据汇编、已有文档和源码冻结 `GetEmptyBagSlot`、`SaveItem`、`FindSaveSlot`、`MoveItem` 和所有生产者的调用链；不修改保存、不上真机。
2. **阶段 2/3（已完成，仅作冻结基线）**：读取支持矩阵和旁路裁决，不重复判断 `GetEmptyBagSlot`、生产者调用者或旁路边界；只有新实现证据推翻具体结论时才做定向复核。
3. **阶段 4 做 Host 对照**：覆盖原版满/有空、扩展有空/全满、堆叠同异身份、不可堆叠和失败回滚。
4. **阶段 5 再进行单项真机验证**：只验证静态分析已经明确、且有明确验收变量的调用链。
5. **按调用者迁移旁路**：真机路径稳定后，逐个删除重复的原版业务模拟；普通使用、强化和宝石优先迁移，不能整体删除扩展事务。
6. **最后处理保存冲突**：比较 P6 保存协调器、原版自动保存、`SAVE_*` 与 `INVEN_*` 保存函数，再决定是否需要 Hook。
7. **完成全局真机验收**：通过后才更新 control-plane 的 P7 状态；旧文档仅作历史 ABI 记录，不再作为开发顺序依据。

## 7. 当前待实现函数列表（按优先级）

### A. 最高优先级：可能统一全局入库

- `INVEN_GetEmptyBagSlot`：静态确认仅扫描物理袋且无精确 BL caller；不作为统一扩展入口。
- `INVEN_FindSaveSlot`：已冻结 ABI、返回值和物理槽契约；仅作原版物理袋预检。
- `INVEN_SaveItem`：已冻结 ABI、主要生产者调用链和堆叠释放边界；扩展对象仍不得直接传入。
- `INVEN_SaveItemOnEmpty`：已确认仅面向目标原版袋；扩展 fallback 保留在上层事务。
- `INVEN_IsEmptyBag`：统一满包/空袋判断，但不伪造扩展物理槽。
- `INVEN_IsHavingEmptySlot`：补齐递归、安装回滚和真机全生产者验收。

### B. 高优先级：查询和堆叠边界

- `INVEN_FindItemSlot`：只完成扩展逻辑映射接口，不返回虚假物理槽。
- `INVEN_MoveItem`：ABI 已登记；原版物理移动与扩展三方向事务继续隔离。
- `ITEMSYSTEM_Divide`：已确认返回新 UID 对象但不入库；继续由上层接管落位和所有权。
- `INVEN_GetCumulateSaveSlotEx`：先与保存范围隔离，确认是否存在非保存调用者。
- `INVEN_RemoveItemData`：已确认仅扫描物理袋并进入 `RemoveItemDirect`/`ITEMPOOL_Free`；扩展对象禁止进入。

### C. 生产者接入

- `ITEMSYSTEM_CreateItem`、`ITEMSYSTEM_MakeItem`、`ITEMSYSTEM_ProcessUnpack`
- `MAPITEMSYSTEM_CreateItem`、`CHAR_PickItemAll`
- `MIXSYSTEM_UseStuff`、`MIXSYSTEM_MakeItem`
- `UIStore_*`、`DEALSYSTEM_*`

> `NetworkStore_AddItem` 已从当前 P7 生产者接入清单移除；相关符号、共享堆叠布局 patch 和 P6 保存调用点仅保留原版运行/历史审计用途。

### D. 明确暂缓

- `INVEN_CalculateEmptySlotCountForSave`
- `INVEN_GetEmptySaveSlotEx`
- `INVEN_GetNeededSaveSlotEx`
- `SAVE_SaveInventory`、`SAVE_Save`、`SAVE_LoadInventory`
- `SAVE_SaveItem`、`SAVE_LoadItem`

暂缓不是放弃；待 P6 保存协调与原版自动保存冲突检查完成后重新评估。

## 8. 验收要求

- ABI：`game_symbols.h` 唯一来源，动态符号优先，VMA 只作 fallback；运行 `uv run python scripts/maintenance/check_symbols.py`。
- 静态：`git diff --check`，相关 Host tests，依赖方向和锁边界审计。
- 构建：只能运行 `scripts/build-debug.sh` 或 `scripts/build-release.sh`。
- 前期验证：阶段 0–4 只使用原版汇编、已有文档、当前代码、符号表、静态脚本和 Host tests，禁止直接真机验证。
- 真机：阶段 5 以后才使用唯一主验收设备 `192.168.3.54:5555`；原版/扩展开关、拾取、任务物品、奖励、开箱、合成、商店、装备、堆叠、满包和重启逐项验证。
- 证据：记录 APK 身份、配置、调用链、原版物理袋、扩展逻辑袋、payload、数量、所有权、日志、回滚和用户结论。
- 结论：函数只有在 ABI、实现机制、旁路迁移决定和真机行为全部明确后，才能标记为完成。

## 9. 与旧文档的关系

- 本文替代 `native-inventory-hook-development.md` 的开发顺序和函数决策。
- `native-inventory-hook-development.md` 保留为早期 Native Hook PoC/ABI 历史记录，不能单独作为 P7 完成依据。
- `control-plane.md` 仍是扩展背包总体控制面；本文只负责 P7 原版库存函数的全局接入检查、Hook 必要性和旁路迁移决策。
