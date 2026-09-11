# 扩展背包库存接入决策册

> 状态：CURRENT；本册是库存、物品函数逐函数接入方式的唯一权威。
>
> 基线：工作区当前代码与 `game_symbols.h`。
>
> 问题 B：未解决 + 已布防。扩展袋 0 的 a↔b 交换仍可能令原版袋 0 同号槽物品消失并在保存后固化，禁止将其写成已修复。

## 1. 使用规则

本册回答每个原版库存入口“是否接管、在哪里接管、谁负责原版对象和扩展对象”。它不把已经安装的 Hook 等同于生产者全覆盖，也不批准 P7 Overall。状态、锁和禁止事项的总规则由 `rulebook.md` 负责；保存时序由 `module-save-store.md` 负责；交互状态机由 `drag-protocol.md` 负责。

决策术语如下：

| 术语 | 含义 |
|---|---|
| Native Hook | 在函数入口用 LSPosed Native API 截获；原版对象先走 backup，扩展对象才走扩展分支。 |
| 上层旁路 | 不改全局原版函数，在 API、UI 入口、生产者 caller 或扩展事务中按对象身份分流。 |
| GOT | 只改函数指针表或回调槽；必须写明宿主、生命周期和恢复条件。 |
| patch | 只替换已经确认的单个 BL/调用点；不得因一个调用点推导所有 caller。 |
| 不接管 | 保持原版 ABI、返回值、物理槽和对象池语义；扩展需求在就近上层完成。 |

### 1.1 当前物理域

* 原版 `INVEN_pItem` 是 6 袋物理数组：袋 `0..5`，每袋最多 16 个指针，槽步长 `0x80`；容量由袋对象字段决定。当前证据：`game_symbols.h:79-80`、`rulebook.md:46-56`。
* 普通扩展入库目标只有原版袋 `0..4`。袋 `5` 是任务袋：原版查询可以读它，但扩展逻辑不得将普通物品写入或把它改作扩展目标。
* 扩展逻辑袋是逻辑索引 `6..10`，不在 `INVEN_pItem` 内；任何物理槽编码、`SaveItemDirect`、`RemoveItemDirect`、原版保存链都不能接收它。
* 物理槽编码由 `bag = encoded >> 5`、`slot = encoded & 0x1f` 解出；来源为 `game_symbols.h:391,626` 及 `rulebook.md:60-70`。
* 物品对象的 payload 是 `SAVE_SaveItem` 生成的原版序列化记录。扩展描述符、对象缓存、handle、generation 必须同步；sidecar 只存描述符和 payload，不存 native 指针。来源：`ExtensionBagUiBridge.kt:11-15`。

### 1.2 常驻入口事实

当前 `native_inventory_hook.cpp` 安装 23 个常驻 Native Hook：既有 16 个库存/移动/装备页
入口（含 `UIEquip_EquipControlEventProc@0xb8f7c`、`UIEquip_RefreshItemArea@0xb7a00`），
加上 S2 读/写侧框架 `ITEM_GetCumulateCount@0x106094`、`INVEN_SaveItemDirect@0x103bf0`、
`ITEMSYSTEM_Divide@0x1083f8`、`ITEMSYSTEM_MakeItem@0x10c6c8`、
`INVEN_RemoveItemData@0x1040a8`，以及 R-55 出售接管 `UIEquip_ButtonDestroyExe@0xb6240`、
`UIEquip_OKDestroyItem@0xb83d0`。安装失败按既有成功顺序逆序回滚，清空 backup；安装日志
`hook install OK api=2 count=23`。

所有 VMA 均以下表和 `game_symbols.h` 为准；表内“现码”列只描述当前工作区，不表示已完成真机验收。

## 2. 函数决策矩阵

### 2.1 INVEN 查询、容量和槽位

| 函数 | VMA | 当前处置 | caller 面 | 扩展语义 | 风险边界 |
|---|---:|---|---|---|---|
| `INVEN_FindItem` | `0x10438c` | Native Hook，original-first | 使用、材料查询、装备查找及未知原版 caller | 先返回原版对象；原版为空时由 `inventory_find_item_original_first` 查询扩展对象 | 返回的扩展指针不是物理槽对象；下游不得据此调用物理删除或物理保存。现码：`native_inventory_hook.cpp:79-101`。 |
| `INVEN_HaveItem` | `0x104870` | Native Hook | 配方、任务、使用前检查 | 原版非零优先；否则聚合逻辑袋类别存在性 | 只表示类别存在，不提供扩展物理位置；现码：`native_inventory_hook.cpp:114-118`、`inventory_hook_stage4.cpp:3-13`。 |
| `INVEN_GetItemCount` | `0x104260` | Native Hook | 配方、消费、生产者数量检查 | 返回原版物理数量加扩展逻辑数量 | 不能用总量替代材料对象身份、payload 或入库槽判断；现码：`native_inventory_hook.cpp:104-112,120-124`。 |
| `INVEN_FindItemSlot` | `0x103704` | 上层旁路；不新增 Hook | 原版确认使用、物理删除和原版 UI | 只返回 `INVEN_pItem` 的真实 1 字节物理编码；扩展用 bag/slot 逻辑引用 | 扩展袋 `6..10` 无法安全编码到既有 `int8_t` 槽位，且物理袋 `0..5` 已占用编码空间；不可伪造 out 参数。确认使用改在 `0xb8478` 分流。 |
| `INVEN_GetBagSize` | `0x103250` | 直接调用原版；不接管 | 绘制、容量和原版窗口 | 只读原版袋对象容量；扩展容量走 `g_virtual_bag_state.capacities` | 仅接受 `0..5`；不能拿扩展逻辑袋号调用。 |
| `INVEN_GetEmptyBagSlot` | `0x103280` | 不接管；上层旁路预检 | 原版生产者寻找空袋 | 原版固定扫描袋 `0..4`，返回袋索引而非槽编码；扩展空位另算 | 不检查袋 `5`，也不是统一扩展入库入口；不能因为它名字相似而宽 Hook。 |
| `INVEN_IsEmptyBag` | `0x1032e0` | 不接管；必要时在上层分发 | 满包提示、原版 UI 和生产者预检 | 扩展袋独立由 `virtual_bag_has_empty_slots` 判断 | 不得将扩展空袋结果伪装成原版袋结果；指定袋仍须在物理域内。 |
| `INVEN_IsHavingEmptySlot` | `0x103460` | Native Hook，原版优先后扩展 fallback | 拾取、奖励、商店购买和入库前门禁 | `needed > 0` 时原版按 `include_task_bag` 后再查扩展空位；`needed <= 0` 由现码明确返回 `1`（可放行） | 不改变 `include_task_bag`；扩展 fallback 不把任务袋纳入扩展目标。现码：`native_inventory_hook.cpp:126-131`、`inventory_hook_stage4.cpp:25-42`；Host：`test_inventory_hook_stage4.cpp:122-125`。 |
| `INVEN_CalculateEmptySlotCountForSave` | `0x103f10` | 不接管，暂缓保存审计 | 原版保存辅助 | 只计算物理保存所需槽 | 不传扩展袋号；与 P6 保存协调器不重复接入。 |
| `INVEN_GetEmptySaveSlotEx` | `0x105070` | 不接管；原版保存辅助 | 原版保存/批量入库 | 只输出原版物理槽编码 | ABI 有登记但失败输出细节未形成扩展契约，不能返回逻辑槽。 |
| `INVEN_GetNeededSaveSlotEx` | `0x105440` | 不接管；原版保存辅助 | 保存和组合空槽/堆叠查询 | 仅原版物理袋和原版输出参数 | 不向它传扩展袋或扩展对象；未知失败输出留 P6。 |
| `INVEN_GetCumulateSaveSlotEx` | `0x1051b0` | 不接管；原版保存/堆叠辅助 | 保存槽及原版堆叠查询 | 原版身份和堆叠上限不改写 | 与扩展 payload 合并规则分离，不能从返回槽推导逻辑槽。S2：候选槽余量读 b 段残量（bits25–31），`+0x164/0x17c` 的 99/998 判定保持原版（999 对 mod128 残量无意义，不进 clamp 表），b 满判定 fail-closed。 |
| `INVEN_CheckSaveInNotEmptySlot` | `0x103d78` | 不接管；只保留原版校验原语 | 保存前物理槽检查 | 不承担扩展入库 | 参数角色和失败契约不足以支持扩展接入；按名称猜测会造成错误写入。S2：`+0x14c` 的 b 段读取保持原版（S1 布局 patch 已移除），`+0xa4` 的 99 剩余判定保持原版（fail-closed），不进 clamp 表。 |

### 2.2 INVEN 入库、移动、删除和对象

| 函数 | VMA | 当前处置 | caller 面 | 扩展语义 | 风险边界 |
|---|---:|---|---|---|---|
| `INVEN_FindSaveSlot` | `0x103960` | 上层分发；商店两处 callsite patch | API 创建、拾取、奖励、商店和原版入库 | 只预检原版物理袋；商店 gate 在原版失败且扩展有空位时返回“有空间” | 不改 out 槽为扩展编码；任务物品仍按任务语义；商店 patch 见 `extension_bag_store.inc:630-644,714-726`。S2：`+0x238` clamp 保留（两堆 getter hook 完整数量之和 ≤999）；可堆叠判定与数量读取经 `ITEM_GetCumulateCount` H-17 解码。 |
| `INVEN_SaveItem` | `0x104528` | Native Hook，backup first，失败后扩展 adopt | 当前代码注释登记的 18 条已创建物品漏斗：拾取、奖励、事件、开箱、商店、合成等 | 原版成功完全透传；backup 返回 0 时，若扩展有空位则 `adopt_native_item` 并返回 1 | wrapper 当前只取 item 参数；不能假设未知 caller 的 context 已被覆盖。商店投影写入前先恢复容量，见 `native_inventory_hook.cpp:184-199`。 |
| `INVEN_SaveItemDirect` | `0x103bf0` | Native Hook（H-18，`save_item_direct_wrapper`）：空槽写入不改写，同身份堆叠合并采集目标槽旧全量、backup 后确认原版 b 写再回写 a+b | `SaveItem`、`SaveItemOnEmpty` 和批量保存内部 | 物理槽写入或同身份堆叠；合并可能 `ITEMPOOL_Free(item)` | 扩展对象绝不传入；返回值不可作为槽位、被替换对象或释放结果。S2：`+0xdc/0xf0` clamp 保留（getter hook 完整数量判定 ≤999）；delta≡0 mod 128 退化 fail-closed 跳过。 |
| `INVEN_SaveItemOnEmpty` | `0x104be0` | 上层旁路；投影 drop 单点 patch gate | 跨袋拖动、原版卸下回包和恢复事务 | 原版目标袋 `0..4` 由原版写入；扩展→原版由事务先提交，不让借出对象进入原版写链 | 当前 drop callsite 为代码中 `g_base + 0xb8cc0`（`extension_bag_lifecycle.inc:648-686`，该调用点未在 `game_symbols.h` 建独立常量）；原版返回 0 才能避免同槽清理，见 `extension_bag_render.inc:441-490`。 |
| `INVEN_SaveItemData` | `0x104614` | 上层分发；不接管 | 类别/数量批量生产和原版回滚 | 扩展生产者不得把类别回滚误认为 UID 精确回滚 | 原版失败会按类别/数量进入 `RemoveItemData`，可能部分完成；扩展对象不得进入该链。S2：`+0x7c` clamp 保留（入参创建总量判定 >998 拆堆）；`+0x8c` fill 常量保持原版 99（fill 写 b 段，与 `sub #0x63` 拆堆配对，不可独立 999 化）。 |
| `INVEN_RemoveItem` | `0x104044` | Native Hook | 原版消费、删除、装备和 UI 事件 | 识别扩展对象后逻辑删除；否则 backup | 扩展释放失败必须停在扩展分支，不得再 backup；现码：`inventory_hook_stage4.cpp:64-80`。 |
| `INVEN_RemoveItemDirect` | `0x103fd8` | 禁止扩展接入；保留原版原语 | 物理槽删除、原版回滚和物理旁路 | 只处理真实物理 bag/slot，并负责 `ITEMPOOL_Free` | 返回值不可信，即使删成功也可能为 0；必须读删后槽位确认。扩展对象进入这里会产生释放、悬挂指针或错误物理槽。 |
| `INVEN_RemoveItemData` | `0x1040a8` | Native Hook（H-21，`remove_item_data_wrapper`）：backup 前快照匹配类别堆、backup 后重扫 + `stage4_remove_item_data_plan` 修正部分删堆 | `SaveItemData` 失败回滚、类别消耗 | 只扫描物理袋 `0..5`，可部分删除 | 不支持扩展身份和 payload；任务袋语义按原版保留，扩展对象不得伪装成类别数据。S2：关闭态原版 b 写天然正确，wrapper 不启用修正。 |
| `INVEN_ConsumeItem` | `0x1047bc` | Native Hook | `CHAR_UseItemEx`、快捷键、开箱/解封/骰子结果 | 原版消费时机由 backup 决定；扩展分支更新数量、payload、逻辑槽和投影 | 不可堆叠归零时逻辑删除，禁止进入物理释放链。现码：`native_inventory_hook.cpp:146-168`、`inventory_hook_stage4.cpp:45-61`。 |
| `INVEN_MoveItem` | `0x104934` | **Native Hook（方案 A：guard 仅拦扩展身份源；原版对象全量 backup+取证）** | 原版拖动、拆堆、投影移动 | 原版对象保持原版；扩展源在函数层拒绝进入物理移动，扩展三方向事务仍由上层处理 | 函数四参不足以还原扩展意图，否决方案 B 分流接管；仅受控装备交换为唯一窄例外，身份与快照取证后必须放锁再调 backup；问题 B 仍未解决。 |
| `ITEMSYSTEM_Divide` | `0x1083f8` | Native Hook（H-19，`item_system_divide_wrapper`）：拆出 count 入栈新对象设值、源堆确认原版 b 写后回写 a+b=pre-count | 原版拆堆和 UI 输入 | 返回的新对象由 caller 明确交给原版或扩展事务 | 新对象未入库，不得直接写扩展 descriptor 后又交原版，也不得双重释放；整堆拆分时原版可能释放源对象，源堆触磁前必须确认存活。 |

### 2.3 CHAR、ITEMSYSTEM 和生产入口

| 函数/函数族 | VMA | 当前处置 | caller 面 | 扩展语义 | 风险边界 |
|---|---:|---|---|---|---|
| `CHAR_UseItemEx` | `0xeb670` | 优先原版调用；API/UI 入口做逻辑槽门禁和受控物化 | 药水、卷轴、技能书、配方书、增益、打包物和确认使用 | 扩展 item 物化后调用原版效果；成功消费由 `ConsumeItem` Hook 承接 | 不能重复模拟效果或扣数量；效果函数若直接依赖物理槽，必须在适配层隔离。API 现码：`game_inventory_use.inc:98-106`。 |
| `CHAR_ProcessShortcut` | `0xec028` | 上层分发；不新增 Hook | 快捷栏直接使用路径 | 快捷栏仍由原版处理；扩展对象通过查询/物化/消费适配 | 不以 `GetItemCount` 代替快捷栏对象身份；快捷键全 caller 运行时覆盖待验。 |
| `CHAR_EquipItem`（`EquipItemInInven` 语义） | `0xe51c0` | 优先原版规则；扩展装备由上层事务调用适配 | 自动找装备槽、API 装备和 UI | 扩展对象不直接进入不受控的原版自动装备；扩展侧先校验角色和目标槽 | 不能用固定角色替代当前菜单角色；装备对象所有权必须和角色槽同步。 |
| `CHAR_EquipItemFromInvenToSlot` | `0xe5368` | Native Hook；扩展视图对象受控临时借入物理槽 | 装备交换、被替换装备回包 | 原版交换入口继续决定装备规则；扩展物品只在调用期间暴露，被替换装备转扩展所有权 | `extension_bag_internal_equip_active` 防止递归；失败必须恢复物理槽和角色槽。现码：`native_inventory_hook.cpp:201-221`、`extension_bag_equip.inc:155-220`。 |
| `CHAR_UnequipItemToInven` | `0xe2f68` | Native Hook；原版失败后扩展兜底 | UI 卸装备、卸袋和未知 caller | 原版能入物理袋则透传；原版失败时扩展收编；扩展袋卸下必须非空拒绝 | 只信原版返回值会丢扩展兜底；目标任务袋 `5` 排除。现码：`native_inventory_hook.cpp:133-143`。 |
| `CHAR_PickItemAll` / 拾取族 | `0xec4d8`；回调注释 `0xdd15c` | 不 Hook；保留 `nav_pick_items` 就近旁路 | 官方按键拾取、后台导航拾取、掉落回调 | 模块复刻范围判断和 `NOTIFIER_Add`，主线程回调最终进入 `INVEN_SaveItem`；原版优先、满包再由 SaveItem wrapper adopt | 直接后台调用原版会触发空音频句柄崩溃；不能改变掉落对象、通知节点和失败释放。现码：`game_world_movement.inc:4-35`，VMA `game_symbols.h:422-425`。 |
| `ITEMSYSTEM_PutJewel` | `0x10bcb4` | Native Hook，原版 backup + 扩展材料适配 | 装备页、API 镶嵌、装备槽拖放和 apply 分支 | 原版返回 0 决定效果；扩展材料源由 H-15 或同袋 apply 路由承接，`stage4_put_jewel` 处理宝石两端分流 | 扩展宝石禁止 `RemoveItemDirect`；扩展宝石+原版装备不得直调 `virtual_bag_put_jewel_native`，消费由 `ConsumeItem` Hook 完成。现码：`native_inventory_hook.cpp:297-301`、`inventory_hook_stage4.cpp:106-119`。 |
| `UIEquip_IsApplyStuff` | `0xb8d4c` | 直接调用原版判定；apply 路由前置校验 | 同袋扩展材料→扩展装备、装备槽拖放 | 对目标装备和扩展宝石/强化卷轴判定是否可镶嵌/强化；不改原版判定结果 | 仅同一扩展逻辑袋的 apply 候选进入该路径；判定失败返回 `Blocked`，不得降级为扩展 swap。现码：`extension_bag_public_runtime.inc:132-145`。 |
| `UIEquip_ApplyStuff` | `0xb8df8` | **apply 路由经原版 `ApplyStuff`**；材料消费经既有 Hook | 同袋扩展材料→扩展装备 | `SAVE_IsOK` 通过后放锁调用原版 `ApplyStuff`；原版内部按材料类型进入 `PutJewel` 或 `EnchantItem`，目标 payload/hash 随后同步 | 只允许源为扩展宝石/强化卷轴、目标为装备且同袋；失败必须 `Blocked`，不降级为 swap。提交 `19dbc77`；现码：`extension_bag_public_runtime.inc:132-245`。 |
| `SAVE_IsOK` | `0x128c14` | apply 路由前置调用；不接管保存流程 | `UIEquip_ApplyStuff` 成功前状态检查 | 返回失败时终止 apply、abort token/session，不消费材料、不改装备、不进入 swap | 不把该状态检查等同于完整保存提交；VMA/类型登记见 `game_symbols.h`。 |
| `ITEMSYSTEM_EnchantItem` | `0x10b330` | 原版 `ApplyStuff` 分支直接调用；材料消费经既有 `ConsumeItem` Hook | 强化卷轴→装备 | 强化卷轴成功更新装备 payload/等级，`ConsumeItem` 恰好消费一张卷轴；扩展只同步逻辑 descriptor/object/hash/projection | 不新增独立强化业务或绕过 `ApplyStuff`；不可强化、非卷轴和失败路径保持材料与装备不变。 |
| `UIEquip_EquipControlEventProc` | `0xb8f7c` | **Native Hook，原版 proc 继续执行** | 装备槽拖放（event `0x04`） | 源为扩展 apply 材料（宝石/强化卷轴）且 object/descriptor/generation/session 校验通过时取 token、放锁调用原版 proc；非扩展源/非 apply 材料直接 backup | 校验或事务失败 Blocked，吞事件且不降级 backup；`game_patch_move_merge.inc` 不参与 apply 路由。 |
| `UIEquip_RefreshItemArea` | `0xb7a00` | **Native Hook，非重入锁内 trampoline 后覆盖投影** | 原版背包区域刷新、扩展投影安装/恢复及事务收尾 | 原版一次刷新由 H-16 在锁内同步当前投影；模块内部主动刷新统一走 raw-original dispatcher | `refresh_depth>0`、restore 抑制或投影未安装时只走 trampoline；draw-end 不执行帧级 heal，无刷新事务使用定点 slot sync。现码：`native_inventory_hook.cpp:211-218`、`game_ui_virtbag.cpp:237-245`。 |
| `ITEMSYSTEM_ReleaseSealed` | `0x10af4c` | 上层分发；不接管 | 解封类使用 | API 先记录原版库存变化，成功后消费输入 | 产物可能多件；输入消费、扩展落点和失败原子性未闭合。 |
| `ITEMSYSTEM_OpenItemBox` | `0x10e970` | 上层分发；不 Hook | 开箱按钮/API | 原版开箱决定随机产物并内部走 `SaveItem`；扩展源通过受控使用入口 | 不凭 SaveItem wrapper 宣称多产物原子回滚；输入消费和部分成功仍需逐 caller 验证。现码：`game_inventory_use.inc:82-95`。 |
| `ITEMSYSTEM_ProcessUnpack` | `0x10ce50` | 上层分发；不 Hook | 拆包、打包物使用 | 逐产物原版优先，失败时才进入扩展兜底策略 | 输入消费时机、第三参数和前序产物回滚未闭合，保留现有旁路。 |
| `ITEMSYSTEM_CreateItem` / `MakeItem` | `0x10be9c` / `0x10c6c8` | 优先原版创建；caller 明确移交；`MakeItem` 另有 H-20 Native Hook（`make_item_wrapper`，纯透传、数量回写恒 0——arg2 是静态表查找/品质参数非数量，R-52） | 任务奖励、开箱、合成、API 创建 | 创建对象不是已入库对象；选择原版 `SaveItem` 或扩展事务之一 | 失败必须释放仍由 caller 持有的对象；不能创建后同时被 SaveItem 和扩展 ledger 接管；不得把 arg2/category/flag 当数量回写。 |
| `MIXSYSTEM_MakeItem` / `UseStuff` | `0x11af58` / `0x11b300` | 上层分发；保留合成旁路 | 5 个合成 caller、模块合成入口 | 原版负责产物规则；模块负责已确认的材料/产物边界和失败检查 | 没有安全、静态可证明的完整逆操作，不猜测提交回滚；材料删除顺序和产物释放待验证。 |

### 2.4 UI、详情、商店和销毁入口

| 函数/入口 | VMA | 当前处置 | caller 面 | 扩展语义 | 风险边界 |
|---|---:|---|---|---|---|
| `UIEquip_ButtonUseExe` | `0xb80b8` | 详情按钮数组 PtrHook/上层分发；非全局 Native Hook | 详情“使用”按钮 | 原版按钮负责确认/效果入口；扩展详情 execute 识别逻辑槽并调用原版效果 | 确认类物品必须让原版创建弹窗，再由 `0xb8478` Hook 接管 OK；不能在详情 hook 中重复消费。现码：`extension_bag_equip.inc:802-858`。 |
| `UIEquip_ButtonUseAfterConfirmExe` | `0xb95f8` | PtrHook 后保留原版弹窗 | 需要确认的物品 | 扩展对象保持详情身份，确认阶段交给 OK Hook | 原版 OK 会先 `FindItemSlot`；不能让扩展对象落入该物理查找失败路径。 |
| `UIEquip_ButtonEquipExe` | `0xb7c18` | Native Hook，函数级三态 | 详情装备、背包类装备和未知按钮 caller | `Handled` 扩展事务；`Blocked` 吞掉原版；`NotExtension` backup | `Blocked` 绝不能降级 backup，否则原版内联删源；现码：`native_inventory_hook.cpp:305-318`。 |
| `UIEquip_ButtonUnequipExe` | `0xb7e14` | Native Hook | 卸装备、卸袋详情按钮 | 卸袋非空/无空间弹模块提示，其余 backup | 只处理模块确认的当前详情身份；popup 和 desc 清理必须成对恢复。 |
| `UIEquip_OKConfrimUseItem` | `0xb8478` | Native Hook，original-first | `ConfirmUseItem` 的 OK 回调 | 扩展详情对象直接调 `CHAR_UseItemEx`，token 完成后刷新投影；非扩展 backup | token/generation/owner 不匹配时不得释放或替换对象；现码：`native_inventory_hook.cpp:263-271`、`extension_bag_equip.inc:684-769`。 |
| `UIEquip_MakeDesc` / 物品详情 | `0xb8980`；唯一调用点 `0xb9188` | BL patch 到 `make_desc_equip_gate`，再做详情按钮 PtrHook | 物品控件打开详情 | 先让原版生成菜单，再捕获当前 item，跳过装备/卸下 proc，接管使用/卖出/销毁 | 控件树重建会使失效指针不可用；操作前必须复核 item+bag+slot+cache。现码：`extension_bag_lifecycle.inc:728-770`、`extension_bag_equip.inc:620-681,985-1019`。 |
| `UIEquip_ButtonDestroyExe` / `OKDestroyItem` | `0xb6240` / `0xb83d0` | 详情 PtrHook + popup OK/Cancel 槽劫持（扩展销毁）；`UIEquip_OKDestroyItem` 另有 H-23 Native Hook（R-55 原版背包详情出售接管：启用态取 canonical 全量、`unit×count×7/10` 加钱删堆刷新，按钮预演只回填展示金额；关闭态/非背包详情/校验失败 backup） | 装备页详情销毁/出售 | 扩展对象确认后逻辑清槽、释放并持久化/刷新；原版背包对象在启用态由 H-23 按 canonical 全量结算 | 全局 popup 槽劫持仅卖出/销毁；装备页与商店 popup 状态不得互用；按钮预演绝不结算，`desc_type!=2` 不接管；原版结算点 `0x1261c4` 内联 b 段读当前不在 R-51 表内。 |
| `UIStore_BuyItem` / `FindSaveSlot` 两处 | `0xd242c`；callsite `0xd24a0`,`0xd2540` | callsite patch 到 `store_buy_find_slot_gate`，再由 SaveItem Hook 收编 | 商店购买普通货物和 CopyAsNewUID 路径 | 原版物理有槽原样购买；物理满而扩展有空位则放行到 SaveItem adopt | gate 返回值只表达“继续流程”，不表达扩展物理槽；扣款/入库失败退款仍是边界。现码：`extension_bag_store.inc:630-644,714-726`。S2：S1 时代的 `+0x1a8` 数量写起始位 patch（bit25→bit22，fix-15）已移除——S2 布局下原版 b 字段（bits25–31）就是低位段，`SetBitValue(start=22)` 的 `count<<22` 错位模式会写坏 a/b 编码；b 原生写对购买量 ≤127 正确，>127 归调用方级写点（S2-P3）开放点。 |
| `UIStore_MakeDesc` / 商店详情卖出 | `0xd27a0`；callsite `0xd287c`；按钮 `0xd1818`；原版卖出 `0xd25f0` | 商店独立 BL patch + 商店详情 PtrHook + popup callback | 商店页扩展投影卖出 | 商店投影独立，先加钱、逻辑删除，失败减回；只刷新商店投影 | 禁止调用 `UIStore_RefreshInvenItem` 破坏整列扩展投影；不复用 UIEquip 详情状态。现码：`extension_bag_store.inc:439-557,559-613`。 |
| UI 详情/商店/销毁全局 popup | `G_POPUP_FPOK_VMA=0x3070e0`、`G_POPUP_FPCANCEL_VMA=0x3070d8` | GOT 槽临时替换，完成后恢复原回调 | 卖出、销毁确认 | 回调只接受当次详情身份和价格/数量快照 | 槽是全局且可被覆盖；必须保存原 callback，取消、失败、成功和 stale identity 都清理。VMA：`game_symbols.h:96-97`。 |

**矩阵结论**：本节函数的 VMA、现码处置和边界以当前 `game_symbols.h` 与 Hook/适配实现为准；“Native Hook”只表示机制已安装或有安装路径，不代表每个游戏内 caller 已真机验证。

#### 2.4.1 出售取价函数处置

`extension_bag_sell_price` 继续调用原版 `ITEM_GetSellPrice` 获取单位价，不复制原版
category/ability/payload 分支。适配层先按 `stack_codec::max_count(stack_limit_enabled())`
将数量收敛到 99 或 999，再要求单位价和最终价均不超过 `INT32_MAX`；装备页销毁/粉碎
保留 70% variant，装备页卖出、商店页和 API 保留 100% variant。

装备页和商店页在写入 `fn_popup_create_yesno_from_textdata` 前必须完成对象、generation、
release guard、类别、数量和价格校验；校验失败不创建弹窗、不写弹窗金额。API 卖出复用同一
适配器，非法价格直接返回业务失败并记录 `sell price reject reason=...`。成功取价记录
`source/category/count/unit/variant/final/payload/gen`，用于与原版单位价和真机弹窗参数对照。

原版背包详情出售（R-55）经 H-23 `UIEquip_OKDestroyItem` 接管时使用同一 70% variant 口径：
`vanilla_sell_money` 取已 hook getter 的 canonical 全量、按
`unit×min(canonical,999)×7/10` 计算，并复用 `sell_price::calculate` 的边界与溢出校验；
canonical 0、单价越界、结果越界一律拒绝并 backup。按钮预演（`x23=0x666`）只回填该金额，
弹窗 OK 才加钱删堆；加钱失败/删堆未生效退款，不半执行。

### 2.5 矩阵使用核对表

下列核对项是每次变更函数决策时的最小证据集；它们不是新增函数行，也不改变上表裁定：

| 核对项 | 必须回答的问题 | 当前证据入口 |
|---|---|---|
| ABI | 参数宽度、返回值、out 参数是否与当前 hook/callsite 一致？ | `game_symbols.h` 的函数签名区；`inventory_hook_stage4.h:6-56` |
| 物理域 | 是否扫描 `0..5`，是否由参数决定是否含任务袋 `5`？ | `rulebook.md:46-56,72-84` |
| 身份 | 当前指针能否同时由 descriptor、object、bag/slot 和 generation 证明？ | `extension_bag_ownership.inc:81-143` |
| 所有权 | 成功、失败、合并、回滚各自由谁释放或 handover？ | `extension_bag_ownership.inc:34-78`；原版 `ITEMPOOL_Free` VMA `0x108160` |
| 原版副作用 | 是否会改物理槽、更新快捷栏、刷新控件、触发任务或释放传入对象？ | 当前函数实现与 `game_symbols.h:268-270,458-463` |
| caller 覆盖 | 是函数全局入口，还是只有一个 UI/生产者 callsite？ | `native_inventory_hook.cpp:328-442`；商店 patch `extension_bag_store.inc:646-726` |
| 锁 | 是否持有 `g_virtual_bag_mtx` 调用了刷新、原版函数或 `op_ok()`？ | `extension_bag_public_runtime.inc:277-324`；规则册锁纪律 |
| 回滚 | 失败后是否验证槽位、对象内容、数量和 ledger，而不是只信返回值？ | `inventory_hook_stage4.cpp:64-80`；`extension_bag_equip.inc:100-152` |
| 保存 | 变化是否只能由完整 `SAVE_Save` 成功后的 participant commit 落盘？ | `module_save.cpp:62-112`；`extension_bag_runtime.inc:820-826` |

### 2.6 原版返回值不可直接当成功的函数

以下事实必须在调用方保留删后/写后验证：

* `INVEN_RemoveItemDirect@0x103fd8` 的返回值不可信，成功删除仍可能返回 0；读取 `INVEN_pItem[bag][slot]` 是成功判定。
* `INVEN_SaveItemDirect@0x103bf0` 的返回寄存器不代表槽号、被替换对象或合并释放结果；caller 必须按原版已证明的分支语义处理。
* `INVEN_RemoveItemData@0x1040a8` 的类别/数量回滚可能部分完成，不能按一次调用原子清空。
* `CHAR_UseItemEx@0xeb670` 的 0/非 0 只代表当前效果路径是否成功；扩展消费是否发生还要通过 `ConsumeItem` Hook、descriptor 数量和 object 身份复核。
* `UIEquip_OKConfrimUseItem@0xb8478` 是 void 回调，完成与否由 token finish、消费状态和详情清理证明，不以函数返回值判断。

### 2.7 单函数变更顺序

任何新接入必须按以下顺序推进，不能跳过 caller 和所有权审计：

1. 从 `game_symbols.h` 读取 VMA、typedef 和结构布局，确认现码没有第二个同名来源。
2. 记录全部直接 BL、间接回调和 UI/GOT/patch 入口；单个 API 或按钮不代表全局覆盖。
3. 先定义原版对象路径：参数、backup、原版返回值、物理槽副作用和对象释放点。
4. 再定义扩展对象路径：身份分流、物化、逻辑状态提交、投影刷新、handle/generation 和失败回滚。
5. 用 Host 测试覆盖原版优先、扩展 fallback、满包、堆叠、不可堆叠和 stale identity。
6. 证据闭合后才评估旁路调整；未完成真机生产者证据时保持“暂缓/保留”。

这套顺序特别适用于 `SaveItem`、`FindSaveSlot`、装备交换和 `ConsumeItem`：它们分别连接生产者漏斗、商店门禁、角色装备所有权和确认使用 token，任何一处先改机制再补边界都会扩大问题 B 的取证范围。

### 2.8 类别判定与袋对象安全门

* `item_count_encoding` 和扩展运行时 `category_is_equip` 共用三态结果：
  `kEncoded` 表示可堆叠数量，`kNotEncoded` 表示装备/非堆叠，`kUnknown` 表示表基址、
  静态表或 stride 不可用。`kUnknown` 在 `state_json`、扩展消费、`game_inventory_basic`、
  `game_patch_move_merge` 和 apply/装备按钮路径必须拒绝，不能按可堆叠处理。
* `patch_payload_count` 自带类别门控；非 `kEncoded` 直接 no-op 并记录日志。ext→ext 合并
  比较必须传入 `virtual_bag_category_uses_stack_count`，装备 payload 不得被数量归一化。
* 原生袋对象 `+0x10` 的容量 bit0..24 与普通物品数量位不同；页签、商店和卸下袋对象三处
  统一使用 `stack_codec::write_native_bag_object_marker`，不得使用 `write_count`。
* `is_backpack_category` 只判断扩展 `BagType 1..4`；`category_is_extension_backpack`
  只判断原生 `ITEMCLASSBASE +2 == 0x1f`。两者不是同一语义，不能互换。

## 3. 全局裁定

### 3.1 original-first 与对象身份分流

1. 原版对象先走原版函数和 backup，原版负责效果、规则、成功/失败和原版消费时机。
2. 扩展对象先以 `bag/slot + descriptor + cached object + handle + generation` 复核身份，再进入扩展分支；不能仅按类别判断扩展对象。
3. 扩展对象绝不进入 `INVEN_RemoveItemDirect`、`INVEN_SaveItemDirect`、`INVEN_RemoveItemData`、`ITEMPOOL_Free` 或原版物理保存链。原版对象也不能在扩展逻辑事务中被重复持有。
4. 物化只是得到可被原版效果函数读取的临时 native 对象，不改变它的最终所有权；成功、失败、回滚和释放由适配层决定。
5. 释放条件由 generation/token/owner 共同决定。活动 confirm-use 期间禁止替换；pending-release 对象不能复用。详细契约转交 `rulebook.md` 的 token/generation 规则。

### 3.2 “能底层 hook 走底层，不能的上层就近分流”

这是覆盖和副作用判据，不是“尽量多 Hook”：

* 确认使用是正例：`UIEquip_OKConfrimUseItem@0xb8478` 是单一确认回调，原版先用 `FindItemSlot`，扩展对象必然在物理数组外；Native Hook 在回调边界 original-first 分流即可，且不会把扩展对象伪装成物理槽。代码：`native_inventory_hook.cpp:263-271`。
* `ConsumeItem`/`RemoveItem` 是正例：原版已决定消费/删除时机，Hook 只把扩展对象截断到逻辑后端，原版对象走 backup。
* `FindItemSlot` 是反例：它返回 `int8_t` 物理编码；扩展袋编号无法无损编码，且 caller 会把输出当作 `INVEN_pItem` 坐标。故只能在确认使用、详情、API 等上层就近分流，不能做全局伪造 Hook。
* `SaveItem` 是边界例外：当前代码已证明它是统一创建物品漏斗，并且 wrapper 可在 backup 失败后以 item 身份做扩展 adopt，因此采用 Native Hook；但它不替代各生产者对任务语义、context、产物回滚的审计。

**未定-需真机证据**：18 条注释 caller 是否都在真实设备上满足任务域分类、失败释放、
多产物回滚和扩展 adopt 后的所有权交接，静态安装链不能判定；按 VM-19–VM-22 逐 caller
取证，不能用 H-13 已安装替代生产者覆盖结论。

### 3.3 宽 Hook 禁令

* 禁止因 `SaveItem` 目前登记 18 个 caller 就扩大到 `SaveItemDirect`、`SaveItemData` 或所有保存辅助；wrapper 只在 `SaveItem` 失败且 item 尚未进入物理库存时收编。
* 禁止把 `FindItemSlot` 的 1 字节 output 改成扩展槽。裁定依据是 `game_symbols.h:391,626`、`rulebook.md:60-70`；该 output 的位布局只描述真实物理数组。
* 禁止以一个 `FindSaveSlot` callsite、一个商店入口或一个 API endpoint 宣称所有生产者覆盖。
* 禁止在持有 `g_virtual_bag_mtx` 时调用会触发缓存刷新的 `op_ok()`；跨层调用先释放锁并按 participant/transaction 规则处理。

## 4. 生产者漏斗与五个不接管边界

### 4.1 统一漏斗

```text
创建/掉落/奖励/开箱/合成/商店对象
    → 原版 FindSaveSlot/SaveItem
    → 原版有物理槽：backup 完成并保持原版所有权
    → 原版无槽：save_item_wrapper → extension_bag_adopt_native_item
    → 返回 1 让 caller 按原版成功语义继续
```

`save_item_wrapper` 在 backup 前调用 `extension_bag_store_restore_for_original_write`，避免商店投影临时放大容量把对象写入真实容量外；代码：`native_inventory_hook.cpp:184-199`、`extension_bag_port.cpp:149-155`。商店购买还必须先经 `FindSaveSlot` 两处 gate，否则原版满包会在到达 SaveItem 前直接弹满包。

### 4.2 五个不接管边界

| 边界 | 裁定 | 现码/依据 |
|---|---|---|
| 任务袋 `5` | 任务物品按原版任务语义留在 `5`；普通扩展收编只允许目标 `0..4` 失败后转逻辑袋 | `rulebook.md:46-56` |
| 任务奖励分类 | 不把所有奖励统一改成扩展物品；先判任务物品、类别和 caller context | `runtime-architecture.md:294-305` |
| 开箱 | 不 Hook `ITEMSYSTEM_OpenItemBox`；由原版决定随机产物，SaveItem wrapper 只处理单件漏斗 | `game_inventory_use.inc:82-95`、`game_symbols.h:448` |
| 拆包 | 不 Hook `ITEMSYSTEM_ProcessUnpack`；逐产物核对输入消费、部分失败和回滚 | `game_symbols.h:471`、`runtime-architecture.md:294-305` |
| 合成 | 不做全局 `MIXSYSTEM` Hook；保留模块旁路，5 个 caller 逐一核验，未证明完整逆操作不猜测回滚 | `game_patch_craft.inc`、`runtime-architecture.md:294-305` |

`NetworkStore_AddItem` 的原版链仍保留，但不属于当前扩展背包生产者验收；其保存 callsite 仍属于完整保存协调清单。

## 5. 当前路径与保留边界

### 保留

* 扩展三方向事务、逻辑袋投影、跨袋拖动和任务袋门禁。
* `ConsumeItem`、`RemoveItem`、装备、卸下和宝石适配；它们是原版时机的扩展后端，不是可删除的重复业务。
* 扩展对象 ledger、payload 物化和 sidecar participant。

* ownership ledger 耗尽拒绝登记；触摸窗口内的对象释放进入定长延迟回收队列，排空前检查投影控件和模块对象缓存引用。

### 当前待补

* 查询 Hook 维持 original-first/fallback；调用方使用 `inventory_item_ref_at` 等身份化引用。
* 使用/强化/宝石保留原版效果，扩展只承接物化、消费、payload、所有权和 UI 同步。
* `SaveItem` 统一收编已存在；生产者 caller 仍需补 context、任务域、失败释放和真机证据。

### 保持条件

没有同时满足全部 caller 覆盖、原版/扩展成功失败、物理槽和任务袋 sentinel、对象释放、堆叠/payload、保存边界、Host 与真机回归前，任何旁路不得删除。问题 B 也不能因为 0x18 单一 owner、物理快照 guard 或 pre/post 插桩存在就标记解决。

## 6. token/generation 释放契约摘要

* 空槽合法状态要求 descriptor 为空、object 为空、handle 为 0、`active=false`、`pending_release=false`；`extension_bag_ownership.inc:81-93`。
* `module_use_begin_locked` 为同一对象递增 generation 并绑定 owner thread；finish/abort 必须匹配 bag、slot、item、generation、owner 和当前 object，见 `extension_bag_ownership.inc:146-244`。
* active 时释放只能由匹配 token 标为 pending-release；finish 再完成释放。无 token、失效 object 或不同 owner 均拒绝，见 `extension_bag_ownership.inc:247-295`。
* 交换必须同时移动 descriptor、object、category/hash、handle 和 use state；缓存与 descriptor 不一致时先拒绝，不得先释放后尝试回滚，见 `extension_bag_transaction.inc:352-538`。
* 触摸窗口内不得直接释放借出对象；视图恢复先归还 view borrow，再恢复原版容量和控件。完整规则只在 `rulebook.md` 维护。

## 7. 问题 B：未解决 + 已布防

事实是扩展袋 0 的双非空交换成功后，原版袋 0 同号 b 槽物品可消失，视图重开仍消失，保存后固化。当前 drop 三态门、rollback/handle/视图归属校验、`moveMergeEnabled` 解耦、0x18 单一 owner、物理 `0..5×16` 快照和 `orig_event pre/post` 插桩均未形成修复证明。取证缺口仍是 H3/H4 日志未采集，`ERROR physical inventory mutation` 与 digest 是否触发未知；素材：`archive/extension-bag/swap-loss-log-excerpt-20260908.txt`（关键段摘录）。

当前布防包括：

1. 投影 release 的 `0x18` 是唯一提交者；事务失败也返回 1 吞掉原版，见 `extension_bag_lifecycle.inc:297-315`。
2. 原版事件调用前后捕获物理指针快照，检测变化后尝试恢复并同步投影，见 `extension_bag_lifecycle.inc:116-166`。
3. `SAVE_Save` 内唯一 `SAVE_SaveInventory` BL 在写盘前恢复投影，见 `extension_bag_render.inc:373-390` 与 `extension_bag_lifecycle.inc:609-646`。
4. 但快照恢复的是指针数组，不是被原版释放/破坏的对象内存；若原版保存序列继续读取被毁内存，物理空槽或坏记录仍可能被固化。因此问题 B 仍是“未解决+已布防”，不得改写为修复完成。

## 8. 维护与验收入口

* 修改函数决策时，必须同步检查 `game_symbols.h` 的 VMA、当前 wrapper、caller 面和对象释放边界；不接受手填地址。
* Hook 变更需验证原版对象 backup、扩展对象识别、递归 guard、安装回滚和 token/generation；生产者变更需逐 caller 记录原版满/未满、扩展有空/全满、堆叠和失败释放。
* 本册不批准删除 `extension_bag_transaction.inc`、`extension_bag_api_impl.inc`、`extension_bag_equip.inc` 或 P6 保存协调器；移除结论必须落到对应操作验收卡。
* 相关静态验收：`git diff --check`、相关 Host tests、`scripts/maintenance/check_symbols.py`。Android 构建与真机验收由主代理按阶段要求执行。
