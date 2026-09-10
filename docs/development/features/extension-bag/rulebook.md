# 扩展背包规则册

> 状态：CURRENT（规则域）
>
> 本册只规定扩展背包的编号空间、锁边界、Hook 不变式、共享状态主表和跨功能雷区。
> 范围/阶段/验收状态见 [`control-plane.md`](control-plane.md)；结构和依赖见
> [`docs/development/architecture.md`](../../architecture.md)；逐函数接入决策见库存册；
> 触摸状态机见拖动册；sidecar 时序见存档册。
>
> 本册按工作树源码逐条复核，基线为 `archive/extension-bag/writing-materials/doc-set-plan.md` 所指向的任务基线。
> 源码行号是当前文件行号；提交变更后必须重新 grep 锚点，不能把行号当作永久 ID。

## §0 使用方式

### 0.1 必读对象

1. 所有修改扩展背包代码的人，必须完整阅读本册。
2. 这里的“扩展背包代码”包括 `feature/extension_bag/`、扩展背包 port、库存 Hook、
   相关 patch、库存 API 适配和保存参与者。
3. 只改 UI、商店、库存 API、对象缓存或保存路径，也不能只读对应小节后直接提交。
4. 依赖方向仍由架构册唯一裁决：`gamebridge → 所有层`，data 只依赖 STL，禁止
   data→parse、域间非聚合互调和 include 环（`docs/development/architecture.md:87-97`）。
5. 本册不复制 AGENTS.md 的通用规则；§6 只登记本功能特有的禁止事项。

### 0.2 改动说明格式

1. 每个扩展背包改动说明必须列出受影响的 `R-xx`。
2. 每个列出的 `R-xx` 必须给出保持证据：现有 host test 名、测试文件行，或真机用例号。
3. 没有现成锚点时必须写“无锚-待补”，不得用“已检查”替代证据。
4. 若改动影响操作流程，还必须列出对应操作契约卡；本册不代替验收册。
5. 如果源码行号变化，先更新引用再提交，不能保留失效的文件:行。
6. 规则编号是稳定引用名；段落标题变化不改变 `R-xx`。

### 0.3 证据等级

1. `源码`：当前实现可直接打开的文件:行。
2. `host`：当前 host test 中可执行的测试函数名和文件:行。
3. `真机`：控制面登记的 `E-...` 或验收册用例号。
4. “静态成立”只说明当前代码满足局部不变式，不等于整体真机验收通过。
5. 问题 B 在 §7 单独登记为“未解决+已布防”，不得被规则锚点改写成已修复。

### 0.4 原版基线行为契约（B 系列）

> 适用范围：B-01～B-04 适用于扩展背包已启用、但操作对象是**非扩展物品**的原版行为；
> B-05 另记录模块扩展袋合并能力，不归入原版行为基线。本节记录必须保留的
> 原版行为快照；“扩展已启用”不能成为把原版点击改成拖拽、把原版镶嵌改成交换，或
> 把原版袋内移动改送扩展事务的理由。每条契约都必须独立记录当前输入、预期输出、失败
> 语义和证据；一条操作的证据不能替代另一条操作。

#### B-01 点击原版消耗品“使用”必须直接消耗

- **输入**：扩展背包启用时，点击原版袋内的非扩展消耗品并选择“使用”；若该物品需要确认，
  继续点击原版确认按钮。
- **预期输出**：沿原版使用链直接执行效果并按原版时机减少数量/清槽；点击不建立拖拽
  session，不出现拖拽态，不把物品提交给扩展移动事务。
- **关联机制点**：H-04 `INVEN_ConsumeItem` 必须 original-first；确认使用只在
  H-12 `UIEquip_OKConfrimUseItem` 的扩展身份分支接管。C1 事件层只拒绝已识别的扩展
  source，原版消耗品不得被 C1 设为拖拽 owner；H4 `INVEN_MoveItem` 仅在确认扩展身份时
  拒绝，不能拦截原版消费链。

#### B-02 点击原版装备必须直装或替换

- **输入**：扩展背包启用时，点击原版袋内的非扩展装备并执行原版“装备”；覆盖角色装备
  槽为空和已有装备两种情况。
- **预期输出**：空槽时直接装备；已有装备时按原版交换/替换规则完成，被替换装备按原版回包
  语义处理；点击不建立拖拽 session，不把装备按钮降级成拖动操作。
- **关联机制点**：H-06 `CHAR_EquipItemFromInvenToSlot` 保持原版交换规则；H-10
  `UIEquip_ButtonEquipExe` 只有明确识别扩展详情对象时才使用 `Handled/Blocked`，
  非扩展对象必须 backup。C1/H4 的例外条件均是“扩展 source/扩展身份命中”，不得把
  原版装备误判为扩展源。

#### B-03 原版宝石拖到装备必须镶嵌而非交换

- **输入**：扩展背包启用时，将原版袋内的非扩展宝石拖到有对应孔位的原版装备；覆盖无孔、
  非宝石和空装备目标的失败输入。
- **预期输出**：成功时沿原版镶嵌语义写入装备 socket/payload，并只消耗一颗宝石；失败
  时返回原版失败语义，宝石和装备保持不变；不得把源、目标当作两个背包槽交换。
- **关联机制点**：H-07 `ITEMSYSTEM_PutJewel` 保持原版成功/失败返回和消费时机；扩展
  宝石源由 H-15 `UIEquip_EquipControlEventProc@0xb8f7c` 校验后放锁继续原版 proc，
  再由 `PutJewel`/`ConsumeItem` 链承接；扩展宝石+原版装备不得直调
  `virtual_bag_put_jewel_native`，只有两端身份都命中时才进入双端扩展适配。C1 只对扩展
  source 的事件路由设例外，原版宝石到原版装备仍走原版 proc；H4 只拦扩展身份对象进入
  `INVEN_MoveItem`，不得把原版镶嵌误记为移动。

#### B-04 原版袋内拖动必须保持原版移动语义

- **输入**：扩展背包启用时，在原版物理袋 `0..5` 内将非扩展物品拖到合法空槽或原版目标槽。
- **预期输出**：原版 source/target 槽按原版移动规则更新，扩展逻辑槽、projection、
  pending 和扩展 ownership 不发生变化；不重复提交、不进入扩展三方向事务。
- **独立证据要求**：本条必须单独核对原版 source/target 槽、扩展状态和拖拽 session；若
  出现扩展 swap、物理槽异常或拖拽 session 残留，本条失败。
- **关联机制点**：H-14 `INVEN_MoveItem` 对原版对象必须 original-first backup；C1
  事件层只拒绝扩展 source，原版 source 必须放行。H4 仅在 `module_slot_of_item_locked`
  证明扩展身份时 `backup=skipped`，原版对象不得命中 guard。

#### B-05 模块扩展袋内合并必须保持模块堆叠语义

- **输入**：扩展逻辑袋内拖动两个同袋、同类、同 payload 的扩展堆叠物；覆盖 payload 不同、
  超过 stack limit 和跨袋等模块失败/非合并边界。
- **预期输出**：满足模块合并条件时按 stack limit 合并并正确减少 source；不满足条件时
  按模块移动/交换或失败返回；更新扩展 pending、object、handle 和 projection，不改写
  原版物理袋。
- **范围**：B-05 是模块功能契约，不是原版能力契约；原版袋行为由 B-04 及 VM-B01～B04
  的原版基线卡覆盖。
- **关联机制点**：`move_extension_to_extension_locked` 按 payload、同袋、开关和 stack
  limit 判定；扩展对象的 source protection 与原版对象 original-first 分开，不能把模块
  合并结果写成原版 `INVEN_MoveItem` 能力。

## §1 袋号空间契约

### 1.1 三个编号空间

1. 原版物理袋空间是 `0..5`，每袋物理数组最多 16 槽，数组步长为 `0x80`，每槽
   是 8 字节对象指针（`api/native/game_inventory_read.inc:80-97`）。
2. 原版事务空间只允许 `0..4`；模型常量 `kOriginalTransactionBagCount=5` 明确排除
   任务袋（`feature/extension_bag/model/virtual_bag_state.h:18-24`）。
3. 物理袋 `5` 是任务物品专用袋，必须作为 sentinel 保持原样，不能作为扩展移动、
   投影、恢复或自动收编目标。
4. 扩展逻辑公共袋号是 `6..10`，恰好对应五个扩展逻辑袋。
5. 扩展内部索引是 `0..4`，数组第一维只能使用该内部索引；逻辑袋 `6` 映射到内部
   `0`，逻辑袋 `10` 映射到内部 `4`（`model/virtual_bag_transaction_rules.inc:1-16`）。
6. 映射必须经过 `extension_internal_bag()`；调用者不能手写 `logical_bag-6` 后跳过
   边界检查。

### 1.2 物理槽编码

1. `INVEN_FindItemSlot` 的 ABI 是 `int(void*, int8_t*)`，符号源登记在
   `data/native/game_symbols.h:391` 和 `data/native/game_symbols.h:626`。
2. 原版编码为 `(bag << 5) | slot`；解码是 `bag = encoded >> 5`、`slot = encoded & 0x1f`。
3. 该编码是原版物理槽编码，不是扩展逻辑引用；当前边界以本节和 `game_symbols.h` 为准。
4. 输出参数是 `int8_t*`，原版路径按一字节写回；调用链存在 `uxtb`/字节截断语义，
   因此扩展逻辑袋号不能靠“编码后再塞进一个字节”表达。
5. 任何 wrapper 遇到扩展对象，都返回扩展逻辑 bag/slot 或走扩展适配器，不能伪造
   `out_slot`，也不能让原版调用者继续按物理槽解码。
6. `game_state.cpp:134-160` 先分支逻辑袋，再只对 `bag < 5` 读 `g_inven`；这是统一
   读入口的边界证据。

### 1.3 数组写入禁区

1. 扩展逻辑状态写入只能落在 `State.items[0..4][0..15]`。
2. 原版 `g_inven` 写入只能使用物理袋 `0..5` 的真实槽；普通事务目标进一步限制为
   `0..4`。
3. 逻辑袋 `6..10` 禁止作为原版 `bag_table`、`g_inven`、`INVEN_SaveItemOnEmpty`、
   `INVEN_RemoveItemDirect` 或 `INVEN_MoveItem` 的袋参数。
4. 原版读遍历可以包含任务袋 `5`，但这是只读查询语义，不能从遍历范围推导目标范围
   （`game_state.cpp:186-198`）。
5. `inventory_slot_locked()` 的物理保护是 `bag >= 0 && bag < 6`，扩展目标必须在
   进入该函数前完成逻辑分流（`extension_bag_runtime.inc:503-509`）。
6. 任何把 `kExtensionLogicalBagFirst + internal` 传给物理数组的改动，都是越界写风险，
   即使当时地址看似可访问也必须拒绝。

### 1.4 容量与槽边界

1. 扩展数组固定为五袋、每袋 16 槽（`virtual_bag_state.h:14-16`）。
2. 有效扩展容量不是固定 16；`derive_capacity(BagType)` 是唯一入口，类型 `1..4`
   对应容量 `4/8/12/16`，类型 `0` 对应容量 `0`
   （`virtual_bag_transaction_rules.inc:66-78`）。
   页签拖放中“可装备为扩展袋”的类别判定统一使用
   `is_backpack_category(category)`，其范围与上述 `1..4` 类型契约一致；不得在调用点
   重新写一套类别区间。
3. `State.capacities` 是派生字段，注释已明确禁止直写；修改 `types` 后必须 normalize
   （`virtual_bag_state.h:57-60`）。
4. 投影、绘制、命中、移动校验和自动入库必须使用同一派生容量，不能只检查 descriptor
   是否非空。
5. 任务袋 `5` 即使有物品也不能被扩展逻辑容量计算吸收；任务袋是物理 sentinel，不是
   第六个扩展袋。

## §2 锁纪律

### 2.1 `g_virtual_bag_mtx` 的保护范围

1. `g_virtual_bag_mtx` 是扩展背包运行时的总互斥锁，定义在
   `game_ui_virtbag.cpp:67-100`。
2. 它保护 `g_virtual_bag_state` 的逻辑状态：types、capacities、items、mode、选中、
   inspected、info 和 pending。
3. 它保护对象缓存 `g_module_objects`、category/hash、handle、use token、generation 和
   ownership ledger；对象与 descriptor 必须在同一锁上下文保持一致。
4. 它保护正式投影：`g_projected_item_root`、`g_module_view_index`、窗口原版袋号、
   原版容量快照和投影控件绑定。
5. 它保护详情身份：`g_extension_desc_item` 及其 bag/slot、详情按钮 Hook 和 popup 回调
   保存值。
6. 它保护拖动 session、tab generation、tab 指针和 pending tab；这些状态不能在无锁
   快照后继续使用原生指针。
7. `extension_bag_public_runtime.inc:342-357` 在锁内遍历逻辑物品并以公共逻辑袋号
   输出，说明“读逻辑状态”也属于保护范围。

### 2.2 持锁禁调用项

1. 持有 `g_virtual_bag_mtx` 时，禁止调用 `op_ok()`。
2. 原因是 `op_ok()` 会调用 `frame_cache_force_refresh()`
   （`core/native/game_ops_common.cpp:9-12`），该函数会再锁 `g_cache_mtx` 并构造各域
   缓存（`core/native/game_cache.cpp:158-168`）。
3. 持锁时也禁止调用任何会触发原版 UI 刷新、原版库存刷新、缓存刷新或回调重入的函数，
   除非该函数已经明确标为 locked-safe 且不走上述路径。
4. `refresh_module_item_area_locked()` 是锁内专用的原版刷新路径，只能在其既定调用点
   使用，并且必须接受其“刷新后立即重投影”副作用
   （`extension_bag_render.inc:152-165`）。
5. 持锁调用原版函数前，必须先确认该函数不会回调进入扩展 Hook；否则采用放锁模式。

### 2.3 原版调用与 UI 刷新模式

1. 模式 A：锁内只读取并验证 descriptor、对象身份、token 和必要参数；复制出不含悬挂
   指针的值后解锁；调用原版函数；重新加锁验证身份，再提交状态。
2. 模式 B：需要保持临时物理槽劫持时，在锁内写入临时值，立即解锁调用原版交换入口，
   再加锁恢复真实槽并校验结果；装备路径明确采用该模式
   （`extension_bag_equip.inc:176-180`）。
3. 模式 C：先在锁内提交扩展逻辑事务，再在锁内更新控件；禁止让原版 TouchHandle 再
   处理同一个已接管 release 事件。
4. 模式 D：原版 UI 刷新必须在锁内完成状态保护，但成功响应 `op_ok()` 必须放到解锁后
   生成；若刷新函数会再入扩展路径，改用“锁内标记、锁外刷新、锁内复核”。
5. 投影安装时只写控件和临时容量字段，不写 `g_inven`；恢复时先还原容量字段，再调用
   原版刷新（`extension_bag_render.inc:493-497`、`:579-607`）。

### 2.4 `try_lock` 与失败回退

1. 确认使用入口使用 `std::try_to_lock`；抢锁失败必须立即返回“未接管”，不能等待、
   不能写半成品状态（`extension_bag_equip.inc:688-699`）。
2. Native wrapper 收到“未接管”后，才可调用 original backup；确认使用 wrapper 的回退
   位置是 `native_inventory_hook.cpp:263-270`。
3. 只有身份明确属于扩展且事务被拒绝时，才能返回 Handled/Blocked 语义阻止原版；不能
   把锁忙误判成扩展拒绝。
4. 锁外原版调用完成后必须重新检查 item、bag、slot、generation 和对象指针；检查失败
   时走失败语义，不得按失效快照提交。
5. 所有 lock/unlock 路径都必须覆盖异常前的状态恢复；本项目禁止空异常处理。

## §3 Hook 铁律

### 3.1 16 个常驻 Native Hook

安装链是唯一清单，当前必须保持以下 16 个且不增不减：

| 编号 | 目标 | wrapper | 安装证据 |
|---|---|---|---|
| H-01 | `INVEN_FindItem` | `find_item_wrapper` | `native_inventory_hook.cpp:472-475` |
| H-02 | `INVEN_HaveItem` | `have_item_wrapper` | `native_inventory_hook.cpp:475-478` |
| H-03 | `INVEN_GetItemCount` | `get_item_count_wrapper` | `native_inventory_hook.cpp:478-481` |
| H-04 | `INVEN_ConsumeItem` | `consume_item_wrapper` | `native_inventory_hook.cpp:481-484` |
| H-05 | `INVEN_RemoveItem` | `remove_item_wrapper` | `native_inventory_hook.cpp:484-487` |
| H-06 | `CHAR_EquipItemFromInvenToSlot` | `equip_item_from_inven_to_slot_wrapper` | `native_inventory_hook.cpp:487-491` |
| H-07 | `ITEMSYSTEM_PutJewel` | `put_jewel_wrapper` | `native_inventory_hook.cpp:491-494` |
| H-08 | `INVEN_IsHavingEmptySlot` | `is_having_empty_slot_wrapper` | `native_inventory_hook.cpp:494-498` |
| H-09 | `CHAR_UnequipItemToInven` | `unequip_item_to_inven_wrapper` | `native_inventory_hook.cpp:498-502` |
| H-10 | `UIEquip_ButtonEquipExe` | `button_equip_exe_wrapper` | `native_inventory_hook.cpp:502-506` |
| H-11 | `UIEquip_ButtonUnequipExe` | `button_unequip_exe_wrapper` | `native_inventory_hook.cpp:506-510` |
| H-12 | `UIEquip_OKConfrimUseItem` | `ok_confirm_use_item_wrapper` | `native_inventory_hook.cpp:510-514` |
| H-13 | `INVEN_SaveItem` | `save_item_wrapper` | `native_inventory_hook.cpp:533-536` |
| H-14 | `INVEN_MoveItem` | `move_item_wrapper` | `native_inventory_hook.cpp:537-540` |
| H-15 | `UIEquip_EquipControlEventProc` | `equip_control_event_proc_wrapper` | `native_inventory_hook.cpp:541-544` |
| H-16 | `UIEquip_RefreshItemArea` | `refresh_item_area_wrapper`；原版一次刷新走 trampoline 后由 gate 做 post-projection，模块主动刷新走 raw-original dispatcher | `native_inventory_hook.cpp:205-213`（wrapper）、`:561-564`（安装）；`game_ui_virtbag.cpp:237-246`（gate）；`extension_bag_render.inc:533-568` |

1. 安装目标必须来自 `game_symbols.h`/resolver；域文件不得新增裸 VMA。
2. 每个 Hook 都必须保留 backup；Hook 不是默认机制，架构册规定优先读内存、调用函数、
   PtrHook、patch，只有必要时才引入 Native Hook（`docs/development/architecture.md:148-154`）。

### 3.2 original-first 与安装事务

1. 查询 Hook 先调用原版，再仅在原版没有结果时查扩展；`FindItem` 的实现和递归保护见
   `native_inventory_hook.cpp:79-101`，Have/Count 的统一分流见
   `inventory_hook_stage4.cpp:3-23`。
2. `ConsumeItem`、`RemoveItem`、装备和镶嵌必须按对象身份分流；扩展对象不能先落入
   原版释放或消费函数。
3. 对可回退的非扩展路径，original backup 是唯一回退；不得复制一份近似原版逻辑。
4. 安装任一 Hook 失败时，已经成功安装的 Hook 必须按成功顺序逆序卸载，并清空 backup。
   当前链的 rollback 实现在 `native_inventory_hook.cpp:286-307`，安装循环在
   `native_inventory_hook.cpp:378-395`。
5. rollback 不完整时必须阻断重试，当前语义是 `g_install_blocked=true`
   （`native_inventory_hook.cpp:302-306`）。
6. Host 锚点：`test_install_transaction`，`tests/test_inventory_hook_stage4.cpp:220-232`。

### 3.3 SaveItem 特别纪律

1. `SaveItem` 是“已创建物品入袋”的漏斗，调用者覆盖奖励、事件发奖、开箱、拾取、商店、
   合成等多个 caller；wrapper 只能在 backup 失败后尝试扩展 adopt
   （`native_inventory_hook.cpp:184-198`）。
2. 禁止因某一个 caller 修改 SaveItem 的全局成功/失败语义；先核对所有 caller，再只在
   wrapper 边界做统一失败接管。
3. 商店窗口可能临时放大容量；SaveItem backup 前必须先恢复商店投影，否则原版找槽会
   写入真实容量之外（`native_inventory_hook.cpp:190-194`、`game_ui_virtbag.h:90-93`）。
4. 商店买入的两处 FindSaveSlot patch 只负责“允许流程进入 SaveItem”，真正收编仍由
   SaveItem wrapper 完成（`extension_bag_store.inc:630-644`、`:714-728`）。

### 3.4 OK/Cancel 与按钮三态

1. 全局 popup OK/Cancel 槽劫持只允许装备页卖出和销毁两处；当前实现按原始按钮地址
   区分 `UIStore_Sell` 与 `UIEquip_Destroy`，证据为
   `extension_bag_equip.inc:861-907`。
2. 确认使用必须使用 `UIEquip_OKConfrimUseItem@0xb8478` Native Hook，保持
   original-first；不得引入全局“确认使用”槽劫持。
3. 当前详情按钮安装明确跳过装备/卸下 proc；装备和卸下只由函数级 Hook 负责
   （`extension_bag_equip.inc:645-657`）。
4. 装备按钮分流是三态：`Handled`、`Blocked`、`NotExtension`，声明见
   `game_ui_virtbag.h:74-81`。
5. `Handled` 表示扩展已完成，不能调用 backup；`NotExtension` 才调用 backup；
   `Blocked` 表示已识别扩展但必须阻止原版，禁止降级 backup
   （`native_inventory_hook.cpp:305-318`）。
6. 这是为了防止原版内联删源在模块事务前执行；按钮 wrapper 的注释和分支是当前
   证据（`native_inventory_hook.cpp:232-241`）。

## §4 共享状态写-主表

> 下表把素材 §二 逐行与当前源码对码。写入者是“主写路径”，读者是“影响面”；
> 任何新增写者必须先补本表，再改实现。

| 状态 | 写者（已核实） | 读者/契约（已核实） |
|---|---|---|
| `g_virtual_bag_mtx` | 所有扩展 API、UI、事务、生命周期入口在 `game_ui_virtbag.cpp:67-100` 共享同一 mutex | 逻辑状态、对象、投影、token 都受保护；持锁禁 `op_ok()`，见 `extension_bag_public_runtime.inc:277-324` 与 §2 |
| `g_module_objects` | 物化/释放、交换、装备收编、消费，定义和 use 状态见 `game_ui_virtbag.cpp:100-117` | 使用、装备、镶嵌、详情、卖出/销毁、投影；必须与 descriptor、category/hash 一致，`extension_bag_render.inc:343-370` |
| `g_module_object_handles` | 分配/释放、交换、角色所有权移交，账本字段见 `extension_bag_ownership.inc:1-4` | ownership、投影借用、释放、回滚；handle 只绑定当前对象，冷缓存按 `handle_if_object_matches` 复核，`virtual_bag_transaction_rules.inc:164-167` |
| `g_module_view_index` | 安装/切换/恢复视图，安装写入见 `extension_bag_render.inc:498-541`，恢复清理见 `:579-607` | 容量、投影、输入、刷新、装备；必须与 `capacities[index]` 同袋，且和 `g_module_window_original_bag` 区分 |
| `g_projected_item_root` | UIEquip 投影安装/恢复，见 `extension_bag_render.inc:522-540`、`:602-607` | 控件绑定、拖拽识别、ownership；drop 必须单一 owner 并先解除投影借用 |
| `g_extension_desc_item` + bag/slot | MakeDesc gate 捕获，见 `extension_bag_equip.inc:985-1024` | 确认使用、装备、卖出、销毁和详情 Hook；操作前重校验 item+bag+slot+cache，卖出校验见 `:433-447` |
| generation/owner/pending_release token | `module_use_begin/finish/abort`，见 `extension_bag_ownership.inc:146-244` | 确认使用、删除、移动、交换；active 禁替换/释放，仅匹配 generation+owner 的 token 可完成 |
| `g_virtual_bag_state.capacities` | normalize/equip/load，派生写入见 `virtual_bag_state_ops.inc:1-6` | 槽边界、投影、容量预检、API JSON；入口、绘制、命中、移动、自动入库必须共享派生结果 |
| `g_tab_bag_items` / `g_extension_tab_buttons` | 页签安装、装备收尾、生命周期，页签对象刷新见 `extension_bag_tabs.inc:11-38` | 页签绘制、切换、商店/装备页；控件树重建要递增 generation，`extension_bag_runtime.inc:117-164` |
| 原版 `bag_table` 容量字段 | `install_module_view` 临时写、restore 恢复，见 `extension_bag_render.inc:508-520`、`:593-600` | 原版绘制、FindSaveSlot、SaveItem、商店购买；原版写入前必须恢复真实容量 |
| 投影控件数组 | `refresh_module_item_area_locked`、事务交换/消费，见 `extension_bag_render.inc:547-568`、`extension_bag_transaction.inc:521-529` | UI 点击、拖拽、详情、ownership；控件 object 必须等于当前 `g_module_objects[bag][slot]` |
| `g_extension_drag_session` / `g_extension_drag` | press/move/release 和事务路由，见 `extension_bag_lifecycle.inc:297-315`、`:367-407` | 拖拽与原版事件 guard；generation 过期取消，`0x18` 是唯一提交者，失败也阻止原版二次移动 |

## §5 雷区规则

> 素材 §三 的 25 条候选均已逐条复核。R-01..R-25 按素材顺序对应；其中 R-20
> 对码确认 R-26..R-36 为当前实现的同类耦合规则。

### R-01 确认使用必须刷新并重投影

- **规则一句话**：确认使用成功后必须在同一锁上下文走 `refresh_module_item_area_locked(bag)`，并清理详情身份。
- **为什么**：确认使用函数在锁外调用原版，消费完成后若不刷新，控件会保留已消费物品；收尾代码见 `extension_bag_equip.inc:725-762`。
- **典型破坏方式**：只更新 descriptor 或只清详情 Hook，导致界面闪回原版背包或已失效对象仍显示。
- **验证锚**：无锚-待补；源码锚 `extension_bag_equip.inc:760-766`。

### R-02 投影拖拽不得放行原版 0x18

- **规则一句话**：投影 session 的 `0x18` 必须由扩展事务先接管，失败也必须吞掉原版事件；受控装备交换是唯一允许 `MoveItem` guard 放行的窄例外。
- **为什么**：当前代码明确把 `0x18` 定义为唯一提交 owner，并在事务前返回 `1`（`extension_bag_lifecycle.inc:297-315`）。
- **与 R-33 的边界**：R-33 只在该事件已被吞掉后补 TouchHandle 清理，不重新派发或重新打开原版
  `0x18`，因此不改变本条的单一提交 owner。
- **典型破坏方式**：让 TouchHandle 或 `INVEN_MoveItem` 再跑一次，清掉真实物理槽；或把装备交换例外扩大到普通投影拖动。
- **验证锚**：`test_p52_drag_session`，`tests/test_host.cpp:1586-1678`；源码 `extension_bag_lifecycle.inc:309-314`。

### R-03 投影同步必须同时修对象缓存

- **规则一句话**：`virtual_bag_sync_projected_*` 不能只刷控件，必须让控件、descriptor 和 object cache 同步。
- **为什么**：投影刷新按 `g_module_objects[bag][slot]` 绑定控件（`extension_bag_render.inc:547-568`），而物化路径按 descriptor hash 校验（`:343-369`）。
- **典型破坏方式**：消费、卖出或销毁后只清 UI，后续使用/释放命中失效对象。
- **验证锚**：无锚-待补；host 可参考 `test_ownership_ledger_p43`，`tests/test_host.cpp:1280-1341`。

### R-04 `module_item_locked` 不得换角色源或袋号

- **规则一句话**：`module_item_locked` 的 bag/slot 必须来自扩展逻辑状态，投影装备还必须匹配当前 view 和窗口原版袋。
- **为什么**：装备入口要求 `g_module_view_index == source_bag` 且窗口袋在 `0..4`（`extension_bag_equip.inc:155-173`）。
- **典型破坏方式**：用当前原版袋替代扩展源袋，造成错误角色、错误袋或错误投影对象。
- **验证锚**：`test_object_operations` 的 fallback 装备断言，`tests/test_inventory_hook_stage4.cpp:158-180`。

### R-05 空槽判定不能只看 descriptor

- **规则一句话**：容量判断必须使用派生容量和 object/token 可分配条件，不能只数 descriptor 空槽。
- **为什么**：`stage4_is_having_empty_slot` 同时服务生产者和商店 gate（`inventory_hook_stage4.cpp:25-43`），扩展真实空槽还由 `extension_bag_has_empty_slots` 提供。
- **典型破坏方式**：收编、生产或购买误报有容量，最终写入不可用槽或错误提示满包。
- **验证锚**：`test_queries`，`tests/test_inventory_hook_stage4.cpp:115-125`；源码 `extension_bag_runtime.inc:394-406`。

### R-06 装备按钮三态不能合并为 bool

- **规则一句话**：装备按钮必须保留 Handled/Blocked/NotExtension 三态，Blocked 绝不能 backup。
- **为什么**：Blocked 用于阻止原版内联删源，wrapper 明确对 Blocked 直接 return（`native_inventory_hook.cpp:312-316`）。
- **典型破坏方式**：扩展事务失败后降级原版，原版先删掉投影源，模块无法回滚。
- **验证锚**：无锚-待补；源码 `game_ui_virtbag.h:74-81`、`native_inventory_hook.cpp:235-243`。

### R-07 扩展 Remove 失败不得 backup

- **规则一句话**：已识别扩展对象的删除失败必须停在扩展分支，不能调用原版 backup。
- **为什么**：Stage4 明确注释“释放失败必须停在扩展分支”，实现返回 `0`（`inventory_hook_stage4.cpp:64-80`）。
- **典型破坏方式**：原版不认识逻辑对象却继续按指针反查，破坏失败语义或误释放。
- **验证锚**：`test_object_operations`，`tests/test_inventory_hook_stage4.cpp:150-156`。

### R-08 装备入口必须保留 `extension_item_at` 兜底

- **规则一句话**：投影视图下原版源槽为空时，必须用带视图门禁的 `extension_item_at` 物化扩展源。
- **为什么**：原版 INVEN 保持真实，投影对象不在物理槽；Stage4 在 `item==nullptr` 时调用 fallback（`inventory_hook_stage4.cpp:83-103`）。
- **典型破坏方式**：删除兜底后扩展详情装备按钮走原版空槽路径，表现为点击无效或装备失败。
- **验证锚**：`test_object_operations`，`tests/test_inventory_hook_stage4.cpp:168-180`。

### R-09 PutJewel 必须识别 jewel 与 equip 两端

- **规则一句话**：镶嵌分流必须分别识别扩展宝石和扩展装备，只有两端都属于扩展才进入扩展适配。
- **为什么**：Stage4 先识别 jewel，再识别 equip（`inventory_hook_stage4.cpp:106-119`）；API 路径也先校验宝石 descriptor（`extension_bag_api_impl.inc:200-245`）。
- **典型破坏方式**：只识别 jewel，扩展装备被原版处理；或把原版装备错误送入扩展路径。
- **验证锚**：`test_object_operations`，`tests/test_inventory_hook_stage4.cpp:182-195`。

### R-10 卸下必须保留原版失败后的扩展收编

- **规则一句话**：`UnequipItemToInven` 只有原版返回失败时才尝试扩展空位收编。
- **为什么**：Stage4 先调用 backup，原版成功直接返回，失败才调用 `extension_adopt`（`inventory_hook_stage4.cpp:122-133`）。
- **典型破坏方式**：只信原版返回值，原版满包时袋对象丢失或扩展兜底永远不可达。
- **验证锚**：`test_unequip_to_inven`，`tests/test_inventory_hook_stage4.cpp:198-217`。

### R-11 SaveItem wrapper 的 backup 前必须恢复商店容量

- **规则一句话**：商店投影安装时，SaveItem backup 前必须先恢复真实容量字段和原版投影。
- **为什么**：wrapper 顺序固定为 restore 再 backup（`native_inventory_hook.cpp:190-196`）；商店安装确实临时写容量（`extension_bag_store.inc:250-271`）。
- **典型破坏方式**：原版 FindSaveSlot 扫到真实范围外，恢复窗口后物品消失。
- **验证锚**：无锚-待补；源码 `game_ui_virtbag.h:90-93`。

### R-12 商店扩展卖出只能刷新商店投影

- **规则一句话**：商店扩展卖出后使用 `store_refresh_projection_locked`，不得调用 `UIStore_RefreshInvenItem` 覆盖整列。
- **为什么**：商店卖出实现明确只刷新扩展投影（`extension_bag_store.inc:536-543`）。
- **典型破坏方式**：原版刷新把窗口袋内容重新写入商店控件，扩展剩余物品消失或显示原版内容。
- **验证锚**：无锚-待补；源码 `extension_bag_store.inc:536-541`。

### R-13 装备页与商店页不能共享 popup 状态

- **规则一句话**：装备页卖出/销毁和商店卖出必须分别保存、恢复各自 OK/Cancel 原回调及详情身份。
- **为什么**：商店有独立 `store_sell_original_ok/cancel`（`extension_bag_store.inc:20-25`），装备页有独立字段（`game_ui_virtbag.cpp:145-153`）。
- **典型破坏方式**：两个宿主互覆盖全局槽，确认回调串线到另一个页面。
- **验证锚**：无锚-待补；源码 `extension_bag_store.inc:50-67`、`extension_bag_equip.inc:389-416`。

### R-14 详情 Hook 必须跳过装备/卸下 proc

- **规则一句话**：详情按钮数组安装观察 Hook 时必须跳过装备和卸下 proc，避免函数级 Hook 与 PtrHook 双重处理。
- **为什么**：当前代码比较两个原版 proc 地址后 continue（`extension_bag_equip.inc:645-657`）。
- **典型破坏方式**：一次点击先进入观察 wrapper 再进入函数 Hook，重复提交装备或清理详情状态。
- **验证锚**：无锚-待补；源码 `extension_bag_equip.inc:620-681`。

### R-15 active/pending_release 对象不可释放或复用

- **规则一句话**：active 或 pending_release 对象必须拒绝替换、释放和槽复用；ownership ledger 耗尽时拒绝登记；触摸窗口内的释放请求必须进入定长延迟回收队列并以成功释放路径返回，排空前通过投影控件和对象缓存引用检查。
- **为什么**：`module_slot_is_assignable_locked` 和 release guard 同时检查 token 状态；`retire_custody_item_locked` 在触摸窗口内只退 custody、不立即物理释放，队列排空由 draw-end 安全点执行（`extension_bag_ownership.inc`）。
- **典型破坏方式**：确认使用尚未完成时释放对象，产生悬挂指针和 token mismatch；在无可用 handle 时继续登记对象。
- **验证锚**：`deferred free enqueue`/`deferred free drain`；S-05、VM-29；源码
  `extension_bag_ownership.inc:35-53,71-119,147-171`。

### R-16 交换必须同步 descriptor/object/hash/handle

- **规则一句话**：扩展交换必须成组交换 descriptor、native object、category、hash、handle、generation 和 ownership；合并判定必须比较归一化 payload，而非原始载荷字节。
- **为什么**：交换实现保存完整快照并同时更新对象、hash、handle，见 `extension_bag_transaction.inc:374-449`、`:487-518`。
- **典型破坏方式**：只换 descriptor 导致控件显示错物品，随后释放或使用命中另一槽对象。
- **验证锚**：`test_p44_transaction_stages`，`tests/test_host.cpp:1527-1544`；真机问题 B 仍未解决。

### R-17 物化失败前必须拒绝 stale endpoint

- **规则一句话**：交换或替换物化前必须先校验非空缓存的 category/hash，stale 时拒绝而不是先释放再重建。
- **为什么**：当前交换在物化前比较已有 endpoint 与 descriptor hash（`extension_bag_transaction.inc:388-399`）。
- **典型破坏方式**：对象已经释放后新对象物化失败，原对象无法恢复。
- **验证锚**：无锚-待补；源码 `extension_bag_transaction.inc:388-416`。

### R-18 投影同步不得在错误锁序触发原版刷新

- **规则一句话**：同步接口持扩展锁时只能执行已验证的投影控件修复，不能递归触发会反向取锁的原版刷新。
- **为什么（修正）**：素材说 `extension_bag_sync_projected_bag` 当前会触发原版刷新；对码显示它只锁内调用 `refresh_projection_if_overwritten_locked`（`extension_bag_public_runtime.inc:320-324`），该函数只比较并设置控件（`extension_bag_render.inc:547-568`）。真正的原版刷新路径是 `refresh_projected_module_view_after_move_locked` 的 `refresh_module_item_area_locked`（`extension_bag_render.inc:571-576`）。
- **典型破坏方式**：未来把原版 RefreshItemArea 塞进同步接口，形成递归刷新、锁序反转或容量错位。
- **验证锚**：无锚-待补；本条为“修正后收录”，不是对素材原句的照抄。

### R-19 页签清理必须递增 generation

- **规则一句话**：页签/控件树清理必须递增 generation，并使失效事件、pending tab 和失效 root 不可提交。
- **为什么**：`disable_extension_tab_buttons_locked` 先递增 `g_inventory_generation` 再清引用（`extension_bag_runtime.inc:166-175`）。
- **典型破坏方式**：失效控件事件提交到新面板状态，装备页或商店页重复创建标签。
- **验证锚**：`test_p52_drag_session` 的 stale generation 分支，`tests/test_host.cpp:1643-1649`。

### R-20 原版事件 guard 必须比较并恢复物理快照

- **规则一句话**：投影 session 调原版事件前后必须比较 6×16 物理槽快照，发生变化时恢复快照并同步投影。
- **为什么**：当前 guard 在 `orig_event pre/post` 记录 digest，检测变化后逐槽记录并恢复（`extension_bag_lifecycle.inc:116-165`）。
- **典型破坏方式**：原版 TouchHandle 在投影 session 中调用移动，真实 INVEN 被毁且没有补救。
- **验证锚**：`test_virtual_bag_mergeable_items` 中快照纯函数断言，`tests/test_host.cpp:735-741`；运行时 H3/H4 见 §7。

### R-21 `inventory_item_ref_at` 必须识别 6..10

- **规则一句话**：页签 `0..4` 必须先转换为逻辑袋 `6..10` 再进入扩展路由；共同库存入口读取逻辑袋 native item 前必须完成域转换并判空，不能落入物理数组。
- **为什么**：`inventory_item_ref_at` 在物理读取前专门判断 `extension_bag_is_logical_bag`（`game_state.cpp:134-160`）。
- **典型破坏方式**：API 读、用、卖、弃、移全部报告空槽或错误读取原版袋。
- **验证锚**：`test_virtual_bag_transaction_domain`，`tests/test_host.cpp:874-900`。

### R-22 聚合遍历范围必须保持原版+扩展语义

- **规则一句话**：`for_each_inventory_item` 必须先遍历物理袋，再遍历扩展逻辑袋；改变范围前要审计 Have/Count/JSON/容量所有调用者。
- **为什么**：实现顺序和两类引用构造见 `game_state.cpp:163-184`，物理只读遍历明确包含任务袋 `5`（`:186-198`）。
- **典型破坏方式**：删掉扩展尾段或把任务袋错误当目标域，使查询、库存 JSON 和生产者容量判断同时漂移。
- **验证锚**：`test_p7_stage4_find_item_poc`，`tests/test_host.cpp:76-121`；源码 `game_inventory_read.inc:80-124`。

### R-23 卖出价格必须保留宿主 variant

- **规则一句话**：装备页销毁/卖出与商店卖出必须分别传入各自的折扣 variant，不能统一改价格参数。
- **为什么**：`extension_bag_sell_price` 用 `apply_variant_discount` 区分乘数，装备页与商店调用点不同（`extension_bag_port.cpp:109-117`、`extension_bag_store.inc:488-505`、`extension_bag_equip.inc:887-896`）。
- **典型破坏方式**：商店价格继承装备页粉碎折扣，或装备页按商店原价结算，导致价格语义串扰。
- **验证锚**：无锚-待补；源码 `extension_bag_equip.inc:894-895`。

### R-24 菜单角色必须来自当前菜单角色

- **规则一句话**：详情装备、确认使用和相关角色操作必须通过 `F_GET_MENU_CHARACTER_VMA` 获取当前菜单角色，不能固定角色 0。
- **为什么**：`extension_menu_character()` 解析并调用当前菜单角色函数（`extension_bag_equip.inc:8-18`），确认使用在 `:713-729` 使用它。
- **典型破坏方式**：给队员 A 打开详情却把物品装备/使用到队员 B。
- **验证锚**：无锚-待补；源码 `data/native/game_symbols.h:312-316`。

### R-25 persist 顺序必须遵守事务所有权顺序

- **规则一句话**：ext→orig 必须在原版入库确认后再清扩展逻辑源；pending、释放、刷新和 sidecar 顺序不能任意移动。
- **为什么**：当前路径先 pending、Load/SaveItemOnEmpty、落位复核，再释放模块对象、移交 ownership、清源（`extension_bag_transaction.inc:192-307`）。
- **典型破坏方式**：先清源或先持久化，随后原版入库失败，造成物品丢失或 pending 与真实库存不一致。
- **验证锚**：`test_p44_transaction_stages`，`tests/test_host.cpp:1393-1584`。

### R-26 窗口原版袋号与扩展 view index 必须分离

- **规则一句话**：`g_module_view_index` 是扩展逻辑内部袋号，`g_module_window_original_bag` 是投影所借用的物理原版窗口袋号，二者禁止互换。
- **为什么**：安装时分别写入 `g_module_window_original_bag=original_bag` 和 `g_module_view_index=bag`（`extension_bag_render.inc:502-541`）。
- **典型破坏方式**：用目标扩展袋号刷原版容量，或把扩展源投影到错误物理袋；问题 A 的根因正是收尾容量袋号与投影 view 不一致。
- **验证锚**：无锚-待补；真机事实见 `impact-matrix-source.md:81`。

### R-27 direct/GOT 原版袋选择值必须成对保存恢复

- **规则一句话**：临时切换或屏蔽原版袋时，direct 与 GOT 两份袋号必须成对写入、成对恢复。
- **为什么**：`set_original_bag_locked` 同时写两处（`extension_bag_render.inc:256-261`），绘制屏蔽也保存并恢复两处（`:181-200`）。
- **典型破坏方式**：只改一份导致绘制、FindSaveSlot、输入和退出路径看到不同的当前袋。
- **验证锚**：无锚-待补；源码 `extension_bag_render.inc:325-340`。

### R-28 extension source protection 必须与 merge 开关解耦

- **规则一句话**：扩展背包开启或堆叠合并开启任一成立，都必须保留扩展源保护。
- **为什么**：纯模型判定明确返回 `extension_bag_enabled || move_merge_enabled`，见 `virtual_bag_transaction_rules.inc:169-172`。
- **典型破坏方式**：只按扩展开关安装保护，merge 开启时原版事件仍可改写投影源。
- **验证锚**：`test_virtual_bag_mergeable_items`，`tests/test_host.cpp:728-734`。

### R-29 ext→ext pending 与 journal 域不能混用

- **规则一句话**：ext→ext 可以使用进程内 `PendingTransfer`，但不能伪装成会创建原版 prepare journal 的跨域事务。
- **为什么**：模型明确区分 pending 域和 journal v1 域，ext→ext 不进入 original 阶段；常规 pending 记录当前是 `durable=false`，双 journal 的落盘边界由存档册 §4 裁定（`virtual_bag_transaction_rules.inc:40-48`、`:213-216`、`extension_bag_runtime.inc:560-576`）。
- **典型破坏方式**：给 ext→ext 创建原版 journal，恢复器误删物理源或把逻辑袋当原版袋。
- **验证锚**：`test_p44_transaction_stages`，`tests/test_host.cpp:1393-1402`。

### R-30 控件树重建后必须先失效 root 再接收事件

- **规则一句话**：root 或子控件重建时必须递增 generation、清理失效指针，并要求 source/destination 控件属于当前 root。
- **为什么**：drop gate 同时核对 live root、projected root 和 source control（`extension_bag_public_runtime.inc:81-107`），tab 重建由 `extension_bag_runtime.inc:117-164` 执行。
- **典型破坏方式**：失效控件事件提交到新状态，或 stale 控件把物品送进错误袋槽。
- **验证锚**：无锚-待补；源码 `extension_bag_public_runtime.inc:109-149`。

### R-31 扩展对象禁止进入原版移动链

- **规则一句话**：扩展对象禁止进入原版移动链（proc 层+函数层双拦）；事件层先拒绝进入原版 proc，`INVEN_MoveItem@0x104934` 收到扩展身份对象时还必须完成锁内取证后记录 `ERROR MoveItem GUARD reject` 并返回 `0`，不得调用 backup；仅受控装备交换是唯一窄例外，原版对象一律 original-first。
- **为什么**：该函数只有 item/count/target bag/slot 四参，不能安全还原扩展拖动意图；让它分流扩展事务会与 `0x18` owner 产生双提交，且可能改写物理 `g_inven`。
- **判据同源**：事件层与函数层均使用共享 helper `module_slot_of_item_locked` 反查 `g_module_objects`，避免两套扩展身份判据漂移。
- **锁纪律**：身份、当前 view/session 和源/目标物理槽摘要在 `g_virtual_bag_mtx` 内采集；返回后放锁，再调用 backup。backup 可能进入 Remove/Save/Consume hook，持锁调用会死锁。
- **验证锚**：`native_inventory_hook.cpp:226-273` 的 `MoveItem GUARD reject`/`MoveItem pre/post` 日志格式及 `native_inventory_hook.cpp:347-357` 的装备槽事件分流；VM-10、VM-27；问题 B 仍以“未解决+已布防”判定。

### R-32 模块内原版刷新必须走 trampoline

- **规则一句话**：模块内部主动刷新不得经被 Hook 的 `UIEquip_RefreshItemArea` 地址自调原版刷新，必须走 `g_backup_refresh_item_area` trampoline；模块路径仍自行完成投影覆盖。
- **为什么**：模块刷新点多数持有 `g_virtual_bag_mtx`，经被 Hook 地址回入 H-16 wrapper 会再次尝试加锁，形成死锁；raw-original dispatcher 同时保留 Hook 未就绪时的现有函数回退。
- **验证锚**：源码 grep `inventory_native_hook_call_refresh_item_area_original` 覆盖模块主动刷新点；VM-B01～VM-B04 观察原版路径未被扩展刷新覆盖，真机已确认；VM-B05 只属于模块合并卡。

### R-33 吞掉原版 release 必须完成等价清理

- **规则一句话**：任何吞掉 `0x18` 的出口必须经 `complete_original_release_cleanup_locked` 完成
  原版 release 五步等价清理；`TouchHandle_ResetMovingControl` 的 `0x08` 派发和
  `TouchHandle_ResetSelectedControl` 必须放锁后执行。
- **为什么**：只清扩展 session 会保持原版 TouchState、moving/on 标志或选中控件，下一次
  原版点击可能进入幽灵拖拽；原版 UI 回调可重入模块，持有 `g_virtual_bag_mtx` 派发会死锁。
- **范围**：owner/terminal、非世界取消、capture 空坐标、capture click/drop、原版袋 drop
  五个吞出口；C1/C2/H-15 proc 内返回值回流路径不重复清理。
- **验证锚**：grep `complete_original_release_cleanup_locked`、日志
  `release_cleanup ctl=... flags_cleared`、VM-28 变体选择回归；H-16 仍由 R-32/VM-B 覆盖。

### R-34 moving 保留必须通过六条件门

- **规则一句话**：draw-end 只有在 session phase 为 Pressed/NativeMoving/TargetResolved/
  TransactionInFlight、模块投影仍安装且 mode 为 Module、view index 等于 source bag、
  session/tab/inventory 三代相等、live root 等于 `g_projected_item_root`，并且
  TouchState moving 控件等于当前 root 的 `source_slot` 子控件时，才可保留 moving；
  任一条件失败必须清 `TouchState+0x30` 及控件 `+0x0a/+0x0b` flag。
- **为什么**：仅按控件是否属于某个投影 root 保留 moving，会把失效 session、失效 generation、
  错误页签或重建后的控件继续交给原版 TouchHandle，形成 stale drag。
- **边界**：该门只清 stale moving 状态，不改变 `0x18` owner、release 五出口、C1/C2、
  H-15、H-4、T3 安装/恢复或 S-03 未解决口径；S-05 已由真机确认；H-16 负责刷新后的
  投影覆盖，帧级 projection heal 不再由 draw-end 调用，无刷新事务只做受影响槽定点同步。
- **验证锚**：`extension_bag_input.inc:34-77` 的六条件门与 stale 日志；VM-B01～VM-B04
  的真机回归确认 draw-end 不再逐帧写控件，VM-B05 为模块合并卡。

### R-35 页签命中优先于格子解析

- **规则一句话**：扩展源 release 的目标解析必须先检查页签，再检查扩展网格；页签矩形与
  网格行 y 区间重叠时，页签命中优先解释为切袋意图。
- **为什么**：`route_projected_session_drop_locked` 在格子解析前遍历页签并直接路由
  `route_projected_session_to_tab_locked`；格子优先会短路页签路由，使页签装备/跨页签提交
  分支无法执行（`extension_bag_transaction.inc:791-812`）。
- **验证锚**：`tab commit handled=1`、`cross tab reject reason=...`、
  `tab reject reason=transaction_failed ...`；S-05、VM-29。

### R-36 触摸窗口释放必须延迟回收

- **规则一句话**：触摸窗口内的释放请求必须先进入延迟释放队列并返回成功，禁止直接调用
  `ITEMPOOL_Free`，也不得因触摸窗口活动而拒绝事务提交；非触摸窗口的排空只在无投影控件
  和逻辑槽引用时调用 `ITEMPOOL_Free`。
- **为什么**：触摸窗口仍可能由 moving 控件、projection 或逻辑槽持有对象；直接释放或
  拒绝 source release 会分别形成 UAF 风险或 `extension merge source release rejected`
  事务失败。队列、引用检查和 draw-end 排空见 `extension_bag_ownership.inc:35-53,71-119,147-171`
  与 `extension_bag_render.inc:712-770`。
- **验证锚**：日志 `deferred free enqueue`/`deferred free drain`；S-05、VM-29。

## §6 禁止事项汇总

> 仅列本册特有事项；AGENTS.md 的通用禁止项不在此重复。通用依赖方向链接到
> [`docs/development/architecture.md:87-97`](../../architecture.md#12-依赖方向强制规则可用-include-检查脚本验证)，
> 通用符号来源和裸地址规则链接到 [`docs/development/architecture.md:148-156`](../../architecture.md#关键约定)。

1. 禁止把扩展逻辑袋 `6..10` 编码成原版 `int8_t out_slot`。
2. 禁止向物理数组传入逻辑袋号，禁止把任务袋 `5` 当扩展事务目标。
3. 禁止持 `g_virtual_bag_mtx` 调 `op_ok()` 或未经审计的缓存/原版刷新函数。
4. 禁止复活确认使用的全局 popup OK/Cancel 槽劫持；确认使用固定走 `0xb8478` Native Hook。
5. 禁止把装备按钮三态压成 bool，尤其禁止 Blocked backup 降级。
6. 禁止删除 `0x18` 单一提交 owner 或让失败事件回到原版二次移动。
7. 禁止只改 descriptor、不改 object/hash/handle/generation/ownership。
8. 禁止把 ext→ext pending 当作原版 journal，或用 journal 恢复器修改物理源。
9. 禁止在问题 B 尚未有新证据前宣称交换丢失已修复。
10. 禁止模块内部经被 Hook 的 `UIEquip_RefreshItemArea` 地址自调原版刷新；必须走
    raw-original dispatcher，并由模块路径自行完成投影覆盖。

## §7 未解决登记：问题 B

### 7.1 结论口径

1. 问题 B 是“扩展袋 0 的 a↔b 交换成功后，原版袋 0 同号 b 格物品消失；视图、
   重开和保存后仍消失”。
2. 当前结论必须写作：**未解决+已布防**。
3. 禁止写作“已修复”“已消除”或用 Host 事务模型通过替代真机复现结论。
4. 当前控制面结论仍为“未解决+已布防”；取证以 `verification-matrix.md` 的 VM-12 和 H3/H4
   字段为准。

### 7.2 当前防御

1. drop 三态门阻止扩展源在失败时降级到原版路径；对应按钮三态和 drop gate
   见 `native_inventory_hook.cpp:235-243`、`extension_bag_public_runtime.inc:64-149`。
2. rollback 索引、冷缓存 handle 和视图归属校验保护当前事务；stale endpoint 和
   handle 匹配证据见 `extension_bag_transaction.inc:388-416`。
3. `moveMergeEnabled` 与扩展保护解耦，`0x18` 保持单一 owner，并使用物理袋
   `0..5 × 16` 快照守卫与恢复；快照大小和比较见 `virtual_bag_transaction_rules.inc:174-184`。
4. 事件 guard 在原版事件前后记录 digest，证据格式为 `orig_event pre/post`
   （`extension_bag_lifecycle.inc:105-114`、`:120-142`）。
5. 检测物理变化后输出 `ERROR physical inventory mutation` 并尝试恢复，逐槽日志
   位置为 `extension_bag_lifecycle.inc:143-164`。
6. `INVEN_MoveItem@0x104934` 的 Native guard：扩展身份记录
   `MoveItem GUARD reject` 并跳过 backup；原版身份 original-first，活动 view/session
   记录 `MoveItem pre/post`（`native_inventory_hook.cpp:226-273`）。
7. `UIEquip_EquipControlEventProc@0xb8f7c` Hook：仅扩展宝石源进入
   锁内 descriptor/generation/session 校验；通过后取 `ModuleUseToken`，放锁调用原版 proc，
   由 `PutJewel`/`ConsumeItem` 链完成镶嵌和消费，校验失败直接 Blocked，不降级 backup。

### 7.3 剩余缺口与守卫日志锚

1. 用户复测仍复现问题 B；因此防线只能降低破坏面，不能证明根因已定位。
2. 剩余取证缺口是 H3/H4 复测未采集：未知 `ERROR physical inventory mutation` 是否
   触发，未知 `orig_event pre/post` digest 是否显示变化。
3. 后续验收必须保留同一操作前后物理 96 槽快照、扩展 descriptor/object/hash/handle、
   `pending` 和 sidecar 状态。
4. 发现 `ERROR physical inventory mutation` 时，应同时记录 `bag`、`slot`、before/after
   指针及 before/after digest；当前逐槽日志已提供这些字段。
5. 拖动协议、单一 drop owner、session 生命周期和复测步骤详见
   [`drag-protocol.md`](drag-protocol.md)；本册只保留不可违反的规则和证据口径。

### 7.4 本册交付核对

1. 本册规则总数：`R-01..R-36`，共 36 条。
2. 当前规则包含：`R-26`（窗口袋与 view index 分离）、`R-27`（direct/GOT 成对恢复）、
   `R-28`（source protection 与 merge 解耦）、`R-29`（pending/journal 分域）、
   `R-30`（root 重建与 stale event 门禁）、`R-31`（扩展对象禁入原版移动链）、
   `R-32`（模块内原版刷新必须走 trampoline）、`R-33`（吞掉原版 release 必须完成等价清理）、
   `R-34`（moving 六条件保留门）、`R-35`（页签命中优先于格子解析）、
   `R-36`（触摸窗口释放必须延迟回收）。
3. 当前 sync 接口只做投影控件修复，原版 RefreshItemArea 位于移动收尾路径。
4. 无锚规则：`R-01`、`R-03`、`R-05`、`R-06`、`R-11`、`R-12`、`R-13`、`R-14`、
   `R-15`、`R-17`、`R-18`、`R-23`、`R-24`、`R-26`、`R-27`、`R-30`，共 16 条；
   其中已给源码锚但尚无专门 host/真机锚的规则，验收册仍应补操作证据。
5. 规则正文以当前源码和本册证据锚为准。
