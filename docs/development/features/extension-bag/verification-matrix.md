# 扩展背包验收矩阵

> **状态：CURRENT；本册是验收唯一权威。**
>
> 本册把当前规则册、运行时架构册、库存接入决策册、拖动协议和存档册交叉压成可执行的操作契约。源码行号按当前工作树记录；代码变更后必须重新核对文件:行。Host 测试名只引用当前存在的测试，`缺口：`表示本册登记的待补测试，不表示已经实现。
>
> **问题 B 固定口径：未解决+已布防。** 扩展袋 0 双非空交换后，原版袋 0 同号槽物品仍可能消失并被保存固化。F1 三态门、G 轮对象/回滚修正、H 轮单一 `0x18` owner、merge 解耦和 H3/H4 物理快照守卫均不是修复证明。

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
3. 任务袋 `5` 的任何扩展源/目标操作必须返回 `task bag excluded` 或等价拒绝，且袋 5 前后内容不变。
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
- **真机用例号(编号规范 VM-xx，写操作步骤+预期)**：`VM-04`：①打开扩展详情；②点需要确认的“使用”；③确认；④读取 inventory 和当前面板；预期扩展物品按原版效果消费，确认后无旧控件残留，原版袋无伪造槽。
- **证据锚类型**：真机必需 + 源码；不能由 Host 单独通过。

### VM-05 原版袋物品装备为扩展背包

- **操作**：将类别 1..4 的原版背包物品拖到未装备扩展标签，使其成为扩展袋对象。
- **原版链(VMA)**：原版 `TouchHandle` 的 drop `0x04`/控件 proc；无单一装备函数 VMA 可替代该 tab drop 链。
- **扩展接管点(文件:函数)**：`extension_bag_runtime.inc:extension_tab_item_proc` → `try_equip_on_extension_tab_drop_locked`；`extension_bag_equip.inc:equip_extension_bag_item_locked`。
- **共享状态读写**：读原版 source、目标 `types/capacities`；写扩展 descriptor/payload/object/handle/ownership、原版源槽清除、tab data 和 projection。
- **必须保持的不变式(引 R-xx)**：目标袋空且类别合法才收编；原版源确认清空后提交；源/目标不能误认角色装备（R-08、R-16、R-25、R-26）。
- **失败语义**：目标已装备、类别非法、容量不足、物化或源清除失败均拒绝并保持源；不得把失败降级成原版二次移动。
- **Host 测试名(现有或「缺口」)**：`test_equip_bag`、`test_virtual_bag_state`；缺口：`test_original_to_extension_bag_equip_commit`（断言源槽清空前不清逻辑源，失败可重试）。
- **真机用例号(编号规范 VM-xx，写操作步骤+预期)**：`VM-05`：①记录原版 bag/slot 的背包物品；②拖到未装备扩展标签；③读两侧 state 并切出/切回；预期扩展类型/容量正确、原版源空、切换后不显示旧袋物品。
- **证据锚类型**：真机 + Host 部分模型 + 源码。

### S-05 页签 drop 装备与跨页签提交

- **反例**：背包类物品拖到空页签无效果；拖到其它页签时源视觉消失但没有
  `txn committed`，目标无物品，切回源仍在。
- **步骤**：①记录扩展源的逻辑 `bag/slot/category/generation`；②将类别 1..4 的扩展
  物品拖到空页签；③再用非背包类物品或已装备页签拖到其它页签；④切回源页签并读取
  两个逻辑袋、projection 和 session 收尾日志。
- **预期**：空页签仅对 `is_backpack_category(category)` 成立的源走装备事务；其它目标
  走 ext→ext。target kind、session generation、tab/inventory generation 或 source
  不匹配时明确拒绝，逻辑源保持不变；只有事务成功才提交源/目标、持久化并刷新目标
  projection，`0x18` 只执行 cleanup-only 收尾。扩展源 drop 目标解析先判页签、后判
  网格；标签矩形与网格行的 y 坐标带存在重叠时，页签命中必须按切袋意图优先。
- **日志锚**：成功必须出现
  `tab commit handled=1 bag=.. slot=..`；失败必须出现
  `cross tab reject reason=...` 或 `tab reject reason=transaction_failed ...`，失败路径
  不得出现扩展 `txn committed`。
- **关联规则/真机卡**：R-02、R-16、R-19、R-25、R-30；本卡真机证据待补。

### VM-06 扩展物品装备到角色

- **操作**：从扩展投影详情将装备穿到当前菜单角色的目标装备槽。
- **原版链(VMA)**：`CHAR_EquipItemFromInvenToSlot@0xe5368` 读取物理 `INVEN[bag][slot]` 并交换。
- **扩展接管点(文件:函数)**：`native_inventory_hook.cpp:equip_item_from_inven_to_slot_wrapper` → `inventory_hook_stage4.cpp:stage4_equip_item` → `extension_bag_equip.inc:extension_bag_equip_projected_item`。
- **共享状态读写**：读当前菜单角色、view/window bag、descriptor/hash/handle；临时借入物理槽，写角色装备、旧装备扩展槽、ownership、generation 和 projection。
- **必须保持的不变式(引 R-xx)**：临时物化不改变最终所有权；失败恢复物理槽和角色槽；`module_item_locked` 使用逻辑 bag 与当前 view（R-04、R-08、R-16、R-24、R-26）。
- **失败语义**：角色/等级/职业/装备槽或物化不合法返回原版失败；扩展源失败不得把投影对象交给物理释放链。
- **Host 测试名(现有或「缺口」)**：`test_object_operations`（fallback 装备与角色指针断言）；缺口：`test_projected_equip_handover_and_rollback`。
- **真机用例号(编号规范 VM-xx，写操作步骤+预期)**：`VM-06`：①在角色 A 菜单打开扩展装备详情；②点击装备；③确认角色 A 装备和扩展源/旧装备位置；④切换角色 B 重复；预期角色身份不串、失败不丢物。
- **证据锚类型**：Host + 真机 + 源码。

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

### VM-10 宝石镶嵌

- **操作**：以原版或扩展宝石向原版/扩展装备镶嵌。
- **原版链(VMA)**：`ITEMSYSTEM_PutJewel@0x10bcb4`；成功后原版 UI/库存删除宝石。
- **扩展接管点(文件:函数)**：`native_inventory_hook.cpp:put_jewel_wrapper` → `inventory_hook_stage4.cpp:stage4_put_jewel`；装备槽拖放另经 `equip_control_event_proc_wrapper`（`0xb8f7c`）；API `game_inventory_equipment.inc:data_op_jewel` → `extension_bag_api_put_jewel_impl`。
- **共享状态读写**：读 jewel/equip 两端身份、socket 和 payload；写装备 payload、宝石 descriptor/count、object hash、handle、projection 和 dirty；装备槽拖放由 `ModuleUseToken` 关联消费。
- **必须保持的不变式(引 R-xx)**：两端分别识别 jewel/equip；扩展宝石不走 `RemoveItemDirect`；成功消费恰一次；扩展宝石→原版装备不得直调 `virtual_bag_put_jewel_native`，扩展宝石→扩展装备才由既有双端适配承接（R-09、R-16、R-25、R-31）。
- **失败语义**：无孔 `no socket`、非宝石 `not jewel`、空装备 `equip slot empty`；失败不消费宝石、不改变装备。
- **Host 测试名(现有或「缺口」)**：`test_object_operations`；断言原版 backup 结果保留、扩展两端 seam 只调用一次；`test_inventory_hook_stage4` 还断言目标装备槽 predicate 与 finish 失败必须走 token abort seam；缺口：`test_extension_jewel_atomic_consumption`。
- **真机用例号(编号规范 VM-xx，写操作步骤+预期)**：`VM-10`：①记录扩展宝石 count 和原版装备 socket；②拖放扩展宝石→原版装备并核对原版 payload；③拖放扩展宝石→扩展装备并核对扩展 payload；④对无孔、非宝石、空装备各重试；预期两条成功路径均只消费一次并更新 payload，失败保持两侧原状，成功路径无 `MoveItem GUARD` 日志。
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
- **必须保持的不变式(引 R-xx)**：任务袋 5 为源时拒绝；物理源清除必须删后复核；逻辑 bag 不能传进物理函数（R-02、R-21、R-22、R-25）。
- **失败语义**：目标非法/满、物化或物理删除复核失败回滚逻辑目标并保留原版源；原版源仍占用时不提交。
- **Host 测试名(现有或「缺口」)**：`test_p44_transaction_stages`、`test_virtual_bag_transaction_domain`；缺口：`test_original_to_extension_source_slot_postcondition`。
- **真机用例号(编号规范 VM-xx，写操作步骤+预期)**：`VM-15`：①记录原版 bag/slot payload；②POST `move_item` 到 bag 6..10；③重读原版和扩展；预期源真实空、扩展 payload 完整，bag 5 源操作返回 `task bag excluded`。
- **证据锚类型**：Host + API 真机。

### VM-16 装备页详情销毁

- **操作**：从装备页扩展详情打开销毁确认，确认或取消。
- **原版链(VMA)**：`UIEquip_ButtonDestroyExe@0xb6240` → popup `OKDestroyItem@0xb83d0` → 原版物理删除。
- **扩展接管点(文件:函数)**：`extension_bag_equip.inc:extension_destroy_ok`；详情按钮数组 PtrHook 安装/观察；装备页 popup OK/Cancel GOT 临时槽。
- **共享状态读写**：读详情 bag/slot/object/hash/handle；确认写逻辑清槽、释放、projection、dirty；成功/失败/取消恢复 popup 原 callback 和详情身份。
- **必须保持的不变式(引 R-xx)**：扩展对象不进原版 Direct remove；全局 popup 劫持只用于卖出/销毁，回调不可跨页串线（R-07、R-13、R-15、R-30）。
- **失败语义**：取消无状态变化；stale identity/释放失败拒绝；确认成功才清逻辑源和物化对象。
- **Host 测试名(现有或「缺口」)**：缺口：`test_extension_destroy_identity_and_popup_restore`（断言取消/成功/陈旧身份的槽、handle、callback 清理）。
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

### VM-19 商店购买收编

- **操作**：原版物理袋满、扩展有空槽时购买商店物品；覆盖原版有槽、全满、失败退款。
- **原版链(VMA)**：`UIStore_BuyItem@0xd242c` → 两处 `FindSaveSlot` callsite `0xd24a0/0xd2540` → `INVEN_SaveItem@0x104528`。
- **扩展接管点(文件:函数)**：`extension_bag_store.inc:store_buy_find_slot_gate`；`native_inventory_hook.cpp:save_item_wrapper` 在 backup 失败后 `extension_bag_adopt_native_item`。
- **共享状态读写**：读原版容量、扩展容量、商品和 money；写扩展 adopt、商品/货架状态、money、projection 和保存 dirty；商店投影写前恢复原版容量。
- **必须保持的不变式(引 R-xx)**：gate 只表达继续流程，不伪造扩展槽；SaveItem backup 前恢复容量；原版成功优先（R-05、R-11、R-26、R-27）。
- **失败语义**：物理或扩展均满返回原版满包；保存/扣款失败不留下扩展对象，退款失败显式错误；任务商品仍按任务语义。
- **Host 测试名(现有或「缺口」)**：缺口：`test_save_item_adopt_after_original_failure`、`test_store_buy_capacity_restore`（断言 backup 顺序、adopt 只发生于失败、容量恢复成对）。
- **真机用例号(编号规范 VM-xx，写操作步骤+预期)**：`VM-19`：①准备物理满/扩展有空位；②POST `/api/item/shop/buy_item`；③读取 money、inventory 和投影；④再测全满；预期第一种收编成功，第二种统一失败且无扣款/残留。
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
- **扩展接管点(文件:函数)**：`feature/patch/game_patch_craft.inc` 的上层旁路；`native_inventory_hook.cpp:save_item_wrapper` 只覆盖已创建对象入库失败。
- **共享状态读写**：读材料对象/category/count、产物临时对象和物理/扩展容量；写材料删除、产物 adopt、ledger 和 dirty；禁止把类别回滚当 UID 回滚。
- **必须保持的不变式(引 R-xx)**：不做全局 MIX Hook；材料删除前后校验，产物失败释放；无安全逆操作不添加猜测性回滚（R-07、R-15、R-22、R-25）。
- **失败语义**：材料不足/产物保存失败/扣款失败返回明确错误；已成功前序产物是否回滚必须按 caller 记录，不自动推导。
- **Host 测试名(现有或「缺口」)**：缺口：`test_craft_product_failure_ownership`（断言未入库产物释放、材料源校验、失败不伪造扩展提交）。
- **真机用例号(编号规范 VM-xx，写操作步骤+预期)**：`VM-22`：①记录材料 payload/count；②执行成功合成；③执行满包/失败合成；④检查材料、产物和日志；预期成功/失败所有权均可追溯，不能以一次成功覆盖五个 caller。
- **证据锚类型**：真机必需；阶段 5/P6 边界未闭合。

### VM-23 视图切换、页签和投影

- **操作**：进入、切换、退出扩展逻辑袋，覆盖重复点击、快速切换和退出中事件。
- **原版链(VMA)**：原版 inventory event、袋控件和 `TouchHandle`；投影安装不等同于原版背包写入。
- **扩展接管点(文件:函数)**：`extension_bag_runtime.inc:extension_tab_item_proc`、`install_extension_tab_buttons_locked`、`disable_extension_tab_buttons_locked`；`extension_bag_render.inc:install_module_view_locked/restore_module_view_locked`。
- **共享状态读写**：读写 `mode/selected/inspected`、`g_module_view_index`、`g_module_window_original_bag`、`g_projected_item_root`、tab pointers/generation 和 capacity snapshot。
- **必须保持的不变式(引 R-xx)**：view index 与窗口原版袋号分离；direct/GOT 成对恢复；root 重建先使旧事件失效（R-19、R-26、R-27、R-30）。
- **失败语义**：未就绪容器、无容量、退出中或 stale root 拒绝；原版袋 5 不可成为投影窗口；恢复失败不得释放借用对象。
- **Host 测试名(现有或「缺口」)**：`test_virtual_bag_state`、`test_extension_bag_exit_rendering_state`、`test_p52_drag_session`；缺口：`test_view_window_bag_pair_restore`。
- **真机用例号(编号规范 VM-xx，写操作步骤+预期)**：`VM-23`：①进入 bag 6/7/8；②快速来回切换；③退出到原版袋；④重开；预期只显示当前逻辑袋，退出后原版容量/direct/GOT/root 恢复，旧控件事件无效。
- **证据锚类型**：Host + 真机 + 源码。

### VM-24 详情弹窗与身份失效

- **操作**：打开原版/扩展物品详情，验证使用、装备、卖出、销毁按钮分流和 stale identity。
- **原版链(VMA)**：`UIEquip_MakeDesc@0xb8980`（唯一调用点 `0xb9188`）→ `SetDescMenu`→详情按钮 proc；装备/卸下 proc 走函数级 Hook。
- **扩展接管点(文件:函数)**：`extension_bag_lifecycle.inc:make_desc_equip_gate`；`extension_bag_equip.inc` 详情按钮 PtrHook 安装与 `extension_*_ok` 分流。
- **共享状态读写**：读当前 control/root、item/bag/slot/cache；写 `g_extension_desc_item`、details generation、button hooks、popup callback；重建时清旧指针。
- **必须保持的不变式(引 R-xx)**：操作前再次校验 item+bag+slot+cache；详情 PtrHook 跳过装备/卸下 proc；root 重建清 stale（R-14、R-15、R-24、R-30）。
- **失败语义**：详情生成失败/身份失效/按钮数组 stale 只清理本次身份，不执行原版删源或错误角色操作。
- **Host 测试名(现有或「缺口」)**：缺口：`test_detail_identity_and_proc_exclusion`（断言 stale identity 被拒、装备/卸下不双层进入、hook 清理完整）。
- **真机用例号(编号规范 VM-xx，写操作步骤+预期)**：`VM-24`：①打开扩展详情；②切袋/重建面板后继续点旧详情按钮；③分别操作原版与扩展详情；预期 stale 操作无副作用，扩展按钮只处理扩展对象。
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

## 2.1 VM-B 原版基线行为包

> VM-B 与规则册 B-01～B-05 一一对应。前置条件：扩展背包已启用；每张卡均使用非扩展
> 物品，并记录设备、APK SHA-256、配置开关、操作前后原版 `0..5×16` 物理槽摘要和
> 扩展逻辑状态。S-01～S-03 是本轮已知真机回归反例：VM-B 必须证明它们不再发生；
> B-04/B-05 另须证明原版袋内移动/合并没有被扩展路由污染。
>
> 本轮新增判读：draw-end 不再逐帧写投影控件；若出现非活 session、generation/root/view
> 不匹配或 source 控件失效的 stale moving，必须观察 `TouchState+0x30` 被清空及控件
> `+0x0a/+0x0b` flag 被清除。该判读对应 R-34，不替代 VM-B 的原版行为断言。

| 原版基线契约 | 对应真机卡 | 核心回归面 |
|---|---|---|
| B-01 | VM-B01 | 点击消耗品直接使用/消耗，不进入拖拽态 |
| B-02 | VM-B02 | 点击装备直装或替换，不进入拖拽态 |
| B-03 | VM-B03 | 宝石拖到装备执行镶嵌，不执行交换 |
| B-04 | VM-B04 | 原版袋内拖动保持原版移动语义 |
| B-05 | VM-B05 | 原版袋内合并保持原版堆叠语义 |

### VM-B01 原版消耗品点击使用

- **对应契约**：B-01；反例样本 S-01。
- **步骤**：①准备原版袋内非扩展消耗品并记录 bag/slot/count；②点击物品的“使用”，
  对需要确认的物品继续点击原版确认；③立即读取数量、物理槽和拖动 session 状态。
- **预期**：原版效果执行并按原版时机直接扣量/清槽；不产生 `0x17/0x19/0x18` 拖动
  session，不进入扩展移动事务；松手后无 moving/on 残留标志和幽灵拖拽；失败时物品数量和
  槽位保持原状。
- **日志锚**：`UIEquip_OKConfrimUseItem`（如有确认）、`INVEN_ConsumeItem`/`ConsumeItem`
  前后、`g_extension_drag_session` 无创建；不得出现扩展 `MoveItem GUARD reject`。

### VM-B02 原版装备点击直装/替换

- **对应契约**：B-02；反例样本 S-02。
- **步骤**：①准备原版袋内非扩展装备；②在角色装备槽为空时点击“装备”；③恢复原状后
  在同一槽已有装备时再次点击“装备”；④检查角色槽、原版袋和拖动 session。
- **预期**：空槽直接装备；已有装备按原版交换/替换并正确回包；两次点击均不进入拖拽态，
  不触发扩展装备事务，松手后无 moving/on 残留标志和幽灵拖拽，失败不清理错误源槽。
- **日志锚**：`UIEquip_ButtonEquipExe`、`CHAR_EquipItemFromInvenToSlot` 的调用前后；
  原版对象必须走 backup，不能出现 C1 扩展 source 拒绝或 H4 `backup=skipped`。

### VM-B03 原版宝石拖到装备镶嵌

- **对应契约**：B-03；反例样本 S-03。
- **步骤**：①记录原版宝石 count 和原版装备 socket/payload；②将宝石拖到有孔装备；
  ③分别对无孔、非宝石和空装备目标重试；④读取装备 payload、宝石数量、两侧槽位。
- **预期**：有孔时只执行一次原版镶嵌并消耗一颗宝石，装备不被交换；其余三类失败保持
  宝石、装备和槽位不变；松手后无 moving/on 残留标志和幽灵拖拽；不得创建扩展 swap/pending。
- **日志锚**：`ITEMSYSTEM_PutJewel`/`PutJewel` 返回值与前后 socket/count；成功路径不得
  以 `INVEN_MoveItem` 作为提交者，不得命中扩展 `MoveItem GUARD reject`。

### VM-B04 原版袋内拖动

- **对应契约**：B-04。
- **步骤**：①在原版袋 `0..5` 选取非扩展物品并记录 source/target；②拖到合法空槽；
  ③再拖到原版合法目标槽；④退出并重开原版视图，比较物理快照、扩展 state 和 projection。
- **预期**：source/target 只按原版移动语义变化；扩展逻辑槽、object/handle、pending、
  projection 不变；不发生扩展事务双提交、物理槽丢失或 stale 拖动 session。
- **日志锚**：原版对象的 `MoveItem pre/post` 或 passthrough；C1 必须放行原版 source，
  H4 不得输出 `extension_item=1`/`backup=skipped`；`orig_event pre/post` digest 只可
  反映本次预期原版移动。

### VM-B05 原版袋内合并

- **对应契约**：B-05。
- **步骤**：①准备同一原版袋内同 payload 的两个非扩展堆叠物；②拖动合并；③分别测试
  payload 不同、超过 stack limit 和跨袋目标；④读取 source/target count、物理摘要和
  扩展 pending/journal。
- **预期**：仅满足原版条件时按原版上限合并；其他情况按原版移动/交换或失败返回；不
  创建扩展 pending/journal，不修改扩展 object/handle，不因扩展开关改写原版合并结果。
- **日志锚**：`moveMergeEnabled` 配置、原版 `MoveItem pre/post`、`C1` 放行和 `C2`
  二次 proc guard；原版对象不得出现 H4 `backup=skipped`，无 `txn committed` 扩展日志。

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
  日志锚：`hook install OK ... count=16`、原版刷新调用前后及投影控件 `data[0]` 保持当前
  扩展对象；不应出现 draw-end 逐帧写控件，非活 stale moving 应出现清理日志并清除
  `TouchState+0x30` 与控件 flags。H-16 post-projection 是唯一刷新后覆盖职责。

## 3. 规则锚表

下表是 R-01..R-34 的反查表；“卡”列列出覆盖该规则的主卡。

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
| R-15 | VM-03、VM-04、VM-06、VM-16、VM-24 | 现有 ledger 无 active/pending token 专用断言，Host 缺口。
| R-16 | VM-06、VM-10、VM-12、VM-13 | `test_p44_transaction_stages` 只证明 descriptor 模型；VM-13 增加归一化 payload；问题 B 真机单独保留。
| R-17 | VM-06、VM-12、VM-14 | stale endpoint 当前无专用 Host；失败/回滚需真机与缺口。
| R-18 | VM-03、VM-25 | 锁序/刷新重入必须运行时观察，Host 只可做 seam。
| R-19 | VM-09、VM-11、VM-23 | `test_p52_drag_session` 有 stale generation；tab root 需真机。
| R-20 | VM-11、VM-12、VM-14、VM-25 | `test_virtual_bag_mergeable_items` 有快照纯函数；H3/H4 真机为主。
| R-21 | VM-01、VM-02、VM-14、VM-15、VM-26 | `test_virtual_bag_transaction_domain` 现成；页签 0..4→6..10 转换和 native item 判空由 VM-26/VM-15 取证。
| R-22 | VM-01、VM-02、VM-13、VM-20、VM-21、VM-22、VM-26 | `test_p7_stage4_find_item_poc` + 生产者逐 caller 真机。
| R-23 | VM-17、VM-18 | 价格 variant 可 Host，popup/钱回滚需真机。
| R-24 | VM-04、VM-06、VM-17、VM-24 | 菜单角色和详情身份只能真机闭合。
| R-25 | VM-03、VM-04、VM-08、VM-10、VM-14、VM-15、VM-17、VM-19、VM-25 | `test_p44_transaction_stages`/journal Host + 保存真机。
| R-26 | VM-02、VM-05、VM-06、VM-19、VM-23、VM-25 | `test_extension_bag_exit_rendering_state` 部分覆盖；view/window 真机。
| R-27 | VM-23、VM-25 | direct/GOT 成对恢复必须真机日志确认。
| R-28 | VM-12、VM-13 | `test_virtual_bag_mergeable_items` 现成，问题 B 仍未解决。
| R-29 | VM-13、VM-14、VM-25 | `test_p44_transaction_stages` 覆盖 pending/journal 域模型。
| R-30 | VM-07、VM-11、VM-16、VM-23、VM-24 | `test_p52_drag_session` 覆盖 generation；root/control 仍需缺口和真机。
| R-31 | VM-10、VM-11、VM-12、VM-13、VM-27 | `test_inventory_hook_stage4` 覆盖源判据；Native `MoveItem GUARD reject` 与装备槽 proc 日志仍需真机。
| R-32 | VM-B01～VM-B05 | H-16 安装日志 `count=16` 与 raw-original grep 锚已补；原版刷新后的扩展投影保持不被顶掉仍需 VM-B01～B05 真机行为证据。
| R-33 | VM-28 | 五个吞掉 `0x18` 出口统一经 `complete_original_release_cleanup_locked`；handled/unhandled 变体和锁外原版清理需真机日志确认。
| R-34 | VM-B01～VM-B05 | Host 可静态核对六条件表达式；真机需确认 draw-end 不逐帧写控件、非活 stale moving 被清理；当前待补。 |

### 3.1 16 条原无专门锚规则的定锚方案

规则册登记的无锚集合为 R-01、03、05、06、11、12、13、14、15、17、18、23、24、26、27、30、31。下表明确“可 Host/可 VM/不可锚”，不写实现方案。

| 规则 | 定锚结果 | 测试名或 VM 用例；断言要点 |
|---|---|---|
| R-01 | 可 VM | VM-04；确认成功后扩展控件不残留旧对象、详情身份清理、消费数量与最新 state 一致。
| R-03 | 可 Host | 缺口 `test_projection_descriptor_object_sync`；断言 descriptor、object、hash、handle 和 control `data[0]` 五者在消费/交换/清槽后相同。
| R-05 | 可 Host | 缺口 `test_virtual_bag_empty_slot_predicate`；断言派生 capacity、descriptor、object/token 不可分配条件共同决定结果，bag 5 不被计为扩展目标。
| R-06 | 可 Host | 缺口 `test_equip_button_three_state`；断言 Handled/Blocked/NotExtension 分别为不 backup/不 backup/一次 backup。
| R-11 | 可 VM | VM-19；商店投影开启时 SaveItem backup 前原版容量恢复，买入不落真实容量外槽。
| R-12 | 可 VM | VM-18；扩展卖出后剩余商店列仍是商店 projection，原版刷新不覆盖扩展项。
| R-13 | 可 VM | VM-16→VM-18；装备页与商店页交替确认后，OK/Cancel callback 与详情身份各自恢复。
| R-14 | 可 VM | VM-24；装备/卸下 proc 不经详情观察 PtrHook 重复处理，使用/卖出/销毁仍各执行一次。
| R-15 | 可 Host | 缺口 `test_ownership_active_pending_release`；断言 active/pending_release 对象不能替换、释放或复用，匹配 token finish 后才可终态释放。
| R-17 | 可 Host | 缺口 `test_stale_endpoint_rejection`；断言非空 cache category/hash 与 descriptor 不符时先拒绝，旧对象仍可审计回滚。
| R-18 | 可 VM | VM-25；保存/投影同步不递归死锁、不将错误锁序造成的刷新写入当成功。
| R-23 | 可 Host | 缺口 `test_sell_price_variants`；断言装备页和商店页传入不同 variant，价格结果互不污染。
| R-24 | 可 VM | VM-04、VM-06；角色 B 菜单操作不修改角色 A，确认使用和装备均按当前菜单角色。
| R-26 | 可 VM | VM-23；切袋/装备收尾后容量来自当前 view，窗口原版 bag 只承担临时投影字段。
| R-27 | 可 VM | VM-23、VM-25；进入/退出/保存前后 direct 与 GOT 袋号相等且恢复为原值。
| R-30 | 可 Host | 缺口 `test_projection_root_generation_gate`；断言 root 重建递增 generation、旧 root/source control 被拒、新 root 控件才可提交。
| R-31 | 可 VM | VM-27；扩展对象命中 guard 必须记录完整参数/身份/物理摘要并跳过 backup，原版对象必须保持原版结果。

**定锚分布：可 Host 7 条（R-03/05/06/15/17/23/30），可 VM 10 条（R-01/11/12/13/14/18/24/26/27/31），不可锚 0 条。** “可 Host”中的 7 个测试名均为缺口，不把缺口写成已通过。

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
| 出现 `ERROR MoveItem GUARD reject ... backup=skipped` | 扩展对象到达 `INVEN_MoveItem`，被第 14 个 Hook 在物理写入前阻断 | 回查此前 event/source/session 路由；该次不再改物理槽，但不代表问题 B 根因已修复。
| 出现 `MoveItem pre`/`post` 且对象为原版 | 原版对象按 original-first 调 backup；摘要可直接比较该函数前后 | digest 或源/目标指针变化时将 `INVEN_MoveItem` 作为确认写点；无变化则继续查其他候选。
| 只有 `MoveItem passthrough`，或完全没有 MoveItem 日志 | 前者是不在扩展 view/session 的原版低频路径，后者表示写点绕过该 Hook 或日志窗口缺失 | 继续核对 raw `SaveItemOnEmpty`、未包裹 event 和 caller；不能按“无 guard 日志”判安全。

### 4.3 当前预期结果

`VM-12` 的当前预期失败是**已知未解决**：允许观察到原版同号槽消失，但必须留下完整 H3/H4 取证。F1/G/H 是已布防而非已修复；Host 的 `test_p44_transaction_stages`、`test_p52_drag_session`、`test_virtual_bag_mergeable_items` 不能替代该真机结论。

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
| `event`/`proc`/Hook 安装层 | **VM-B01、VM-B02、VM-B03、VM-B04、VM-B05 全跑**；不得只跑受影响的修复卡 |

**硬规则**：diff 触及 event、proc 或 Hook 安装层任一层时，必须全跑 VM-B；只跑修复闭环
或只依赖静态审查，不构成回归通过。

### 5.3 通过条件

1. Host 相关测试全绿，且 `git diff --check` 无错误。
2. 真机记录 build identity、设备、配置、操作前后状态、日志起止和用户 verdict；API 结论不能冒充物理触摸结论。
3. 任何涉及 VM-12 的改动都保留“未解决+已布防”，没有新的 H3/H4 证据不得提高结论等级。
4. SaveItem、生产者、sidecar 和 popup 任何一项仅有静态代码不得标记 P7 Overall 通过。

## 6. 当前 Host 测试索引与缺口

### 6.1 现有测试名（完整清单）

`test_host.cpp`：`test_p7_stage4_find_item_poc`、`test_json_escape`、`test_base64_decode`、`test_parse_int_field`、`test_tiles_parse`、`test_nav_bfs`、`test_nav_bfs_multi`、`test_stack_codec`、`test_virtual_bag_state`、`test_extension_bag_exit_rendering_state`、`test_virtual_bag_payload_bridge`、`test_virtual_bag_base64`、`test_virtual_bag_payload_helpers`、`test_virtual_bag_merge_count`、`test_virtual_bag_mergeable_items`、`test_virtual_bag_json_roundtrip`、`test_virtual_bag_legacy_json`、`test_virtual_bag_normalize_payload`、`test_virtual_bag_recovery`、`test_virtual_bag_transaction_domain`、`test_save_preflight_classify`、`test_save_preflight_stage`、`test_save_preflight_json`、`test_prepare_journal`、`test_ownership_ledger`、`test_ownership_ledger_p43`、`test_unequip_bag`、`test_equip_bag`、`test_p44_transaction_stages`、`test_p52_drag_session`、`test_p45_isolation`。

`test_inventory_hook_stage4.cpp`：`test_queries`、`test_object_operations`、`test_unequip_to_inven`、`test_install_transaction`。

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

* SaveItem 的 H-13 函数处置、旧 stage 裁定迁移和 caller 未覆盖边界，见
  [`inventory-integration-decision-plan.md §2.2、§3.2`](inventory-integration-decision-plan.md)。
* 拖动规则编号以 [`rulebook.md`](rulebook.md) 的冻结 `R-01..R-34` 为准；本册只保留操作卡覆盖关系。
* `IsHavingEmptySlot` 的 `needed<=0` 返回 `1` 事实及源码/Host 锚，见库存册 §2.1；VM-02 只负责验收。
