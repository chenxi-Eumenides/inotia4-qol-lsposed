# 扩展背包验收矩阵

> **状态：CURRENT；本册是验收唯一权威。**
>
> 本册把当前规则册、运行时架构册、库存接入决策册、拖动协议和存档册交叉压成可执行的操作契约。源码行号按当前工作树记录；代码变更后必须重新核对文件:行。Host 测试名只引用当前存在的测试，`缺口：`表示本册登记的待补测试，不表示已经实现。
>
> **问题 B 固定口径：未解决+已布防。** 扩展袋 0 双非空交换后，原版袋 0 同号槽物品仍可能消失并被保存固化。当前三态门、对象/回滚校验、单一 `0x18` owner、merge 解耦和 H3/H4 物理快照守卫均不是修复证明。B-05 是模块扩展袋同堆合并能力，不属于原版基线能力。

## 1. 使用约定

### 1.1 编号和证据

* `VM-01` 起为真机用例号；每张卡只有一个主用例号，复合操作用同一用例中的编号步骤区分阶段。
* 原版物理袋为 `0..5`，普通交易目标为 `0..4`，任务袋 `5` 只保留原版语义；扩展逻辑袋对外为 `6..10`、内部为 `0..4`。
* VMA 只从 `data/native/game_symbols.h` 反查；域文件不新增裸地址。卡片中的 VMA 是验收定位，不改变调用 ABI。
* `Host` 表示可运行的当前测试；`真机` 表示唯一设备 `192.168.3.54:5555` 的行为证据；源码锚不能替代真机结果。
* API 面遵循 `docs/reference/api-reference.md`：库存读写使用 `/api/item/inventory*`，扩展袋并入 `bag=6..10`；视图控制仍是 `/api/debug/extension_bag/*` 的开发期能力。

### 1.2 失败判定总则

1. 原版对象优先走原版 backup；扩展对象先做 bag/slot、descriptor、object、hash、handle、generation 和 owner 复核。
2. 失败不得以 UI 看似恢复或 `txn committed` 单独判为成功；必须同时核对物理槽、逻辑槽、对象账本和必要的 sidecar 状态。
3. 任务袋 `5` 作为扩展物品操作的源/目标时必须返回 `task bag excluded` 或等价拒绝，且袋 5
   前后内容不变；作为当前原版页签来源时，页签切换到扩展袋或原版袋不属于物品操作，必须允许。
4. 保存只有完整 `SAVE_Save@0x129600` 成功并完成 participant commit 才算持久化成功；内存刷新、退出视图和 prepare 不算保存。
5. 问题 B 用 H3/H4 表判读，任何失败卡不得被 Host 事务模型替代为通过。

## 2. 操作契约卡

### VM-01 查询物品、持有量和数量

- **操作**：按类别读取/判断物品（`FindItem`、`HaveItem`、`GetItemCount`）。
- **原版链(VMA)**：`INVEN_FindItem@0x10438c`、`INVEN_HaveItem@0x104870`、`INVEN_GetItemCount@0x104260` 扫描原版物理袋；任务袋 5 是否参与由原版入口语义决定。
- **扩展接管点(文件:函数)**：`native_inventory_hook.cpp:find_item_wrapper`、`have_item_wrapper`、`get_item_count_wrapper`；Stage4 dispatcher；扩展遍历由 `game_state.cpp:inventory_item_ref_at`/`for_each_inventory_item` 完成。
- **共享状态读写**：读 `g_virtual_bag_mtx` 保护下的 descriptor、`g_virtual_bag_state.items`；不写对象、物理槽或 sidecar。
- **必须保持的不变式(引 R-xx)**：原版命中优先；扩展只作 fallback/聚合；逻辑袋不可编码为物理槽（R-21、R-22）。
- **失败语义**：原版和扩展均未命中返回空/0；未知类别不创建对象；查询失败不改变任何状态。
- **Host 测试名(现有或「缺口」)**：`test_p7_stage4_find_item_poc`、`test_queries`；断言原版优先、扩展 fallback、递归 guard 清理和数量聚合。
- **真机用例号(编号规范 VM-xx，写操作步骤+预期)**：`VM-01`：①进入 world；②GET `/api/item/inventory/items` 并选一个只在扩展袋的类别；③读取数量/物品；预期原版+扩展分别可见，袋 5 仍按原版读语义出现，不产生写入。
- **证据锚类型**：Host + API 真机 + 源码（`game_symbols.h:388-390`）。

### VM-02 容量、空槽和任务袋边界

- **操作**：查询原版袋容量、扩展派生容量和可用空槽。
- **原版链(VMA)**：`INVEN_GetBagSize@0x103250`、`INVEN_GetEmptyBagSlot@0x103280`、`INVEN_IsHavingEmptySlot@0x103460`；`include_task_bag` 决定原版是否扫描 5。
- **扩展接管点(文件:函数)**：`inventory_hook_stage4.cpp:stage4_is_having_empty_slot`；`extension_bag_runtime.inc:extension_bag_has_empty_slots`；`virtual_bag_transaction_rules.inc:derive_capacity`。
- **共享状态读写**：读 `capacities`、`types`、descriptor、对象和 token；原版 `bag_table` 只在投影窗口临时读写并必须恢复。
- **必须保持的不变式(引 R-xx)**：容量统一派生，任务袋不成为扩展目标，空槽不能只看 descriptor（R-05、R-21、R-22、R-26）。
- **失败语义**：容量 0/无可分配槽返回满或拒绝；`needed<=0` 由当前 `stage4_is_having_empty_slot` 明确返回 `1`（可放行，不调用 backup 或扩展 fallback）；袋 5 扩展目标直接拒绝。
- **Host 测试名(现有或「缺口」)**：`test_queries`（`test_inventory_hook_stage4.cpp:115-125` 已断言 `needed<=0` 返回 `1`）、`test_virtual_bag_state`；缺口：`test_virtual_bag_empty_slot_predicate`（断言派生容量、active/token 对空槽的影响及 bag 5 排除）。
- **真机用例号(编号规范 VM-xx，写操作步骤+预期)**：`VM-02`：①读取 `/api/item/inventory/bag/{bag}/info`；②分别检查未装备、4/8/12/16 容量袋；③向 bag 5 发起一次移动；预期容量与 `derive_capacity` 一致，bag 5 返回 `task bag excluded`。
- **证据锚类型**：Host + API 真机 + 源码。

### VM-03 普通使用物品

- **操作**：使用原版袋或扩展逻辑袋中的药水、卷轴或其他可用物品。
- **原版链(VMA)**：`UIEquip_ButtonUseExe@0xb80b8` → `CHAR_UseItemEx@0xeb670` → `INVEN_ConsumeItem@0x1047bc`；骰子、解封、开箱另有类别分支。
- **扩展接管点(文件:函数)**：`api/native/game_inventory_use.inc:data_op_use_item`；`extension_bag_api_impl.inc:extension_bag_api_use_item_impl`；消费由 `native_inventory_hook.cpp:consume_item_wrapper` 承接，API 原生调用前后复核 item token/身份。
- **共享状态读写**：读逻辑 descriptor/object/token；成功写 count/payload 或清槽，更新 object cache、projection 和 dirty；锁外调用原版效果。
- **必须保持的不变式(引 R-xx)**：原版效果与消费时机优先，扩展对象不进入物理释放链，锁内不调 `op_ok()`（R-03、R-07、R-15、R-18、R-25）。
- **失败语义**：空槽/不可用/冷却/状态不符返回 `slot empty`、`item not usable` 或 `on cooldown`；失败不扣数量、不释放 stale 对象。
- **Host 测试名(现有或「缺口」)**：缺口：`test_use_item_original_first_and_consume`（断言原版效果只调用一次、扩展 count/payload 与 projection 同步、失败无扣减）。
- **真机用例号(编号规范 VM-xx，写操作步骤+预期)**：`VM-03`：①GET inventory 记录 bag/slot/count；②POST `/api/item/inventory/use_item`；③再次读取并观察冷却/状态；预期成功才消费并返回最新 state，失败返回统一错误 envelope；原生调用前后 token 复核不通过则拒绝消费。
- **证据锚类型**：API 真机 + 源码；全类别消费仍需 stage-5 证据。

### VM-04 确认使用

- **操作**：对需要确认的扩展物品打开详情并点击确认使用。
- **原版链(VMA)**：`UIEquip_ButtonUseAfterConfirmExe@0xb95f8` → `UIEquip_OKConfrimUseItem@0xb8478` → `INVEN_FindItemSlot@0x103704` → `CHAR_UseItemEx@0xeb670`。
- **扩展接管点(文件:函数)**：`native_inventory_hook.cpp:ok_confirm_use_item_wrapper` → `extension_bag_equip.inc:extension_confirm_use_item`/`virtual_bag_handle_confirm_use_item`。
- **共享状态读写**：读 `g_extension_desc_item`、bag/slot、generation、owner 和 use token；写 token finish/abort、descriptor/count、object cache、projection、详情身份。
- **必须保持的不变式(引 R-xx)**：确认使用固定走 `0xb8478` Native Hook，不能复活全局确认槽；成功后刷新并重投影（R-01、R-15、R-24、R-25）。
- **失败语义**：详情身份、token、角色或 generation 不匹配返回 false/handled 并清理 marker；抢锁失败按未接管回 backup，不等待、不写半成品。
- **Host 测试名(现有或「缺口」)**：缺口：`test_confirm_use_token_and_projection_refresh`（断言匹配 token 才 finish，失败 abort，成功 count/descriptor/control 一致）。
- **真机用例号(编号规范 VM-xx，写操作步骤+预期)**：`VM-04`：①打开扩展详情；②点需要确认的“使用”；③确认；④读取 inventory 和当前面板；预期扩展物品按原版效果消费，确认后无失效控件残留，原版袋无伪造槽。
- **证据锚类型**：真机必需 + 源码；不能由 Host 单独通过。

### VM-05 原版袋物品装备为扩展背包

- **操作**：将类别 1..4 的原版背包物品拖到未装备扩展标签，使其成为扩展袋对象。
- **原版链(VMA)**：原版 `TouchHandle` 的 drop `0x04`/控件 proc；无单一装备函数 VMA 可替代该 tab drop 链。
- **扩展接管点(文件:函数)**：`extension_bag_runtime.inc:extension_tab_item_proc` → `try_equip_on_extension_tab_drop_locked`；`extension_bag_equip.inc:equip_extension_bag_item_locked`。
- **共享状态读写**：读原版 source、目标 `types/capacities`；写扩展 descriptor/payload/object/handle/ownership、原版源槽清除、tab data 和 projection。
- **必须保持的不变式(引 R-xx)**：目标袋空且类别合法才收编；原版源确认清空后提交；源/目标不能误认角色装备（R-08、R-16、R-25、R-26）。
- **失败语义**：目标已装备、类别非法、容量不足、物化或源清除失败均拒绝并保持源；不得把失败降级成原版二次移动。
- **Host 测试名(现有或「缺口」)**：`test_equip_bag`、`test_virtual_bag_state`；缺口：`test_original_to_extension_bag_equip_commit`（断言源槽清空前不清逻辑源，失败可重试）。
- **真机用例号(编号规范 VM-xx，写操作步骤+预期)**：`VM-05`：①记录原版 bag/slot 的背包物品；②拖到未装备扩展标签；③读两侧 state 并切出/切回；预期扩展类型/容量正确、原版源空、切换后不显示非当前袋物品。
- **证据锚类型**：真机 + Host 部分模型 + 源码。

### S-05 页签 drop 装备与跨页签提交

- **状态**：已解决（2026-09-10 真机确认；提交 `3539918`）。
- **步骤**：①记录扩展源的逻辑 `bag/slot/category/generation`；②将类别 1..4 的扩展
  物品拖到空页签；③用非背包类物品从扩展源拖到其它页签；④用已装备页签拖到其它
  页签；⑤用原版物品拖到页签，作为原版→页签对照；⑥切回源页签并读取两个逻辑袋、
  原版物理袋、projection 和 session 收尾日志。
- **预期**：空页签仅对 `is_backpack_category(category)` 成立的源走装备事务；其它目标
  走 ext→ext；原版源保持原版 tab/drop 语义。target kind、session generation、
  tab/inventory generation 或 source 不匹配时明确拒绝并保持逻辑源；只有事务成功才提交
  源/目标、持久化并刷新目标 projection，`0x18` 只执行 cleanup-only 收尾。扩展源 drop
  目标解析先判页签、后判网格；标签矩形与网格行 y 坐标带存在重叠时，页签命中按切袋意图
  优先。
- **日志锚**：成功出现 `tab commit handled=1 bag=.. slot=..`；失败出现
  `cross tab reject reason=...` 或 `tab reject reason=transaction_failed ...`；释放相关
  路径出现 `deferred free enqueue` 与 `deferred free drain`。失败路径不得出现扩展
  `txn committed`。
- **关联规则/真机卡**：R-02、R-16、R-19、R-25、R-30、R-35、R-36；主卡 VM-29。

### S-03 扩展宝石/强化卷轴拖装备

- **状态**：已实现、真机验证通过（apply branch，提交 `19dbc77`）；覆盖宝石镶嵌和强化卷轴
  强化，apply 仅限同一扩展逻辑袋。R-50 准入收口（提交 `260911` 工作树）：apply 仅在
  「同袋 + 宝石/卷轴源 + `item_is_equip(target)` 为真 + `IsApplyStuff` 判真」时进入，
  普通物品目标回退正常交换。
- **步骤**：①在同一扩展逻辑袋准备扩展宝石、强化卷轴和目标装备，记录源的
  `bag/slot/count/handle/generation` 及目标装备 payload/socket/等级；②分别将扩展宝石和
  强化卷轴拖到目标装备；③读取装备 payload/socket/等级、材料数量、扩展逻辑槽和原版物理
  槽；④对无孔、不可应用材料、不可强化装备和空装备目标重复失败路径；⑤将宝石/卷轴拖到
  同袋普通物品（非装备）目标，确认走扩展交换而非 apply。
- **预期**：成功路径分别沿原版 `ApplyStuff` 的 `PutJewel` 或 `EnchantItem` 分支执行一次，
  并由既有 `ConsumeItem` 链恰好消费一份材料；成功路径不得出现 `extension swap`。同袋
  装备目标上已准入 apply 的执行失败（无孔、不可强化、`SAVE_IsOK` 失败等）返回 `Blocked`，
  不降级为 swap，宝石/卷轴、装备 payload 和槽位保持不变；**普通物品目标不进入 apply**，
  必须按扩展交换/移动处理并提交；跨袋边界不进入 apply 分支，其普通扩展事务不由本卡判定。
- **日志锚**：`apply branch kind=jewel|scroll`、`PutJewel|EnchantItem`、`ConsumeItem`、
  `apply committed`/`apply reject`、`session finish/abort`；已准入 apply 的失败路径应有
  `Blocked`，成功路径不得出现 `extension swap`；普通物品目标路径应出现
  `drop routed ext->ext` 或 `p5 session drop committed`（交换/移动），不得出现 `apply branch`。
- **关联规则/真机卡**：R-02、R-09、R-15、R-16、R-31、R-50；VM-10；原版对照 VM-B03。

### VM-29 页签拖放与原版页签对照

- **操作**：覆盖扩展源→空页签装备、扩展源→其它页签移动和原版源→页签对照。
- **步骤**：①准备类别 1..4 的扩展背包源和空页签；②拖放并确认扩展袋装备；③准备非
  背包类扩展源，拖到其它页签；④准备原版物品，按原版页签/drop 操作拖到页签；⑤切换
  页签并读取扩展逻辑袋、原版物理袋和 session。
- **预期**：①扩展背包源装备到空页签并清理原扩展源；②非背包类扩展源提交 ext→ext
  移动；③原版源不进入扩展事务；三种操作均无 stale moving。触摸窗口内释放先入队，
  队列满时保留 custody 且事务继续成功，draw-end 关闭窗口后再按控件/槽位引用排空。
- **日志锚**：`tab commit handled=1`、`deferred free enqueue`、`deferred free queue full; custody retained`、`deferred free drain`；
  原版对照使用原版 `MoveItem pre/post` 或 tab proc 日志，不能只用 UI 结果判定。
- **关联规则**：R-02、R-19、R-30、R-35、R-36；S-05 已解决。

### VM-06 扩展物品装备到角色

- **操作**：从扩展投影详情将装备穿到当前菜单角色的目标装备槽。
- **原版链(VMA)**：`CHAR_EquipItemFromInvenToSlot@0xe5368` 读取物理 `INVEN[bag][slot]` 并交换。
- **扩展接管点(文件:函数)**：`native_inventory_hook.cpp:equip_item_from_inven_to_slot_wrapper` → `inventory_hook_stage4.cpp:stage4_equip_item` → `extension_bag_equip.inc:extension_bag_equip_projected_item`。
- **共享状态读写**：读当前菜单角色、view/window bag、descriptor/hash/handle；临时借入物理槽，写角色装备、被替换装备的扩展槽、ownership、generation 和 projection。
- **必须保持的不变式(引 R-xx)**：临时物化不改变最终所有权；失败恢复物理槽和角色槽；`module_item_locked` 使用逻辑 bag 与当前 view（R-04、R-08、R-16、R-24、R-26）。
- **失败语义**：角色/等级/职业/装备槽或物化不合法返回原版失败；扩展源失败不得把投影对象交给物理释放链。
- **Host 测试名(现有或「缺口」)**：`test_object_operations`（fallback 装备与角色指针断言）；缺口：`test_projected_equip_handover_and_rollback`。
- **真机用例号(编号规范 VM-xx，写操作步骤+预期)**：`VM-06`：①在角色 A 菜单打开扩展装备详情；②点击装备；③确认角色 A 装备和扩展源/被替换装备位置；④切换角色 B 重复；预期角色身份不串、失败不丢物。
- **证据锚类型**：Host + 真机 + 源码。

### VM-31 任务袋来源页签切换与扩展目标边界

- **操作**：选中原版任务袋 `5` 后切换到扩展页签、切换到原版页签，再从扩展投影点击任务袋页签并尝试拖动物品到任务袋。
- **步骤**：①进入库存并选中任务袋 `5`；②点击一个容量和控件正常的扩展页签；③记录投影宿主原版袋号并切回原版任务袋/普通原版袋；④从扩展投影点击任务袋页签确认视图可切换；⑤分别尝试把扩展物品拖到任务袋和通过 API 将任务袋作为源/目标。
- **预期**：任务袋作为当前来源时可切到扩展视图，投影宿主只能是原版 `0..4`，视图状态为 `mode=kModule、selected=目标`；宿主不可用或容量/控件异常时拒绝并记录 `reason=projection_host_unavailable source_original_bag=5`。从扩展视图点击任务袋只切原版视图，不建立物品事务；拖动物品到任务袋及 API 任务袋源/目标继续拒绝，袋 `5` 内容不变。
- **日志锚**：成功 `extension tab selected bag=.. original_bag=5 projection_host=..`；拒绝 `projection_host_unavailable source_original_bag=5`、`invalid_transaction_domain bag=5` 或 `task bag excluded`；切回路径核对 `complete_success`/`write_restore`。
- **Host 测试名(现有或「缺口」)**：`test_virtual_bag_state`、`test_extension_bag_exit_rendering_state`、`test_p52_drag_session`；缺口：`test_task_bag_source_tab_projection_host`。
- **关联规则/原版基线/真机卡**：R-19、R-26、R-27、R-30；B-04；VM-31、VM-B04；API 任务袋边界追加 VM-15/VM-26。
- **证据锚类型**：真机必需 + Host 状态/事务模型；Host 不能替代任务袋来源页签真机行为。

### VM-07 装备按钮三态

- **操作**：点击扩展详情的装备按钮，并分别覆盖成功、已识别但阻断、非扩展物品。
- **原版链(VMA)**：`UIEquip_ButtonEquipExe@0xb7c18` 内联删除源并进入装备链。
- **扩展接管点(文件:函数)**：`native_inventory_hook.cpp:button_equip_exe_wrapper` → `virtual_bag_handle_backpack_button_equip_result`。
- **共享状态读写**：读详情 identity、view、token 和 projection；Handled 写事务状态，Blocked 不写原版源，NotExtension 不写扩展状态。
- **必须保持的不变式(引 R-xx)**：`Handled/Blocked/NotExtension` 不得压成 bool；Blocked 禁止 backup，防原版内联删源（R-06、R-16、R-30）。
- **失败语义**：扩展识别但事务拒绝返回 Blocked 并吞原版；非扩展才 backup；无效详情清理身份后按未接管处理。
- **Host 测试名(现有或「缺口」)**：缺口：`test_equip_button_three_state`（断言 Handled 不调 backup、Blocked 不调 backup、NotExtension 调一次 backup）。
- **真机用例号(编号规范 VM-xx，写操作步骤+预期)**：`VM-07`：①原版物品点装备；②扩展物品点装备；③制造 stale/无效目标再点；预期原版按钮仍完整可用，扩展拒绝不清真实同槽物品。
- **证据锚类型**：真机 + Host seam 缺口；源码锚 `native_inventory_hook.cpp:305-318`。

### VM-08 卸下角色装备到背包

- **操作**：卸下角色装备，覆盖原版有空槽、原版满但扩展有空槽、失败恢复。
- **原版链(VMA)**：`CHAR_UnequipItemToInven@0xe2f68`；详情按钮 `UIEquip_ButtonUnequipExe@0xb7e14`。
- **扩展接管点(文件:函数)**：`native_inventory_hook.cpp:unequip_item_to_inven_wrapper`；`inventory_hook_stage4.cpp:stage4_unequip_item_to_inven`；按钮 wrapper `extension_bag_handle_original_bag_unequip`。
- **共享状态读写**：读原版/扩展容量与目标 bag；成功写物理入库或扩展 adopt、ownership 和 projection；失败不清角色装备。
- **必须保持的不变式(引 R-xx)**：先 backup，只有原版失败才扩展兜底；任务袋不作扩展目标；扩展对象不进 Direct remove（R-07、R-10、R-21、R-25）。
- **失败语义**：原版成功直接返回；原版失败且扩展无位返回 0/`no space`；扩展袋非空卸下返回 `not empty`。
- **Host 测试名(现有或「缺口」)**：`test_unequip_to_inven`；断言 backup 成功不 adopt，backup 失败按 adopt 成功/失败返回 1/0。
- **真机用例号(编号规范 VM-xx，写操作步骤+预期)**：`VM-08`：①卸下普通装备；②填满原版袋再卸下；③再填满扩展袋；预期按优先级入库，最终失败有明确提示且角色装备仍可恢复。
- **证据锚类型**：Host + 真机。

### VM-09 卸下扩展袋背包

- **操作**：卸下一个已装备的扩展背包对象，覆盖空袋和非空袋。
- **原版链(VMA)**：`CHAR_UnequipItemToInven@0xe2f68`/`UIEquip_ButtonUnequipExe@0xb7e14`；原版并不认识逻辑袋容量。
- **扩展接管点(文件:函数)**：`extension_bag_equip.inc:stage4_unequip_bag`、`extension_bag_runtime.inc:unequip_bag`；按钮由 `button_unequip_exe_wrapper` 分流。
- **共享状态读写**：读 bag `types/items/capacities/info` 和角色装备；成功写 bag type/capacity、角色装备槽、selected/inspected、ownership；非空拒绝不写。
- **必须保持的不变式(引 R-xx)**：扩展袋有物品不得解除；容量只能由 types normalize；详情和投影清理先于对象释放（R-05、R-15、R-19、R-30）。
- **失败语义**：非空返回 `not empty`；原版物理无空位返回 `no space`；任何失败保留原装备及逻辑 items。
- **Host 测试名(现有或「缺口」)**：`test_unequip_bag`；断言空袋可解除、非空拒绝、selected/info/mode 清理。
- **真机用例号(编号规范 VM-xx，写操作步骤+预期)**：`VM-09`：①装备空扩展袋后卸下；②向扩展袋放一物后再卸下；③重新打开面板；预期第一步成功，第二步拒绝，物品和投影无残留。
- **证据锚类型**：Host + 真机。

### VM-10 宝石镶嵌与强化卷轴强化

- **操作**：以扩展宝石或强化卷轴向同一扩展逻辑袋内的扩展装备执行镶嵌/强化。
- **原版链(VMA)**：`SAVE_IsOK@0x128c14` → `UIEquip_ApplyStuff@0xb8df8` →
  `ITEMSYSTEM_PutJewel@0x10bcb4` / `ITEMSYSTEM_EnchantItem@0x10b330`；成功后由原版链更新
  装备并删除/消费材料。
- **扩展接管点(文件:函数)**：`extension_bag_public_runtime.inc:apply_extension_material_to_slot_locked`
  经 `UIEquip_IsApplyStuff`/`UIEquip_ApplyStuff` 路由；装备槽拖放另经
  `equip_control_event_proc_wrapper`（`0xb8f7c`）；API 宝石路径仍为
  `game_inventory_equipment.inc:data_op_jewel` → `extension_bag_api_put_jewel_impl`。
- **共享状态读写**：读 apply 材料/装备两端身份、同袋关系、socket、等级和 payload；写装备
  payload/等级、宝石或卷轴 descriptor/count、object hash、handle、projection 和 dirty；
  原版 `ApplyStuff` 调用由 `ModuleUseToken` 关联消费。
- **必须保持的不变式(引 R-xx)**：源必须是扩展宝石或强化卷轴，且与扩展装备同袋；目标必须
  是装备类（`item_is_equip` 为真，数据不可用按非装备 fail-closed 回退交换，R-50）；
  扩展材料不走 `RemoveItemDirect`；成功沿原版 `ApplyStuff` 的 `PutJewel`/`EnchantItem`
  分支执行并由 `ConsumeItem` 恰好消费一次；已准入 apply 的执行失败返回 `Blocked`；
  普通物品目标不是 apply 失败，必须回退扩展交换/移动（R-09、R-15、R-16、R-25、R-31、
  R-50）。
- **失败语义**：无孔 `no socket`、非 apply 材料 `not applicable`、不可强化 `cannot enchant`、
  空装备 `equip slot empty`；失败不消费宝石/卷轴、不改变装备；普通物品目标不产生
  `apply reject`，走交换/移动事务。
- **Host 测试名(现有或「缺口」)**：`test_object_operations`；断言原版 backup 结果保留、扩展
  两端 seam 只调用一次；`test_inventory_hook_stage4` 还断言 apply 材料、目标装备槽 predicate
  与 finish 失败必须走 token abort seam，以及 R-50 准入判据
  `stage4_is_extension_apply_candidate`（宝石/卷轴+装备目标→apply；+普通物品目标→非 apply
  应 swap；普通物品源→非 apply）；缺口：`test_extension_jewel_atomic_consumption`。
- **真机用例号(编号规范 VM-xx，写操作步骤+预期)**：`VM-10`：①记录同袋扩展宝石/强化卷轴
  count 和扩展装备 socket/等级；②分别拖放宝石、强化卷轴并核对 payload/socket/等级；③对
  无孔、不可应用材料、不可强化装备和空装备目标各重试；④将宝石/卷轴拖到同袋普通物品
  目标；预期②成功路径分别执行一次 `PutJewel`/`EnchantItem` 并由 `ConsumeItem` 消费一次，
  ③失败返回 `Blocked`、保持两侧原状、不出现 `extension swap`，④普通物品目标走扩展交换
  （两侧位置互换或移动提交）、不出现 `apply branch`。
- **证据锚类型**：Host + 真机 + VMA。

### VM-11 投影拖动 press/move/release

- **操作**：拖动扩展投影物品，验证 `0x17` press、`0x19` move、`0x18` release 的状态机。
- **原版链(VMA)**：原版 `TouchHandle` 消费 `0x17/0x19/0x18`，release 可进入 `INVEN_MoveItem@0x104934`。
- **扩展接管点(文件:函数)**：`extension_bag_lifecycle.inc:virtual_bag_event`、`consume_projected_release_locked`；session helper `extension_bag_drag_session.inc`；物理入口守卫 `native_inventory_hook.cpp:move_item_wrapper`。
- **共享状态读写**：创建/推进/清理 `g_extension_drag_session`，读 `g_projected_item_root`、generation 和 moving control；release 写 transaction phase 和投影控件。
- **必须保持的不变式(引 R-xx)**：活动投影 session 的 `0x18` 是唯一提交者；失败也返回 1 吞原版；0x17/0x19 不提交；扩展对象到达 `INVEN_MoveItem` 时由 guard 拦截；`moveMergeEnabled=false` 时原版合并不走自定义合并分支；P5 观测开启时页签点击路径不得死锁（R-02、R-19、R-20、R-30、R-31）。
- **失败语义**：非法目标、过期 generation、重复 release 或取消进入 rejected/cancelled；不得交给原版二次移动。
- **Host 测试名(现有或「缺口」)**：`test_p52_drag_session`；断言合法状态转移、claim 只成功一次、stale/取消幂等、任务袋目标拒绝。
- **真机用例号(编号规范 VM-xx，写操作步骤+预期)**：`VM-11`：①在扩展物品上按下并移动；②放到空槽、非法区域、原版袋；③分别取消和重复松手；预期一次 release 一次提交，失败不移动真实原版槽。
- **证据锚类型**：Host + 真机 + `orig_event`/`MoveItem GUARD` 日志。

### VM-12 扩展双非空交换（问题 B 主卡）

- **操作**：扩展袋 0 的 a↔b 双非空交换，重点核对同号原版物理槽。
- **原版链(VMA)**：原版候选为 `INVEN_MoveItem@0x104934`；扩展投影 release 理论上应在原版入口前提交；若扩展对象仍到达该入口，必须由第 14 个 Native guard 拦截。
- **扩展接管点(文件:函数)**：`extension_bag_transaction.inc:swap_extension_slots_locked`；release 路由 `extension_bag_lifecycle.inc:consume_projected_release_locked`。
- **共享状态读写**：读两端 descriptor/object/category/hash/handle/generation/ownership/token；写双槽交换、pending、dirty、控件 `data[0]` 和 physical snapshot 诊断。
- **必须保持的不变式(引 R-xx)**：交换成组同步，stale endpoint 先拒绝；不触碰 `g_inven`；MoveItem guard 命中扩展身份后不调 backup；H3/H4 只作防御，不能把逻辑提交当物理安全（R-16、R-17、R-20、R-28、R-30、R-31）。
- **失败语义**：身份/token/目标不合法回滚双槽并吞 release；若物理 digest 变化，记录 mutation 并尝试恢复，最终仍按问题 B 未解决判定。
- **Host 测试名(现有或「缺口」)**：`test_p44_transaction_stages`、`test_virtual_bag_mergeable_items`；它们只能证明模型/阶段，不证明真机物理安全。
- **真机用例号(编号规范 VM-xx，写操作步骤+预期)**：`VM-12`：①在原版袋 0 的同号槽记录 b 物品 payload/指针摘要；②扩展袋 0 两物品双向交换；③读取原版袋、切视图、重启前后并保存；预期当前验收**允许复现失败**，必须保留完整 H3/H4 日志，不能写“已修复”。
- **证据锚类型**：真机阻断卡 + Host 模型 + H3/H4/MoveItem 日志；当前预期失败=已知未解决。

### VM-13 扩展同袋合并与跨袋不合并

- **操作**：同袋相同 payload 合并、不同 payload 不合并、跨扩展袋不合并。
- **原版链(VMA)**：`INVEN_MoveItem@0x104934` 的原版同类堆叠；扩展路径不调用其物理逻辑。
- **扩展接管点(文件:函数)**：`extension_bag_transaction.inc:move_extension_to_extension_locked`；`virtual_bag_transaction_rules.inc:mergeable_items`。
- **共享状态读写**：读两个 descriptor/payload/count、capacity 和 merge 开关；写目标 count/payload、源清空或交换、handles/hashes/generation、projection 和 pending。
- **必须保持的不变式(引 R-xx)**：同袋才合并、上限受 stack limit；merge 开关不能关闭 source protection；descriptor/object/hash 成组更新，合并比较归一化 payload；原版合并对象由 MoveItem backup 保持原版语义（R-16、R-22、R-28、R-29、R-31）。
- **失败语义**：超上限、不同 payload、跨袋或满目标不合并；按空槽/交换规则继续或回滚，不创建原版 journal。
- **Host 测试名(现有或「缺口」)**：`test_virtual_bag_merge_count`、`test_virtual_bag_mergeable_items`、`test_p44_transaction_stages`。
- **真机用例号(编号规范 VM-xx，写操作步骤+预期)**：`VM-13`：①同袋同 payload 移动；②同袋改 payload 后移动；③跨袋同 payload 移动；预期只有第一步合并，后两步保留独立物品，状态 JSON 的 pending 域不冒充 journal。
- **证据锚类型**：Host + API 真机；问题 B 影响双非空 swap 时仍须 VM-12。

### VM-14 扩展到原版移动

- **操作**：把扩展逻辑袋物品拖/移到原版普通袋。
- **原版链(VMA)**：目标写入 `INVEN_SaveItemOnEmpty@0x104be0`，底层 `INVEN_SaveItemDirect@0x103bf0`；不允许把扩展对象直接交给原版 drop 写链。
- **扩展接管点(文件:函数)**：`extension_bag_transaction.inc:move_extension_to_original_locked`；`extension_bag_render.inc:save_item_on_empty_gate` 延迟记录目标；release 路由同 VM-11。
- **共享状态读写**：读扩展 source payload/handle 和原版目标容量；写 PendingTransfer、临时物化对象、物理入库确认、handover、清扩展源和 projection。
- **必须保持的不变式(引 R-xx)**：目标只允许 `0..4`；先确认落位和 payload，再清扩展源；handover 后模块不再触碰对象（R-02、R-17、R-20、R-25、R-29）。
- **失败语义**：满包、任务袋、Load/SaveItemOnEmpty/落位复核失败保留扩展源并释放仍归模块对象；不得只信返回值。
- **Host 测试名(现有或「缺口」)**：`test_p44_transaction_stages`、`test_virtual_bag_recovery`、`test_virtual_bag_transaction_domain`；缺口：`test_extension_to_original_handover_after_slot_verify`。
- **真机用例号(编号规范 VM-xx，写操作步骤+预期)**：`VM-14`：①读取扩展 payload；②POST `/api/item/inventory/move_item` 以 bag 6..10 为源、原版 0..4 为目标；③读取目标和源；预期目标物品字节一致、源清空，失败时源和 payload 保留。
- **证据锚类型**：Host + API 真机 + 源码。

### VM-15 原版到扩展移动

- **操作**：把普通原版物品移动到已装备扩展逻辑袋的空槽。
- **原版链(VMA)**：原版源由 `INVEN_FindItemSlot@0x103704`/物理槽读取；原版删除原语 `INVEN_RemoveItemDirect@0x103fd8` 只可在确认扩展逻辑提交后受控使用。
- **扩展接管点(文件:函数)**：`extension_bag_transaction.inc:move_original_to_extension_locked`；标签 drop `extension_bag_runtime.inc:extension_tab_item_proc`。
- **共享状态读写**：读物理 source、目标容量和 payload；写 PendingTransfer、扩展 descriptor/object/handle、确认物理源为空、projection 和 dirty。
- **必须保持的不变式(引 R-xx)**：任务袋 5 为源时拒绝；物理源清除必须删后复核；逻辑 bag 不能传进物理函数；持锁原版删除/刷新只能经 R-44 受控 dispatcher，且不得触发同线程重入死锁（R-02、R-18、R-21、R-22、R-25、R-44）。
- **失败语义**：目标非法/满、物化或物理删除复核失败回滚逻辑目标并保留原版源；原版源仍占用时不提交。
- **Host 测试名(现有或「缺口」)**：`test_p44_transaction_stages`、`test_virtual_bag_transaction_domain`（含 `original_to_extension_source_slot_postcondition`）；`test_original_only_queries`。
- **真机用例号(编号规范 VM-xx，写操作步骤+预期)**：`VM-15`：①记录原版 bag/slot payload；②真实拖拽路径：把原版袋物品拖到已装备扩展袋页签后释放（`0x18` release 走 orig→ext 事务）；③重读原版源槽与扩展目标槽，预期源槽真实空（后置条件成立）、扩展 payload 完整；④卡死检查：拖拽释放后游戏画面与输入持续响应，`/api/health` 连续轮询可达，无主线程冻结；⑤API 对照：POST `move_item` 到 bag 6..10 同断言，bag 5 源返回 `task bag excluded`。
- **日志锚**：`txn committed id=... original->extension bag=.. slot=.. src=../..`；`release_cleanup ctl=.. flags_cleared`；持锁原版调用期间出现 `original-only native_call=1`；预期不存在 `event_release` 记录后的处理停滞（release 后事务在同一事件循环内完成）。
- **证据锚类型**：Host + 真机（拖拽触摸路径 + API 对照）+ 日志锚；无卡死以 health 连续可达与画面响应为证，不以 API 成功替代。

### VM-16 装备页详情销毁

- **操作**：从装备页扩展详情打开销毁确认，确认或取消。
- **原版链(VMA)**：`UIEquip_ButtonDestroyExe@0xb6240` → popup `OKDestroyItem@0xb83d0` → 原版物理删除。
- **扩展接管点(文件:函数)**：`extension_bag_equip.inc:extension_destroy_ok`；详情按钮数组 PtrHook 安装/观察；装备页 popup OK/Cancel GOT 临时槽。
- **共享状态读写**：读详情 bag/slot/object/hash/handle；确认写逻辑清槽、释放、projection、dirty；成功/失败/取消恢复 popup 原 callback 和详情身份。
- **必须保持的不变式(引 R-xx)**：扩展对象不进原版 Direct remove；全局 popup 劫持只用于卖出/销毁，回调不可跨页串线（R-07、R-13、R-15、R-30）。
- **失败语义**：取消无状态变化；stale identity/释放失败拒绝；确认成功才清逻辑源和物化对象。
- **Host 测试名(现有或「缺口」)**：缺口：`test_extension_destroy_identity_and_popup_restore`（断言取消/成功/失效身份的槽、handle、callback 清理）。
- **真机用例号(编号规范 VM-xx，写操作步骤+预期)**：`VM-16`：①打开扩展物品详情；②取消确认；③再次打开并确认销毁；④切换到商店页；预期取消不变，确认后物品消失且其他页面 popup 不串线。
- **证据锚类型**：真机 + 源码；Host 为待补 seam。

### VM-17 装备页详情卖出

- **操作**：从装备页扩展详情出售物品。
- **原版链(VMA)**：详情按钮 → popup → `ITEM_GetSellPrice@0x10a500` → 物理删除/加钱。
- **扩展接管点(文件:函数)**：`extension_bag_equip.inc:extension_sell_ok`；装备页详情 PtrHook 和 popup callback。
- **共享状态读写**：读详情身份、price variant、count 和 money；先写 money，再逻辑删除，失败退款；更新 projection/dirty 并恢复 popup 状态。
- **必须保持的不变式(引 R-xx)**：装备页 variant 与商店 variant 分离；删失败退款，不能先清源；价格不能被另一宿主覆盖（R-13、R-15、R-23、R-25）。
- **失败语义**：无售、价格无效、stale identity 或删除失败拒绝；删除失败必须减回已加金币，退款失败显式报告。
- **Host 测试名(现有或「缺口」)**：缺口：`test_sell_price_variants_and_refund`（断言两 variant 独立、删除失败 money 回滚、源仍在）。
- **真机用例号(编号规范 VM-xx，写操作步骤+预期)**：`VM-17`：①记 money/物品；②打开装备页详情并确认卖出；③重复同身份卖出；预期只成交一次，价格正确，stale 第二次失败，popup 恢复。
- **证据锚类型**：真机 + 源码；价格纯函数可补 Host。

### VM-18 商店扩展物品卖出

- **操作**：在商店页扩展投影中出售物品。
- **原版链(VMA)**：商店 `UIStore` 详情/确认 → `UIStore_SellItem@0xd25f0` 及原版刷新链；原版商店 callback 与装备页不同。
- **扩展接管点(文件:函数)**：`extension_bag_store.inc:store_desc_sell_execute`、`store_sell_ok`、`store_refresh_projection_locked`。
- **共享状态读写**：读 `store_desc_*`、扩展 object/price variant 和 money；写商店独立 callback、逻辑删除/退款、商店 projection，不写装备页 desc。
- **必须保持的不变式(引 R-xx)**：只刷新商店投影，不调 `UIStore_RefreshInvenItem` 覆盖整列；商店/装备 popup state 分离（R-12、R-13、R-23、R-30）。
- **失败语义**：价格/身份/删除失败不成交并减回金币；商店刷新失败不覆盖原版或其他扩展项。
- **Host 测试名(现有或「缺口」)**：缺口：`test_store_sell_projection_and_callback_isolation`（断言商店刷新只改 projection、装备 callback 不被覆盖、失败退款）。
- **真机用例号(编号规范 VM-xx，写操作步骤+预期)**：`VM-18`：①进入商店扩展投影；②出售一个扩展物品；③观察剩余列和装备页详情；预期剩余商店内容不被原版整列覆盖，装备页 popup 仍可用。
- **证据锚类型**：真机 + 源码。

### VM-30 出售价格边界与原版单位价对照

- **操作**：覆盖装备页卖出、装备页销毁/粉碎、商店卖出和库存 API 卖出，核对各类别、
  堆叠数量、异常数量及原版 `ITEM_GetSellPrice` 单位价。
- **步骤**：①分别准备普通类别、带特殊 ability/payload 分支的类别和不可出售类别，记录
  原版单位价、扩展 descriptor payload/hash、generation；②在 stack limit 关闭时测试
  `count=1/99/100`，开启时测试 `count=999/1000`；③分别在关闭/开启两态下准备装备与损坏
  装备，执行装备/损坏判定、存档查找和非空槽检查，核对 99/999 上限语义；④载入
  `count=1000` 及更大异常值的 JSON，再从装备页、商店页和 API 取价；⑤以启用态保存
  `count=199` 的 state JSON，在关闭态读档，再以关闭态保存同一数量并在启用态读档；⑥对
  单位价或最终价超过 `INT32_MAX` 的边界注入记录拒绝日志。
- **预期**：单位价与原版 `ITEM_GetSellPrice` 一致；数量按当前
  `stack_codec` 上限收敛为 99/999；装备页卖出为 `100%`、销毁/粉碎为 `70%`，商店和
  API 为 `100%`。非法单位价/最终价只出现 `sell price reject reason=...`，不创建弹窗、
  不把未校验值写入弹窗、不加钱、不删除物品；异常 JSON 不得保留超上限 descriptor/payload
  count。双态（99/999）下装备/损坏判定、存档查找与空槽检查对同一数量边界保持一致。
  状态 JSON 载入时，canonical descriptor count 只按绝对上限 999 收敛，不按当前配置
  截断；启用态→关闭态及关闭态→启用态读档的 `count=199` 均保持 199，且 payload 数量
  位保持/同步为 199。非堆叠装备 payload 与输入逐字节一致；可堆叠 payload 仅在与
  descriptor 不一致时同步，绝对超限 descriptor 收敛为 999 并记录日志。启用上限后装备
  显示必须保持已鉴定，装备详情内容正常，不得被判为未鉴定或损坏。
- **日志锚**：
  `sell price source=equip|store category=.. count=.. unit=.. variant=.. final=.. payload=.. gen=..`、
  `sell price reject reason=..`；API 路径保留 `source=api` 同一字段语义。
- **Host 测试名**：`test_sell_price_bounds`、`test_virtual_bag_json_count_clamp`（含装备
  payload 逐字节不变、绝对超限收敛、双向跨配置读档不变和 descriptor/payload 同步）；
  `test_stack_codec` 继续覆盖 99/999 上限。
- **关联规则/真机卡**：R-23、R-37、R-38、R-43；主卡 VM-17、VM-18，API 对照追加 VM-26。
- **证据锚类型**：Host 纯函数 + 真机价格/弹窗/物品状态日志；不能由 API 成功响应替代
  弹窗未写入和物品未删除证据。

### VM-19 商店购买收编与堆叠数量布局

- **操作**：购买 1 个可堆叠商店物品并核对数量位段；同时覆盖原版物理袋满、扩展有空槽、原版有槽、全满和失败退款。
- **原版链(VMA)**：`UIStore_BuyItem@0xd242c` → 两处 `FindSaveSlot` callsite `0xd24a0/0xd2540` → `INVEN_SaveItem@0x104528`。
- **扩展接管点(文件:函数)**：`extension_bag_store.inc:store_buy_find_slot_gate`；`native_inventory_hook.cpp:save_item_wrapper` 在 backup 失败后 `extension_bag_adopt_native_item`。
- **共享状态读写**：读原版容量、扩展容量、商品和 money；写扩展 adopt、商品/货架状态、money、projection 和保存 dirty；商店投影写前恢复原版容量。
- **必须保持的不变式(引 R-xx)**：gate 只表达继续流程，不伪造扩展槽；SaveItem backup 前恢复容量；原版成功优先（R-05、R-11、R-26、R-27）。
- **失败语义**：物理或扩展均满返回原版满包；保存/扣款失败不留下扩展对象，退款失败显式错误；任务商品仍按任务语义。
- **Host 测试名(现有或「缺口」)**：缺口：`test_save_item_adopt_after_original_failure`、`test_store_buy_capacity_restore`（断言 backup 顺序、adopt 只发生于失败、容量恢复成对）。
- **真机用例号(编号规范 VM-xx，写操作步骤+预期)**：`VM-19`：①选择可堆叠商品，购买 1 个前记录物品 `count` 与原始 `+0x10`；②POST `/api/item/shop/buy_item`；③读取购买物品 `count`、`+0x10` 和 `ITEM_GetCumulateCount`；④再测物理满/扩展有空位及全满。预期购买 1 个后 `count=1`，`+0x10` 的 bit22..31 为 `1`（即 `1<<22`，其余位保持），getter 返回 `1`，不得出现 `9`；第一种收编成功，第二种统一失败且无扣款/残留。
- **证据锚类型**：真机阻断边界 + 源码；SaveItem 全 caller 覆盖未被此卡证明。

### VM-20 拾取与奖励入库

- **操作**：拾取普通掉落、领取普通奖励和领取任务物品。
- **原版链(VMA)**：`CHAR_PickItemAll@0xec4d8` → `NOTIFIER_Add@0x11e1b8` → `CHAR_ActivePickupEvent` → `INVEN_SaveItem@0x104528`；任务奖励 caller 另有分支。
- **扩展接管点(文件:函数)**：`api/native/game_world_movement.inc:nav_pick_items`；`native_inventory_hook.cpp:save_item_wrapper`；任务语义由 caller 保留。
- **共享状态读写**：读物理/扩展容量和掉落对象；SaveItem backup 成功时只保留原版所有权，失败时 adopt；写扩展 descriptor/object/handle、通知和 dirty。
- **必须保持的不变式(引 R-xx)**：原版优先；任务袋 5 不被普通 fallback 吸收；SaveItem wrapper 不宣称所有 caller 完整覆盖（R-05、R-21、R-22、R-25）。
- **失败语义**：满包按原版/扩展空位顺序处理；任务对象分类失败不得静默改为普通物品；对象释放归 caller/原版边界。
- **Host 测试名(现有或「缺口」)**：缺口：`test_producer_save_item_original_first_and_adopt`（断言成功透传、失败 adopt、任务类别不进入扩展）。
- **真机用例号(编号规范 VM-xx，写操作步骤+预期)**：`VM-20`：①记录普通掉落/奖励和任务物品；②执行拾取/领取；③检查 bag 0..5、6..10 和日志；预期普通物品按容量收编，任务物品保持任务语义，失败有明确日志。
- **证据锚类型**：真机必需；当前预期是阶段 5 未闭合，不得以 SaveItem Hook 静态存在代替。

### VM-21 开箱、解封和拆包产物

- **操作**：使用开箱、解封、拆包物品，覆盖多产物和部分失败。
- **原版链(VMA)**：`CHAR_UseItemEx@0xeb670` → `ITEMSYSTEM_OpenItemBox@0x10e970`/`ITEMSYSTEM_ReleaseSealed@0x10af4c`/`ITEMSYSTEM_ProcessUnpack@0x10ce50` → 多次 `INVEN_SaveItem@0x104528`。
- **扩展接管点(文件:函数)**：`game_inventory_use.inc:data_op_use_item`；不 Hook 三个产物函数，仍由 `save_item_wrapper` 在单件漏斗边界接管。
- **共享状态读写**：读输入 payload/count 和容量；写产物入库/扩展 adopt、输入消费、pending/dirty；多产物回滚状态必须逐 caller 记录。
- **必须保持的不变式(引 R-xx)**：不以 SaveItem wrapper 宣称多产物原子回滚；输入和产物对象不能被双重接管；原版效果优先（R-07、R-15、R-22、R-25）。
- **失败语义**：不可用/无槽返回错误；部分产物成功的最终状态必须如实记录，不能臆测全回滚；坏 payload 隔离而不进对象池。
- **Host 测试名(现有或「缺口」)**：`test_virtual_bag_payload_bridge`、`test_p45_isolation` 只覆盖 payload/隔离；缺口：`test_unpack_partial_product_ownership`。
- **真机用例号(编号规范 VM-xx，写操作步骤+预期)**：`VM-21`：①分别准备开箱/解封/拆包输入；②执行对应使用；③记录每个产物、输入 count、错误和日志；预期每个 caller 独立有证据，部分失败不写成整体通过。
- **证据锚类型**：真机必需 + payload Host；当前生产者覆盖未闭合。

### VM-22 合成产物和材料回滚

- **操作**：执行普通合成和失败注入/失败条件，核对材料、产物和对象释放。
- **原版链(VMA)**：`MIXSYSTEM_MakeItem@0x11af58`/`UseStuff@0x11b300`，不同 caller 可能原地改装备或生成后保存；产物常经 `INVEN_SaveItem`。
- **扩展接管点(文件:函数)**：材料扣减 `native_inventory_hook.cpp:remove_item_data_wrapper`（R-56：物理实扣不足时按 category 经 `extension_bag_port.cpp:extension_bag_consume_category` 从扩展袋补扣）；产物入库 `native_inventory_hook.cpp:save_item_wrapper`（H-13 合并/adopt）；`feature/patch/game_patch_craft.inc` 的上层旁路仅覆盖自定义批量宝石入口（当前死代码）。
- **共享状态读写**：读材料对象/category/count、产物临时对象和物理/扩展容量；写材料删除、产物 adopt、ledger 和 dirty；禁止把类别回滚当 UID 回滚。
- **必须保持的不变式(引 R-xx)**：不做全局 MIX Hook；材料删除前后校验，产物失败释放；扩展补扣量 = `max(0, count − 物理实扣)`、绝不重复扣（R-56）；无安全逆操作不添加猜测性回滚（R-07、R-15、R-22、R-25）。
- **失败语义**：材料不足/产物保存失败/扣款失败返回明确错误；已成功前序产物是否回滚必须按 caller 记录，不自动推导。
- **Host 测试名(现有或「缺口」)**：`test_remove_data_extension_shortfall`（R-56 补扣量：足额/不足/全扩展/`requested<=0`/数据反向）；缺口：`test_craft_product_failure_ownership`（断言未入库产物释放、材料源校验、失败不伪造扩展提交）。
- **真机用例号(编号规范 VM-xx，写操作步骤+预期)**：`VM-22`：①记录材料 payload/count；②执行成功合成；③执行满包/失败合成；④检查材料、产物和日志；预期成功/失败所有权均可追溯，不能以一次成功覆盖五个 caller。
- **证据锚类型**：真机必需；阶段 5/P6 边界未闭合。

### VM-23 视图切换、页签和投影

- **操作**：进入、切换、退出扩展逻辑袋，覆盖重复点击、快速切换和退出中事件。
- **原版链(VMA)**：原版 inventory event、袋控件和 `TouchHandle`；投影安装不等同于原版背包写入。
- **扩展接管点(文件:函数)**：`extension_bag_runtime.inc:extension_tab_item_proc`、`install_extension_tab_buttons_locked`、`disable_extension_tab_buttons_locked`；`extension_bag_render.inc:install_module_view_locked/restore_module_view_locked`。
- **共享状态读写**：读写 `mode/selected/inspected`、`g_module_view_index`、`g_module_window_original_bag`、`g_projected_item_root`、tab pointers/generation 和 capacity snapshot。
- **必须保持的不变式(引 R-xx)**：view index 与窗口原版袋号分离；direct/GOT 成对恢复；root 重建先使失效事件失效（R-19、R-26、R-27、R-30）；扩展视图激活时原版袋列高亮被遮蔽（R-57）。
- **失败语义**：未就绪容器、无容量、退出中或 stale root 拒绝；原版袋 5 不可成为投影窗口；恢复失败不得释放借用对象。
- **Host 测试名(现有或「缺口」)**：`test_virtual_bag_state`、`test_extension_bag_exit_rendering_state`、`test_p52_drag_session`；缺口：`test_view_window_bag_pair_restore`。
- **真机用例号(编号规范 VM-xx，写操作步骤+预期)**：`VM-23`：①进入 bag 6/7/8；②快速来回切换；③退出到原版袋；④重开；预期只显示当前逻辑袋，退出后原版容量/direct/GOT/root 恢复，失效控件事件无效；⑤扩展视图激活时原版 6 袋全部取消高亮、扩展页签高亮（R-57）。
- **证据锚类型**：Host + 真机 + 源码。

### VM-24 详情弹窗与身份失效

- **操作**：打开原版/扩展物品详情，验证使用、装备、卖出、销毁按钮分流和 stale identity。
- **原版链(VMA)**：`UIEquip_MakeDesc@0xb8980`（唯一调用点 `0xb9188`）→ `SetDescMenu`→详情按钮 proc；装备/卸下 proc 走函数级 Hook。
- **扩展接管点(文件:函数)**：`extension_bag_lifecycle.inc:make_desc_equip_gate`；`extension_bag_equip.inc` 详情按钮 PtrHook 安装与 `extension_*_ok` 分流。
- **共享状态读写**：读当前 control/root、item/bag/slot/cache；写 `g_extension_desc_item`、details generation、button hooks、popup callback；重建时清理失效指针。
- **必须保持的不变式(引 R-xx)**：操作前再次校验 item+bag+slot+cache；详情 PtrHook 跳过装备/卸下 proc；root 重建清 stale（R-14、R-15、R-24、R-30）。
- **失败语义**：详情生成失败/身份失效/按钮数组 stale 只清理本次身份，不执行原版删源或错误角色操作。
- **Host 测试名(现有或「缺口」)**：缺口：`test_detail_identity_and_proc_exclusion`（断言 stale identity 被拒、装备/卸下不双层进入、hook 清理完整）。
- **真机用例号(编号规范 VM-xx，写操作步骤+预期)**：`VM-24`：①打开扩展详情；②切袋/重建面板后继续点失效详情按钮；③分别操作原版与扩展详情；预期 stale 操作无副作用，扩展按钮只处理扩展对象。
- **证据锚类型**：真机 + 源码；PtrHook 双层行为需运行时证据。

### VM-25 保存、切档、重启和 sidecar

- **操作**：完成扩展变更后显式保存，覆盖原版保存失败、切档、回主菜单、重启和 pending 恢复。
- **原版链(VMA)**：8 个 `SAVE_Save` callsite patch → `SAVE_Save@0x129600` → `SAVE_SaveInventory@0x127d8c`；内部 callsite `0x129770` 经 `save_inventory_wrapper`。
- **扩展接管点(文件:函数)**：`extension_bag_lifecycle.inc:patch_all_save_callsites/save_inventory_wrapper`；`core/native/module_save.cpp:module_save_game`；`extension_bag_persistence.cpp` participant。
- **共享状态读写**：prepare 读 state/pending/ownership 并写 `module.save.journal`；原版保存读真实 `g_inven`；commit 写 `extensionbags.items`，清 pending/dirty；load 重建 descriptor/object。
- **必须保持的不变式(引 R-xx)**：先恢复投影和容量再保存；原版成功后才 sidecar commit；两个 journal 分域；native 指针不落盘（R-11、R-18、R-20、R-25、R-29）。
- **失败语义**：prepare 失败逆序 abort；`SAVE_Save==0` 不 commit；结果未知保留 journal/待恢复；仅残留 `module.save.journal` 时当前没有 coordinator 自动恢复，必须按存档册 §6.3 取证；坏 sidecar quarantine/last-good，原版存档不被污染。
- **Host 测试名(现有或「缺口」)**：`test_prepare_journal`、`test_p44_transaction_stages`、`test_p45_isolation`、`test_virtual_bag_recovery`；缺口：`test_module_save_prepare_commit_boundary`。
- **真机用例号(编号规范 VM-xx，写操作步骤+预期)**：`VM-25`：①改变扩展物品；②POST `/api/system/save`；③force-stop/restart；④处理 agreement 后 enter_slot 并读 inventory；⑤另测未保存变更和切档；预期只把完整保存成功内容恢复，pending/坏 payload 按 rollback 或 isolation 记录。
- **证据锚类型**：Host + 真机 + sidecar/日志；问题 B 保存固化仍不能判修复。

### VM-26 API 统一移动/使用/装备边界

- **操作**：使用正式库存 API 驱动原版、扩展和跨域操作，核对路由、状态快照和错误 envelope。
- **原版链(VMA)**：API controller → service → `NativeBridge` → `data_op_*`；原版移动最终 `INVEN_MoveItem@0x104934`，使用 `CHAR_UseItemEx@0xeb670`，装备 `CHAR_EquipItemFromInvenToSlot@0xe5368`。
- **扩展接管点(文件:函数)**：`game_inventory_use.inc`、`game_inventory_equipment.inc`、`extension_bag_api_impl.inc`；Controller 只做路由/参数解析。
- **共享状态读写**：读统一 `inventory_item_ref_at`；跨域写三方向事务、descriptor/object/handle/generation、projection 和 unsaved journal/pending。
- **必须保持的不变式(引 R-xx)**：API 原版袋走原版、含 6..10 才进扩展 move；袋 5 排除；Controller 不直接碰 NativeBridge（R-02、R-21、R-22、R-25）。
- **失败语义**：参数错 400、权限 403、找不到 404、内部 500、未实现 501、未就绪 503；业务错误统一 `ok=false,error`，不返回半成品 state。
- **Host 测试名(现有或「缺口」)**：`test_virtual_bag_transaction_domain`、`test_virtual_bag_state`；缺口：`test_inventory_api_domain_and_error_envelope`。
- **真机用例号(编号规范 VM-xx，写操作步骤+预期)**：`VM-26`：①GET inventory/items；②分别 POST use/move/equip/put_jewel；③传 bag 5、越界 slot、count≤0、扩展开关关闭；预期路径和错误码与 API 参考一致，state 为操作后快照。
- **证据锚类型**：API 真机 + Host domain + API 文档。

### VM-27 `INVEN_MoveItem` guard 与原版合并回归

- **操作**：分别让扩展身份对象和原版对象到达 `INVEN_MoveItem@0x104934`，并覆盖活动扩展 view/session 与普通原版路径。
- **设备/APK/快照前置（按 VM-12 SOP）**：记录真机 `192.168.3.54:5555`、最新 debug APK SHA-256、`extensionBagEnabled`、`moveMergeEnabled` 与 source protection 最终值；在操作前记录原版袋 `0..5×16` 物理快照 digest、nonnull 和目标槽摘要。
- **原版链(VMA)**：`INVEN_MoveItem@0x104934`；模块直调点为 `game_patch_move_merge.inc:74` 和 `game_inventory_use.inc:210`。
- **扩展接管点(文件:函数)**：`native_inventory_hook.cpp:move_item_wrapper`、`equip_control_event_proc_wrapper`；身份/物理摘要 helper 为 `virtual_bag_capture_move_item_observation`。
- **共享状态读写**：锁内读取 `g_module_objects`/ownership、`g_module_view_index`、drag session 和物理 `6×16` 摘要；锁外调用 backup，记录 caller、参数及 pre/post。
- **必须保持的不变式(引 R-xx)**：扩展身份命中输出 `ERROR MoveItem GUARD reject ... backup=skipped` 并返回 `0`；原版对象一律 backup；不得持 `g_virtual_bag_mtx` 调 backup；`moveMergeEnabled=false` 时原版合并不走自定义合并分支（R-20、R-31）。同时断言 C1 事件层输出扩展 source 拒绝日志且不进入原版 proc，C2 guard 拒绝后 merge wrapper 吞事件且不二次调用原版 proc，C3 `item==nullptr` 时 source 取证保持 `unknown`（`source=-1/-1`）而不误记首个空槽。
- **失败语义**：扩展源永不进入原版物理移动；原版源返回值、堆叠和失败语义不改变；guard 本身不宣称问题 B 已修复。
- **Host 测试名(现有或「缺口」)**：`test_inventory_hook_stage4` 覆盖
  `stage4_is_extension_equip_control_source` 的 event/source/jewel/目标装备槽四元判据，
  以及 `stage4_finish_requires_abort(false/true)` 的 token abort 出口；Native/LSPosed
  wrapper 的锁、控件重绑和真实 token 仍依赖 Android 运行时，不能在 Host 直接 mock；
  既有 `test_virtual_bag_mergeable_items` 与 `test_p44_transaction_stages` 保持通过。
- **真机用例号(编号规范 VM-xx，写操作步骤+预期)**：`VM-27`：①复现扩展源进入 MoveItem，预期 ERROR、return 0、无 backup；②执行原版同类合并，预期 `pre/post` INFO（活动 view/session）或 passthrough INFO，结果与无 Hook 基线一致；③核对 caller `libgame+0x...` 和源/目标摘要。
- **证据锚类型**：真机日志 + APK；Host 仅保留既有模型回归。

### VM-32 S2 存档数量一致性（无版本标识、统一 S2、宽容忽略旧字段）

- **操作**：验证 sidecar 与原版袋 `+0x10` 数量在保存/读档/跨重启下按统一 S2 语义保持不变；携带历史 `encodingVersion` 字段的 sidecar 被宽容忽略且数值不变；非可堆叠类别逐字节不变。
- **设备/APK/快照前置（按 VM-12 SOP）**：记录真机 `192.168.3.54:5555`、最新 debug APK SHA-256、`extensionBagEnabled`；操作前记录原版袋 `0..5×16` 物理快照 digest、nonnull 与可堆叠槽数量字摘要，并导出 sidecar JSON 副本（可含旧 `encodingVersion` 字段）。
- **原版链(VMA)**：`ITEM_GetCumulateCount@0x106094`（读侧解码，S2-P2）、`SAVE_SaveInventory@0x127d8c`。
- **扩展接管点(文件:函数)**：`virtual_bag_state_json.inc:state_json/parse_state_json`（无版本字段写出、旧字段宽容忽略）；`extension_bag_persistence.cpp:extension_bag_load_state_from_store`。
- **共享状态读写**：load 只解析数量数据面（descriptor canonical 与 payload 数量位），无版本分支；payload 与 canonical 不一致时以 S2 重编码对齐。
- **必须保持的不变式(引 R-xx)**：无版本标识、数量位只有 S2 布局（R-48）；非可堆叠（装备/宝石/袋对象）bits22–24 逐字节不变（R-46）；读档不 clamp、不按配置重编码（R-48 载入侧约束）；sidecar 解析失败不写回调用方，保留文件级 last-good。
- **失败语义**：sidecar JSON 结构损坏走既有隔离/last-good 链路；旧 `encodingVersion` 字段（任意值）不触发拒绝、不触发迁移；被历史版本写坏的存量（如 count=705）读档原样保留，恢复需经操作层写点或手动修 sidecar。
- **Host 测试名(现有或「缺口」)**：`test_stack_codec_mode_invariance`（99/127/128/217/999 两态写入/解码一致、切换模式不改值）、`test_stack_codec_s2`（S2 编解码往返/位保留）、`test_virtual_bag_json_count_clamp`（跨配置 canonical 不截断）、`test_virtual_bag_cross_config_roundtrip`（跨配置三往返 + 旧 `encodingVersion` 字段宽容忽略）；读侧 getter 依赖游戏内存，Host 缺口。
- **真机用例号(编号规范 VM-xx，写操作步骤+预期)**：`VM-32`：①启用态把可堆叠物品设为 217 并保存、force-stop、重启、进档；②读 inventory 可堆叠数量与装备/宝石/袋容量，预期 217 保持、非可堆叠逐字节不变；③用手动注入 `encodingVersion` 字段（1/2/99 任一）的 sidecar 副本进档，预期解析成功、数量不变、日志无 `unknown_version` 拒绝；④保存后核对 sidecar JSON 不含 `encodingVersion` 字段。
- **证据锚类型**：Host + 真机 + sidecar/日志；读侧 getter（S2-P2）已落地，写侧门控（S2-P3）未取证前不得判 S2 整体通过。

### VM-35 读档跨配置往返（canonical 不改写、模式不变性）

- **操作**：启用态保存含可堆叠数量（重点 217，另覆盖 99/127/128/199/999）的存档 → 关闭堆叠上限 → 读档 → 核对数量与 payload 不变；再启用 → 再读档核对；覆盖切换模式不改变值与 sidecar 缺失行为。
- **设备/APK/快照前置（按 VM-12 SOP）**：记录真机 `192.168.3.54:5555`、最新 debug APK SHA-256；操作前导出 sidecar JSON 副本、记录目标物品 `+0x10` 原始值与 UI 显示数。
- **原版链(VMA)**：`ITEM_GetCumulateCount@0x106094`（读侧解码）、`SAVE_SaveInventory@0x127d8c`。
- **扩展接管点(文件:函数)**：`extension_bag_runtime.inc:ensure_state_loaded_locked`（无迁移门控，直接载入）、`virtual_bag_state_json.inc:parse_state_json`（统一 S2 解码与 canonical 对齐）、`extension_bag_persistence.cpp:extension_bag_load_state_from_store/extension_bag_save_state_to_store`。
- **共享状态读写**：读档仅载入 sidecar canonical；无版本状态机、无物理袋扫描。
- **必须保持的不变式(引 R-xx)**：解码永远按 S2、与 `stack_limit_enabled()` 无关（R-48）；读档不 clamp、不按当前配置重编码，已持久化 canonical 原样保留（R-48 载入侧约束）；启用/关闭切换不改写既有持久化数值（R-47/决策 b：关闭态只影响运行时视图与操作）；`stack_limit_enabled()` 只经 `effective_clamp`/`effective_view_count` 影响新建/合并/消费/派生的 99/999 上限与运行时视图。
- **失败语义**：sidecar 缺失/解析失败使用空状态并记日志，无任何字段重写路径；被历史版本写坏的存量（如 count=705）不会被自动改回——canonical 语义下读档保持原值，恢复需经操作层写点或手动修 sidecar。
- **Host 测试名(现有或「缺口」)**：`test_virtual_bag_cross_config_roundtrip`（99/127/128/217/999 三往返 payload 逐字节不变 + 旧字段宽容忽略）、`test_stack_codec_mode_invariance`（两态写入/解码一致、切换模式不改值）、`test_virtual_bag_json_count_clamp`（跨配置 canonical 不截断）；物理袋读侧依赖游戏内存与 Bridge 存储，Host 缺口。
- **真机用例号(编号规范 VM-xx，写操作步骤+预期)**：`VM-35`：①启用态把可堆叠物品设为 217（API 或游戏内获取）并保存；②关闭堆叠上限，读档，预期 UI/API count 仍 217、`+0x10` 位段与保存时逐字节一致；③再启用，读档，预期仍 217；④对 99/127/128/199/999 重复①②③；⑤改无 sidecar 的存档槽进档，预期空状态载入且原版袋 `+0x10` 不变。

### VM-36 关闭态低 7 位视图与 a 保留（199→71、重开 199、off 上限 99）

- **操作**：启用态构造 canonical 数量（重点 199）的可堆叠物品 → 关闭堆叠上限 → 核对所有读写/操作按低 7 位视图（读 71、上限 99、`a` 位段不变）→ 关闭态执行消耗/合并 → 重新启用 → 核对重开读回完整 canonical 值。
- **设备/APK/快照前置（按 VM-12 SOP）**：记录真机 `192.168.3.54:5555`、最新 debug APK SHA-256、`extensionBagEnabled`、`stackLimitEnabled`；操作前记录目标物品 `+0x10` 原始值与 sidecar JSON 副本。
- **原版链(VMA)**：`ITEM_GetCumulateCount@0x106094`（读侧模式视图解码）、`INVEN_ConsumeItem@0x1047bc+0x104818/0x104844/0x104858`（b 递减）、`INVEN_MoveItem@0x104934`（合并写 b）。
- **扩展接管点(文件:函数)**：`inventory_hook_stage4.cpp:stage4_get_cumulate_count`（`effective_read_count` 模式视图）、`core/native/stack_codec.h:effective_read_count/effective_write_count/effective_clamp/effective_view_count`、`native_inventory_hook.cpp:consume_item_wrapper`（借位预置仅启用态）、`extension_bag_transaction.inc:move_extension_to_extension_locked`（合并按视图、descriptor 回写 canonical）、`extension_bag_equip.inc:consume_extension_item_after_native_locked`。
- **共享状态读写**：关闭态模块侧写点只改 payload b 位；descriptor `count` 与 sidecar 恒为 canonical；读档路径不参与本卡（见 VM-35）。
- **必须保持的不变式(引 R-xx)**：关闭态有效数量 = `b` = `count mod 128`、上限 99（R-47/决策 b）；关闭态任何写点不得改写 `a`（bits22–24）与 bits0–21（R-45/R-47）；重新开启后经 S2 解码读回完整 canonical（R-45/R-47）；canonical 199 关闭态读 71、关闭态消耗 1 后重开读 198、关闭态写 99 后重开读 227（128a+99）；非可堆叠类别位段不受影响（R-46）。
- **失败语义**：关闭态 b≤1 的堆消耗到空按原版删除（有效数量 0，不预置借位）；关闭态合并超 99 按上限拒绝/分堆，不得出现视图 >99；模式切换瞬间不得出现反向同步或 `a` 清零。
- **Host 测试名(现有或「缺口」)**：`test_stack_codec_effective_mode`（199 off 读 71/重开 199、off 写 99 → 227、off 消耗 → 198、127/128/99 off 视图、clamp 999/99、`effective_view_count`）、`test_virtual_bag_mode_aware_ops`（`patch_payload_count` off 写保留 a、off 合并视图 71+71→99 → canonical 227）、`test_get_cumulate_count_s2`（getter 分流两态视图）、`test_virtual_bag_merge_count`/`test_virtual_bag_cross_config_roundtrip`（合并上限与持久化 canonical 不改写）；原版袋写侧行为依赖游戏内存，Host 缺口以真机为准。
- **真机用例号(编号规范 VM-xx，写操作步骤+预期)**：`VM-36`：①启用态经 API 创建 canonical `199` 的可堆叠物品，读 `+0x10` 预期 `a=1,b=71`、UI/API count=199；②关闭堆叠上限，读 GET `/api/item/inventory/items`，预期同一物品 `count=71` 且 `+0x10` 逐字节不变；③关闭态经 API 创建/合并使请求量 100，预期落地上限 99（b=99）且 `a` 仍为 1（`+0x10`=227 的位段）；④关闭态消耗一次（使用物品），预期 b 71→70、`a` 不变，重开读 198；⑤重新启用堆叠上限，读 count 与 `+0x10`，预期恢复完整 canonical（③后 227、④后 198）；⑥对 canonical 128 重复②，预期关闭态读 0（b=0）且 `+0x10` 不变；⑦logcat 核对关闭态无 `S2 writeback ... mismatch` ERROR。
- **证据锚类型**：真机 + Host codec/模式感知 + 源码；本卡未取证前 S2-P5（R-47 决策 b）不得判通过。

### VM-33 S2 读侧 getter 显示与查询（99/100/127/128/199/999）

- **操作**：验证 `ITEM_GetCumulateCount@0x106094` 读侧 hook 对可堆叠类别按 `128a+b` 解码、对非可堆叠类别直通原版，覆盖 S2 边界数量在背包显示、API `count` 与原版 UI 三处一致。
- **设备/APK/快照前置（按 VM-12 SOP）**：记录真机 `192.168.3.54:5555`、最新 debug APK SHA-256、`extensionBagEnabled`；**前置：S2-P3 写侧已落地**（数量写入经 `s2_write_count`），否则数量值非 S2 布局、本卡预期不成立。
- **原版链(VMA)**：`ITEM_GetCumulateCount@0x106094`（原版只见 bits25–31，即 b 段；可堆叠返回 `b`，装备返回 `1`，空指针返回 `0`）。
- **扩展接管点(文件:函数)**：`native_inventory_hook.cpp:get_cumulate_count_wrapper`（门控 `item_count_encoding` == kEncoded 时返回 `stack_codec::s2_read_count(*(+I_COUNT))`，否则直通 backup）；安装链 `install_locked` 第 17 个 hook（`GetCumulateCount`）。
- **共享状态读写**：纯读物品 `+0x10` 与 ITEMCLASSBASE 类别表；不取 `g_virtual_bag_mtx`，不写任何状态。
- **必须保持的不变式(引 R-xx)**：类别判定 fail-closed（表不可用→backup）；非可堆叠（装备 marker bits25–31、宝石 bits18–23、袋容量 bits0–24）不解释 bits22–24（R-45、R-46、R-49、R-40）。
- **失败语义**：`item=nullptr` 返回 `0`；非可堆叠/未知类别返回原版语义（装备 `1`）；hook 安装失败时回滚整链并阻塞重试，不得半安装。
- **Host 测试名(现有或「缺口」)**：`test_stack_codec_s2`（`s2_read_count` 解码往返、`s2_clamp` 999/99 上限）；wrapper 门控依赖游戏内存类别表，Host 缺口，以本卡真机为准。
- **真机用例号(编号规范 VM-xx，写操作步骤+预期)**：`VM-33`：①经写侧构造可堆叠物品数量 `99/100/127/128/199/999` 各一（99 前后各取关闭态/开启态一次）；②每档读取 GET `/api/item/inventory/items` 的 `count` 与原始 `+0x10`，核对 `count == 128*((raw>>22)&7) + ((raw>>25)&0x7F)`；③在背包 UI 与详情弹窗目视数量；④对一件装备、一颗宝石、一个袋对象重复②③；预期：六档 `count` 与 UI 显示一致且等于 S2 解码值（重点 `100`、`127→128` 进位边界、`999` 上限不出现 `1000`）；装备 `count=1`、宝石/袋容量数值不受影响；`logcat` 出现 `hook install OK api=2 count=23 ... GetCumulateCount=`；关闭扩展后可堆叠物品显示回落为 `count mod 128`（`a` 保留，R-47），重新启用后恢复完整值。
- **证据锚类型**：真机 + Host codec + 源码；读侧单点通过不代表写侧（VM-34）整体通过。

### VM-34 S2 写侧门控与进位/借位（创建/合并/消耗）

- **操作**：驱动模块写路径（创建、扩展袋内合并、消耗扣减）产生跨 127 边界的数量变化，核对 `+0x10` 位段的 S2 拆段写入、非可堆叠类别 bits22–24 不变与 fail-closed 拒绝。
- **设备/APK/快照前置（按 VM-12 SOP）**：记录真机 `192.168.3.54:5555`、最新 debug APK SHA-256、`extensionBagEnabled`、`moveMergeEnabled`；操作前记录原版袋 `0..5×16` 物理快照 digest、目标物品原始 `+0x10` 与宝石/装备/袋对象位段摘要。
- **原版链(VMA)**：写侧 clamp 点 `INVEN_FindSaveSlot@0x103960+0x238`、`INVEN_SaveItemDirect@0x103bf0+0xdc/0xf0`、`INVEN_MoveItem@0x104934+0x150/0x158`（完整数量判定 999）；`INVEN_CheckSaveInNotEmptySlot+0xa4`、`INVEN_GetCumulateSaveSlotEx+0x164/0x17c` 保持原版 b 满判定（不进 clamp 表）。
- **扩展接管点(文件:函数)**：`model/virtual_bag_transaction_rules.inc:patch_payload_count/merge_count`、`api/native/game_inventory_basic.inc:data_op_add_item`、`extension_bag_equip.inc:consume_extension_item_after_native_locked`；统一 `stack_codec::s2_write_count/s2_clamp`（进位/借位内建）。
- **共享状态读写**：写点持 `g_virtual_bag_mtx` 修改 payload 数量位与 descriptor；类别判定走 `item_count_encoding`（不持锁读 ITEMCLASSBASE）。
- **必须保持的不变式(引 R-xx)**：仅 count-encoded 类别可写数量位（R-46）；创建/合并/消耗后 `raw == ((count>>7&7)<<22)|((count&127)<<25)`，`127→128` 写 `a=1,b=0`、`128→127` 写 `a=0,b=127`（R-45）；宝石 bits18–23、袋容量 bits0–24、装备 marker bits25–31 逐字节不变（R-46）；袋对象 marker 写仅 bit25。
- **失败语义**：非可堆叠/类别不可用时写点拒绝并记 `patch_payload_count ignored ... not_count_encoded`，不得按可堆叠处理；合并超上限走分堆/拒绝，不产生 >999 数量。
- **Host 测试名(现有或「缺口」)**：`test_stack_codec_s2`（拆段写回/进位/借位/clamp）、`test_stack_codec_mode_invariance`（两态写入/解码一致）、`test_virtual_bag_payload_helpers`（非 count-encoded 与未知类别拒绝）、`test_virtual_bag_merge_count`/`test_virtual_bag_mergeable_items`（合并上限与身份门控）、`test_remove_item_data_plan`（H-21 批量删除部分删堆修正：跨 127 借位、旧 a 残留、已正确不写、全删、count≤0、多缩减/越域 fail-closed）；「S2 编码写回后位模式」断言已随 Host 用例统一 `s2_read_count` 落地（见 §3 对码表备注）。
- **真机用例号(编号规范 VM-xx，写操作步骤+预期)**：`VM-34`：①启用态经 API 创建数量 `128` 的可堆叠物品，读 `+0x10` 预期 `a=1,b=0`；②扩展袋内合并 `100+100`，预期目标 `a=1,b=44` 且源槽清空；③使用消耗一次使 `128→127`，预期 `a=0,b=127`；④对宝石重复①，预期拒绝且宝石 bits18–23 不变；⑤袋对象装备/卸下各一次，预期容量位 bits0–24 与 marker bit25 不受数量路径影响；⑥关闭扩展重复①，预期原版只作用 b（`a` 保留，R-47）；⑦启用态让同类可堆叠双堆 `199+50`（API 创建两堆不同槽），经合成/事件扣 60（触发 `INVEN_RemoveItemData`，logcat `S2 writeback RemoveItemData`），预期部分删堆 `+0x10` 解码 `139`（`a=1,b=11`），整删堆槽清空。
- **证据锚类型**：真机 + Host codec/门控 + 源码；本卡未取证前 S2 写侧不得判通过，原版袋内 >127 合并的 a 进位开放点按 §2.1.1 登记归调用方级写点（S2-P3）。16 函数写点收口状态（2026-09-11 反汇编冻结）：已接管 5（含 H-21 `INVEN_RemoveItemData`@0x1040a8，调用方 MIXSYSTEM_UseStuff/QUESTSYSTEM/回滚路径）、数量回写已撤销 1（`ITEMSYSTEM_MakeItem`，R-52：arg2 非数量）、无需接管 8（证据见 `native_inventory_hook.cpp` 语义表）、待勘察 2（`NetworkStore_InitializeMenuData@0x15b7b0`、`NetworkStore_AddItem@0x15d640`——数量来自网络数据，离线不可达，冻结值域前不判通过也不判失败）。

### VM-37 出售/拆堆数量按 S2 全量（直接位读重定向）

- **操作**：启用堆叠上限，商店卖出可堆叠物品（整堆与部分），核对出售数量输入框上限、整堆判定与结算金额按 `128a+b` 全量而非 b 残量。
- **设备/APK/快照前置（按 VM-12 SOP）**：记录真机 `192.168.3.54:5555`、最新 debug APK SHA-256、`extensionBagEnabled`、堆叠上限开关；操作前记录目标物品原始 `+0x10`（含 a/b 拆段）与 `count=128a+b`。
- **原版链(VMA)**：`UIStore_ButtonSellExe@0xd1818+0xb8/0xd0`（b≤1 整堆判定 + `UIInputItemCount_Create` 上限）、`UIStore_SellItem@0xd25f0+0xc8`（b≤1 判定 + `ITEMSYSTEM_Divide@0x1083f8+0x60/0x94` 拆堆守卫/remain）、`UIStore_SellOKInputItemCount@0xd1948`（金额=单价×输入数量）。
- **扩展接管点(文件:函数)**：`game_patch_core.inc:g_stack_getter_redirect_patches` + `apply_s2_getter_redirects`（5 条 `mov x0,xN; bl ITEM_GetCumulateCount`）；被调用 getter 为 H-17 `native_inventory_hook.cpp:get_cumulate_count_wrapper`。
- **共享状态读写**：重定向为纯指令 patch（常驻、不随开关 revert）；getter 内部不取 `g_virtual_bag_mtx`（R-44）；拆堆源堆 a+b 回写由 `item_system_divide_wrapper` 确认后完成（R-49）。
- **必须保持的不变式(引 R-xx)**：重定向读值等于 `effective_read_count(field, stack_limit_enabled())`（R-51）；关闭态与原版 `GetBitValue(31,25)` 逐位一致（R-47）；装备 marker/宝石选项等非数量位段不被解释为全量（R-43/R-46）。
- **失败语义**：patch 原指令不匹配（版本差异）时 `redirect mismatch` 并拒绝安装，保持原版 b 读；getter 未安装时退回原版 getter（b 读）。
- **Host 测试名(现有或「缺口」)**：`test_sell_divide_s2_read_window`（getter 两态解码等价、十位窗口伪修复反例、200=a1b72）。
- **真机用例号(编号规范 VM-xx，写操作步骤+预期)**：`VM-37`：①启用态持有一堆 `200`（a=1,b=72）中药水，商店整堆卖出，预期输入框上限 200、结算按 200 计价（对照原版实得 546 缺陷）；②输入 `150` 部分出售，预期背包剩 50、所得按 150；③关闭堆叠上限重复①，预期输入框上限 72（b 视图，与原版一致）；④`logcat` 出现 5 条 `Inotia4Export: redirect ...` 与 `stack canonical layout applied`、`hook install OK api=2 count=23`。
- **证据锚类型**：真机 + Host codec + 源码；重定向安装日志已在部署流程取证，行为断言待人工商店操作。

### VM-38 拾取/掉落可堆叠物品数量为 1（MakeItem 回写撤销）

- **操作**：启用扩展袋，拾取/掉落/生成可堆叠物品，核对一次入库数量为 1，不随类别/品质参数放大。
- **设备/APK/快照前置（按 VM-12 SOP）**：记录真机、APK SHA-256、`extensionBagEnabled`；记录拾取前后原版袋与扩展袋数量。
- **原版链(VMA)**：`CHARSYSTEM_DropItem`（`0xf50b4/0xf50fc/0xf5144/0xf515c`，`ITEMSYSTEM_MakeItem(category, 2..5, flag)`）、`DEALSYSTEM_MakeSale`（`0xf6a90`，arg2=5）、`ITEMSYSTEM_MakeItem@0x10c6c8`（数量写点 `0x10ca3c` = `CAL_Calculate` 公式）。
- **扩展接管点(文件:函数)**：`native_inventory_hook.cpp:make_item_wrapper`（纯透传）+ `inventory_hook_stage4.cpp:stage4_make_item_writeback_count`（恒 0）。
- **共享状态读写**：wrapper 不取 `g_virtual_bag_mtx`；不做数量写（R-44/R-46）。
- **必须保持的不变式(引 R-xx)**：产物数量恒等于原版公式值（域 ≤99，b 写即全量）；arg2/category/flag 不得进入数量位（R-52）。
- **失败语义**：拿不到可信数量源时不写（恒 0），保持原版产物。
- **Host 测试名(现有或「缺口」)**：`stage4_hook_tests::test_make_item_writeback_count`（掉落品质 2..5、商店 arg2=5、边界/负数/未知类别全部 0）。
- **真机用例号(编号规范 VM-xx，写操作步骤+预期)**：`VM-38`：①启用态拾取药水/卷轴/材料各一次，预期各得 1 个（对照缺陷 2/3/4）；②击杀怪物掉落可堆叠物，预期落地数量 1；③关闭扩展重复①，预期不变。
- **证据锚类型**：真机 + Host 纯函数 + 源码；行为断言待人工拾取。

### VM-39 拾取并入扩展袋同类堆（入库前合并 + adopt 合并）

- **操作**：扩展袋已有同类堆时拾取同类可堆叠物品，覆盖原版袋有空槽与原版袋满两种前置，核对均并入扩展已有堆而非新建槽（原版/扩展）。
- **设备/APK/快照前置（按 VM-12 SOP）**：记录真机、APK SHA-256、`extensionBagEnabled`、`moveMergeEnabled`；记录扩展袋已有堆槽位/数量与原版袋空槽/满状态。
- **原版链(VMA)**：`INVEN_SaveItem@0x104528` → `INVEN_FindSaveSlot@0x103960`（成功则原版入库，失败则返回 0）；原版物理袋 `SaveItem→FindSaveSlot` 自带同类堆自动合并语义。
- **扩展接管点(文件:函数)**：`save_item_wrapper`（backup 前 `extension_bag_merge_native_item` → `game_ui_virtbag.cpp:virtual_bag_merge_native_item`；backup 失败后 `extension_bag_adopt_native_item` → `virtual_bag_adopt_native_item`）；两路径共用 `model/virtual_bag_transaction_rules.inc:adopt_merge_into`。
- **共享状态读写**：持 `g_virtual_bag_mtx`；合并改 descriptor + payload + `g_module_object_hashes`，物化对象数量位同步、原版对象延迟释放回池（R-36）。
- **必须保持的不变式(引 R-xx)**：合并判据与移动合并同源（`mergeable_items` 归一化 payload + 模式视图总量 ≤ 上限，R-45/R-47）；不受 `move_merge_enabled` 门控（R-53）；非可堆叠拒绝（R-46）；descriptor.count 恒 canonical（R-38/R-48）；backup 前合并不新建扩展槽。
- **失败语义**：无可合并堆则 backup 前合并返回 false、原版 original-first 入库；原版满且无可合并堆则 adopt 新建扩展槽；扩展袋也满则如实返回 false（上层按原版语义掉地/释放）。
- **Host 测试名(现有或「缺口」)**：`test_adopt_merge_plan`（同类合并、身份不同拒绝、启用 999 上限、关闭态视图相加且 a 保留、非可堆叠拒绝；backup 前后共用同一判据）。
- **真机用例号(编号规范 VM-xx，写操作步骤+预期)**：`VM-39`：①扩展袋放同类堆（如药水 150），原版袋留空槽，拾取药水 1 个，预期扩展堆变 151、原版不新开格；②填满原版袋重复①，预期仍并入扩展堆 151、不新建扩展槽；③扩展袋满且无可合并堆时拾取，预期新建扩展槽或按原版掉地；④关闭 `moveMergeEnabled` 重复①②，预期仍并入（合并不受拖动开关门控）。
- **证据锚类型**：真机 + Host 纯函数 + 源码；行为断言待人工拾取。

### VM-40 投影中扩展物品移入原版宿主袋（宿主容量字与显示袋守卫）

- **操作**：扩展视图打开（宿主袋为原版袋 0），把扩展物品拖到原版袋 0（宿主袋）与袋 1，核对均可移入、视图不出现宿主分叉、物品真实移动。
- **设备/APK/快照前置（按 VM-12 SOP）**：记录真机、APK SHA-256、`extensionBagEnabled`；记录操作前原版袋 0/1 容量字与 `0..5×16` 物理快照、`g_module_window_original_bag`。
- **原版链(VMA)**：`INVEN_GetBagSize@0x103250`（读袋对象 +0x10 容量位）、`INVEN_SaveItemOnEmpty@0x104be0`（空槽扫描）、`UIEQUIP_CUR_BAG`（direct/GOT，R-26/R-27）。
- **扩展接管点(文件:函数)**：`extension_bag_transaction.inc:move_extension_to_original_locked`（`HostCapacityGuard` RAII + `ext2orig_should_switch_display_bag`）；`model/virtual_bag_transaction_rules.inc:ext2orig_requires_host_capacity_restore`。
- **共享状态读写**：持 `g_virtual_bag_mtx`；事务期临时恢复宿主容量字（纯内存写），任何出口 RAII 写回投影值；投影中不改写 CUR_BAG。
- **必须保持的不变式(引 R-xx)**：目标袋 == 投影宿主袋时预检/插入按真实容量（R-54）；投影中 `g_module_window_original_bag` 与 CUR_BAG 不分叉（R-26/R-27/R-54）；失败不产生「原版袋容量+扩展内容」错位视图。
- **失败语义**：真实容量下无空槽则拒绝并保留扩展源；load/insert 失败走既有 isolate/abort 路径，宿主容量字仍写回投影值。
- **Host 测试名(现有或「缺口」)**：`test_ext2orig_host_guards`（宿主判定/非宿主/非投影/无效宿主，显示袋切换仅非投影）。
- **真机用例号(编号规范 VM-xx，写操作步骤+预期)**：`VM-40`：①扩展视图（宿主袋 0）拖扩展物品到原版袋 0，预期成功移入、`logcat` 出现 `ext2orig host capacity restored target=0`、视图仍为扩展袋、物品真实移动（对照缺陷 4）；②拖到原版袋 1，预期成功；③原版袋 0 真实满时拖入，预期拒绝且扩展源保留、无错位视图；④关闭扩展重复①，预期原版语义。
- **证据锚类型**：真机 + Host 纯函数 + 源码；行为断言待人工拖放。

### VM-41 原版背包详情出售按 canonical 全量接管（R-55）

- **操作**：启用堆叠上限，在装备页背包详情对可堆叠物品执行出售（按钮弹确认框→OK），核对确认框展示金额、实得金币与删除数量按 `128a+b` 全量；取消预演不得售出。
- **设备/APK/快照前置（按 VM-12 SOP）**：记录真机 `192.168.3.54:5555`、最新 debug APK SHA-256、`extensionBagEnabled`、堆叠上限开关；操作前记录目标物品原始 `+0x10`（含 a/b 拆段）与 `count=128a+b`、money 前后值。
- **原版链(VMA)**：`UIEquip_ButtonDestroyExe@0xb6240`（按钮预演 `bl 0xb83d0`，进入前 `x23=0x666`）→ `UIEquip_OKDestroyItem@0xb83d0`（`0xb8468` `bl 0x1261c4`）→ `0x1261c4` 内联 `ldr w0,[x21,#0x10]` + `bl UTIL_GetBitValue(_,31,25)` 只读 b 段、clamp `b∈[1,99]?b:1`、`unit×count×7/10`；弹窗 OK 经 `UIPopupMsg_ButtonOKExe@0xcaa14 blr x1`。
- **扩展接管点(文件:函数)**：`native_inventory_hook.cpp:ok_destroy_item_wrapper`（H-23）+ `button_destroy_exe_wrapper`（H-22 预演标记）；纯函数 `inventory_hook_stage4.cpp:vanilla_sell_route`/`vanilla_sell_money`；安装链第 13/15 个 hook。
- **共享状态读写**：wrapper 不取 `g_virtual_bag_mtx`；读 `G_UIEQUIP_DESC_TYPE_VMA`/`G_UIEQUIP_CUR_BAG_VMA`/`G_UIEQUIP_PANEL_CTRL_VMA` 面板上下文与物理槽对象；接管路径调 `fn_add_money`/`fn_minus_money`/`fn_remove_item_direct`/`fn_ui_equip_refresh_item_area`（不持扩展锁）。
- **必须保持的不变式(引 R-xx)**：仅 `desc_type==2` 且 count-encoded 的背包详情接管（R-46/R-55）；金额 `unit×min(canonical,999)×7/10`，canonical 0/单价越界/结果越界拒绝（R-37/R-55）；关闭态、非背包详情、装备详情与校验失败逐指令 backup；按钮预演绝不结算。
- **失败语义**：加钱失败直接返回 0 不删堆；提交前槽位复核失败/删除未生效则 `fn_minus_money` 退款；任一前置校验失败 backup 原版（fail-safe，不半执行）。
- **Host 测试名(现有或「缺口」)**：`stage4_hook_tests::test_vanilla_sell_takeover`（两态路由、金额边界与越界拒绝、调用点常量）、`test_native_equip_sell_count`（原版 b 段语义 199→71 缺陷基线）、`test_equip_sell_redirect_mapping`（重定向映射常量证据，该点当前不入表）。
- **真机用例号(编号规范 VM-xx，写操作步骤+预期)**：`VM-41`：①启用态持有一堆 `200`（a=1,b=72）中药水，背包详情点出售，确认框展示金额应为 `unit×200×7/10`（对照原版缺陷按 71 结算），点 OK 后 money 增加该金额、整堆删除、背包刷新；②点出售后在确认框取消，预期不售出、数量与 money 不变；③关闭堆叠上限重复①，预期走原版 b 段语义（与原版一致）；④对装备详情（desc_type=0）重复出售，预期走原版路径；⑤`logcat` 出现 `hook install OK api=2 count=23` 与 `vanilla sell preview/committed ...`；⑥制造越界单价或加钱失败，预期不删堆并记 ERROR。
- **证据锚类型**：真机 + Host 纯函数 + 源码；行为断言待人工详情出售。

### VM-42 三个 UI 宿主页签随扩展背包开关挂载/卸载（R-58）

- **操作**：启用扩展背包，分别打开背包页/商店页/合成器页，确认扩展页签挂载；运行期 `POST /api/config/set {"extensionBagEnabled":false}`，确认三处页签全部卸载；再启用确认恢复挂载。
- **设备/APK/快照前置**：记录真机 `192.168.3.54:5555`、最新 debug APK SHA-256、`extensionBagEnabled`；操作前后读 `/api/debug/extension_bag/status` 的 `extension_tab_button` 与 `enabled`。
- **原版链(VMA)**：`UIEquip_CreateInvenControl@0xb6688`（`ldr x3,[0x2f5410]` 取 item proc）；`UIStore`/`UIMix` 袋容器（见 `extension_bag_store.inc`/`extension_bag_mix.inc` 挂载点）。
- **扩展接管点(文件:函数)**：`extension_bag_runtime.inc:install_extension_tab_buttons_locked`、`extension_bag_store.inc:store_install_tab_buttons_locked`、`extension_bag_mix.inc:mix_install_tab_buttons_locked`（三处入口 `g_virtual_bag_enabled` 门控）；`extension_bag_render.inc:draw_tab_buttons_in_frame_locked`、`extension_bag_store.inc:store_draw_end_wrapper`、`extension_bag_mix.inc:mix_draw_end_wrapper` 的 disabled 分支清除。
- **共享状态读写**：`g_virtual_bag_enabled`（`std::atomic`）、`g_extension_tab_buttons`/`store_tab_buttons`/`mix_tab_buttons` 与各自 generation。
- **必须保持的不变式(引 R-xx)**：三 install 函数入口判 `!g_virtual_bag_enabled.load()` 即返回；关闭时删除已挂载控件并递增 generation（R-19/R-58）；调用方不重复判断。
- **失败语义**：`fn_touch_handle_delete_control` 为空时仅清引用，不崩溃。
- **Host 测试名(现有或「缺口」)**：无（UI 挂载依赖游戏控件树）；缺口 `test_tab_mount_gate`（纯逻辑可补，当前未建）。
- **真机用例号(编号规范 VM-xx，写操作步骤+预期)**：`VM-42`：①启用态开背包页，`status.extension_tab_button=true`；②`POST /api/config/set {"extensionBagEnabled":false}` 后 `extension_tab_button=false`；③再启用恢复 `true`；④商店页/合成器页分别重复，页签挂载随开关；⑤`logcat` 无 patch 报错。
- **证据锚类型**：真机 + 源码；行为断言待人工商店/合成器页签确认。

### VM-43 monster 版物品数量上界兼容（R-59）

- **操作**：在 monster v23（**未改 APK**）上启用模块，`enter_slot 0` 载入含 S2 扩展数量的存档，确认进入世界不崩溃；对非 monster（大修）版确认无改动。
- **设备/APK/快照前置**：真机 `192.168.3.54:5555`、最新 debug APK SHA-256；记录 `monster item-count compat` 日志。
- **原版链(VMA)**：monster 物品准入 helper `0x741764`（注入段 `LOAD @0x740000`），取记录 `+14`（= `I_COUNT` 最高字节）做 `((v-2)>>1) > 0x62`。
- **扩展接管点(文件:函数)**：`game_patch_core.inc:apply_monster_item_count_compat()`；调用点 `game_access.cpp:bridge_init()` 末尾。
- **必须保持的不变式(引 R-xx)**：按特征字节 `1F 89 01 71 68 00 00 54`（`cmp w8,#0x62; b.hi`）在 `libgame.so` 可执行映射内定位；非命中不改；禁止改 APK/重打包（R-59）。
- **失败语义**：无命中（大修版）记 `monster item-count compat: no target` 并继续。
- **Host 测试名(现有或「缺口」)**：无（依赖真机内存布局）。
- **真机用例号(编号规范 VM-xx，写操作步骤+预期)**：`VM-43`：①monster v23 + 模块，log `monster item-count compat: 0x… cmp w8,#0x62 -> #0x7F`；②`enter_slot 0` → `screen=world`，进程存活且无 FATAL/tombstone；③大修版 log `no target` 且内存零改动。
- **证据锚类型**：真机日志 + 源码。

### VM-44 save gate 按符号 hook SAVE_SaveInventory 函数入口（R-60）

- **操作**：monster v23 + 模块启动，确认 `save gate hook installed` 且 `save gate patch mismatch` 为 0；注入链完整；扩展视图可用；保存成功。
- **设备/APK/快照前置**：真机 `192.168.3.54:5555`、最新 debug APK SHA-256。
- **原版链(VMA)**：`SAVE_Save@0x129600+0x170`（大修 `bl 0x127d8c`；monster `bl 0x741168` 包装器，内部 `0x741204 bl 0x127d8c`）；原版 `SAVE_SaveInventory@0x127d8c`。
- **扩展接管点(文件:函数)**：`extension_bag_lifecycle.inc:inject_locked`（`native_hook_func()(target, &save_inventory_wrapper, &backup)`）；`extension_bag_render.inc:save_inventory_wrapper`（经 `g_backup_save_inventory` 调原函数）。
- **共享状态读写**：`g_backup_save_inventory`、`g_save_inventory_patch_addr`。
- **必须保持的不变式(引 R-xx)**：按 `fn_resolve("F_SAVE_SAVE_INVENTORY_VMA")` 定位函数入口，不依赖调用点原指令字；wrapper 必须走框架 backup（不得直调符号地址导致重入）；hook 幂等（`g_backup_save_inventory==nullptr` 判定）；backup 为空即失败。
- **失败语义**：`hook_func` 为空 / result!=0 / backup 为空 → `inject_locked` 返回 false（整链不装）。
- **Host 测试名(现有或「缺口」)**：无（依赖真机 inline hook）。
- **真机用例号(编号规范 VM-xx，写操作步骤+预期)**：`VM-44`：①monster 启动 log `save gate hook installed target=… backup=…`、`save gate patch mismatch` 计数 0；②8× `save callsite hooked` + drop/draw/desc/event/save panel/store host/mix host 全部安装；③`enter_view {"bag":6}` → `state.mode="module"`；④`POST /api/system/save` → `save complete slot=0 tx=… participants=1`，进程存活。
- **证据锚类型**：真机日志 + 源码。

## 2.1 VM-B 原版基线行为包

> VM-B01～VM-B04 对应规则册 B-01～B-04 的原版基线。前置条件：扩展背包已启用；每张
> 卡均使用非扩展物品，并记录设备、APK SHA-256、配置开关、操作前后原版 `0..5×16`
> 物理槽摘要和扩展逻辑状态。B-05 是模块扩展袋合并功能，另由 VM-B05/VM-13 取证。
>
> 当前判读：draw-end 不再逐帧写投影控件；若出现非活 session、generation/root/view
> 不匹配或 source 控件失效的 stale moving，必须观察 `TouchState+0x30` 被清空及控件
> `+0x0a/+0x0b` flag 被清除。该判读对应 R-34，不替代 VM-B 的原版行为断言。

| 原版基线契约 | 对应真机卡 | 核心回归面 |
|---|---|---|
| B-01 | VM-B01 | 点击消耗品直接使用/消耗，不进入拖拽态 |
| B-02 | VM-B02 | 点击装备直装或替换，不进入拖拽态 |
| B-03 | VM-B03 | 宝石拖到装备执行镶嵌，不执行交换 |
| B-04 | VM-B04 | 原版袋内拖动保持原版移动语义 |

### VM-B01 原版消耗品点击使用

- **对应契约**：B-01。
- **步骤**：①准备原版袋内非扩展消耗品并记录 bag/slot/count；②点击物品的“使用”，
  对需要确认的物品继续点击原版确认；③立即读取数量、物理槽和拖动 session 状态。
- **预期**：原版效果执行并按原版时机直接扣量/清槽；不产生 `0x17/0x19/0x18` 拖动
  session，不进入扩展移动事务；松手后无 moving/on 残留标志和幽灵拖拽；失败时物品数量和
  槽位保持原状。
- **日志锚**：`UIEquip_OKConfrimUseItem`（如有确认）、`INVEN_ConsumeItem`/`ConsumeItem`
  前后、`g_extension_drag_session` 无创建；不得出现扩展 `MoveItem GUARD reject`。

### VM-B02 原版装备点击直装/替换

- **对应契约**：B-02。
- **步骤**：①准备原版袋内非扩展装备；②在角色装备槽为空时点击“装备”；③恢复原状后
  在同一槽已有装备时再次点击“装备”；④检查角色槽、原版袋和拖动 session。
- **预期**：空槽直接装备；已有装备按原版交换/替换并正确回包；两次点击均不进入拖拽态，
  不触发扩展装备事务，松手后无 moving/on 残留标志和幽灵拖拽，失败不清理错误源槽。
- **日志锚**：`UIEquip_ButtonEquipExe`、`CHAR_EquipItemFromInvenToSlot` 的调用前后；
  原版对象必须走 backup，不能出现 C1 扩展 source 拒绝或 H4 `backup=skipped`。

### VM-B03 原版宝石拖到装备镶嵌

- **对应契约**：B-03。
- **步骤**：①记录原版宝石 count 和原版装备 socket/payload；②将宝石拖到有孔装备；
  ③分别对无孔、非宝石和空装备目标重试；④读取装备 payload、宝石数量、两侧槽位。
- **预期**：有孔时只执行一次原版镶嵌并消耗一颗宝石，装备不被交换；其余三类失败保持
  宝石、装备和槽位不变；松手后无 moving/on 残留标志和幽灵拖拽；不得创建扩展 swap/pending。
- **日志锚**：`ITEMSYSTEM_PutJewel`/`PutJewel` 返回值与前后 socket/count；成功路径不得
  以 `INVEN_MoveItem` 作为提交者，不得命中扩展 `MoveItem GUARD reject`。

> 本卡对应原版源→原版装备的 B-03；S-03 是扩展 apply 分支（宝石/强化卷轴→装备、同袋），
> 已由 VM-10 真机验证通过；两者的对象身份和失败判定仍分别维护。

### VM-B04 原版袋内拖动

- **对应契约**：B-04。
- **步骤**：①在原版袋 `0..5` 选取非扩展物品并记录 source/target；②拖到合法空槽；
  ③再拖到原版合法目标槽；④退出并重开原版视图，比较物理快照、扩展 state 和 projection。
- **预期**：source/target 只按原版移动语义变化；扩展逻辑槽、object/handle、pending、
  projection 不变；不发生扩展事务双提交、物理槽丢失或 stale 拖动 session。
- **日志锚**：原版对象的 `MoveItem pre/post` 或 passthrough；C1 必须放行原版 source，
  H4 不得输出 `extension_item=1`/`backup=skipped`；`orig_event pre/post` digest 只可
  反映本次预期原版移动。

### VM-B05 模块扩展袋内合并（非原版基线）

- **对应契约**：B-05。
- **步骤**：①准备同一扩展逻辑袋内同 payload 的两个扩展堆叠物；②拖动合并；③分别测试
  payload 不同、超过 stack limit 和跨袋目标；④读取 source/target count、逻辑对象、
  projection 和 pending。
- **预期**：仅满足模块条件时按 stack limit 合并；其他情况按模块移动/交换或失败返回；
  不改写原版物理袋，不把模块合并能力写成原版 `INVEN_MoveItem` 能力。
- **日志锚**：模块 `txn prepared/committed` 或失败日志、source protection 和 projection
  更新；原版四象限结论不由本卡替代。

### VM-28 吞掉 release 的原版清理变体

- **对应契约**：R-33；覆盖 owner/terminal、非世界取消、capture 空坐标、capture
  click/drop 和原版袋 drop 五个吞出口。
- **步骤**：①分别触发五个出口；②记录 `TouchState` 的 `+0x00..+0x10`、`+0x30`、
  `+0x38`、`+0x48` 以及 moving/on 标志；③核对 `release_cleanup ctl=... flags_cleared`；
  ④确认 handled 跳过 selected 清理、unhandled 执行 selected 清理。
- **预期**：每个出口只清理一次；`0x08` 派发和相对坐标恢复发生在锁外；松手后不残留
  moving/on 标志、不出现幽灵拖拽，且不触碰 `g_inven`。
- **日志锚**：`release_cleanup ctl=... flags_cleared`、原版 `0x08` proc 前后；Host
  仅能覆盖变体判定/字段布局，原版 UI 派发必须由真机确认。

### VM-B 刷新关卡判读（H-16）

- Hook 后原版一次 `RefreshItemArea` 应由 H-16 先走 trampoline、再在锁内覆盖当前扩展
  投影；模块内部刷新必须走 raw-original dispatcher，因此原版刷新不再把扩展物品顶掉。
  日志锚：`hook install OK ... count=23`、原版刷新调用前后及投影控件 `data[0]` 保持当前
  扩展对象；不应出现 draw-end 逐帧写控件，非活 stale moving 应出现清理日志并清除
  `TouchState+0x30` 与控件 flags。H-16 post-projection 是唯一刷新后覆盖职责。

## 3. 规则锚表

下表是 R-01..R-55 的反查表；“卡”列列出覆盖该规则的主卡。

| 规则 | 被哪些卡覆盖 | 当前锚结论 |
|---|---|---|
| R-01 | VM-04 | 真机确认使用收尾；无现成 Host UI 锚，列入 VM-only。
| R-02 | VM-10、VM-11、VM-14、VM-15、VM-26 | `test_p52_drag_session` + VM-10 装备槽拖放和三方向 API/真机。
| R-03 | VM-01、VM-03、VM-06 | Host 有 ledger 部分；对象/descriptor/控件同步仍需缺口测试。
| R-04 | VM-06、VM-24 | 现有 `test_object_operations` 覆盖 fallback；view/窗口配对需真机。
| R-05 | VM-02、VM-13、VM-19、VM-20 | `test_queries`/模型容量 + 生产者真机。
| R-06 | VM-07、VM-11 | 三态按钮 Host 缺口、投影 drop 有 `test_p52_drag_session`。
| R-07 | VM-03、VM-08、VM-10、VM-21、VM-22 | `test_object_operations` 只覆盖 Remove/Consume seam；生产者真机未闭合。
| R-08 | VM-05、VM-06 | `test_object_operations` 已覆盖 view fallback；orig→ext 装备真机。
| R-09 | VM-10 | `test_object_operations` 覆盖 jewel 两端分流，原子消费待真机。
| R-10 | VM-08 | `test_unequip_to_inven` 现成 Host，真机覆盖满包。
| R-11 | VM-19、VM-25 | SaveItem/商店容量顺序无现成 Host，真机/日志为主。
| R-12 | VM-18 | 商店投影刷新无现成 Host，真机观察整列不覆盖。
| R-13 | VM-16、VM-17、VM-18 | popup 隔离无现成 Host，真机覆盖两宿主切换。
| R-14 | VM-24 | 详情 proc 排除只能由真实 PtrHook/按钮行为确认。
| R-15 | VM-03、VM-04、VM-06、VM-16、VM-24 | 释放守卫静态检查 active 与 pending_release；现有 ledger 无专用断言，Host 缺口。
| R-16 | VM-06、VM-10、VM-12、VM-13 | `test_p44_transaction_stages` 只证明 descriptor 模型；VM-13 增加归一化 payload；问题 B 真机单独保留。
| R-17 | VM-06、VM-12、VM-14 | stale endpoint 当前无专用 Host；失败/回滚需真机与缺口。
| R-18 | VM-03、VM-15、VM-25 | `test_original_only_queries` 覆盖 TLS original-only seam；锁序/刷新重入仍需运行时观察。
| R-19 | VM-09、VM-11、VM-23、VM-31 | `test_p52_drag_session` 有 stale generation；tab root 需真机。
| R-20 | VM-11、VM-12、VM-14、VM-25 | `test_virtual_bag_mergeable_items` 有快照纯函数；H3/H4 真机为主。
| R-21 | VM-01、VM-02、VM-14、VM-15、VM-26 | `test_virtual_bag_transaction_domain` 现成；页签 0..4→6..10 转换和 native item 判空由 VM-26/VM-15 取证。
| R-22 | VM-01、VM-02、VM-13、VM-20、VM-21、VM-22、VM-26 | `test_p7_stage4_find_item_poc` + 生产者逐 caller 真机。
| R-23 | VM-17、VM-18 | 价格 variant 可 Host，popup/钱回滚需真机。
| R-24 | VM-04、VM-06、VM-17、VM-24 | 菜单角色和详情身份只能真机闭合。
| R-25 | VM-03、VM-04、VM-08、VM-10、VM-14、VM-15、VM-17、VM-19、VM-25 | `test_p44_transaction_stages`/journal Host + 保存真机。
| R-26 | VM-02、VM-05、VM-06、VM-19、VM-23、VM-25、VM-31 | `test_extension_bag_exit_rendering_state` 部分覆盖；view/window 真机。
| R-27 | VM-23、VM-25、VM-31 | direct/GOT 成对恢复必须真机日志确认。
| R-28 | VM-12、VM-13、VM-B05 | `test_virtual_bag_mergeable_items` 现成，问题 B 仍未解决；VM-B05 是模块合并卡。
| R-29 | VM-13、VM-14、VM-25 | `test_p44_transaction_stages` 覆盖 pending/journal 域模型。
| R-30 | VM-07、VM-11、VM-16、VM-23、VM-24、VM-31 | `test_p52_drag_session` 覆盖 generation；root/control 仍需缺口和真机。
| R-31 | VM-10、VM-11、VM-12、VM-13、VM-27 | `test_inventory_hook_stage4` 覆盖源判据；Native `MoveItem GUARD reject` 与装备槽 proc 日志仍需真机。
| R-32 | VM-B01～VM-B04 | H-16 安装日志 `count=23`（当前总链 23 个）与 raw-original grep 锚已补；原版刷新后的扩展投影保持不被顶掉，VM-B01～B04 真机已确认；VM-B05 不属于原版刷新基线。 |
| R-33 | VM-28 | 五个吞掉 `0x18` 出口统一经 `complete_original_release_cleanup_locked`；handled/unhandled 变体和锁外原版清理需真机日志确认。
| R-34 | VM-B01～VM-B04 | Host 可静态核对六条件表达式；真机已确认 draw-end 不逐帧写控件、非活 stale moving 被清理。 |
| R-35 | S-05、VM-29 | 页签先于网格解析；真机已确认页签装备、跨页签移动和原版对照。 |
| R-36 | S-05、VM-29 | 触摸窗口释放使用延迟队列；队列满转 custody 保管且事务继续；以 `deferred free enqueue/drain`、`queue full; custody retained` 和 `tab commit` 日志核对。 |
| R-37 | VM-17、VM-18、VM-30 | Host 价格边界/clamp 断言 + 真机装备/商店弹窗与 API 对照；非法价格不得写弹窗或结算。 |
| R-38 | VM-30 | `test_virtual_bag_json_count_clamp` 断言非堆叠装备 payload 逐字节不变、可堆叠超限才收敛、合法数量幂等；真机存档回归仍需核对 payload 语义。 |
| R-39 | VM-09、VM-31 | `test_stack_codec` 断言 marker 只改 bit25..31 且保留 bit0..24；真机核对袋容量、切换和 `count=23` 安装日志。 |
| R-40 | VM-01、VM-03、VM-05、VM-10、VM-13 | `test_virtual_bag_json_count_clamp`、`test_virtual_bag_mergeable_items`、`test_virtual_bag_payload_helpers` 覆盖已知/非适用/未知 fail-closed；真机拒绝路径不得降级为堆叠或 apply。 |
| R-41 | VM-13、VM-30 | `test_virtual_bag_payload_helpers` 断言装备/未知类别 patch no-op，`test_virtual_bag_mergeable_items` 断言合并前门控；真机同袋合并与存档载荷对照。 |
| R-42 | VM-05、VM-29 | `test_virtual_bag_state` 断言 BagType 容量派生；真机原版物品→空页签和扩展页签路由核对两套判据不串用。 |
| R-43 | VM-30 | `llvm-objdump` 证明装备 marker 判定不属于数量 patch；真机需核对启用上限后的装备显示与详情。 |
| R-44 | VM-15、VM-B01～VM-B04 | `test_original_only_queries` 与 `test_virtual_bag_transaction_domain` 覆盖 Host seam；真机 VM-15 核对跨域删除后源槽为空、事务提交且 health 持续可达。 |
| R-45 | VM-32、VM-33、VM-34 | `test_stack_codec_s2` 断言 S2 拆段编解码往返、位保留与 clamp；`test_stack_codec_mode_invariance` 断言两态写入/解码一致；真机 VM-32 核对数量跨重启保持，VM-33 核对读侧 `128a+b` 解码展示与查询，VM-34 核对写侧创建/合并/消耗跨 127 进位与 `+0x10` 拆段位模式。 |
| R-46 | VM-32、VM-33、VM-34 | `test_virtual_bag_json_count_clamp` 断言非可堆叠 payload 逐字节不变；`test_virtual_bag_payload_helpers` 保留装备/未知类别 no-op；读侧门控已落地（H-17 kEncoded-only），VM-33 核对装备/宝石/袋容量不受解码影响；写侧门控已落地（S2-P3：`patch_payload_count`/`data_op_add_item`/消耗写回三处 `item_count_encoding` 门控），VM-34 核对写点拒绝与非可堆叠 bits22–24 不变。 |
| R-47 | VM-32、VM-35 | 关闭态高位保留无 Host 直接锚；真机 VM-32/VM-35 观察关闭期原版操作只作用低 7 位且 `a` 保留；Host `test_virtual_bag_cross_config_roundtrip` 断言跨配置读档不改写 payload。 |
| R-48 | VM-32、VM-35 | `test_stack_codec_mode_invariance` 覆盖 99/127/128/217/999 两态写入/解码一致与切换模式不改值；`test_virtual_bag_cross_config_roundtrip` 覆盖 99/127/128/217/999 跨配置三往返与旧 `encodingVersion` 字段宽容忽略；无版本标识（sidecar 不写、读到即忽略）与读侧 getter 依赖真机（VM-32 步骤③④、VM-35 全流程）。 |
| R-49 | VM-32、VM-33、VM-34 | 读侧 getter hook 已落地（S2-P2：`native_inventory_hook.cpp:get_cumulate_count_wrapper` → `inventory_hook_stage4.cpp:stage4_get_cumulate_count`，wrapper 门控 Host `test_get_cumulate_count_s2`）；真机 VM-33 核对 99/100/127/128/199/999 显示与查询解码，VM-32 覆盖读侧存档一致性；写侧调用方级门控与进位已落地（S2-P3：数量产生点统一 `s2_write_count`/`s2_clamp`，无统一 setter），VM-34 核对写侧。 |
| R-51 | VM-37 | 重定向表 `g_stack_getter_redirect_patches` + `apply_s2_getter_redirects`（5 条 `mov x0,xN; bl ITEM_GetCumulateCount`）已落地；Host `test_sell_divide_s2_read_window` 断言 getter 两态解码等价与十位窗口伪修复反例；真机重定向安装日志已在部署流程取证，商店整堆/部分出售行为归 VM-37。装备页详情结算点 `0x1261c4` 当前不在表内（由 R-55 H-23 接管，归 VM-41）。 |
| R-52 | VM-38 | `make_item_wrapper` 纯透传 + `stage4_make_item_writeback_count`（恒 0）已落地；Host `stage4_hook_tests::test_make_item_writeback_count` 覆盖掉落品质 2..5、商店 arg2=5 与边界；真机拾取/掉落数量归 VM-38。 |
| R-53 | VM-39 | `adopt_merge_into`（model 纯函数）+ `virtual_bag_merge_native_item`（backup 前）+ `virtual_bag_adopt_native_item`（backup 失败后）合并前置查找已落地；Host `test_adopt_merge_plan` 覆盖同类合并/身份不同/上限/关闭态/非可堆叠；真机拾取（原版有空/满）归 VM-39。 |
| R-54 | VM-40 | `HostCapacityGuard` RAII + `ext2orig_requires_host_capacity_restore`/`ext2orig_should_switch_display_bag` 已落地；Host `test_ext2orig_host_guards` 覆盖宿主/非宿主/非投影判定；真机投影拖入宿主袋归 VM-40。 |
| R-55 | VM-41 | `ok_destroy_item_wrapper`（H-23）+ `button_destroy_exe_wrapper`（H-22 预演标记）+ `vanilla_sell_route`/`vanilla_sell_money` 已落地；Host `test_vanilla_sell_takeover` 覆盖两态路由与金额边界，`test_native_equip_sell_count` 固化原版 b 段缺陷基线；真机背包详情出售归 VM-41。 |
| R-59 | VM-43 | `apply_monster_item_count_compat` 按特征字节 `1F 89 01 71 68 00 00 54` 在可执行映射内把 `cmp w8,#0x62` 改写为 `#0x7F`；非 monster 版无操作（`no target`）。真机 monster v23 归 VM-43（`enter_slot 0` 不崩）。 |
| R-60 | VM-44 | save gate 改为 `native_hook_func()` hook `SAVE_SaveInventory` 函数入口，wrapper 经 `g_backup_save_inventory` 调原函数；不依赖调用点原指令字。真机 monster v23 归 VM-44（`save gate hook installed`、`patch mismatch`=0、注入链完整、保存成功）。 |

### 3.1 16 条原无专门锚规则的定锚方案

规则册登记的无锚集合为 R-01、03、05、06、11、12、13、14、15、17、23、24、26、27、30、31。下表明确“可 Host/可 VM/不可锚”，不写实现方案。

| 规则 | 定锚结果 | 测试名或 VM 用例；断言要点 |
|---|---|---|
| R-01 | 可 VM | VM-04；确认成功后扩展控件不残留失效对象、详情身份清理、消费数量与最新 state 一致。
| R-03 | 可 Host | 缺口 `test_projection_descriptor_object_sync`；断言 descriptor、object、hash、handle 和 control `data[0]` 五者在消费/交换/清槽后相同。
| R-05 | 可 Host | 缺口 `test_virtual_bag_empty_slot_predicate`；断言派生 capacity、descriptor、object/token 不可分配条件共同决定结果，bag 5 不被计为扩展目标。
| R-06 | 可 Host | 缺口 `test_equip_button_three_state`；断言 Handled/Blocked/NotExtension 分别为不 backup/不 backup/一次 backup。
| R-11 | 可 VM | VM-19；商店投影开启时 SaveItem backup 前原版容量恢复，买入不落真实容量外槽。
| R-12 | 可 VM | VM-18；扩展卖出后剩余商店列仍是商店 projection，原版刷新不覆盖扩展项。
| R-13 | 可 VM | VM-16→VM-18；装备页与商店页交替确认后，OK/Cancel callback 与详情身份各自恢复。
| R-14 | 可 VM | VM-24；装备/卸下 proc 不经详情观察 PtrHook 重复处理，使用/卖出/销毁仍各执行一次。
| R-15 | 可 Host | 缺口 `test_ownership_active_pending_release`；断言 active/pending_release 对象不能替换、释放或复用，匹配 token finish 后才可终态释放。
| R-17 | 可 Host | 缺口 `test_stale_endpoint_rejection`；断言非空 cache category/hash 与 descriptor 不符时先拒绝，原对象仍可审计回滚。
| R-23 | 可 Host | 缺口 `test_sell_price_variants`；断言装备页和商店页传入不同 variant，价格结果互不污染。
| R-24 | 可 VM | VM-04、VM-06；角色 B 菜单操作不修改角色 A，确认使用和装备均按当前菜单角色。
| R-26 | 可 VM | VM-23；切袋/装备收尾后容量来自当前 view，窗口原版 bag 只承担临时投影字段。
| R-27 | 可 VM | VM-23、VM-25；进入/退出/保存前后 direct 与 GOT 袋号相等且恢复为原值。
| R-30 | 可 Host | 缺口 `test_projection_root_generation_gate`；断言 root 重建递增 generation、失效 root/source control 被拒、新 root 控件才可提交。
| R-31 | 可 VM | VM-27；扩展对象命中 guard 必须记录完整参数/身份/物理摘要并跳过 backup，原版对象必须保持原版结果。

**定锚分布：可 Host 7 条（R-03/05/06/15/17/23/30），可 VM 9 条（R-01/11/12/13/14/24/26/27/31），不可锚 0 条。** “可 Host”中的 7 个测试名均为缺口，不把缺口写成已通过。R-18 已脱离无锚集合：现锚为 Host `test_original_only_queries` 与 VM-15、VM-25（见 §3 锚表）。

## 4. 问题 B 专项：H3/H4 复现和日志判读

### 4.1 VM-12 复现步骤

1. 记录 APK SHA-256、设备、配置：`extensionBagEnabled`、`moveMergeEnabled`、source protection 最终值；记录原版袋 `0..5×16` 快照 digest、nonnull 和同号 b 槽物品 payload/指针摘要。
2. 进入扩展袋 0，记录 a、b 两个非空 descriptor/object/category/hash/handle/generation/ownership。
3. 从 press 前开始抓取 `orig_event pre/post`、`delegate_pre_orig/post_orig`、`event=0x17/0x19/0x18`、pending、txn、H-16 `projection post-refresh`、mutation 和 rollback 全日志；不期待 draw-end heal。
4. 执行 a↔b 双向交换；立刻读取原版袋 0 同号 b 槽、扩展袋 0 两槽和 projection。
5. 切出原版视图再切回扩展视图；显式保存；force-stop/restart；重新进入相同存档并再次读取。
6. 对比五个时间点：swap 前、每个原版事件 post、`0x18` commit、H-16 post-refresh 后、切回原版/保存后。第一处 digest 变化才是下一次写点反查边界。

### 4.2 H3/H4 日志判读表

| 观察 | 读法 | 下一步候选/结论 |
|---|---|---|
| `orig_event pre` 与 `post` digest 相同、nonnull 相同 | 被 `g_orig_event` 包住的调用未改变 96 槽 | 继续查 guard 外的 `game_patch_move_merge.inc:46-105`、非 guard `INVEN_MoveItem`、`SaveItemOnEmpty` 直通点；不能判安全。
| `orig_event post` digest 变化，出现 `ERROR physical inventory mutation` | 原版事件返回后检测到物理数组变化 | 按 event/time 反查 `extension_bag_lifecycle.inc:218/430` 等放行点；检查 `rollback restored=1` 后是否再次变化。
| mutation 行定位 `bag/slot` 为同号 b 槽 | 原版写点命中问题 B 症状槽 | 保存前后分别核对对象内容是否已被释放/破坏；数组恢复不等于对象内容恢复。
| `rollback restored=0` | `g_inven` 指针变更或快照不可用，未证明恢复 | 该次直接阻断；记录 before/after digest 和指针，不得写“守卫已修复”。
| `rollback restored=1` 且 projection_sync=1，但保存后仍丢 | guard 恢复后仍有后续写点或对象已损坏 | 查恢复后事件、保存、direct/GOT 和未包裹的物理调用；问题 B 仍未解决。
| `orig_event pre/post` 日志完全缺失 | H3/H4 观测链未建立，不能判 digest 未变化 | 先修取证过滤/日志窗口；当前样例 `archive/extension-bag/swap-loss-log-excerpt-20260908.txt`（原日志 7951-7973 段）只有 delegate、pending、post-refresh、commit，不足以闭合 H3/H4。
| 只有 `txn committed` 和 `projection post-refresh` | 只证明逻辑事务/控件收尾 | 不证明物理数组和对象安全；按 VM-12 重新抓 pre/post/mutation。
| `moveMergeEnabled=false applied=true` 但 swap 仍提交 | merge 开关不是扩展事务总开关 | 检查 source protection 是否仍启用；该日志不能解释为扩展 swap 被关闭。
| 出现 `ERROR MoveItem GUARD reject ... backup=skipped` | 扩展对象到达 `INVEN_MoveItem`，被 Native guard 在物理写入前阻断 | 检查 event/source/session 路由；该次不再改物理槽，但不代表问题 B 根因已修复。
| 出现 `MoveItem pre`/`post` 且对象为原版 | 原版对象按 original-first 调 backup；摘要可直接比较该函数前后 | digest 或源/目标指针变化时将 `INVEN_MoveItem` 作为确认写点；无变化则继续查其他候选。
| 只有 `MoveItem passthrough`，或完全没有 MoveItem 日志 | 前者是不在扩展 view/session 的原版低频路径，后者表示写点绕过该 Hook 或日志窗口缺失 | 继续核对 raw `SaveItemOnEmpty`、未包裹 event 和 caller；不能按“无 guard 日志”判安全。

### 4.3 当前预期结果

`VM-12` 的当前预期失败是**已知未解决**：允许观察到原版同号槽消失，但必须留下完整 H3/H4 取证。当前防御是已布防而非已修复；Host 的 `test_p44_transaction_stages`、`test_p52_drag_session`、`test_virtual_bag_mergeable_items` 不能替代该真机结论。

## 5. 回归清单

### 5.1 每次扩展事务/投影改动的最小 Host 集

* `test_virtual_bag_transaction_domain`
* `test_p44_transaction_stages`
* `test_p52_drag_session`
* `test_virtual_bag_mergeable_items`
* `test_ownership_ledger_p43`
* 受影响函数组对应的 `test_queries`、`test_object_operations`、`test_unequip_to_inven` 或 `stage4_hook_tests` 中同名测试。

### 5.2 每类改动的最小 VM 集

| 改动范围 | 必跑卡 |
|---|---|
| 查询/容量/API 域 | VM-01、VM-02、VM-26 |
| 使用/详情/对象身份 | VM-03、VM-04、VM-06、VM-24 |
| 装备/卸下/宝石 | VM-05、VM-06、VM-08、VM-09、VM-10 |
| 拖动/移动/合并 | VM-11、VM-12、VM-13、VM-14、VM-15、VM-27 |
| 商店/生产者 | VM-18、VM-19、VM-20、VM-21、VM-22 |
| 投影/页签/保存 | VM-23、VM-25；涉及问题 B 时追加 VM-12 |
| `event`/`proc`/Hook 安装层 | **VM-B01、VM-B02、VM-B03、VM-B04 全跑**；模块合并改动追加 VM-B05；不得只跑受影响的修复卡 |

**硬规则**：diff 触及 event、proc 或 Hook 安装层任一层时，必须全跑原版基线 VM-B01～B04；
涉及模块同堆合并时追加 VM-B05。只跑修复闭环或只依赖静态审查，不构成回归通过。

### 5.3 通过条件

1. Host 相关测试全绿，且 `git diff --check` 无错误。
2. 真机记录 build identity、设备、配置、操作前后状态、日志起止和用户 verdict；API 结论不能冒充物理触摸结论。
3. 任何涉及 VM-12 的改动都保留“未解决+已布防”，没有新的 H3/H4 证据不得提高结论等级。
4. SaveItem、生产者、sidecar 和 popup 任何一项仅有静态代码不得标记 P7 Overall 通过。

## 6. 当前 Host 测试索引与缺口

### 6.1 现有测试名（完整清单）

`test_host.cpp`：`test_p7_stage4_find_item_poc`、`test_json_escape`、`test_base64_decode`、`test_parse_int_field`、`test_tiles_parse`、`test_nav_bfs`、`test_nav_bfs_multi`、`test_stack_codec`、`test_stack_codec_s2`、`test_stack_codec_effective_mode`、`test_stack_codec_mode_invariance`、`test_sell_price_bounds`、`test_virtual_bag_state`、`test_extension_bag_exit_rendering_state`、`test_virtual_bag_payload_bridge`、`test_virtual_bag_base64`、`test_virtual_bag_payload_helpers`、`test_virtual_bag_merge_count`、`test_virtual_bag_mergeable_items`、`test_virtual_bag_mode_aware_ops`、`test_virtual_bag_json_roundtrip`、`test_virtual_bag_json_count_clamp`、`test_virtual_bag_cross_config_roundtrip`、`test_virtual_bag_legacy_json`、`test_virtual_bag_normalize_payload`、`test_virtual_bag_recovery`、`test_virtual_bag_transaction_domain`、`test_sell_divide_s2_read_window`、`test_adopt_merge_plan`、`test_ext2orig_host_guards`、`test_save_preflight_classify`、`test_save_preflight_stage`、`test_save_preflight_json`、`test_prepare_journal`、`test_ownership_ledger`、`test_ownership_ledger_p43`、`test_unequip_bag`、`test_equip_bag`、`test_p44_transaction_stages`、`test_p52_drag_session`、`test_p45_isolation`。

`test_inventory_hook_stage4.cpp`：`test_queries`、`test_original_only_queries`、`test_get_cumulate_count_s2`、`test_remove_item_data_plan`、`test_native_equip_sell_count`、`test_equip_sell_redirect_mapping`、`test_make_item_writeback_count`、`test_vanilla_sell_takeover`、`test_object_operations`、`test_unequip_to_inven`、`test_install_transaction`。

### 6.2 Host 测试缺口清单

* `test_projection_descriptor_object_sync`
* `test_virtual_bag_empty_slot_predicate`
* `test_equip_button_three_state`
* `test_confirm_use_token_and_projection_refresh`
* `test_original_to_extension_bag_equip_commit`
* `test_projected_equip_handover_and_rollback`
* `test_extension_jewel_atomic_consumption`
* `test_save_item_adopt_after_original_failure`
* `test_store_buy_capacity_restore`
* `test_extension_destroy_identity_and_popup_restore`
* `test_sell_price_variants_and_refund`
* `test_store_sell_projection_and_callback_isolation`
* `test_producer_save_item_original_first_and_adopt`
* `test_unpack_partial_product_ownership`
* `test_craft_product_failure_ownership`
* `test_view_window_bag_pair_restore`
* `test_detail_identity_and_proc_exclusion`
* `test_module_save_prepare_commit_boundary`
* `test_inventory_api_domain_and_error_envelope`
* `test_extension_to_original_handover_after_slot_verify`
* `test_original_to_extension_source_slot_postcondition`
* `test_ownership_active_pending_release`
* `test_stale_endpoint_rejection`
* `test_sell_price_variants`
* `test_projection_root_generation_gate`

缺口清单只登记测试契约和断言方向，不代表本批修改了测试源码。

## 7. 交叉册引用

本册不复制设计裁定：

* SaveItem 的 H-13 函数处置、caller 未覆盖边界，见
  [`inventory-integration-decision-plan.md §2.2、§3.2`](inventory-integration-decision-plan.md)。
* 规则编号以 [`rulebook.md`](rulebook.md) 的冻结 `R-01..R-55` 为准；本册只保留操作卡覆盖关系。
* `IsHavingEmptySlot` 的 `needed<=0` 返回 `1` 事实及源码/Host 锚，见库存册 §2.1；VM-02 只负责验收。
