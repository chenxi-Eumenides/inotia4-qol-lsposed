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
3. 物理袋 `5` 是任务物品专用袋，必须作为 sentinel 保持原样；它作为当前来源时允许
   通过页签切换离开到扩展袋或原版袋，但不能作为扩展移动、投影、恢复或自动收编目标。
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

### 3.1 24 个常驻 Native Hook

安装链是唯一清单，当前必须保持以下 24 个且不增不减（下表为 H-01..H-23 既有 23 个；
H-24 为 R-63 佣兵徽章使用函数级接管新增，安装证据见 R-63，23 个既有 + H-24 = 24，与
源码安装日志 `count=24` 一致。H-17 为 S2 读侧统一 getter，H-18..H-21 为 S2 写侧进位框架
追加，其中 H-20 `ITEMSYSTEM_MakeItem` 数量回写已撤销、wrapper 纯透传，见 R-52；
H-22/H-23 为 R-55 原版背包详情出售接管追加）：

| 编号 | 目标 | wrapper | 安装证据 |
|---|---|---|---|
| H-01 | `INVEN_FindItem` | `find_item_wrapper` | `native_inventory_hook.cpp:255-278`（wrapper）、`:1126-1128`（安装） |
| H-02 | `INVEN_HaveItem` | `have_item_wrapper` | `native_inventory_hook.cpp:291-298`（wrapper）、`:1129-1131`（安装） |
| H-03 | `INVEN_GetItemCount` | `get_item_count_wrapper` | `native_inventory_hook.cpp:300-308`（wrapper）、`:1132-1134`（安装） |
| H-04 | `INVEN_ConsumeItem` | `consume_item_wrapper` | `native_inventory_hook.cpp:358-434`（wrapper）、`:1139-1141`（安装） |
| H-05 | `INVEN_RemoveItem` | `remove_item_wrapper` | `native_inventory_hook.cpp:436-448`（wrapper）、`:1142-1144`（安装） |
| H-06 | `CHAR_EquipItemFromInvenToSlot` | `equip_item_from_inven_to_slot_wrapper` | `native_inventory_hook.cpp:759-780`（wrapper）、`:1145-1148`（安装） |
| H-07 | `ITEMSYSTEM_PutJewel` | `put_jewel_wrapper` | `native_inventory_hook.cpp:782-787`（wrapper）、`:1149-1151`（安装） |
| H-08 | `INVEN_IsHavingEmptySlot` | `is_having_empty_slot_wrapper` | `native_inventory_hook.cpp:333-343`（wrapper）、`:1152-1155`（安装） |
| H-09 | `CHAR_UnequipItemToInven` | `unequip_item_to_inven_wrapper` | `native_inventory_hook.cpp:345-356`（wrapper）、`:1156-1159`（安装） |
| H-10 | `UIEquip_ButtonEquipExe` | `button_equip_exe_wrapper` | `native_inventory_hook.cpp:789-802`（wrapper）、`:1160-1163`（安装） |
| H-11 | `UIEquip_ButtonUnequipExe` | `button_unequip_exe_wrapper` | `native_inventory_hook.cpp:814-829`（wrapper）、`:1164-1167`（安装） |
| H-12 | `UIEquip_OKConfrimUseItem` | `ok_confirm_use_item_wrapper` | `native_inventory_hook.cpp:831-839`（wrapper）、`:1172-1175`（安装） |
| H-13 | `INVEN_SaveItem` | `save_item_wrapper` | `native_inventory_hook.cpp:450-470`（wrapper）、`:1180-1183`（安装） |
| H-14 | `INVEN_MoveItem` | `move_item_wrapper` | `native_inventory_hook.cpp:702-757`（wrapper）、`:1184-1187`（安装） |
| H-15 | `UIEquip_EquipControlEventProc` | `equip_control_event_proc_wrapper` | `native_inventory_hook.cpp:946-956`（wrapper）、`:1188-1191`（安装） |
| H-16 | `UIEquip_RefreshItemArea` | `refresh_item_area_wrapper`；原版一次刷新走 trampoline 后由 gate 做 post-projection，模块主动刷新走 raw-original dispatcher | `native_inventory_hook.cpp:669-677`（wrapper）、`:1192-1195`（安装）；`game_ui_virtbag.cpp:237-246`（gate）；`extension_bag_render.inc:533-568` |
| H-17 | `ITEM_GetCumulateCount` | `get_cumulate_count_wrapper`；S2 读侧统一解码（R-49）：count-encoded 类别按模式视图解码 `+0x10`（启用态 `128a+b`、关闭态低 7 位 b，R-47 决策 b），非可堆叠/类别未知（kUnknown）original-first backup（R-46 fail-closed） | `native_inventory_hook.cpp`（wrapper，分流在 `inventory_hook_stage4.cpp:stage4_get_cumulate_count`）、安装链第 4 个 |
| H-18 | `INVEN_SaveItemDirect` | `save_item_direct_wrapper`；同身份堆叠合并时采集目标槽旧全量、backup 后确认原版 b 写再回写 a+b（R-45/R-49） | `native_inventory_hook.cpp:save_item_direct_wrapper`、安装链第 20 个 |
| H-19 | `ITEMSYSTEM_Divide` | `item_system_divide_wrapper`；拆堆源堆借位与新对象设值（R-45/R-49） | `native_inventory_hook.cpp:item_system_divide_wrapper`、安装链第 21 个 |
| H-20 | `ITEMSYSTEM_MakeItem` | `make_item_wrapper`；**数量回写已撤销**（arg2 是静态表查找/品质参数非数量，R-52/VM-38），wrapper 纯透传、回写计划经纯函数 `stage4_make_item_writeback_count` 恒 0 | `native_inventory_hook.cpp:make_item_wrapper`、安装链第 22 个 |
| H-21 | `INVEN_RemoveItemData` | `remove_item_data_wrapper`；快照/重扫 + `stage4_remove_item_data_plan` 修正部分删堆（R-45/R-49），并做扩展桥接（R-56）：物理实扣不足时按 category 从扩展袋补扣 | `native_inventory_hook.cpp:remove_item_data_wrapper`、安装链第 23 个 |
| H-22 | `UIEquip_ButtonDestroyExe` | `button_destroy_exe_wrapper`；R-55 预演标记源：进入按钮执行时置 thread_local，供 H-23 区分「按钮预演」与「弹窗 OK 真实结算」 | `native_inventory_hook.cpp:button_destroy_exe_wrapper`、安装链第 13 个 |
| H-23 | `UIEquip_OKDestroyItem` | `ok_destroy_item_wrapper`；R-55 原版背包详情出售接管：启用态弹窗 OK 完全接管结算（canonical 全量、`unit×count×7/10` 加钱、删整堆、刷新），按钮预演只回填展示金额；关闭态/非背包详情/校验失败 backup | `native_inventory_hook.cpp:ok_destroy_item_wrapper`、安装链第 15 个 |

1. 安装目标必须来自 `game_symbols.h`/resolver；域文件不得新增裸 VMA。
2. 每个 Hook 都必须保留 backup；Hook 不是默认机制，架构册规定优先读内存、调用函数、
   PtrHook、patch，只有必要时才引入 Native Hook（`docs/development/architecture.md:148-154`）。

### 3.2 original-first 与安装事务

1. 查询 Hook 先调用原版，再仅在原版没有结果时查扩展；`FindItem` 的实现和递归保护见
   `native_inventory_hook.cpp:92-115`，Have/Count 的统一分流见
   `inventory_hook_stage4.cpp:3-23`。
   1.1. H-17 `ITEM_GetCumulateCount` 是唯一例外：count-encoded 类别不先调 backup——
   backup 体内的数量位段仍按原版/S1 布局读取，会与 S2 解码冲突；因此对 kEncoded 直接
   返回模式视图解码值（启用态 `stack_codec::effective_read_count` 全量、关闭态只读 b，
   R-47 决策 b），其余类别保持 original-first backup
   （`inventory_hook_stage4.cpp:35-48`）。
2. `ConsumeItem`、`RemoveItem`、装备和镶嵌必须按对象身份分流；扩展对象不能先落入
   原版释放或消费函数。
3. 对可回退的非扩展路径，original backup 是唯一回退；不得复制一份近似原版逻辑。
4. 安装任一 Hook 失败时，已经成功安装的 Hook 必须按成功顺序逆序卸载，并清空 backup。
   当前链的 rollback 实现在 `native_inventory_hook.cpp:428-450`，安装循环在
   `native_inventory_hook.cpp:555-618`。
5. rollback 不完整时必须阻断重试，当前语义是 `g_install_blocked=true`
   （`native_inventory_hook.cpp:445`）。
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
> 对码确认 R-26..R-44 为当前实现的同类耦合规则；R-45..R-49 为已批准的 S2 数量编码
> 契约规则（读侧、写侧与关闭态语义均已落地；真机锚 VM-32..VM-36 待取证）。

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

### R-05 空槽判定不能只看 descriptor，且扩展兜底只能回答普通袋域

- **规则一句话**：容量判断必须使用派生容量和 object/token 可分配条件，不能只数 descriptor 空槽。
- **任务袋域边界**：`INVEN_IsHavingEmptySlot@0x103460` 的 `include_task_bag!=0` 只查物理任务袋 5、`==0` 只查普通袋 0..4（反汇编 csel `0x103488/0x103490`）；扩展逻辑袋不属于任务袋域，`stage4_is_having_empty_slot` 的扩展兜底只允许回答 `include_task_bag==0`，任务袋域查询必须只信原版结果。
- **为什么**：`stage4_is_having_empty_slot` 同时服务生产者和商店 gate（`inventory_hook_stage4.cpp:94-119`），扩展真实空槽还由 `extension_bag_has_empty_slots` 提供；若任务袋查询用扩展空位改判为 1，上层（`QUESTSYSTEM_CheckPrepare`/`CheckReward`）会误判任务袋有空间并继续，随后 `INVEN_SaveItem` 按 class 路由到袋 5 失败被 H-13 收编进扩展袋（R-53 边界漂移）。
- **典型破坏方式**：收编、生产或购买误报有容量，最终写入不可用槽或错误提示满包；任务袋域查询被扩展空位改判为可放。
- **验证锚**：`test_queries`，`tests/test_inventory_hook_stage4.cpp:137-157`（普通域原版 0 → 扩展 1；任务袋域原版 0 → 0 且 `empty_extension` 未被调用；`needed<=0` 两域均 1 且不查扩展）；源码 `inventory_hook_stage4.cpp:106-116`。

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

- **规则一句话**：active 或 pending_release 对象必须拒绝替换、释放和槽复用；释放守卫即使
  `active=false` 也必须拒绝 `pending_release=true` 的重复释放并记录日志；ownership ledger
  耗尽时拒绝登记；触摸窗口内的释放请求必须进入定长延迟回收队列并以成功释放路径返回，
  排空前通过投影控件和对象缓存引用检查。
- **为什么**：`module_slot_is_assignable_locked` 和 release guard 同时检查 token 状态；`retire_custody_item_locked` 在触摸窗口内只退 custody、不立即物理释放，队列排空由 draw-end 安全点执行（`extension_bag_ownership.inc`）。
- **典型破坏方式**：确认使用尚未完成时释放对象，产生悬挂指针和 token mismatch；在无可用 handle 时继续登记对象。
- **验证锚**：`deferred free enqueue`/`deferred free drain`；S-05、VM-29；源码
  `extension_bag_ownership.inc:35-53,71-119,147-171,237-254`。

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

- **规则一句话**：同步接口持扩展锁时只能执行已验证的投影控件修复；移动事务若必须刷新原版区域，只能经 R-44 的受控 raw-original dispatcher，不能直接递归进入被 Hook 地址。
- **为什么（修正）**：素材说 `extension_bag_sync_projected_bag` 当前会触发原版刷新；对码显示它只锁内调用 `refresh_projection_if_overwritten_locked`（`extension_bag_public_runtime.inc:320-324`），该函数只比较并设置控件（`extension_bag_render.inc:547-568`）。真正的原版刷新路径是 `refresh_projected_module_view_after_move_locked` 的 `refresh_module_item_area_locked`（`extension_bag_render.inc:571-576`）。
- **典型破坏方式**：未来把原版 RefreshItemArea 塞进同步接口，形成递归刷新、锁序反转或容量错位。
- **验证锚**：Host `test_original_only_queries`；真机 VM-15；本条为“修正后收录”，不是对素材原句的照抄。

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
- **任务袋边界**：当前来源为物理袋 `5` 时，页签切换必须先选择合法原版宿主（当前实现优先扫描 `0..4`），不得让 `5` 成为投影窗口；切回原版袋 `5` 仍按原版页签语义恢复。
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
  H-15、H-4、T3 安装/恢复或 S-03 apply 分支口径；S-03、S-05 已由真机确认；H-16 负责刷新后的
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
  `ITEMPOOL_Free`，也不得因触摸窗口活动或延迟队列已满而拒绝事务提交；队列满时将对象放入
  有界 custody 保管区，保持对象 live，事务继续成功。非触摸窗口的排空只在无投影控件和
  逻辑槽引用时调用 `ITEMPOOL_Free`。
- **为什么**：触摸窗口仍可能由 moving 控件、projection 或逻辑槽持有对象；直接释放或
  拒绝 source release 会分别形成 UAF 风险或 `extension merge source release rejected`
  事务失败。队列、队列满时的 custody 保管区、引用检查和 draw-end 排空见
  `extension_bag_ownership.inc:35-53,71-119,147-171` 与 `extension_bag_render.inc:712-770`。
- **验证锚**：日志 `deferred free enqueue`/`deferred free queue full; custody retained`/
  `deferred free drain`；S-05、VM-29。

### R-37 卖出价格必须先收敛堆叠和数值边界

- **规则一句话**：卖出取价先按 `stack_codec::max_count(stack_limit_enabled())` 收敛
  `count`（99/999），再校验单位价和最终价均不超过 `INT32_MAX`；任一价格非法时
  不创建确认弹窗、不写入弹窗参数、不加钱或删除物品，API 以失败返回。
- **为什么**：`ITEM_GetSellPrice` 的特殊类别/能力/payload 分支和异常堆叠数量都可能
  放大最终价；弹窗参数虽使用 `uint32_t`，但业务金额边界必须更严格，避免显示溢出或
  错误结算。
- **variant**：装备页原版卖出保持 `100%`，装备页销毁/粉碎保持 `70%`；商店页和
  API 保持 `100%`，不得因边界修复串改宿主语义。
- **日志锚**：成功使用
  `sell price source=equip|store category=.. count=.. unit=.. variant=.. final=.. payload=.. gen=..`；
  拒绝使用 `sell price reject reason=..`。实现锚为
  `extension_bag_port.cpp:116-126`、`extension_bag_equip.inc:924-1009`、
  `extension_bag_store.inc:503-660` 和 `game_inventory_use.inc:175-200`。
- **验证锚**：`test_sell_price_bounds`、`test_virtual_bag_json_count_clamp`；真机
  `VM-17`、`VM-18`、`VM-30`。无效价格不得以 UI 未弹出之外的静态结果判为通过。

### R-38 持久化 canonical 数量不随配置截断

- **规则一句话**：载入状态 JSON 时，canonical `count` 只按绝对合法范围 `0..999`
  校验/收敛，禁止使用当前 `stack_limit_enabled()` 截断已持久化数量；可堆叠物品的
  payload 数量位仅在与 descriptor 不一致时修正为同一个 canonical 值，装备等非适用物品
  的 payload 必须逐字节保留。99/999 clamp 只作用于新建、合并、消费和派生操作。
- **为什么**：`game_symbols.h:58` 规定物品对象 `+0x10` 的 bit22..31 中 `100` 是装备
  语义，而 `1..999` 才是可堆叠数量；序列化头部将同一字段落在
  `virtual_bag_state.h` 的 payload `+11`。`stack_codec::s2_write_count` 会保留其余
   位（`core/native/stack_codec.h`），但不能改变这些位在非堆叠物品中的业务语义。
  `stack_limit_enabled()` 是运行时操作配置，不是 sidecar 数量语义的组成部分。
- **判据单源**：`item_is_equip`、`category_is_equip` 与载入/扩展合并适配均使用
  `ITEMCLASSBASE +6 bit0` 语义；载入路径通过 `virtual_bag_category_uses_stack_count`
  注入同一类别判据，模型不自行猜测 category 范围。
- **典型破坏方式**：把 descriptor 的 `count=1` 无条件写入装备 payload，将装备原本的
  `100` 语义改成 `1`，后续 `SAVE_LoadItem` 按错误字段解释，表现为未鉴定/图标和详情
  改变。已被写坏且已持久化的存档不由本修复逆向恢复，必须回档到最后正常存档；
  `validate_serialized_payload_buffer` 只能判定长度前缀和尾部等结构完整性，结构合法不
  等于装备语义未被改写，仍需将 payload 的数量位与原始/最后正常 payload 对照。
- **当前实现事实**：原版数量读取、存档查找和非空槽检查的数量路径已覆盖；装备/损坏判定
  保留原版 marker 语义，不纳入数量布局或上限 patch；数量位段固定为 bit22..31。sidecar
   读档不改写、不因配置截断已有 canonical 数量；超过绝对上限 999 的 descriptor 记录
  收敛到 999 并记录日志，payload 仅按 descriptor 不一致同步。
- **验证锚**：`test_virtual_bag_json_count_clamp`（`tests/test_host.cpp`）断言装备 payload
  逐字节不变、绝对超限收敛、双向跨配置读档不变和 descriptor/payload 不一致同步；真机
  存档回归见 `VM-30`。

### R-39 原生袋对象 marker 不得覆盖容量

- **规则一句话**：原生袋对象 `+0x10` 只允许改 bit25..31 的 marker，bit0..24 容量必须保留；
  三个袋对象创建/刷新路径统一调用 `stack_codec::write_native_bag_object_marker`，禁止
  使用普通物品的 `stack_codec::write_count`。
- **为什么**：`INVEN_GetBagSize` 读取 bit0..24；普通数量位从 bit22 起，与容量重叠，误写会
  造成容量越界和后续 UI/库存访问风险。
- **验证锚**：`test_stack_codec` 的位段保留断言；真机 `VM-09`/`VM-31` 核对袋容量和切换后
  原版窗口状态，安装日志须保留 `count=24`。

### R-40 类别判定不可用时必须 fail-closed

- **规则一句话**：`item_count_encoding` 与 `category_is_equip` 必须返回三态；表基址、静态表
  或 stride 不可用时返回 `kUnknown`，所有堆叠、合并、装备/强化和 payload 处理调用方必须
  拒绝或放行原版安全路径，不得按可堆叠或可装备处理。
- **调用方范围**：`state_json`、`extension_bag_equip` 消费、`game_inventory_basic`、
  `game_patch_move_merge`、扩展 apply/装备按钮路径。
- **验证锚**：`test_virtual_bag_json_count_clamp`、`test_virtual_bag_mergeable_items` 和
  `test_virtual_bag_payload_helpers` 的已知/非适用/未知判定断言；真机 `VM-03`、`VM-10`、
  `VM-13`。

### R-41 payload count 修改必须自带类别门控

- **规则一句话**：`patch_payload_count` 必须接收并校验同一 `CategoryStackCountPredicate`；
  非 `kEncoded` 类别（含 `kUnknown`）直接 no-op、记录日志并返回失败。合并比较路径必须
  使用同一判据，不能先改 payload 再判定类别。
- **验证锚**：`test_virtual_bag_payload_helpers` 断言装备/未知类别 payload 不变，
  `test_virtual_bag_mergeable_items` 断言装备不合并；真机 `VM-13`。

### R-42 BagType 判定与原生物品类别判定不得混用

- **规则一句话**：`is_backpack_category` 仅用于已归一化的扩展 `BagType 1..4` 容量路由；
  `category_is_extension_backpack` 仅用于原生物品 `ITEMCLASSBASE +2 == 0x1f` 的详情/按钮
  判定。两者语义不同，调用点不得用一个范围判定替代另一个静态表判定。
- **验证锚**：`test_virtual_bag_state` 的 `derive_capacity` 断言；真机 `VM-05`、`VM-29`。

### R-43 数量 patch 表必须先证明位段语义

- **规则一句话**：任何数量布局或 99/999 上限 patch 在进入
  `game_patch_core.inc` 前，必须由反汇编的数据来源和读写上下文证明目标位段承载堆叠
  数量；装备 marker、背包容量及其他非数量位段一律不得纳入 patch 表。
- **为什么**：`ITEM_IsRealEquip`/`ITEM_IsRealBroken` 读取的是
  `ITEMSYSTEM_CreateItem` 写入 bit25..31 的装备 marker `100`；将其按数量从 bit22 读取并
  套用 999 比较会把装备判定翻转，导致装备显示未鉴定或被视为损坏。
- **验证锚**：`llvm-objdump` 复核 `0x105b58/0x105b60/0x105c18/0x105c20` 与
  `ITEMSYSTEM_CreateItem@0x10c030..0x10c038`（函数内 `+0x194..0x19c`）；真机 `VM-30`
  断言启用上限后装备显示和详情正常。

### R-44 持锁调用原版回调必须启用 TLS 原版直通

- **规则一句话**：持有 `g_virtual_bag_mtx`（非递归锁）期间禁止调用可能回调库存 Hook 的原版函数；确需调用必须二选一：①置 TLS original-only 守卫（`NativeCallScope`），使查询类 Hook 直通原版 backup；②解锁调用原版、重新加锁后复核状态（删除类按物理槽后置条件确认）。
- **为什么**：`g_virtual_bag_mtx` 是非递归 `std::mutex`，同线程重入取锁即自死锁。已证实的死锁链：`0x18` release 持锁 → orig→ext 事务 `move_original_to_extension_locked` 调 `fn_remove_item_direct` → 原版 `INVEN_RemoveItemDirect` 清槽后同步回调 `PLAYER_UpdateShortcut` → 触发 H-03 `GetItemCount` Hook → 同线程重入取锁 → 自死锁。原版删除/刷新同样可能回调 `FindItem`、`HaveItem` 或 `IsHavingEmptySlot`。
- **典型破坏方式**：绕开受控入口持锁直接调用 `INVEN_RemoveItemDirect` 或 raw refresh，原版回调进入扩展查询后游戏冻结；删除失败时仅依据返回值提交逻辑事务，造成源槽与 sidecar 分裂。
- **受控入口清单**：dispatcher 本体与全部受控调用点的当前文件:行，见架构册 §5.4。
- **验证锚**：Host `test_original_only_queries`、`test_virtual_bag_transaction_domain`（含 `original_to_extension_source_slot_postcondition`）；真机 `VM-15` 的 `txn committed ... original->extension`、`release_cleanup ctl=... flags_cleared` 与 API health 连续可达证据；源码 `game_ui_virtbag.cpp:246-269`（删除 dispatcher）、`:283-286`（刷新 dispatcher）、`native_inventory_hook.cpp:89-154`（查询 Hook 直通）。

### R-45 S2 数量编码布局与总量范围

- **规则一句话**：可堆叠（count-encoded）物品的数量位段按 S2 布局解释与写入：
  `a=(v>>22)&0x7`（bits22–24 高位段）、`b=(v>>25)&0x7F`（bits25–31 低位段，即原版
  字段），`count=128a+b`，编码总量范围 `0..1023`，业务上限 `999`；模块启用态的总量
  读写统一走 S2 编码 API（或等价的模式感知 API 启用态分支），禁止绕过编码 API 直接
  拆写单个位段。原版侧 clamp 只允许
  作用于「数量完整值」判定点（比较数来自 getter hook 解码总量或函数入参总量）；
  直接作用于 b 段残量（mod 128）的判定点与 fill 写点不得 999 化，保持原版 99 的
  b 满判定（fail-closed），登记见架构册 §2.1.1 clamp 口径。
- **为什么**：原版单字段 7 位只能表达 `0..127`；S2 在不移动原版 7 位字段位置的前提下，
  用 bits22–24 作高位段扩展到 `0..999`，并保持原版低 7 位恒等于 `count mod 128`。
  b 段残量点上写 999 无算术意义且会高估剩余空间（不 fail-closed）。
- **验证锚**：Host `test_stack_codec_s2`（S2 拆段编解码往返与位保留）、
  `test_stack_codec_effective_mode`（模式感知层）；读侧解码经
  H-17 落地（`native_inventory_hook.cpp` `get_cumulate_count_wrapper`），真机展示/查询核对见 `VM-33`
  （待取证）；写侧进位/门控见 `VM-34`（待取证）。

### R-46 S2 高位段读写必须类别门控且 fail-closed

- **规则一句话**：`a`（bits22–24）的读与写只对 count-encoded（可堆叠）类别生效；
  非可堆叠类别一律不碰 bits22–24——宝石选项位 bits18–23、袋容量位 bits0–24 与高位段
  重叠，装备 marker 位 bits25–31 与低位段重叠，数量路径均不得改写；类别判定不可用时
  按 R-40 fail-closed 拒绝，不得按可堆叠处理。模块数量写点登记（S2-P3）：
  `virtual_bag::patch_payload_count`（`model/virtual_bag_transaction_rules.inc`，唯一
  payload 写入口）、`api/native/game_inventory_basic.inc` 的 `data_op_add_item`（创建
  直写点）、`extension_bag_equip.inc` 的 `consume_extension_item_after_native_locked`
  （消耗写回点）——三处均先经 `item_count_encoding`/注入谓词门控；袋对象 marker 写入
  保持 `write_native_bag_object_marker`（仅 bit25）。
- **为什么**：bits22–24 与袋容量（bits0–24）、宝石选项（bits18–23）位段重叠，
  bits25–31 在装备上是 marker `100` 语义；无门控的高位/低位写入会破坏容量、宝石选项
  和装备判定。
- **判据单源**：类别判定沿用 R-40/R-41 的同一 count-encoded 谓词（`item_count_encoding`），
  不得在 S2 读写点另写一套类别区间。
- **验证锚**：读侧门控已落地——H-17 仅对 kEncoded 解释 bits22–31，kUnknown/非可堆叠
  fail-closed 走 backup（`inventory_hook_stage4.cpp:35-44`），分流边界由 Host
  `test_get_cumulate_count_s2`（kEncoded 直达不调 backup、kNotEncoded/kUnknown/空指针
  走 backup、backup 缺失兜底）断言；Host 写侧门控
  `test_virtual_bag_payload_helpers`（非 count-encoded/未知类别拒绝，`test_host.cpp`）；
  真机读侧核对见 `VM-33`（待取证），装备/宝石/袋对象不变式另见 `VM-32`；写侧门控与
  进位见 `VM-34`（待取证）。

### R-47 关闭态所有数量读写与操作按低 7 位视图（决策 b）

- **规则一句话**：堆叠上限关闭态下，模块与原版的数量读、写、操作与展示统一按低 7 位
  视图——有效数量 = `b`（`count mod 128`），上限 99；`a`（bits22–24）不读、不写、
  不参与运算，只原样保留；重新开启后经 S2 解码读回完整 canonical 值，关闭期操作
  造成的 `128a+b` 变化是决策 b 的既定语义，不按数据损坏处理，也不得引入关闭期的
  反向同步或 `a` 清零。
- **实现锚（模式感知 helper，唯一模式入口）**：
  `stack_codec::effective_read_count / effective_write_count / effective_clamp /
  effective_view_count`（`core/native/stack_codec.h`）；读侧 getter 分流
  `stage4_get_cumulate_count`（`inventory_hook_stage4.cpp`）；模块合并按视图相加且
  descriptor 回写 canonical（`extension_bag_transaction.inc`）；H-04 消耗借位预置仅在
  启用态生效（`native_inventory_hook.cpp` `consume_item_wrapper`——关闭态 b≤1 消耗到
  空按原版删除，不为了保留 a 而让有效数量 0 的堆无法消耗）。
- **descriptor canonical 边界（R-38/R-48 不变）**：descriptor `count` 与 sidecar
  持久化恒为 canonical `128a+b`；关闭态只改变运行时视图与操作算术，读档不 clamp、
  不按配置重编码。示例：canonical 199（a=1,b=71）→ 关闭态读 71；关闭态消耗 1 →
  b=70（a 保留）→ 重开读 198；关闭态写 99 → 只改 b → 重开读 227（128a+99）。
- **为什么（决策 b）**：读、写、操作、展示统一收敛到 b 视图，上限 99 与原版 clamp 表
  关闭态 revert 后的原生语义一致；`a` 只保留、不读不写不参与运算，重开即可恢复完整
  canonical 值，关闭期单次操作不会放大成数量跳变，也不需要关闭期的反向同步。
- **验证锚**：Host `test_stack_codec_effective_mode`、`test_virtual_bag_mode_aware_ops`
  （`tests/test_host.cpp`：199 off 读 71/重开 199、off 写 99 → 227、off 消耗 → 198、
  127/128/99 off 视图、合并按视图且 a 保留）、`test_get_cumulate_count_s2`
  （getter 分流两态视图）；真机 `VM-36`（199→71、重开 199/198/227、off 上限 99）。

### R-48 sidecar 无版本标识，数量位统一 S2，模式仅影响运行时视图与 clamp

- **规则一句话**：sidecar 状态 JSON 不携带 `encoding_version`（也不携带任何编码版本
  字段）；数量位只有 S2 布局（`a` 落 bits22–24、`b` 落 bits25–31，`count=128a+b`）
  一种，持久化与 descriptor 读写统一 `s2_read_count`/`s2_write_count`（canonical 域）；
  运行时操作面（展示/查询/消耗/合并/卖出/创建）统一经模式感知层
  `effective_read_count`/`effective_write_count`/`effective_clamp`/`effective_view_count`
  （启用态等价 canonical 全量、关闭态低 7 位视图，R-47 决策 b）；历史 sidecar 若携带
  `encodingVersion` 字段，解析时宽容忽略（不报错、不迁移）。S1→S2 迁移函数与原版袋
  `+0x10` 扫描状态机已删除，当前存档视为已迁移。
- **读档不改写 canonical（R-48 载入侧约束）**：读档路径与 `stack_limit_enabled()`
   完全解耦——已持久化的 canonical 数量（sidecar descriptor）原样保留，读档不 clamp、
   不按当前配置重编码；`stack_limit_enabled()` 只允许出现在新建/合并/消费/派生等操作
   层，且仅经 `effective_clamp`（99/999 上限）与 `effective_view_count`（关闭态运行时
   视图）影响运行时，绝不影响持久化解码或编码。sidecar payload 仅与
   descriptor canonical 对齐（descriptor 是唯一 canonical），与运行时模式无关。
- **为什么**：解码随配置模式变化（或残留 S1 读路径）会让同一存档在不同开关态读出
  不同数量（真机实证：关闭态 S2 写入 99，开启态被 S1 解码读出 792 且随后固定）。
  唯一布局 + 统一 S2 读写 + 宽容忽略旧字段消除了整类「读写代数不一致」缺陷，且开发期
  之外的用户不接触旧格式，无需版本协商。
- **验证锚**：Host `test_stack_codec_mode_invariance`（99/127/128/217/999 两态写入
  产物逐位一致、解码一致、切换模式不改值）、`test_stack_codec_s2`（S2 编解码往返与
  位保留）、`test_virtual_bag_cross_config_roundtrip`（99/127/128/217/999 跨配置三
  往返 payload 逐字节不变 + 旧 `encodingVersion` 字段宽容忽略）；真机 `VM-32`、
  `VM-35`（跨配置往返与模式不变性，见验证矩阵）。

### R-49 S2 读侧统一 getter hook，写侧无统一 setter

- **规则一句话**：S2 数量读取统一经 `ITEM_GetCumulateCount@0x106094` 的 getter hook
  按模式视图解码（启用态 `128a+b` 全量、关闭态只读 b，R-47 决策 b）；写侧不存在统一
  setter，每个知道物品身份的调用方必须各自完成类别门控与模式感知写
  （启用态 `128a+b` 进位/借位、关闭态只写 b 保留 a）；禁止引入 `UTIL_SetBitValue`
  Hook 充当统一写入点。
- **为什么**：原版写路径没有单一「写数量」函数，各 caller 直接改 `+0x10` 位段；
  `UTIL_SetBitValue` 是无物品指针的通用位工具，hook 点无法判定类别（可堆叠/宝石/
  袋容量/装备 marker 共用），也无法计算进位。分层事实见架构册。
- **16 函数写点收口状态（2026-09-11 反汇编冻结，基线 `.tmp/dualmode-full.asm`）**：
  已接管 5 个——`INVEN_MoveItem`、`INVEN_SaveItemDirect`、`ITEMSYSTEM_Divide`、
  `INVEN_ConsumeItem`、`INVEN_RemoveItemData`（H-21 快照/重扫 +
  `stage4_remove_item_data_plan` 修正部分删堆，启用态专用）；数量回写已撤销 1 个
  ——`ITEMSYSTEM_MakeItem`（R-52：arg2 是静态表查找/品质参数非数量，产物量由
  原版 CAL 公式写点生成且 ≤99，b 写即全量）；无需接管 8 个——`UIStore_BuyItem`
  （购买量 max=99 立即数 `0xd17cc`、marker 写非数量）、`ITEMSYSTEM_CreateItem`
  （数量写点恒 b=1、ABI 单参 category）、`UIMix_StartMix`（产物量=配方表只读静态
  数据）、`DEALSYSTEM_MakeSale`（唯一 31/25 写点 marker=126、数量产生经 MakeItem）、
  `GAME_StartNewGame`（初始量常量 5）、`INVEN_SaveItemData`（每笔 ≤99：
  `cmp #0x62` 分支证据）、`ITEMSYSTEM_ProcessUnpack`（产物量=拆包表只读静态数据）、
  `MAPITEMSYSTEM_CreateItem`（count 域=任务/事件表只读数据）；待勘察 2 个——
  `NetworkStore_InitializeMenuData`、`NetworkStore_AddItem`（数量来自网络数据，
  离线不可达无法冻结值域）。**注意**：MakeItem 曾按 `game_symbols.h` 注释把 arg2
  当 count 回写（`count>1` 恒真），导致掉落物数量污染为 2/3/4/5（R-52/VM-38）。
- **验证锚**：读侧已落地——H-17 `get_cumulate_count_wrapper`
  （`native_inventory_hook.cpp:294-318`，分流 `inventory_hook_stage4.cpp:35-48`，
  安装 `native_inventory_hook.cpp:894-897`）；Host 分流断言
  `test_get_cumulate_count_s2`（`stage4_hook_tests`：kEncoded 两态解码直达且不调
  backup、kNotEncoded/kUnknown/空指针 fail-closed 走 backup、backup 缺失兜底、模式
  切换不丢值）；真机 `VM-33`：
  数量 99/100/127/128/199/999 显示与查询走 S2 解码（待取证）。写侧调用方级门控与进位
  已落地（S2-P3）：数量产生点统一 `effective_write_count`/`effective_clamp` 写回，
  登记点见 R-46 规则一句话；H-21 分流断言 `test_remove_item_data_plan`
  （`stage4_hook_tests`：跨 127 借位、旧 a 残留、已正确不写、全删、count≤0、
  多缩减/越域 fail-closed）；真机 `VM-34`：创建/合并/消耗跨 127 进位、非可堆叠不触碰
  bits22–24（待取证）。关闭态读写语义已落地（S2-P5，R-47 决策 b），真机 `VM-36`
  （待取证）。非 getter 读点白名单见架构册 §3.7。

### R-50 apply 准入必须校验目标装备类，非装备目标回退扩展事务

- **规则一句话**：宝石/强化卷轴拖放的 apply 分支准入为「同袋 + 扩展宝石/强化卷轴源 +
  目标为装备类（`item_is_equip(target)` 为真）+ `UIEquip_IsApplyStuff(target, source)`
  判真」四条同时成立；任一不成立必须 fallthrough 到既有扩展交换/移动，不得吞事件、
  不得 Blocked。
- **为什么**：`item_is_equip` 语义是「不可堆叠」（ITEMCLASSBASE+6 bit0==0），非堆叠普通
  物品同样满足；若只按「不可堆叠」近似装备，普通物品目标会误入 apply，被
  `IsApplyStuff` 拒绝后事件被吞、交换失效（`stage4_is_extension_apply_candidate`
  判据集中在 `inventory_hook_stage4.cpp`，消费方为 G-8 路由
  `extension_bag_public_runtime.inc` 与 p5 session drop `extension_bag_transaction.inc`；
  H-15 `game_ui_virtbag.cpp` 的装备槽事件适配同一口径）。
- **典型破坏方式**：用 `kNotEncoded` 代替 `item_is_equip` 判装备；类别数据不可用时返回
  Blocked 而不是按非装备回退；apply 准入失败后提前 return 跳过
  `session_finish_transaction`（p5 会把 session 泄漏在 kTransactionInFlight）。
- **验证锚**：Host `stage4_hook_tests` 的 `stage4_is_extension_apply_candidate` 断言
  （宝石/卷轴+装备目标→apply；+普通物品目标→非 apply 应 swap；普通物品源→非 apply；
  IsApplyStuff 假→非 apply）；真机口径归 `verification-matrix.md` S-03/VM-10
  （宝石/卷轴→装备=apply；→普通物品=swap）。

### R-51 出售/拆堆直接位读必须重定向到统一 getter，禁止单指令窗口 patch

- **规则一句话**：`UIStore_ButtonSellExe`（`0xd18d0`/`0xd18e8`）、`UIStore_SellItem`
  （`0xd26b8`）、`ITEMSYSTEM_Divide`（`0x108458`/`0x10848c`）共 5 条以
  `ldr w0,[xN,#0x10]; bl UTIL_GetBitValue(_,31,25)` 直接读 b 段的数量读点，必须
  重定向为 `mov x0,xN; bl ITEM_GetCumulateCount(item)`（`g_stack_getter_redirect_patches`），
  经统一 getter 的类别门控与模式感知解码读全量；**禁止**把 `GetBitValue` 的 start
  常量 25→22 当全量读。
- **为什么**：S2 布局中 a（bits22–24）在窗口低位、b（bits25–31）在高位，十位窗口
  `(v>>22)&0x3FF` 在数值上是 `a + 8b`，与 `a*128+b` 不等价（`count=1023` 的巧合相等
  不能推广）；直接读 b 会让整堆出售判定、出售数量输入框上限与拆堆守卫按 b 残量
  （真机实证：200 个中药水按 b=72 结算，实得 546 而非 1540）。重定向到已 hook 的
  `ITEM_GetCumulateCount` 复用 R-46/R-47/R-49 的门控与解码，且同属 libgame.so
  （bl 无需 thunk）；getter 关闭态返回 b，与原版逐位一致，故重定向常驻、不随上限
  开关 revert。
- **典型破坏方式**：用单指令窗口 patch 伪造全量；在 `UTIL_GetBitValue` 通用位工具上
  hook（无物品指针，无法判类别，R-46）；把 `SetBitValue` 写点 start 22 化（`v<<22`
  会把 v 低 3 位写进 a、高 7 位写进 b，反序布局）。
- **验证锚**：Host `test_sell_divide_s2_read_window`（getter 解码两态等价 + 窗口伪修复
  反例 + 200=a1b72）；真机 `VM-37`：`logcat` 出现 5 条 `Inotia4Qol ... domain=platform ... redirect ...`
  且 `stack canonical layout applied`；商店整堆/部分出售金额与 UI 数量一致（待取证）。
- **表范围**：`g_stack_getter_redirect_patches` 当前仅上述 5 条；装备页详情出售结算
  函数 `0x1261c4` 的内联 b 段读点（`0x126208`，由 `UIEquip_OKDestroyItem@0xb8468`
  调用）当前不在表内——启用态由 R-55 H-23 `UIEquip_OKDestroyItem` 在回调层整体接管，
  关闭态与装备详情仍走原版 b 段语义。

### R-52 数量产生点入参语义必须以反汇编数据来源证明，不得按签名注释臆断

- **规则一句话**：任何原版函数的「数量入参」判定必须有反汇编证据证明该入参进入
  数量写点；仅凭 `game_symbols.h` 注释或函数名推断的入参不得用于 S2 数量回写，
  拿不到可信数量源时 fail-closed 不写（R-46/R-49）。
- **为什么**：`ITEMSYSTEM_MakeItem`（`0x10c6c8`）的 arg2（w1）实为与静态表 +0x2 域
  匹配的查找/品质参数（`CHARSYSTEM_DropItem` 传 2..5、`DEALSYSTEM_MakeSale` 传 5），
  产物数量由函数内 `CAL_Calculate` 公式写点（`0x10ca3c`，域 ≤99，b 写即全量）生成；
  曾按签名注释把 arg2 当 count 回写（`count>1` 恒真），把掉落物数量污染成 2/3/4/5
  （真机实证：药水 2/卷轴 3/材料 4）。撤销回写后 `make_item_wrapper` 纯透传，
  回写计划经纯函数 `stage4_make_item_writeback_count`（恒 0）。
- **典型破坏方式**：用 `count > 1` 之类的「非零即回写」守卫；把品质/查找参数、
  category、flag 当数量；在缺少数量来源时猜测写入。
- **验证锚**：Host `stage4_hook_tests` 的 `test_make_item_writeback_count`（掉落
  品质分支 2..5 与商店货架 arg2=5 全部返回 0，边界/负数/未知类别同样 0）；真机
  `VM-38`：拾取/掉落可堆叠物品一次得 1 个（待取证）。

### R-53 SaveItem 漏斗在入库前必须先尝试并入扩展袋同类堆，再新建空槽

- **规则一句话**：`INVEN_SaveItem` 漏斗的两条收编路径都必须在新建槽之前先合并：
  ① backup 前 `virtual_bag_merge_native_item`（原版有空槽时也不让物品去原版开新格）；
  ② backup 失败后 `virtual_bag_adopt_native_item`（原版满时）。两条路径共用
  `adopt_merge_into` 判据（`mergeable_items` 身份 + 模式视图总量 ≤ 上限），且
  **不受** `move_merge_enabled` 拖动合并开关门控——原版 `SaveItem→FindSaveSlot`
  对物理袋自带同类堆自动合并语义，收编替代的是同一入库动作。
- **为什么**：只在 adopt 分支合并会让「原版有空槽时拾取」在扩展袋已有同类堆旁
  仍进原版新格；只在 backup 前合并又会让「原版满时拾取」在扩展袋新开一格（真机
  实证：拾取不并入扩展背包已有同类堆）。合并产物 canonical 与移动合并同口径
  （保留 existing payload、按视图相加后经 `patch_payload_count` 写回、
  `descriptor.count` 回填 `s2_read_count`），物化对象数量位同步写回、原版对象
  释放回池（延迟回收 R-36）。
- **payload 布局与身份判据（`SAVE_SaveItem@0x1274f0`）**：payload 为「u8 长度前缀 +
  18B 头 + 4B×N 词缀链」——`[0]` 长度前缀、`[1..8]` u64 UID、`[9..10]` u16 I_TYPE
  位域、`[11..14]` u32 数量位域（bits22–31）、`[15]` I_MAGIC_RATE、`[16]` I_SOCKET、
  `[17..18]` u16 I_ENCHANT、`[19..]` 词缀链。原版「同类可堆叠」判据只比较
  I_TYPE bit6–15（类别）与可堆叠位（`INVEN_FindSaveSlot@0x103b48-0x103b9c`、
  `INVEN_SaveItemDirect@0x103c88-0x103cb0`、`INVEN_MoveItem@0x104a50-0x104a7c`），
  不比较 UID/词缀/镶嵌；模块 `mergeable_identity_equal` 在类别之外把 I_TYPE 其余位、
  镶嵌、附魔与词缀链一并纳入，**只排除 UID 与数量位两个每实例可变字段**，绝不整份
  memcmp（整份 memcmp 因 UID 不同导致拾取永不合并）。
- **典型破坏方式**：把合并挂到 `move_merge_enabled`（拾取行为随拖动开关变化）；
  直接改 descriptor.count 不写 payload/物化对象；合并后不更新
  `g_module_object_hashes`（`module_item_locked` 会判 stale 并重建）；backup 前
  合并后不释放收编对象。
- **验证锚**：Host `test_adopt_merge_plan`（同类合并/身份不同拒绝/启用态 999 上限/
  关闭态视图相加且 a 保留/非可堆叠门控拒绝，backup 前后共用同一判据）；真机
  `VM-39`：①原版有空槽时拾取同类物品并入扩展已有堆（不进原版新格）；②原版袋满时
  拾取同样并入扩展已有堆（待取证）。

### R-54 ext→orig 目标袋为投影宿主时必须事务期恢复真实容量且不得切换显示袋

- **规则一句话**：`move_extension_to_original_locked` 中，当
  `ext2orig_requires_host_capacity_restore(view_installed, target_bag,
  g_module_window_original_bag)` 为真时，事务期（满包预检与 `INVEN_SaveItemOnEmpty`
  之前）必须把被投影替换的宿主袋容量字恢复真实值，并由 RAII 在任何出口写回投影值；
  显示袋切换（`set_original_bag_locked`）仅在
  `ext2orig_should_switch_display_bag(view_installed)` 为真（非投影/API 路径）时执行。
- **为什么**：`INVEN_GetBagSize` 与 `INVEN_SaveItemOnEmpty` 的空槽扫描都读袋对象
  `+0x10` 容量位，而 `install_module_view_locked` 会把宿主袋容量字临时替换为扩展
  容量；目标袋恰为宿主袋时（扩展视图宿主常是原版袋 0），预检/插入按被污染容量
  判定——扩展容量小于真实容量时误判「已满」拒绝（真机实证：无法移入原版袋 0，
  袋 1 正常）。投影安装中 CUR_BAG（direct/GOT 成对写，R-26/R-27）是宿主身份的一
  部分，事务内切换会让宿主记录与 CUR_BAG 分叉，下一次 install 把用户目标袋误选
  为宿主（原版袋容量字被写扩展容量、扩展内容投影上去、物品未真正移动）。
- **典型破坏方式**：只加满包预检而不管容量字；用 `restore_module_view_locked`
  （会卸载投影）代替局部容量字恢复；在投影中 `set_original_bag_locked(target_bag)`
  改写宿主；忘记在失败/成功出口写回投影容量字（RAII）。
- **验证锚**：Host `test_ext2orig_host_guards`（宿主判定/非宿主/非投影/无效宿主，
  显示袋切换仅在非投影）；真机 `VM-40`：投影中扩展物品可移入原版袋 0（宿主袋），
  失败路径不出现宿主分叉视图（待取证）。

### R-55 原版背包详情出售必须由模块按 canonical 全量接管结算

- **规则一句话**：`UIEquip_OKDestroyItem@0xb83d0` 的背包详情（`desc_type==2`）结算
  在堆叠上限启用态下由 H-23 `ok_destroy_item_wrapper` 完全接管——取已 hook 的
  `ITEM_GetCumulateCount` canonical 全量、按 `unit×count×7/10` 加钱、删整堆并刷新；
  按钮预演（`UIEquip_ButtonDestroyExe` 内 `x23=0x666`）只回填展示金额、绝不结算；
  关闭态、非背包详情、类别非 count-encoded、禁售/取价失败/金额越界或槽位复核失败
  一律 backup 原版（fail-safe，不半执行）。
- **为什么**：原版结算函数 `0x1261c4` 内联 `UTIL_GetBitValue(+0x10,31,25)` 只读 b 段
  再 clamp（`b∈[1,99]?b:1`），S2 下 canonical `199`（b=71）被按 71 结算（真机实证：
  实得 546 而非 1540）。只重定向该读点而不改后续 clamp 会把 getter 的 canonical `>99`
  压成 1（证伪记录见 Hub §5.2 经验记录），因此改由回调层整体接管，复用统一 getter 的
  类别门控与模式解码。
- **典型破坏方式**：在按钮预演阶段就真实结算（按下按钮即提前售出、取消不可回滚）；
  用返回地址区分预演/弹窗 OK（经 Dobby 桥后 LR 不可信）；接管后仍按原版 b 段计价；
  加钱失败后继续删堆而不退款；把 `desc_type==1`（扩展/其它袋详情）也当背包结算。
- **验证锚**：Host `stage4_hook_tests::test_vanilla_sell_takeover`
  （`vanilla_sell_route` 两态四分支、`vanilla_sell_money` canonical
  0/1/99/100/199/999 与越界拒绝、调用点常量 `0xb83d0`/`0xb6240`/`desc_type==2`）、
  `test_native_equip_sell_count`（原版 b 段语义 199→71 缺陷基线）；真机 `VM-41`：
  启用态背包详情整堆出售按 canonical 全量计价、取消预演不售出、关闭态回原版
  （待取证）。

### R-56 INVEN_RemoveItemData 物理不足必须从扩展袋按类别补扣

- **规则一句话**：H-21 `remove_item_data_wrapper` 在扩展启用且 `count>0`、`category>0`
  时，先经 H-03 记录该类别总数，调原版 `INVEN_RemoveItemData` 删物理袋 0..5，再用
  `stage4_remove_data_extension_shortfall`（物理实扣 = 调用前后 H-03 总数差）算出缺口，
  经 `extension_bag_consume_category` 从扩展袋按 category 逐堆补扣（按模式视图扣减、
  复用 find/consume 原语）；补扣不足时记录日志、绝不静默。与 H-01..H-05 的
  original-first + 扩展兜底语义一致。
- **为什么**：原版 `INVEN_RemoveItemData@0x1040a8` 只顺序遍历物理袋 0..5，扩展袋对象
  不参与；而 H-03 `INVEN_GetItemCount` 已把扩展计入。二者语义不一致导致「材料被识别为
  充足却不被消耗」——合成药水/宝石孔/混沌/传说（type0/2/3/4）经
  `UIMix_StartMix → MIXSYSTEM_UseStuff → INVEN_RemoveItemData` 全部命中；宝石强化
  （type1）走 `INVEN_RemoveItem`（H-05 按身份已支持扩展）。补齐 H-21 后按类别批量扣料
  自动生效，同时任务回收/回滚等调用方也保持与 H-03 一致。
- **典型破坏方式**：让 H-03 与 H-21 对扩展的可见性不一致（识别而不扣）；在
  `count<=0`/类别非法时补扣；持 `g_virtual_bag_mtx` 调原版（R-44）；在调原版之前就
  补扣导致物理与扩展重复扣；把补扣量算成 `count` 而非「`count` − 物理实扣」。
- **验证锚**：Host `stage4_hook_tests::test_remove_data_extension_shortfall`
  （足额/不足/全扩展/`requested<=0`/数据反向 五类分支）；真机 `VM-22`：把药水材料放入
  扩展袋后执行合成，预期材料按量从扩展袋扣减、产物照常入库（待取证）。

### R-57 进入扩展视图时原版袋列必须取消高亮（互斥）

- **规则一句话**：扩展页、商店页、合成器页在扩展视图激活时，画原版袋列函数
  （`UIEquip_DrawInvenBag` / `UIStore_DrawInvenBagGroup` / `UIMix_DrawInvenBagGroup`）
  之前，必须把当前袋 GOT（`G_UIEQUIP_CUR_BAG_GOT_VMA`）临时置为
  `kNoOriginalBagSelected(6)`，画完恢复；扩展页签的选中高亮由扩展侧在袋列绘制之后
  单独补画。三者共用同一 GOT，语义一致。
- **为什么**：投影安装把宿主物理袋设为当前袋（供原版按容量刷新网格），原版袋列按
  `i == *current_bag` 画选中高亮，于是扩展视图激活时原版袋仍显示为选中，与扩展页签
  选中态冲突。扩展页早已用「画前遮蔽、画后恢复」解决（`virtual_bag_draw_original_bag_wrapper`），
  商店/合成器宿主此前遗漏。
- **典型破坏方式**：把 GOT 永久置 6 而不只在袋列绘制窗口内遮蔽（原版 `RefreshInvenItem`
  会按 6 越界取袋）；遮蔽后忘记恢复导致后续原版刷新/绘制错袋；在扩展页签高亮之前
  恢复（被原版覆盖）；只遮蔽合成器不遮蔽商店（或反之）。
- **验证锚**：真机 `VM-23`（扩展页）、`VM-18`（商店）、`VM-22`（合成器）：选中扩展袋后
  原版 6 袋全部取消高亮、扩展页签高亮；切回原版袋后原版高亮恢复。Host 无（纯 UI 位
  绘制，依赖游戏内存）。

### R-58 三个 UI 宿主页签挂载必须由扩展背包开关唯一门控

- **规则一句话**：扩展背包的三个 UI 宿主页签挂载点——
  `install_extension_tab_buttons_locked`（背包页）、`store_install_tab_buttons_locked`
  （商店页）、`mix_install_tab_buttons_locked`（合成器页）——都必须以
  `g_virtual_bag_enabled` 为唯一门控，在函数入口直接判 `!g_virtual_bag_enabled.load()`
  即返回；运行期关闭扩展背包时，已挂载页签必须被清除：背包页在
  `draw_tab_buttons_in_frame_locked` 入口删除控件并失效，商店页在
  `store_draw_end_wrapper` 的 disabled 分支删除并 teardown，合成器页在
  `mix_draw_end_wrapper` 的 disabled 分支删除并失效。调用方不得再各自重复判断。
- **为什么**：此前背包页 `draw_tab_buttons_in_frame_locked` 每帧无条件重装页签
  （`extension_bag_render.inc:10-17` 只判容器就位、不判开关），商店页
  `store_enter_wrapper` 每次进店无条件 `store_install_tab_buttons_locked`
  （`extension_bag_store.inc:365`），且 `set_virtual_bag_enabled(false)` 只
  `restore_module_view_locked` 而不清理页签；于是配置关闭后扩展 UI 仍挂载（真机实证）。
- **典型破坏方式**：只在其中一个宿主判断开关（另两个漏判）；把门控放在调用方而
  install 函数本身不判；关闭时只清 `*_tab_buttons` 引用而不删除已挂载控件
  （残留控件仍可能被绘制/命中）；关闭时只 restore 投影不处理页签。
- **验证锚**：真机：启用态背包页签 `extension_tab_button=true`；
  `POST /api/config/set {"extensionBagEnabled":false}` 后 `extension_tab_button=false`，
  再启用恢复 `true`（本轮已取证）；商店/合成器页签走同一 install 门控与 disabled
  分支（`VM-42`，待取证）。Host 无（UI 挂载依赖游戏控件树）。

### R-59 多版本物品数量上界兼容必须由模块按特征码动态放行，不得改 APK

- **规则一句话**：monster 版把物品准入校验的 `I_COUNT` 最高字节上界硬编码为 `#0x62`
  （对应原版 `count<=99`），与 S2 扩展上限编码冲突；模块必须在 `bridge_init()` 末尾调用
  `apply_monster_item_count_compat()`，在 `libgame.so` 可执行映射内按特征字节
  `1F 89 01 71 68 00 00 54`（`cmp w8,#0x62; b.hi`）定位，把 `cmp w8,#0x62`(0x7101891F)
  改写为 `cmp w8,#0x7F`(0x7101FD1F)；非 monster 版不命中即无操作（log `no target`）。
- **为什么**：monster mod 重写背包加载器（`SAVE_LoadFile+0xd0` 的 `bl` 改指注入段
  `0x7413b4`），其准入 helper 对每条物品记录取 `+14`（= `I_COUNT` 最高字节）做
  `((v-2)>>1) > 0x62` 判定。S2 编码把高位段 `a` 放在 bits22-24，使该字节从 0 变为非 0，
  于是 `SAVE_LoadFile` 返回 0 → `SAVE_Load` 提前返回（跳过 `SAVE_LoadCharacterAll`/
  `MAP_Load`）→ `pMainPlayer` 保持 null → `GAMESTATE_EnterPlay` 对 null 调
  `CHAR_GetSkillPoint` SIGSEGV。放宽到 `#0x7F` 使派生值 ≤127 全部放行。
- **典型破坏方式**：把兼容做成改 APK/重打包（交付形态错误，必须在模块内运行时 patch）；
  按硬编码 VMA 定位（版本漂移即失效）而非特征码；在数据区同字节误命中；patch 晚于
  `bridge_init` 导致首帧仍崩；对非 monster 版误改。
- **验证锚**：真机 monster v23：log `monster item-count compat: 0x…818 cmp w8,#0x62 ->
  #0x7F`；`enter_slot 0` → `screen=world` 且进程存活无 FATAL。大修版：log `no target`，
  零改动。Host 无（依赖真机内存布局）。

### R-60 save gate 必须按符号 hook 原版 SAVE_SaveInventory 函数入口，不得依赖调用点原指令字

- **规则一句话**：存档门禁（还原投影后交还原版写盘）必须用 `native_hook_func()` 在
  `g_base + fn_resolve("F_SAVE_SAVE_INVENTORY_VMA", …)`（原版 `SAVE_SaveInventory`
  函数入口）安装 inline hook；`save_inventory_wrapper` 经框架 backup 调原函数，禁止再
  改写 `SAVE_Save+0x170` 调用点并校验硬编码原指令字 `0x97fff987`。
- **为什么**：大修版 `SAVE_SaveInventory@0x127d8c` 唯一调用者就是 `SAVE_Save+0x170`，
  调用点改写与函数入口 hook 语义等价；但 monster 把该调用点改为 `bl 0x741168`（mod 存档
  包装器，内部再调原版），硬编码原指令字校验失败 → `save gate patch mismatch got=0x94185e7e`
  → `install_inventory_hooks` 返回 false → 整条 VirtBag 注入链中止（save callsites/drop/
  draw/desc/event/save panel/store host/mix host 全不装）。函数入口 hook 与调用点无关、
  跨版本稳定，且在 monster 上还能同时拦到 mod 包装器的调用。
- **典型破坏方式**：保留调用点改写并对新版本原指令字 fail-closed（版本一变全链不装）；
  `save_inventory_wrapper` 直调符号地址导致重入自身（必须走 backup）；hook 未幂等；
  backup 为空仍继续。
- **验证锚**：真机 monster v23：log `save gate hook installed target=… backup=…`，
  `save gate patch mismatch` 计数 0；8× `save callsite hooked` 与 drop/draw/desc/event/
  save panel/store host/mix host 全部安装；`enter_view {"bag":6}` → `state.mode="module"`；
  `/api/system/save` → `save complete slot=0 tx=… participants=1`。大修版同链仍成立。

### R-61 扩展视图的 moving 控件清理必须避开空闲态，不得清原版活动手势

- **规则一句话**：`clear_stale_projected_moving_locked()` 只能在**模块会话进行中**
  （`g_extension_drag_session.phase != kIdle`）或**模块触摸捕获中**（`g_extension_touch_capture`）
  时清除 `TouchState+0x30`（MOVING_CTRL）；`moving==nullptr && drop_source==nullptr` 时直接返回
  （不记日志）；会话空闲时 `MOVING_CTRL/DROP_SRC_CTRL` 属于原版 TouchHandle 手势，**绝不清除**。
- **为什么**：原版释放的唯一落点派发在 `TouchHandle_ResetMovingControl@0xa371c`——读
  `MOVING_CTRL`，非空时向该控件派发 event 8（真正的移动/合并），随后清空 touch state。模块
  每帧清 `MOVING_CTRL` 会打断原版触摸状态机：真机实证「最后一次成功拖动后 `0x17`×48、
  `0x19`×700、`0x18`×0，持续 ~11 分钟」，即释放事件不再产生、拖动彻底失效；日志同时出现
  `stale projected moving cleared moving=<非0> phase=0`（空闲态清掉了非空原版控件）。
- **典型破坏方式**：空闲态也清（把原版手势当成 stale 投影）；把每帧清空当无害；用
  `reset_drag_state_locked` 的 memset 清掉原版 ACTIVE_CTRL 等状态。
- **验证锚**：真机：进入扩展视图后 `stale projected moving cleared` 计数为 0（每帧噪声消失）；
  连续拖多个物品后仍能继续拖动、`0x18` 持续正常（`VM-45`）。Host 无（依赖游戏控件树/触摸状态机）。

### R-62 所有权账本容量必须覆盖扩展物品上限，交接终态槽必须回收

- **规则一句话**：`ownership::kLedgerCapacity` 必须 ≥ 扩展背包物品上限（5 袋 ×16 = 80）加事务
  余量（当前 128）；`handover_to_inventory` 交接后必须**立即回收槽位**（`slot.state = kNone` +
  generation 递增使旧句柄失效），终态信息仅由 `total_handed_over` 累计记录，`Audit.inventory_owned`
  由该累计值派生。
- **为什么**：账本对**每个已物化扩展物品**占一个 `module-owned` 句柄，且 `kInventoryOwned` 原为
  **永不释放**的终态槽（`handover` 只递增 generation、不置空闲）。原容量 32 在 28 件物品 + 4 个
  终态槽时即耗尽（真机 `module materialize rejected ownership ledger exhausted` →
  `txn committed but destination materialize failed` → `txn abort`），表现为**拖动可解析落点但
  不产生结果**；且 ext→orig 操作次数无上限，任何有限容量若不回收终态槽都终将耗尽。
- **典型破坏方式**：只调大容量当修复（终态槽仍累积，治标）；让 `allocate` 复用 `kInventoryOwned`
  而不回收（两次分配之间连续交接仍会累积）；交接后仍按句柄报告 `kInventoryOwned`。
- **验证锚**：Host `test_ownership_ledger`（池耗尽/槽位复用、交接后 `live_handles` 归零、
  `inventory_owned` 为累计值）；真机：连续「扩展→原版」移动/装备后仍可继续拖动、无
  `ownership ledger exhausted`（`VM-46`）。

### R-63 佣兵徽章使用必须函数级接管并复用原版 MakeMercenary 链

- **规则一句话**：佣兵徽章（category 42..50 或 928..933，`ITEMSYSTEM_IsMercenarySeal`）的效果
  只在原版 `UIEquip_ButtonUseMercenarySealExe@0xb8144` → `MERCENARYSYSTEM_MakeMercenary@0x119658`
  链中，`CHAR_UseItemEx` 不实现；扩展侧必须用函数级 Native Hook（H-24）接管该按钮，按原版顺序
  `UIDesc_SetOff → SAVE_IsOK → IsEmptyManagerSlot → MakeMercenary` 执行，消耗交给已 Hook 的
  `INVEN_RemoveItem`（H-04），不得另写删槽；API 路径（`data_op_use_item` /
  `extension_bag_api_use_item_impl`）必须对 `IsMercenarySeal` 类别豁免 `fn_is_use` 并同样走
  `MakeMercenary`。
- **为什么**：`ITEMDATA_IsUse@0x10583c` 只认记录 +2 ∈ {0x16,0x17}，徽章 +2=0x1d 返回 0，两条
  API 路径都被 `fn_is_use` 拦截返回 `item not usable`；而原版按钮经 `ControlItem_GetItem` 从
  g_inven 取物品，扩展物品不进物理袋会取到 null/错位而静默失败；`CHAR_UseItemEx` 对 category 49
  只走通用路径，不生成佣兵。
- **典型破坏方式**：在扩展详情分流里对 0xb8144 只 `call_orig`（原版取不到扩展物品）；把徽章当
  普通消耗品调 `CHAR_UseItemEx`；在接管分支手动删槽造成双消耗；持 `g_virtual_bag_mtx` 调
  `MakeMercenary`（其内部 `INVEN_RemoveItem` 会再取锁，死锁）。
- **验证锚**：Host 无（依赖原版链）；真机 `VM-47`（扩展袋 UI）与 `VM-48`（API 原版袋/扩展袋），
  日志锚 `extension mercenary-seal handled bag=.. slot=.. used=1` 与
  `hook install OK ... ButtonUseMercenarySealExe=`。

### R-64 扩展详情按钮观察 hook 不得依赖面板偏移与按钮一一对应

- **规则一句话**：`install_extension_desc_item_hooks_locked` 按面板偏移枚举按钮时必须先校验
  控件/数据可访问，并按 execute slot 去重；不得因陈旧面板字段解析出同一 slot 而跳过**真实**
  目标按钮（0x98）的安装；依赖详情按钮观察 hook 的分流不得假设「偏移 ↔ 按钮」稳定一一对应。
- **为什么（反汇编 + 真机证据）**：`UIEquip_SetDescMenu@0xb8504` 的 `+0x68..+0xb8` 是
  **互斥分支槽**，按 `desc_type` 与物品类别每次只写 1–2 个（`+0x80`=`IsShortcutUse`→
  `ButtonUseExe@0xb80b8`；`+0x98`=`ITEMSYSTEM_IsMercenarySeal`→`MercSealExe@0xb8144`；
  `+0xa0`=`IsDice`→`RollDiceExe`）。`UIDesc_ResetMenuGroup@0xb5324` 只 `DeleteChild` 组子控件，
  **不清面板槽指针**；`ControlButton_Create` 地址复用，使陈旧槽与新按钮解析出同一
  `data+CB_EXECUTE_PROC(0x20)`。真机 `.tmp/d1-full.txt` 因此出现
  `skip duplicate slot index=6 offset=0x98 slot=0x...730`（与 offset 0x80 同 slot），且 0x98 的
  `orig` 在不同 desc 间漂移（`0x...be144`/`0x...be280`）；同日志**全程无 `extension desc execute`**，
  证明该观察 hook 未能稳定触达点击。常量佐证 `CO_DATA=0x50`、`CB_EXECUTE_PROC=0x20`。
- **典型破坏方式**：去重仅比较 `slot` 而不校验按钮身份/偏移来源；对陈旧按钮安装 hook；把
  `orig` 漂移误判为目标 proc；把「详情按钮观察 hook 已装」当作「点击必达」。
- **验证锚**：R-63 采用函数级 H-24（`0xb8144` 入口）**不依赖该枚举**，故本漂移不影响佣兵徽章
  修复；其余仍依赖 desc 观察 hook 的分流按本规则取证。真机 dump 详情打开时 11 个偏移的
  button/data/orig 与物品 category，日志 `extension desc hook installed/skip ...` 与点击时
  `extension desc execute ...` 成对。此项为独立取证任务，不阻塞 R-63。

### R-65 S2 写侧回写门控必须与读侧同源（堆叠上限驱动，扩展背包无关）

- **规则一句话**：S2 写侧回写总门控 `s2_writeback_gate` 必须与读侧 getter 门控同源——
  `stack_limit_enabled()` 一开即需写侧进位；扩展背包开关不得作为 S2 数量写侧的必要条件。
  模式统一经纯函数 `stack_codec::writeback_needed(extension_bag_enabled, stack_limit_enabled)`
  进入，禁止在写点单用 `extension_bag_enabled()` 判定。
- **为什么**：读侧 H-17 `get_cumulate_count_wrapper` 按 `stack_limit_enabled()` 解码 S2
  全量（`stage4_get_cumulate_count(..., stack_limit_enabled())`），写侧若以
  `extension_bag_enabled()` 门控，则「堆叠上限开 + 扩展背包关」配置下读按 `128a+b`、
  写只落低 7 位 b，跨 127 的数量增写被截断（真机复现：两组 99 拖拽合并，`198=128+70`
  只写入 b=70，读回 70）。
- **典型破坏方式**：以扩展背包开关代替堆叠上限开关作为 S2 写侧条件；只在「扩展开」配置
  回归，漏测「扩展关 + 堆叠开」组合；影响同门控的全部写点（`move_item_wrapper`、
  `save_item_direct_wrapper`、`consume_item_wrapper`、`remove_item_data_wrapper`）。
- **验证锚**：Host `stage4_hook_tests::test_s2_writeback_gate` 断言
  (ext,stack)∈{(false,false)=false,(false,true)=true,(true,false)=true,(true,true)=true}；
  真机 `VM-49`（堆叠开+扩展关 99+99→198，切换配置读回一致）。

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
7. `UIEquip_EquipControlEventProc@0xb8f7c` Hook：仅扩展 apply 材料源（宝石/强化卷轴）进入
   锁内 descriptor/generation/session 校验；通过后取 `ModuleUseToken`，放锁调用原版 proc，
   由 `PutJewel`/`EnchantItem`/`ConsumeItem` 链完成镶嵌、强化和消费，校验失败直接 Blocked，
   不降级 backup。

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

1. 本册规则总数：`R-01..R-64`，共 64 条。
2. 当前规则包含：`R-26`（窗口袋与 view index 分离）、`R-27`（direct/GOT 成对恢复）、
   `R-28`（source protection 与 merge 解耦）、`R-29`（pending/journal 分域）、
   `R-30`（root 重建与 stale event 门禁）、`R-31`（扩展对象禁入原版移动链）、
   `R-32`（模块内原版刷新必须走 trampoline）、`R-33`（吞掉原版 release 必须完成等价清理）、
   `R-34`（moving 六条件保留门）、`R-35`（页签命中优先于格子解析）、
   `R-36`（触摸窗口释放必须延迟回收）、`R-37`（卖出价格边界）、`R-38`（载荷数量
   收敛适用性）、`R-39`（袋对象 marker 位段）、`R-40`（类别判定 fail-closed）、
   `R-41`（payload count 门控）、`R-42`（BagType/原生类别判定边界）、`R-43`（数量 patch
   表准入判据）、`R-44`（持锁原版回调 TLS 直通与删除后置条件）、`R-45`（S2 编码布局
   与总量范围）、`R-46`（S2 高位段类别门控 fail-closed）、`R-47`（关闭态低 7 位视图
   与 `a` 保留，决策 b）、`R-48`（sidecar 无版本标识、统一 S2、模式仅影响运行时视图
   与 clamp）、`R-49`（读侧统一 getter、写侧
   调用方级处理）、`R-50`（apply 准入目标装备类校验）、`R-51`（出售/拆堆位读重定向
   到统一 getter，禁止窗口伪修复）、`R-52`（数量入参必须反汇编证明，MakeItem arg2
    非数量）、`R-53`（SaveItem 漏斗入库前先并入扩展同类堆）、`R-54`（ext→orig
    投影宿主容量字与显示袋守卫）、`R-55`（原版背包详情出售 canonical 接管与
    预演/结算两态）、`R-56`（INVEN_RemoveItemData 物理不足从扩展袋按类别补扣）、
     `R-57`（进入扩展视图时原版袋列取消高亮）、`R-58`（三个 UI 宿主页签挂载由扩展背包
     开关唯一门控）、`R-59`（monster 物品数量上界由模块按特征码动态放行）、`R-60`（save
     gate 按符号 hook `SAVE_SaveInventory` 函数入口）、`R-61`（扩展视图 moving 控件清理
     避开空闲态、不清原版活动手势）、`R-62`（所有权账本容量覆盖扩展物品上限且交接终态槽回收）、
     `R-63`（佣兵徽章使用函数级 H-24 接管并复用原版 MakeMercenary 链，API 豁免 `fn_is_use`）、
     `R-64`（扩展详情按钮观察 hook 不得依赖面板偏移与按钮一一对应）。
3. `R-37` 以当前价格边界实现和 VM-30 取证为准；`R-38..R-44` 以对应 Host 断言和
   VM-09/VM-13/VM-29/VM-30/VM-31 真机证据为准；`R-45..R-49` 为已批准的 S2 数量编码
   契约，当前落地状态：`R-45` 布局常量与读侧解码已落地（Host `test_stack_codec_s2`）、
   `R-46` 读侧门控已落地（H-17 fail-closed）、`R-47` 关闭态低 7 位视图与 `a` 保留
   （决策 b）已落地（Host `test_stack_codec_effective_mode`、
   `test_virtual_bag_mode_aware_ops`，真机锚 `VM-36` 待取证）、`R-48` 无版本标识统一
   S2 已落地（Host `test_stack_codec_mode_invariance`、
   `test_virtual_bag_cross_config_roundtrip`）、`R-49` 读侧 getter hook 与写侧调用方级
   门控已落地（S2-P2/P3，H-17 与 H-18..H-21 + Host
   `test_get_cumulate_count_s2`、`test_remove_item_data_plan`）；真机证据 `VM-32`..`VM-36` 均待取证。`R-50`（apply 准入目标
   装备类校验）已落地（G-8/p5/H-15 路由 + Host `stage4_hook_tests` 断言），真机
   证据归 S-03/VM-10。`R-51`（出售/拆堆位读重定向 getter）已落地
   （`g_stack_getter_redirect_patches` + Host `test_sell_divide_s2_read_window`，
   真机重定向日志已取证、行为归 `VM-37` 待取证）；`R-52`（MakeItem arg2 非数量、
   回写撤销）已落地（Host `test_make_item_writeback_count`，行为归 `VM-38` 待取证）；
   `R-53`（adopt 先合并同类堆）已落地（Host `test_adopt_merge_plan`，行为归
   `VM-39` 待取证）；`R-54`（ext→orig 宿主容量字与显示袋守卫）已落地
    （Host `test_ext2orig_host_guards`，行为归 `VM-40` 待取证）；`R-55`（原版背包
    详情出售接管）已落地（Host `test_vanilla_sell_takeover`，行为归 `VM-41` 待取证）；
     `R-56`（H-21 扩展桥接）已落地（Host `test_remove_data_extension_shortfall`，行为归
     `VM-22` 待取证）；`R-57`（原版袋列高亮遮蔽）已落地（扩展页既有 + 商店/合成器新增
     袋列绘制遮蔽 wrapper，行为归 `VM-18`/`VM-22`/`VM-23` 待取证）；`R-58`（三个 UI 宿主
     页签挂载由扩展背包开关门控）已落地（三 install 函数入口门控 + 背包/商店/合成器
     disabled 分支清除，背包页真机已取证 enable/disable 往返，行为归 `VM-42`）。
4. 当前 sync 接口只做投影控件修复，原版 RefreshItemArea 位于移动收尾路径。
5. 无锚规则：`R-01`、`R-03`、`R-05`、`R-06`、`R-11`、`R-12`、`R-13`、`R-14`、
   `R-15`、`R-17`、`R-23`、`R-24`、`R-26`、`R-27`、`R-30`、`R-46`、
   `R-49`，共 17 条；
   其中已给源码锚但尚无专门 host/真机锚的规则，验收册仍应补操作证据；
   `R-46`/`R-49` 的真机写侧证据归 `VM-34`，`R-47` 的真机证据归 `VM-36`
   （均待取证）。
6. 规则正文以当前源码和本册证据锚为准。
