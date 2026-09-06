# P7 阶段 0：全局库存静态审计记录

> 状态：STATIC-COMPLETE（静态可证明项已闭合；运行时未知已固定归入阶段 5或 P6）
> 核对日期：2026-09-06
> 范围：只记录仓库现有源码、符号表、登记表和开发文档证据；不包含真机推断。

> 范围调整：`NetworkStore_AddItem` 的原版调用链保留为历史审计证据，但网络商店已移出当前 P7 扩展背包接入与验收范围；不得将其作为当前 P7 待实现阻断项。相关原版符号、共享堆叠布局 patch 和 P6 保存记录继续保留。

## 1. 证据边界

当前仓库可找到 `apk/decompiled/libgame-symbols.txt` 和实际的 `apk/decoded/lib/arm64-v8a/libgame.so`。已使用项目内 NDK 的 `llvm-objdump` 生成临时反汇编，并按符号 VMA 匹配 `BL` 目标；该 ELF 已 strip，反汇编没有可靠的函数名标签，调用者归属使用符号范围匹配。因此：

- `game_symbols.h`、`symbol_registry.h`、`game_access.h` 只能冻结候选符号、VMA 和已登记 ABI；
- 源码搜索可以确认模块自身的直接调用者、返回值处理和事务副作用；
- 目标 VMA 的精确 `BL` 点可以作为二进制证据，但必须同时核对调用点上下文、参数和返回值；
- 未找到的调用者必须标记为未知，不得据此宣布全局 Hook 覆盖；若已完成静态扫描且该未知项已明确登记到阶段 5或 P6，则不阻断阶段 1的物理契约定义。

## 2. 核心符号与 ABI 基线

| 函数 | VMA | ABI | 当前静态结论 |
|---|---:|---|---|
| `INVEN_GetEmptyBagSlot` | `0x103280` | `int()` | 无模块源码直接调用；原版生产者未知 |
| `INVEN_FindSaveSlot` | `0x103960` | `int(void*, int8_t*)` | 返回是否找到，并通过第二参数写入物理槽编码；API 添加物品、商店购买调用 |
| `INVEN_SaveItem` | `0x104528` | `int(void*, void*)` | API 添加、商店购买、合成产物调用 |
| `INVEN_SaveItemOnEmpty` | `0x104be0` | `int(void*, int32_t)` | 扩展事务、装备物化、投影和恢复调用 |
| `INVEN_MoveItem` | `0x104934` | `int(void*, int, int, int)` | API 移动和 UI 合并调用 |
| `ITEMSYSTEM_Divide` | `0x1083f8` | `void*(void*, int32_t)` | 唯一已确认原版 caller 为 `UIStore_SellItem+0xf0`；新对象后续归属未知 |
| `ITEMSYSTEM_CreateItem` | `0x10be9c` | `void*(int32_t, int32_t, int32_t, int32_t)` | API、扩展 tabs、虚拟袋投影调用 |
| `ITEMSYSTEM_MakeItem` | `0x10c6c8` | `void*(int32_t, int32_t, int32_t)` | 无模块源码直接调用 |
| `ITEMSYSTEM_ProcessUnpack` | `0x10ce50` | 暂记 `int(int32_t)`，需继续核对 | 唯一已确认原版 caller 为 `CHAR_UseItemEx+0x258`；产物入库和材料删除链部分已确认 |
| `CHAR_UseItemEx` | `0xeb670` | `int(void*, void*, int)` | 原版使用、扩展使用和装备物化调用 |
| `CHAR_ProcessShortcut` | `0xec028` | `int(void*, int)` | `GAMESTATE_PressKeyPlay+0x54`（`0x9d014`）直接调用 |
| `CHAR_PickItemAll` | `0xec4d8` | `int(void*, int32_t)` | 无模块源码直接调用；拾取旁路保留 |
| `MIXSYSTEM_MakeItem` | `0x11af58` | `int(int32_t, void**)` | 批量宝石合成调用 |
| `NetworkStore_AddItem` | `0x15d640` | 暂记 `int(void)`，需继续核对 | 唯一已确认原版 caller 为 `NetworkStore_Process+0x198`；入库/保存回滚语义未完全冻结 |
| `INVEN_ConsumeItem` | `0x1047bc` | `void(void*)` | 多个显式消费点及 Hook |
| `INVEN_RemoveItem` | `0x104044` | `int(void*)` | API 删除及 Hook |
| `INVEN_RemoveItemDirect` | `0x103fd8` | `int(int32_t, int32_t)` | 物理删除原语；扩展对象禁止进入 |
| `INVEN_RemoveItemData` | `0x1040a8` | `int(int32_t, int32_t)` | 按类别/数量遍历物理袋，直接删除或减少堆叠 |
| `INVEN_SaveItemData` | `0x104614` | `int(int32_t, int32_t)` | 按类别/数量创建并调用 `INVEN_SaveItem`，失败时回滚 |

补充：`INVEN_pItem` 位于 `0x7131c0`；原版物理袋为 `0..4`，任务袋为 `5`，扩展逻辑袋为 `6..10`。`ITEMPOOL_Free` 登记为 `0x108160`，扩展对象不得进入该释放链。

ABI 校正：`INVEN_FindSaveSlot` 的函数体在 `0x103af0`、`0x103ba4` 通过第二参数写入 `strb`，因此 ABI 为 `int(void*, int8_t*)`，返回值仅表示是否找到；`INVEN_SaveItem` 为 `int(void*, void*)`。两个模块调用点已改为传递槽位输出缓冲区并只使用布尔返回值。`ITEMSYSTEM_ProcessUnpack` 的 ABI 已冻结为 `int(int32_t)`；`NetworkStore_AddItem` 仍仅保留历史审计。

## 3. 已确认的模块直接调用者

| 函数/流程 | 直接调用位置 | 返回值/副作用 |
|---|---|---|
| 查槽、保存物品 | `api/native/game_inventory_basic.inc:53-67`、`api/native/game_shop.cpp:51-54` | 检查满包和保存失败；成功后原版接管对象 |
| 保存到空槽 | `feature/extension_bag/extension_bag_transaction.inc:202`、`extension_bag_runtime.inc:729-730`、`extension_bag_equip.inc:119`、`game_ui_virtbag.cpp:360` | 失败回滚或释放临时物化对象；涉及所有权和投影 |
| 移动/合并 | `api/native/game_inventory_use.inc:195-201`、`feature/patch/game_patch_move_merge.inc:68-81` | 检查返回值并刷新源/目标投影 |
| 创建物品 | `api/native/game_inventory_basic.inc:53`、`extension_bag_tabs.inc:31`、`game_ui_virtbag.cpp:331` | null 检查、堆叠限制、数量写入 |
| 使用 | `api/native/game_inventory_use.inc:101`、`extension_bag_api_impl.inc:179`、`extension_bag_equip.inc:558` | `CHAR_UseItemEx` 返回值决定成功；成功消费由原版/Hook 链处理 |
| 消费 | `api/native/game_inventory_use.inc:45,75,90`、`api/native/game_inventory_equipment.inc:41` | 多数调用只做空指针检查；原版普通使用的消费时机来自 `CHAR_UseItemEx` 内部证据 |
| 删除 | `api/native/game_inventory_basic.inc:86`、`api/native/game_inventory_use.inc:155,174`、`feature/patch/game_patch_craft.inc:83` | `RemoveItemDirect` 返回值不可信；扩展删除必须留在逻辑事务 |
| 合成 | `feature/patch/game_patch_craft.inc:73,77,83` | 创建产物、保存产物、删除材料；失败所有权边界仍需补证据 |
| 拾取 | `api/native/game_world_movement.inc:4-35` | `nav_pick_items` 通过 notifier 排队主线程；注释明确 `NOTIFIER_Process` → `CHAR_ActivePickupEvent(0xdd15c)` → `INVEN_SaveItem` 并移除掉落 |

## 4. 已登记的二进制调用证据

- `CHAR_ProcessShortcut`：`0xec1a4` 调用 `INVEN_FindItem`，`0xec1f8` 调用 `CHAR_UseItemEx`。
- `INVEN_ConsumeItem`：文档登记 `0x104830` 的不可堆叠分支尾跳 `INVEN_RemoveItem`。
- `SAVE_Save` 的保存调用点已登记，但 P7 按方案暂缓接管保存入口。

目标 VMA 匹配生成的初筛记录位于 `.tmp/p7-implementation/stage-0-bl-callers.txt`，调用上下文位于 `.tmp/p7-implementation/stage-0-bl-context.txt`。当前提取了 **17 个目标函数** 的精确 BL 结果：

| 目标 | 精确 BL 数量 | 关键调用者/结论 |
|---|---:|---|
| `INVEN_FindSaveSlot` | 5 | 装备、商店、脱装备、`INVEN_FindItem` 内部 |
| `INVEN_SaveItem` | 18 | 装备/佣兵、合成、商店、`CHAR_ActivePickupEvent`、`CHAR_UseItemEx`、事件系统、创建/制造、地图构建 |
| `INVEN_SaveItemOnEmpty` | 1 | `UIEquip_InvenBagControlEventProc+0xb8cc0`；返回值在 `0xb8cc4`/`0xb8cc8` 使用；扩展模块自身调用点不属于 libgame BL 结果 |
| `INVEN_MoveItem` | 1 | `UIEquip_InvenItemControlEventProc+0xb9298`；返回值在 `0xb929c`/`0xb92a0` 使用 |
| `INVEN_ConsumeItem` / `INVEN_RemoveItem` | 5 / 5 | 装备、BUFF、合成、商店、移动、佣兵等；必须保留原版 backup 语义 |
| `ITEMSYSTEM_CreateItem` | 37 | 角色处理、事件、强化、拆包、制造、地图添加、任务和地图构建 |
| `ITEMSYSTEM_MakeItem` | 10 | `CHARSYSTEM_DropItem` 5 个分支、`DEALSYSTEM_MakeSale` 5 个分支 |
| `ITEMSYSTEM_Divide` | 1 | `UIStore_SellItem+0xf0`（`0xd26e0`），拆堆发生在商店出售流程 |
| `ITEMSYSTEM_ProcessUnpack` | 1 | `CHAR_UseItemEx` 的开箱/拆包分支 |
| `MIXSYSTEM_MakeItem` | 5 | 合成 UI 与 `UIMix_StartMix` |
| `NetworkStore_AddItem` | 1 | target `0x15d640`；`NetworkStore_Process+0x194` 的 callsite 为 `0x15d940`，返回值在 `0x15d944`/`0x15d948` 使用 |
| `CHAR_ProcessShortcut` / `CHAR_PickItemAll` | 1 / 1 | `GAMESTATE_PressKeyPlay`，返回值均被后续分支使用 |
| `INVEN_GetEmptyBagSlot` | 0 | 未发现精确 BL，不能视为统一入库入口；函数指针/GOT 间接目标也未解析到该 VMA |

新增关键证据：`CHAR_ActivePickupEvent+0xd4`（`0xdd2c4`）直接调用 `INVEN_SaveItem`；`CHAR_UseItemEx+0x258`（`0xebec8`）直接调用 `ITEMSYSTEM_ProcessUnpack`；`NetworkStore_Process+0x194`（`0x15d940`）直接调用 `NetworkStore_AddItem`。这些结果证明生产者范围比模块源码更广，但仍需逐点核对参数、返回值、对象释放和线程上下文。

间接控制流结论：反汇编中存在大量 `BLR`、寄存器 `BR` 和普通 `B`，但没有发现能解析到 P7 目标 VMA 的 `BLR`、尾跳或 PLT/GOT veneer。未知 `BLR` 不计入生产者；模块 `fn_*` 函数指针调用只证明模块侧 caller，不能替代原版调用链审计。

当前精确匹配统计为 3,178 条 `BLR`、649 条寄存器 `BR`、14,964 条立即数 `B`；这些数量是全 `.text` 控制流统计，不是 P7 调用者数量。

可复现输入：`libgame.so` SHA-256 为 `5f1388ec63fe3a44acd4755cb53296b88d2918a9cfebbe21e4b2a0e0415dd08e`；工具为项目内 NDK `llvm-objdump` LLVM 17.0.2。

## 5. 调用语义冻结矩阵（当前已知部分）

| 函数/调用点 | 返回值 | 对象/槽效果 | 线程/锁 | UI/缓存/持久化 |
|---|---|---|---|---|
| `INVEN_FindSaveSlot` / `INVEN_SaveItem` | `FindSaveSlot` 返回 `1/0`，成功时通过 `int8_t*` 写入物理槽编码；`SaveItem` 成功返回 `1`，空指针/无槽返回 `0` | `FindSaveSlot` 只做物理袋扫描和堆叠容量预检，不把扩展逻辑槽编码写入原版结构；`SaveItem` 随后调用 `SaveItemDirect`，堆叠合并时可能释放传入对象 | 原版线程未知；不能据此推断可在扩展袋锁内直接调用 | `SaveItem` 明确包含内存库存变更、`PLAYER_UpdateShortcut` 和 `QUESTSYSTEM_OnEvent`；不调用 `SAVE_Save`，不等于磁盘持久化 |
| `INVEN_SaveItemOnEmpty`，`0xb8cc0` | `int`，`uxtb` 后成功/失败分支 | `x0=[x21]`，`w1=w19`；目标袋首空槽入库语义已确认，扩展 wrapper 另行管理所有权 | 原版线程未知；扩展调用受事务锁边界约束 | 原版 UI/缓存/持久化副作用未知 |
| `INVEN_ConsumeItem`，`0xb82e8`、`0xb8390`、`0xb8ee4`、`0xeb7a8`、`0xeba54` | `void(void*)`，调用方无消费成功返回值可检查 | 原版消费时机及不可堆叠删除链需与扩展 Hook 分开记录 | 原版线程未知 | `0xb82f0`、`0xb8398` 有原版 UI 刷新；其它刷新未知 |
| `CHAR_UseItemEx`，`0xb8100`、`0xb84ec`、`0xec1f8`、`0xec4ac`、`0xec744` | `int`，均检查；第三参数存在 `0` 与 `1`，不能合并语义 | 成功是否必然消费需按 flag/物品类别核对；原版成功内部可进入 `ConsumeItem` | 原版线程未知 | 成功不等于磁盘保存；扩展投影和 frame cache 另行记录 |
| `ITEMSYSTEM_ProcessUnpack`，`0xebec8` | 暂记 `int(int32_t)`；返回值经 `uxtb` 后在 `0xebed0` 分支 | 先检查空槽，再创建完美物品并逐个 `INVEN_SaveItem`；保存失败释放当前产物，并在后续路径尝试 `INVEN_RemoveItem`/材料回滚；输入物品是否已消费、最终产物袋位和部分成功边界仍需确认 | 原版线程未知 | 失败提示已确认；UI、缓存、保存副作用和回滚完整性仍未闭合 |
| `ITEMSYSTEM_CreateItem` / `ITEMSYSTEM_MakeItem` / `ITEMSYSTEM_Divide` | 返回 native item 指针；多处只检查 null | `CreateItem` 先 `ITEMPOOL_Allocate`，成功后分配 UID 并初始化物品数据；部分初始化失败路径明确调用 `ITEMPOOL_Free`；`Divide` 要求可堆叠且拆分量小于原数量，通过 `ITEMSYSTEM_CopyAsNewUID` 创建新对象并原地减量；三者非空均不等于入库或所有权转移 | 原版线程未知 | `CreateItem`/`Divide` 本身不调用 `INVEN_SaveItem`；菜单、掉落、任务、网络临时对象可能各自不同，不能统一推断 |
| `NetworkStore_AddItem`，target `0x15d640`，唯一 callsite `0x15d940` | 暂记 `int(void)`；创建、入库或保存失败均可返回 `0`，成功返回 `1` | 先从接收数据创建对象并按商品数量设置数量；普通物品调用 `INVEN_SaveItem`，失败路径调用 `ITEMPOOL_Free`；`SAVE_Save` 失败路径调用 `INVEN_RemoveItemData` 并释放对象，但与堆叠合并释放、身份精确回滚的关系仍未闭合 | 原版线程未知；回滚顺序已确认但锁边界未知 | 已确认同时包含内存入库和 `SAVE_Save` 的生产者链，但不能称为完整原子链，也不能泛化到所有 `INVEN_SaveItem` 调用 |

矩阵约束：`memory_inventory_mutation`、`SAVE_Save` 是否发生、磁盘持久化、失败回滚、原版 UI 刷新、扩展投影刷新和 `op_ok()` frame cache 刷新必须分别记录；任何一项没有静态证据均保持 `unknown`。

### 5.1 新增函数本体证据

- `INVEN_SaveItem`（`0x104528`）：空指针直接返回 `0`；普通物品先调用 `INVEN_FindSaveSlot`，成功后读取堆叠数量并调用 `INVEN_SaveItemDirect`，随后调用 `PLAYER_UpdateShortcut` 和 `QUESTSYSTEM_OnEvent`，最终返回 `1`。金钱对象转入 `INVEN_AddMoney`。该函数自身没有 `ITEMPOOL_Free` 或 `SAVE_Save` 调用，但 `INVEN_SaveItemDirect` 的堆叠合并成功路径会释放传入对象，因此不能统一假定由 caller 接管对象。
- `INVEN_ConsumeItem`（`0x1047bc`）：不可堆叠或数量不大于 `1` 时尾跳 `INVEN_RemoveItem`；可堆叠物品只将数量减一并返回，无独立的成功返回值。该事实支持扩展消费 Hook 复用原版 backup，但不支持用返回值判定消费成功。
- `INVEN_RemoveItemDirect`（`0x103fd8`）：读取物理袋/槽对象后调用 `ITEMPOOL_Free`，清空物理槽，再更新快捷栏；这是物理库存删除原语，扩展对象不得进入。
- `INVEN_SaveItemData`（`0x104614`）：ABI 为 `int(int32_t category, int32_t count)`；正数数量循环创建对象并调用 `INVEN_SaveItem`，可堆叠物按最多 99 个拆分保存；任一创建或保存失败时以 `original_count - remaining_count` 调用 `INVEN_RemoveItemData` 回滚并返回 `0`。保存失败前的临时对象释放仍受 `INVEN_SaveItem` 堆叠路径影响，保持该边界为未知。
- `INVEN_RemoveItemData`（`0x1040a8`）：ABI 为 `int(int32_t category, int32_t count)`；遍历物理袋 `0..5`，可堆叠物按数量减少，数量足够时调用 `INVEN_RemoveItemDirect` 释放整槽；`count == -1` 时删除所有匹配类别。该函数只操作物理袋并进入 `ITEMPOOL_Free`，扩展对象不能进入。
- `ITEMSYSTEM_CreateItem`（`0x10be9c`）：先校验类别，再调用 `ITEMPOOL_Allocate` 和 `APPINFO_AllocateItemUID` 初始化对象；技能书初始化失败、部分特殊初始化失败路径调用 `ITEMPOOL_Free`，普通成功路径仅返回对象，不负责入库。
- `ITEMSYSTEM_Divide`（`0x1083f8`）：只接受非空、非负拆分数量，并要求原对象为可堆叠物且拆分数量小于当前数量；通过 `ITEMSYSTEM_CopyAsNewUID` 创建新对象，将原对象数量减去拆量、新对象数量设为拆量后返回新对象。函数本体不入库、不释放新对象；唯一确认 caller 是 `UIStore_SellItem+0xf0`（`0xd26e0`），其后续出售失败时两对象责任仍未知。
- `ITEMSYSTEM_ProcessUnpack`（`0x10ce50`）：先按拆包表累计产物数量并调用 `INVEN_IsHavingEmptySlot`；产物由 `ITEMSYSTEM_CreatePerfectItem` 创建，逐个调用 `INVEN_SaveItem`。当前产物保存失败时在 `0x10d1c8` 释放，并在后续路径尝试删除材料/输入物品；已成功入库的前序产物是否完整回滚仍未知。其 ABI、输入消费和线程语义仍需继续冻结。
- `MIXSYSTEM_MakeItem`（`0x11af58`）：原版 5 个 caller 并非统一“生成→保存→删材料”链，部分分支原地修改装备/混沌/Socket 物品，部分分支才保存产物或删除材料；模块批量合成旁路已在 `game_patch_craft.inc` 为保存失败的未入库输出补充 `ITEMPOOL_Free`，但仍使用 `RemoveItemDirect` 删除材料，不能作为原版原子性证据。
- `NetworkStore_AddItem`（`0x15d640`）：从网络接收数据创建物品并设置数量；普通物品调用 `INVEN_SaveItem`，失败时在 `0x15d72c` 调用 `ITEMPOOL_Free`；随后调用 `SAVE_Save`，失败时在 `0x15d780` 调用 `INVEN_RemoveItemData` 回滚并在 `0x15d78c` 释放对象。该链已确认包含入库和保存，但堆叠合并释放、身份精确回滚和原子性仍未知。

## 6. 阶段 0 未闭合项

本轮已完成所有可由当前源码、符号表和固定 ELF 静态证明的子项。已冻结：

- 生产者链：拾取、任务创建、物品箱、拆包、合成、商店购买、装备/脱装备和普通使用的已知 BL caller、目标函数、参数准备、返回值分支和失败跳转；`NetworkStore_AddItem` 仅保留历史审计。
- 对象语义：`CreateItem`/`Divide` 只创建或拆分对象，不等于入库；`SaveItem` 可能在堆叠合并时释放传入对象；`RemoveItemData` 和 `RemoveItemDirect` 只扫描物理袋并进入 `ITEMPOOL_Free`，扩展对象禁止进入。
- 批量语义：`SaveItemData(category,count)` 逐批创建并保存，失败按差额调用 `RemoveItemData` 回滚；`RemoveItemData(category,count)` 支持堆叠减少、整槽释放及 `count == -1` 全量删除。
- 副作用边界：`SaveItem` 明确触发内存库存变更、快捷栏更新和任务事件，但不调用 `SAVE_Save`；`op_ok()` 只刷新模块 frame cache；只有完整原版 `SAVE_Save` 成功才允许 sidecar commit。
- 间接控制流：当前固定 ELF 未解析出指向 P7 目标的 `BLR`、寄存器 `BR`、尾跳或 veneer；无法由静态扫描证明的函数指针分发仍列为未知，不伪造全局覆盖结论。

以下未知项已收敛为后续阶段的明确验证清单，不再作为开放式静态搜索任务：

1. 原版库存函数的真实线程、重入和原版锁边界。
2. 未解析函数指针/GOT 分发及运行时生成 UI 路径的实际覆盖率。
3. `SaveItem` 堆叠合并后各 caller 的最终对象所有权。
4. 拆包、合成、商店购买/出售、装备交换失败时的完整跨对象回滚。
5. `CHAR_UseItemEx` 第三参数对全部物品类别的消费时机。
6. 拆包前序已入库产物是否全部撤销，以及输入物品的精确消费时机。
7. `MIXSYSTEM_MakeItem` 各分支的运行时原子性和材料消费顺序。
8. `frame_cache_force_refresh()` 是否回入扩展背包锁/UI/native 回调。
9. 快捷栏、任务状态、原版投影和 frame cache 的真实 UI 最终一致性。
10. 自动保存、任务奖励、拾取、开箱、合成和快捷键的真实时序。
11. 保存失败、崩溃或进程终止时原版存档、native 内存和 sidecar journal 的最终一致性（归 P6 保存协调验收）。
12. 保存门禁对保存面板、自动保存、任务事件和切换存档全部入口的实际覆盖（归 P6 保存协调验收）。
13. `MIXSYSTEM_MakeItem` 现有模块旁路的材料删除安全性和各原版 caller 原子性；输出对象保存失败释放已修复并待阶段 5回归。
14. 任务创建对象最终进入任务袋、普通物理袋还是仅作为临时对象。

上述项目属于阶段 5真机/运行时验证，不得在阶段 0静态审计中伪造为已知事实。

## 7. 阶段 0 结论

当前结论为：**阶段 0静态审计已完成：`INVEN_FindSaveSlot` ABI 及两个错误调用点已修正，生产者/所有权/副作用矩阵已收敛；14 项未知已分别归入阶段 5运行时验证或 P6保存协调验收，不再阻断阶段 1物理契约定义。**
