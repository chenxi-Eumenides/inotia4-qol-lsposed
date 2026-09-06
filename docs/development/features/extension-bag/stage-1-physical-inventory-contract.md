# P7 阶段 1：原版物理库存契约

> 状态：STATIC-COMPLETE（静态契约已闭合；运行时未知已归入阶段 5/P6）
> 日期：2026-09-06
> 前置：阶段 0 `STATIC-COMPLETE`，证据见 `stage-0-static-inventory-audit.md` §6–§7。
> 边界：本阶段只定义原版物理库存，不把扩展逻辑袋伪装成原版袋，也不进行真机验证。

## 1. 物理容器

| 编码 | 语义 | 阶段 1规则 |
|---:|---|---|
| `0..5` | 原版物理数组袋域 | `INVEN_pItem` 包含这些袋；具体函数是否纳入任务袋由类别或参数决定 |
| `0..4` | 普通物品接入域 | 扩展“原版袋优先”策略默认只把这些袋作为普通物品目标 |
| `5` | 任务物品袋 | 项目策略禁止扩展逻辑使用；原版函数可扫描/接受该索引的事实不等于所有调用者均为任务语义 |
| `6..10` | 扩展逻辑袋 | 不属于 `INVEN_pItem` 物理数组；不得传给原版物理删除、保存或槽位编码函数 |

`INVEN_pItem` 的物理槽布局由原版袋大小和槽数组决定。`INVEN_GetBagSize` 对 `0..5` 读取对应袋对象 `+0x10` 字段的 bit `0..24`；无效袋或空袋对象返回 `0`。每袋物理数组步长为 `0x80`，每个槽是 8 字节对象指针，数组宽度最多 16 槽，但有效容量由袋对象字段决定。各函数是否扫描全部 16 个数组槽或仅扫描容量范围，必须按函数单独记录，不能由数组布局推导。已由原版 caller 证明物理槽编码为 `bag = encoded >> 5`、`slot = encoded & 0x1f`；它只描述 `INVEN_pItem` 中的真实对象，不能承载扩展逻辑袋编号。

`INVEN_GetEmptyBagSlot` 是独立的空袋查询：逐项检查物理袋指针 `0..4`，返回第一个空袋索引 `0..4`，全部存在时返回 `-1`；不检查任务袋 `5`。它不是空槽编码查询，也不是统一的全局入库入口。

任务袋 `5` 的处理必须按函数分类，不能把“物理数组为 `0..5`”简化成“所有入口均扫描 `0..5`”：`FindItem`、`HaveItem`、`GetItemCount`、`RemoveItemData` 等原版查询/类别操作包含 `5`；`IsHavingEmptySlot` 由 `include_task_bag` 决定是否包含 `5`；`GetEmptyBagSlot` 固定只返回 `0..4`；`SaveItemOnEmpty` 原版函数本身接受调用者传入的袋号，因此扩展普通物品入口必须在调用前拒绝 `5`。模块的 `inventory_item_ref_at` 作为物理查询引用可读取 `0..5`，但 remove/use/discard/sell/move 入口必须在调用前拒绝 `5`。扩展逻辑袋 `6..10` 在任何情况下都不进入该物理数组。

## 2. 函数契约

| 函数 | ABI | 成功/失败 | 物理副作用 |
|---|---|---|---|
| `INVEN_FindSaveSlot` | `int(void* item, int8_t* out_slot)` | 返回 `1/0`；成功通过 `out_slot` 写入槽编码 | 只扫描原版物理袋并检查空槽/同身份堆叠容量；不转移对象所有权 |
| `INVEN_SaveItem` | `int(void* item, void* context)` | 已确认空指针/无槽失败和成功布尔路径；`context` 语义尚未冻结 | 先查物理槽，再调用 `SaveItemDirect`；会更新快捷栏和任务事件；堆叠合并可能释放传入对象 |
| `INVEN_SaveItemDirect` | `int(void* item, int32_t bag, int32_t slot)` | 返回寄存器值不能解释为布尔、槽位、旧对象或合并结果；已检查的 `SaveItem`/`SaveItemOnEmpty` caller 未使用该值 | 写入物理数组或合并数量；合并成功路径调用 `ITEMPOOL_Free(item)` |
| `INVEN_SaveItemOnEmpty` | `int(void* item, int32_t bag)` | 成功路径返回值进入成功分支；各失败分支的统一返回值尚未完全冻结 | 在调用者指定的物理袋内扫描空槽并调用 `SaveItemDirect`；普通物品路径禁止传任务袋 `5`，扩展逻辑袋不可作为参数 |
| `INVEN_RemoveItem` | `int(void* item)` | 返回值语义尚未完全冻结；不得据此证明槽非空、实际删除或对象已释放 | 只接受物理数组中的对象；内部可进入 `RemoveItemDirect` |
| `INVEN_RemoveItemDirect` | `int(int32_t bag, int32_t slot)` | 非空槽成功时返回被删物品类别；空槽路径返回值不可依赖 | 读取物理对象、`ITEMPOOL_Free`、清空槽、更新快捷栏 |
| `INVEN_ConsumeItem` | `void(void* item)` | 无成功返回值 | 可堆叠物数量减一；不可堆叠或数量不大于一时进入 `RemoveItem` |
| `INVEN_SaveItemData` | `int(int32_t category, int32_t count)` | `category == 0` 或 `count <= 0` 走原版特殊成功路径；创建/保存失败返回 `0` | 创建物品并重复调用 `SaveItem`；失败按类别/数量调用 `RemoveItemData`，不是 UID 精确回滚 |
| `INVEN_RemoveItemData` | `int(int32_t category, int32_t count)` | 返回值不可依赖；`count == -1` 表示全量，数量不足时可能部分完成 | 只扫描物理袋 `0..5`，堆叠减少或整槽释放 |
| `INVEN_GetBagSize` | `int(int32_t bag)` | `bag` 非 `0..5` 或袋对象为空时返回 `0` | 读取原版袋容量字段；不创建、不释放、不改变槽 |
| `INVEN_IsHavingEmptySlot` | `int(int32_t needed, int32_t include_task_bag)` | `needed <= 0` 返回 `0`；找到满足数量的空槽返回 `1`，扫描结束返回 `0` | `include_task_bag == 0` 扫描 `0..4`，非零时扫描 `0..5`；不涉及扩展逻辑袋 |
| `INVEN_FindItemSlot` | `int(void* item, int8_t* out_slot)` | 成功写 `out_slot` 并返回 `1`，失败返回 `0` | 反查原版物理槽；不得写入扩展逻辑槽编码 |
| `INVEN_FindItem` | `void*(int32_t category)` | 找到返回原版对象指针，否则返回空指针 | 扫描物理袋 `0..5`；扩展查询只能由上层聚合 |
| `INVEN_HaveItem` / `INVEN_GetItemCount` | `int(int32_t category)` | 返回原版查询结果；不代表扩展逻辑库存总量 | 只对原版物理库存进行类别查询/计数 |
| `INVEN_IsEmptyBag` | `int(int32_t bag)` | 返回指定袋查询结果；具体空袋边界以原版函数体为准 | 只查询物理袋，不创建或释放对象 |
| `INVEN_CalculateEmptySlotCountForSave` | `int(int32_t bag, int32_t needed)` | 返回待保存物品所需空槽数量；边界分支仍需逐函数反汇编闭合 | 原版保存槽计算辅助；不得传入扩展袋号 |
| `INVEN_GetEmptySaveSlotEx` | `int(int32_t bag, int32_t needed, int8_t* out_slot, int32_t mode, int32_t* out_count)` | 物理扫描范围、输出写入点和数量更新已登记；失败返回/输出保留规则仍未知 | 原版保存槽查询辅助；不得伪造扩展物理槽 |
| `INVEN_GetNeededSaveSlotEx` | `int(int32_t bag, int32_t needed, int8_t* out_slot, int32_t mode, int32_t* out_count, int8_t* out_extra, int32_t extra)` | 组合空槽与堆叠槽查询；两个输出参数的失败规则仍未知 | 原版保存槽查询辅助；不得传入扩展袋号 |
| `INVEN_GetCumulateSaveSlotEx` | `int(int32_t bag, int32_t needed, int8_t* out_slot, int32_t mode, int32_t* out_count)` | 堆叠扫描和输出更新点已登记；边界及失败输出规则仍未知 | 原版堆叠槽查询辅助；不得传入扩展对象 |
| `INVEN_CheckSaveInNotEmptySlot` | ABI/VMA 已登记，参数角色与返回契约未冻结 | 不得据名称推导返回值、扫描袋域或副作用 | 阶段 1只登记为待审计原版辅助；不得传入扩展对象 |

## 3. 对象所有权

1. `ITEMSYSTEM_CreateItem`、`ITEMSYSTEM_MakeItem` 和 `ITEMSYSTEM_Divide` 返回的是 native 对象，不代表已入库。
2. 创建成功后，caller 必须明确选择：交给原版 `SaveItem`，或交给扩展逻辑事务；不能两者同时接管。
3. 传入 `SaveItem` 前不能假设 caller 永远保留对象：空槽写入通常转移对象，堆叠合并可能立即释放对象。
4. 进入原版物理数组的对象只能由原版删除路径释放；扩展对象由扩展对象账本释放，不能进入 `ITEMPOOL_Free`。
5. 任意失败回滚必须先判断对象是否已被 `SaveItem` 合并释放，再决定是否释放 caller 持有的临时对象；`SaveItemData` 的类别/数量回滚不能视为 UID 精确回滚。

## 4. 阶段 1支持矩阵

| 行为 | 原版物理基线 | 扩展接入策略 |
|---|---|---|
| 查询/计数 | 原版查询函数（如 `FindItem`/`GetItemCount`）扫描物理袋 `0..5`；任务袋 `5` 仍按原版类别语义参与 | 只读快照可包含并标识任务袋 `5`；扩展逻辑另行聚合，不把该范围当作普通入库目标 |
| 入库 | `FindSaveSlot` + `SaveItem` 只负责原版物理袋 | 原版袋优先；原版失败后由上层转扩展事务 |
| 使用/消费 | `CHAR_UseItemEx` 决定效果，`ConsumeItem` 决定消费路径 | 扩展对象由逻辑槽门禁和物化层适配，原版决定效果/时机 |
| 删除 | `RemoveItem`/`RemoveItemDirect` 可能释放 native 对象 | 扩展对象截断在逻辑删除层，禁止进入物理释放原语 |
| 移动/合并 | `MoveItem(item,count,target_bag,target_slot)` 先反查源槽；源袋与目标袋必须同属普通袋或同属任务袋；同身份堆叠上限由函数内 `99`/patch 决定 | 扩展三方向事务保留，待阶段 2/3决定逐项迁移 |
| 保存 | `INVEN_*` 不等于磁盘保存；完整 `SAVE_Save` 才是 P6提交边界 | 阶段 1不改保存入口、不重复提交 sidecar |

## 5. 待阶段 5验证

- 原版库存函数的真实线程、重入和原版锁边界。
- 堆叠合并、拆包、合成、商店购买/出售和装备交换失败时的完整回滚。
- `CHAR_UseItemEx` flag 对不同类别的消费时机。
- UI、快捷栏、任务状态和 frame cache 的最终一致性。
- 自动保存、任务奖励、拾取、开箱和快捷键的真实时序。

## 6. 生产者目标域基线

| 生产者链 | 静态目标域 | 阶段 1可冻结规则 | 未决项 |
|---|---|---|---|
| 拾取 | 普通物品物理袋 `0..4` | `CHAR_ActivePickupEvent` 最终调用 `INVEN_SaveItem`；扩展 fallback 不得改变原版优先顺序 | 满包对象保留顺序、堆叠合并所有权和回调层 fallback 时序待运行时验证 |
| 任务物品/奖励 | 任务语义候选为袋 `5`；普通奖励为 `0..4`；也可能是临时对象 | 任务对象必须先经任务语义判定，不能默认改写为普通物理入库或扩展入库 | 最终落袋、奖励失败回滚和任务完成时序未闭合 |
| 物品箱/拆包 | 普通产物物理袋 `0..4`；输入/产物在提交前是临时对象 | 产物交给 `SaveItem` 前由 caller 持有；不得写任务袋或扩展袋 | 输入消费时机、部分失败回滚和具体袋位待阶段 5 |
| 混合/制作 | 原版产物为物理袋 `0..4`，部分分支原地修改对象 | 不得把所有 MIX 分支实现成“生成→保存→删材料” | 材料消费顺序、失败原子性和输出所有权未闭合 |
| 商店购买 | 物理袋 `0..4`；货架对象在提交前是临时对象 | `FindSaveSlot` 只作物理预检，最终由 `SaveItem` 入库；不转任务袋或扩展袋 | 扣钱、货架清理与入库失败的原子性未闭合 |
| 装备/卸下 | 装备槽独立；卸下目标为普通物理袋 `0..4` | 装备不是库存生产；扩展对象必须走适配事务，不进入物理释放原语 | 满包交换和旧装备所有权未闭合 |
| 普通使用 | 来源可为物理袋或扩展逻辑袋；效果阶段可有临时物化对象 | 原版负责效果和消费时机；扩展层负责物化、逻辑槽同步和截断释放 | 全类别消费时机及快捷键覆盖待阶段 5 |

生产者静态证据见 `stage-0-static-inventory-audit.md` §4–§5、`inventory-integration-decision-plan.md` §生产者矩阵；`NetworkStore_AddItem` 为历史链，不计入当前 P7 覆盖率。

阶段 5 的九项真机验收与本阶段七类静态分组对应关系固定为：`拾取→拾取`；`任务物品/奖励→任务物品、任务奖励`；`物品箱/拆包→开箱、拆包`；`混合/制作→合成`；`商店购买→商店购买`；`装备/卸下→装备`；`普通使用→使用`。阶段 5仍须按九项分别记录证据，不得用七类静态行合并替代真机验收。

## 7. 静态证据定位

- `INVEN_GetBagSize` / `INVEN_GetEmptyBagSlot`：`libgame-arm64.objdump.txt` `0x103250..0x1032dc`；`GetEmptyBagSlot` 明确扫描 `0..4`，成功返回索引，失败返回 `-1`。
- `INVEN_IsEmptyBag`：`0x1032e0..0x10345c`，按指定袋和容量逐槽检查对象指针，返回空/非空布尔值。
- `INVEN_IsHavingEmptySlot`：`0x103460..0x1036fc`，`needed<=0` 返回 `0`，`include_task_bag` 决定扫描 `0..4` 或 `0..5`，满足空槽数返回 `1`。
- 物理数组基址和槽步长：`INVEN_FindSaveSlot` `0x103a54..0x103a68`、`INVEN_RemoveItemData` `0x1040e8..0x104110`。
- `INVEN_FindSaveSlot` 的输出槽写入：`0x103ba0..0x103bb8` 对第二参数执行 `strb`。
- `INVEN_SaveItem` 第二参数：模块当前传 `nullptr`；原版调用者的 context/source 语义尚未闭合，扩展不得假设所有路径均可替换为 `nullptr`。
- 槽编码：`inventory-remove-save-functions.txt:104070..10407c` 和 `104598..1045a8` 均以 `encoded >> 5` 解出 bag、`encoded & 0x1f` 解出 slot；当前冻结为 `int8_t` 输出的 5-bit slot 编码。
- `INVEN_SaveItemOnEmpty`：`0x104bf8..0x104d3c` 读取指定物理袋容量并扫描 16 个槽，`0x104d40..0x104d54` 调用 `SaveItemDirect` 后返回 `1`。
- `INVEN_MoveItem`：`0x104970..0x1049e4` 反查源槽并禁止普通袋/任务袋跨域移动，`0x104a80..0x104ae0` 处理同身份堆叠，`0x104b38..0x104b98` 处理拆分后写入目标槽。
- `INVEN_RemoveItem` / `INVEN_RemoveItemDirect`：`0x104054..0x104084` 反查并调用删除，`0x103ffc..0x104030` 释放、清槽并返回物品类别。
- `INVEN_SaveItemData` / `INVEN_RemoveItemData`：`0x104614..0x10473c` 批量保存及类别/数量回滚，`0x1040a8..0x10425c` 遍历物理袋并允许部分删除。
- `INVEN_CheckSaveInNotEmptySlot` / `INVEN_CalculateEmptySlotCountForSave`：`0x103d78..0x103f0c`；前者当前仅冻结 VMA/存在性，参数角色、返回值和扫描域仍未知；后者为保存所需空槽计算辅助。
- 保存槽辅助：`0x105070` 的 `GetEmptySaveSlotEx` 依 `include_task_bag` 扫描 `0..4` 或 `0..5`，向槽输出写编码并更新数量；`0x1051b0` 的 `GetCumulateSaveSlotEx` 写堆叠槽编码并更新数量；`0x105440` 的 `GetNeededSaveSlotEx` 组合两者。
- 当前可复核反汇编证据：`.tmp/p7-implementation/libgame-arm64.objdump.txt`、`inventory-consume-saveempty.txt`、`inventory-remove-save-functions.txt`、`stage-0-bl-context.txt`；这些是任务目录内的可重建中间证据，不等同于真机证据。

阶段 1不得以这些未知项为理由伪造扩展支持，也不得在未完成阶段 2/3决策前扩大 Hook 或删除旁路。

## 8. 阶段 1闭合与后续边界

已完成的静态收口：物理数组 `0..5`、普通目标 `0..4` 与任务袋 `5` 的编号边界；物理槽编码 `bag = encoded >> 5`、`slot = encoded & 0x1f`；核心保存、移动、消费和删除函数的主要所有权边界；对应反汇编中间证据已保存在 `.tmp/p7-implementation/`。

阶段 1已完成静态闭合；以下 5 类内容明确转入后续阶段，不构成阶段 1阻塞项：

1. 保存辅助函数的 ABI、物理扫描范围和已观察输出写入点已登记；失败返回/输出保留细节以及 `CheckSaveInNotEmptySlot` 的参数角色仍明确列为后续保存审计项，不在阶段 1伪造结论。
2. `SaveItem` 的 `context` 对原版调用者仍保持 unknown；该未知不影响物理 ABI，已明确禁止把所有原版 caller 统一改传 `nullptr`。
3. 拾取、奖励、拆包、合成、商店、装备/卸下、普通使用七类链路已完成静态目标域登记；最终运行时落点、时序和回滚转阶段 5/P6。
4. 已完成任务袋 `5` 的扩展入口排除审计，以及物理数组 `0..5`、普通接入 `0..4`、扩展逻辑 `6..10` 的文档对账。
5. `git diff --check`、Host 测试、符号检查和 Debug 构建已通过；真机验证、旁路退役和 P7 放行明确不属于本阶段。
