# 扩展背包拖拽、投影与交互协议（触摸链事实册）

> **文档职责**：本册是扩展背包触摸输入链的事实文档，覆盖四件事——原版如何实现
> （§1）、模块如何实现（§2）、模块改动哪些位置与侵入面（§3）、实现目标与验证状态
> （§4）。裁定结论与理由集中在 §5，不与 §1–§4 的事实混排。
> 状态总览归 `control-plane.md`，不变量和锁纪律归 `rulebook.md`，事务持久化归
> `module-save-store.md`。
>
> **基线**：
> - 反汇编事实出处为归档基线 `archive/extension-bag/touch-baseline-20260909/
>   libgame_ui_0xa3000-0xba200.txt`（本册缩写 **UI**）与
>   `archive/extension-bag/touch-baseline-20260909/libgame_inven_0x103000-0x105000.txt`
>   （本册缩写 **INVEN**），引用格式 `UI:行号` / `INVEN:行号`；VMA 只在基线文件内核对。
> - 模块代码行号按本次成稿时工作树核实（commit `56a477b` 之后的工作树）；后续修改代码
>   后必须重新核对全部 `文件:行` 锚点。
> - 规则交叉引用以规则册已冻结的 `R-01..R-34` 为准；本册不得另行定义编号。

## 1. 原版触摸输入事实基线

### 1.1 入口、出口与调用链

原版触摸链的唯一事件入口是 `TouchHandle_Event@0xa380c`（UI:557），参数
`(x0=root, x1=event, x2=param, x3=param2)`。按事件分支：

```text
触摸驱动 → TouchHandle_Event@0xa380c (UI:557)
  ├─ event 0x17 press   → 复制坐标入 TouchState → ControlObject_EventProc@0x9e244 (UI:687)
  ├─ event 0x19 move    → TouchHandle_Move@0xa2ec0 → 0x19 派发 ControlObject_EventProc (UI:643/652)
  │                       → TouchHandle_MoveOut@0xa3060 (UI:658)
  └─ event 0x18 release → ControlObject_EventProc@0x9e244 (UI:613)
                          → 复制 param 三字段入 TouchState+0x18/0x20/0x28 (UI:616-620)
                          → TouchHandle_ResetMovingControl@0xa371c (UI:621)
                          → 清 TouchState+0x48 (UI:622)
                          → 按 proc 返回值分支 TouchHandle_ResetSelectedControl@0xa3414 (UI:627)
```

`ControlObject_EventProc@0x9e244` 在基线区间 `0xa3000-0xba200` 之外，仅作为调用目标
出现（UI:582、602、613、652、687）。它按控件树把事件派发到各控件 proc；背包页三个
proc 的符号与 VMA：

| proc | VMA | 基线行 |
|---|---|---|
| `UIEquip_InvenItemControlEventProc`（物品格） | `0xb911c` | UI:23719 |
| `UIEquip_EquipControlEventProc`（装备槽） | `0xb8f7c` | UI:23613 |
| `UIEquip_InvenBagControlEventProc`（页签/袋） | `0xb89d0` | UI:23244 |

### 1.2 TouchState 全局状态字段

`TouchHandle` 全局状态位于 `0x301000+0xcf8`，字段常量唯一来源为
`game_symbols.h:271-283`（`G_TOUCH_STATE_VMA`、`TOUCH_STATE_*`）。release 输入坐标是
`+0x18`（x）、`+0x20`（y）、`+0x28`（第三字段）三处独立字段（UI:616-620 写入，
UI:521-522 读取）；release 结果控件对在 `+0x50/+0x58/+0x60..+0x70`
（UI:295-311 `TouchHandle_SetReleaseEvent`）。

### 1.3 press（0x17）

`TouchHandle_Event` 对 `0x17` 的处理（UI:631-636、682-706）：

1. 把 `param` 指向的三个字复制到 TouchState+0x18 起的连续字段（UI:682-686，VMA
   `a39fc-a3a0c`）。
2. 调 `ControlObject_EventProc` 派发 0x17（UI:687，VMA `a3a10`）。
3. 返回值分支：返回 1 → 把 TouchState+0x40 复制到 +0x30（激活控件，UI:697-699）；
   返回 2 → 把 +0x40 复制到 +0x48（UI:692-696）；其余 → 以事件码 `0x81` 调
   `TouchHandle_SetSelectedControl@0xa31a8`（UI:700-706）。

### 1.4 move（0x19）

`0x19` 分支（UI:571-572、593-597、637-672）：

1. TouchState+0x30（moving 控件）为空 → 走通用分支向树派发 `0xf000006`（UI:573-592）。
2. 非空 → 把 TouchState+0x40 复制到 +0x30（UI:642），调 `TouchHandle_Move@0xa2ec0`
   （UI:643），随后以 `0x19` 派发 `ControlObject_EventProc`（UI:652），再调
   `TouchHandle_MoveOut@0xa3060`（UI:658）。

### 1.5 release（0x18）五步清理

`0x18` 分支（UI:612-630）依次执行，本册称「原版 release 五步清理」：

| 步 | 动作 | 基线 |
|---|---|---|
| 1 | 派发 `ControlObject_EventProc@0x9e244`（携带 release 参数） | UI:613 |
| 2 | 把 param 三字段复制到 TouchState+0x18/+0x20/+0x28 | UI:616-620 |
| 3 | `TouchHandle_ResetMovingControl@0xa371c`：对 moving 控件派发 `0x08`、恢复相对坐标、清 +0x00..+0x10/+0x30/+0x38 | UI:621；函数体 UI:493-545 |
| 4 | 清 TouchState+0x48 | UI:622 |
| 5 | proc 返回值 `−1 ≤ 1`（无符号比较）→ 返回 1；否则 `TouchHandle_ResetSelectedControl@0xa3414` 后按非零返回 | UI:623-630 |

`TouchHandle_ResetMovingControl@0xa371c`（UI:493-545）内部：moving 控件的
`ControlObject_GetControlEventType` 第 3 位（`0x08`）置位时，先把 TouchState+0x18/+0x20
复制到 +0x60/+0x68、+0x30/+0x38/+0x28 复制到 +0x50/+0x58/+0x70（UI:515-527），再对
moving 控件派发 `x1=0x08`（UI:532-534）；派发返回值非 1 时调
`ControlObject_GetRelativePointFromXY@0x9e718`（UI:540）与
`ControlObject_SetRelativePoint@0x9e928`（UI:544）。函数收尾无条件清
+0x00..+0x10、+0x30、+0x38（UI:506-511）。
`TouchHandle_ResetSelectedControl@0xa3414`（UI:281-288）是到
`TouchHandle_SetSelectedControl@0xa31a8` 的尾调用（参数 `(0, 全局选中槽, 0x80)`）。

### 1.6 起拖判定与 moving 标志

`UIEquip_Process@0xb7964`（UI:22165-22204）在每帧处理中起拖：

1. `TouchHandle_GetMovingControl@0xa3704`（UI:22169，函数体 UI:483-486）非空，且
   `TouchHandle_IsControlEventMove@0xa3cf0`（UI:22172，函数体 UI:888 起）为真。
2. `ControlItem_GetMoving@0xaae88`（UI:22179，函数体 UI:8722-8728）为假（尚未拖动）。
3. `TouchHandle_GetControlOffset@0xa3190`（UI:22182）返回偏移，`MATH_Abs@0xa8b0c` 后
   x 或 y 位移 `> 5`（UI:22184-22191）。
4. 保存 slot index（UI:22195-22198），调 `ContorlItem_SetMoving(ctrl, 1)@0xaae64`
   （UI:22201），再 `UIDesc_SetOff@0xb2b48`（UI:22204）。

`ContorlItem_SetMoving@0xaae64`（UI:8711-8720）实现为向控件 data 的 `+0xa` 字节写
标志（UI:8717）；`ControlItem_GetMoving` 读同一位（UI:8726）。

### 1.7 控件 proc 事件码语义

**物品格 `UIEquip_InvenItemControlEventProc@0xb911c`**（UI:23719-23919）：

| 事件 | 行为 | 基线 |
|---|---|---|
| `0x08`、`0x02` | `ContorlItem_SetMoving(ctrl, 0)` 清 moving 标志，返回 1 | UI:23731-23732、23755-23756、23769-23772 |
| `0x04` | 取 param+8 源控件物品与本控件目标物品（UI:23780-23783）；`UIEquip_IsApplyStuff@0xb8d4c` 为真走镶嵌/附魔（UI:23787-23791、23900-23907），为真且 `SAVE_IsOK@0x128c14` 失败弹窗（UI:23913-23918）；为假清 drag 数组槽后调 `INVEN_MoveItem@0x104934`（UI:23792-23801、23815），成功后播声、恢复数组槽并 `UIEquip_RefreshItemArea@0xb7a00`（UI:23816-23861） | UI:23757-23919 |
| `0x01` | `UIDesc_IsOn@0xb2b9c` 为真返回 1，否则 `UIEquip_MakeDesc@0xb8980` | UI:23759-23760、23862-23872、23895-23898 |
| `0x10` | `ControlItem_GetOn@0xaaec4` 为假时 `ContorlItem_SetOn(ctrl,1)`，返回 1（拖动源登记） | UI:23873-23883 |
| `0x80` | 写全局字节后 `UIEquip_MakeDesc`（详情打开） | UI:23740-23747 |
| `0x40` / `0x20` | 返回 1 / `ContorlItem_SetOn(ctrl,0)` 返回 0 | UI:23737-23739、23884-23894 |

**装备槽 `UIEquip_EquipControlEventProc@0xb8f7c`**（UI:23613-23717）：

| 事件 | 行为 | 基线 |
|---|---|---|
| `0x08`、`0x02` | 返回 1 | UI:23623-23624、23634、23639-23640 |
| `0x04` | 源/目标物品均非空且 `UIEquip_IsApplyStuff`（UI:23650-23660）→ `SAVE_IsOK`（UI:23661）→ `UIEquip_ApplyStuff@0xb8df8`（UI:23666）→ `UIEquip_UpdateCharEquip@0xb7784`（UI:23667）→ `UIEquip_RefreshItemArea`（UI:23668）→ `TouchHandle_SetCursor`（UI:23672），返回 0 | UI:23641-23674 |
| `0x80` / `0x01` / `0x10` / `0x20` | MakeDesc / UIDesc 门 / SetOn / SetOn(0) | UI:23675-23710 |

`UIEquip_ApplyStuff@0xb8df8`（UI:23514-23611）按物品类型分流到
`ITEMSYSTEM_EnchantItem@0x10b330`（UI:23531）或 `ITEMSYSTEM_PutJewel@0x10bcb4`
（UI:23549）；`UIEquip_IsApplyStuff@0xb8d4c`（UI:23469-23503）依次测
`ITEMSYSTEM_IsEnchantScroll@0x10b2f0`、`ITEMSYSTEM_IsJewel@0x10b964`、
`ITEMSYSTEM_IsRestoreChaos@0x10be44`，宝石再经 `ITEMSYSTEM_CanPutJewel@0x10d328`
（UI:23501）。

**页签/袋 `UIEquip_InvenBagControlEventProc@0xb89d0`**（UI:23244-23467）：

| 事件 | 行为 | 基线 |
|---|---|---|
| `0x08`、`0x100`、`0x10`、`0x01` | 返回 1 | UI:23253、23259-23262、23275-23284 |
| `0x02` | `UIEquip_GetBagSlotIndex@0xb7868` → 容量≤0 返回 1；写全局当前袋、`UIEquip_RefreshItemArea`（切袋），返回 1；同袋则 MakeDesc | UI:23271-23272、23393-23427 |
| `0x04` | 目标袋 = 当前袋或袋 `5` → 返回 0（UI:23285-23293）；源物品为空返回 0（UI:23294-23300）；容量/占位判定后调 `INVEN_SaveItemOnEmpty@0x104be0`（UI:23433），成功则清源数组槽、`UIEquip_RefreshBagArea@0xb78bc` + `UIEquip_RefreshItemArea`（UI:23385-23391），返回 0 | UI:23273-23467 |
| `0x80` | `UIDesc_SetOff@0xb2b48`，返回 1 | UI:23257-23258、23411-23419 |

### 1.8 TouchHandle 层的 drop 派发与数据提交点

release 时 `TouchHandle_SetReleaseEvent@0xa343c`（UI:295-380）按 drop 目标控件类型
向目标控件 proc 派发事件：类型含 `0x30` 位 → `x1=0x04`（UI:347-351）；含第 1 位 →
`x1=0x02`（UI:376-379）；派发后清 TouchState+0x30、+0x38（UI:333-336）。
`TouchHandle_ControlEventProc@0xa3590`（UI:382-448）在 `0x18` 且点中控件时调
`TouchHandle_SetReleaseEvent`（UI:444-448）。

数据提交点汇总（全部在控件 proc 内）：

| drop 目标 | 提交函数 | VMA | 基线 |
|---|---|---|---|
| 物品格 | `INVEN_MoveItem@0x104934`（由 item proc `0x04` 调用） | `0x104934` | UI:23815；INVEN:1667 |
| 页签/袋 | `INVEN_SaveItemOnEmpty@0x104be0`（由 bag proc `0x04` 调用） | `0x104be0` | UI:23433；INVEN:1840 |
| 装备槽 | `UIEquip_ApplyStuff@0xb8df8` → `ITEMSYSTEM_PutJewel@0x10bcb4` / `ITEMSYSTEM_EnchantItem@0x10b330` | `0xb8df8` 等 | UI:23666、23531、23549 |

## 2. 模块实现

### 2.1 触摸相关 hook 分层总表

| 层 | 目标 | 模块实现 | 代码锚 |
|---|---|---|---|
| 事件入口 | 库存场景 state entry `+0x38`（原 `F_SCENE_EVENT_EQUIP_VMA`） | 保存原值到 `g_orig_event` 后覆盖为 `virtual_bag_event`；`+0x28`→`virtual_bag_f3_wrapper`、`+0x10`→`virtual_bag_inventory_enter_wrapper` | `extension_bag_lifecycle.inc:518-532`（读取）、`815-820`（覆盖） |
| GOT 替换 | `G_UIEQUIP_INVEN_ITEM_PROC_GOT_VMA`（item proc 槽） | `ui_equip_inven_item_proc_wrapper`；安装条件 `g_move_merge_requested || g_extension_source_protection_requested`，含卸载路径 | `game_patch_move_merge.inc:122-163`、`165-187` |
| H-14 | `INVEN_MoveItem@0x104934` Native Hook | `move_item_wrapper`：扩展身份拒绝+取证，原版对象 original-first | `native_inventory_hook.cpp:238-285`（wrapper）、`553-556`（安装）；`rulebook.md` R-31 |
| H-15 | `UIEquip_EquipControlEventProc@0xb8f7c` Native Hook | `equip_control_event_proc_wrapper`：扩展宝石源校验后放锁调原版 proc | `native_inventory_hook.cpp:359-369`（wrapper）、`557-560`（安装）；`game_ui_virtbag.cpp:248-388`（实现） |
| H-16 | `UIEquip_RefreshItemArea` Native Hook | `refresh_item_area_wrapper`：depth 门 + trampoline + `virtual_bag_refresh_item_area_with_gate` | `native_inventory_hook.cpp:205-213`（wrapper）、`561-564`（安装）；`game_ui_virtbag.cpp:237-246`（gate）；`rulebook.md` R-32 |
| 指令 patch | bag proc `0x04` 内 `bl INVEN_SaveItemOnEmpty`（`g_base+0xb8cc0`） | 替换为 `bl save_item_on_empty_gate`；原字 `0x94012fc8` | `extension_bag_lifecycle.inc:692-730`；gate 实现 `extension_bag_render.inc:431-481` |
| 指令 patch | item proc `0x80` 内 `bl UIEquip_MakeDesc`（`F_UIEQUIP_ITEM_DESC_MAKE_DESC_CALL_VMA`） | 替换为 `bl make_desc_equip_gate`；原字 `0x97fffdfe` | `extension_bag_lifecycle.inc:772-814` |

Native Hook 常驻清单 H-01..H-16 共 16 个且不增不减，编号正文归 `rulebook.md`
§3.1（`rulebook.md:233-253`）；其安装目标校验、失败回滚和安装日志在
`native_inventory_hook.cpp:408-585`（目标校验 `452-469`，回滚 `384-406`）。

### 2.2 `virtual_bag_event` 事件生命周期

入口 `virtual_bag_event(uint64_t event, uint64_t param, uint64_t param2)` 在
`extension_bag_lifecycle.inc:203-508`。

**bypass 段（非启用或非世界态）**（`extension_bag_lifecycle.inc:211-253`）：
扩展未启用或 `gamestate != 0` 时，活动 session 的 `0x18` 先取坐标（参数为空则回读
TouchState+0x60/+0x68）并取消 session，随后以 unhandled 变体执行
`complete_original_release_cleanup_locked` 并返回 1（`212-240`）；其余事件原样经
`call_original_event_with_inventory_guard` 放行（`246`）。

**0x17 press**（`extension_bag_lifecycle.inc:257-324`）：

1. 读 press 坐标（`257-259`）。
2. 旧 capture 活动时吞掉 press（`274-278`）。
3. 投影槽命中且控件有效时建立 projected session（`285-293`、`305-307`，调
   `begin_projected_drag_session_locked`，`extension_bag_drag_session.inc:1-11`）。
4. 扩展网格命中但无投影物品时置 `g_extension_touch_capture` 并吞掉（`294-304`）。
5. 点中原版袋按钮时恢复投影、进入 `kExitingModule`，标记 `exiting_to_original`
   （`308-323`）。
6. 其余放行原版（`451-507`），原版返回后 `0x17/0x19` 只推进 session
   （`481`，调 `advance_projected_drag_session_locked`，`extension_bag_drag_session.inc:13-18`）。

**0x19 move**（`extension_bag_lifecycle.inc:409-431`）：读坐标（参数为空回读
TouchState，`410-417`）；capture 活动时只更新旧拖动态坐标并吞掉（`420-430`）；其余
放行原版，无事务动作。

**0x18 release 五出口**（`extension_bag_lifecycle.inc:325-408`）：
owner 判定 `virtual_bag::projected_release_owns_event`（`event == 0x18 且 session 有效`，
`model/virtual_bag_transaction_rules.inc:178-180`）成立时，`consume_projected_release_locked`
在原版调用前路由事务并返回 1（`340-347`；实现 `extension_bag_lifecycle.inc:169-201`；
目标路由 `extension_bag_transaction.inc:310-350` 与 `796-862`）。五个吞掉出口与清理
变体：

| 出口 | 清理变体 | 代码锚 |
|---|---|---|
| owner/terminal consume | handled（跳过 selected 清理） | `extension_bag_lifecycle.inc:340-347`；terminal 消费 `169-180` |
| 非世界态取消 | unhandled（执行 selected 清理） | `extension_bag_lifecycle.inc:212-240` |
| capture 空坐标 | unhandled | `extension_bag_lifecycle.inc:353-360` |
| capture click / capture drop | click=unhandled；drop 按 `handle_bag_drop_release_locked` 返回值 | `extension_bag_lifecycle.inc:361-389` |
| 原版袋 drop（kOriginal 模式） | handled | `extension_bag_lifecycle.inc:390-397` |

所有出口统一经 `complete_original_release_cleanup_locked` 收尾并返回 1
（`399-402`）。

#### 2.2.1 页签 drop 路由（R1/R2）

- 扩展源 release 的目标解析顺序固定为先遍历 `extension_tab_hit(index,x,y)`，再解析
  扩展网格槽；现有标签矩形与网格行在 y 坐标带上有重叠，因此页签命中优先表达切袋意图，
  不得被有效格子命中抢先消费。
- 空扩展页签且源类别为背包类时，类别统一由 `is_backpack_category(category)` 判定；
  投影扩展源从 session 的 `source_bag/source_slot` 读取，不再依赖 release 前可能已清掉
  moving flag 的控件。装备事务复用 `equip_extension_source_on_tab_locked`。
- 其它页签目标走 `move_extension_to_extension_locked`。路由在 target kind、session
  generation、tab/inventory generation 和 source session 门禁后才 claim transaction；
  任一门禁或事务失败都保留逻辑源，不降级到原版移动链。
- 事务成功的顺序固定为：事务内部提交逻辑源/目标 → 持久化 → 刷新目标 projection；
  `0x18` 仍由 projected owner 吞掉，并以 cleanup-only 变体完成原版 release 清理。
- 真机判读日志锚：成功为
  `tab commit handled=1 bag=.. slot=.. target_bag=..`；门禁失败为
  `cross tab reject reason=...`；事务失败为 `tab reject reason=transaction_failed ...`。

**capture 吞并其余事件**（`extension_bag_lifecycle.inc:432-450`）：capture 活动期间
非 `0x17/0x18/0x19` 事件清拖动态并吞掉（`432-441`），其余事件直接吞掉（`442-450`）。

### 2.3 release 等价清理（两段式）

`complete_original_release_cleanup_locked` 在 `extension_bag_input.inc:34-71`：

1. 锁内写 release 输入坐标到 TouchState+0x18/+0x20/+0x28，空则补采 moving 控件
   （`37-49`）。
2. 放锁调 `fn_touch_handle_reset_moving_control`（对应
   `TouchHandle_ResetMovingControl@0xa371c`，含 `0x08` 派发与相对坐标恢复），
   `handled=false` 变体再调 `fn_touch_handle_reset_selected_control`（`51-61`）。
3. 回锁清 TouchState+0x48（`TOUCH_STATE_DROP_EVENT`）并输出
   `release_cleanup ctl=... flags_cleared`（`63-70`）。

事件派发与 selected 清理不持 `g_virtual_bag_mtx` 的依据与范围见 `rulebook.md`
R-33（`rulebook.md:552-562`）。

### 2.4 session 纯模型

`DragPhase`、`DragTargetKind` 与状态迁移 helper 定义在
`model/virtual_bag_drag_model.inc`（`DragPhase` 第 1 行、`DragTargetKind` 第 28 行、
`session_begin` 81、`session_on_native_moving` 103、`session_resolve_target` 115、
`session_begin_transaction` 137、`session_finish_transaction` 151、
`session_claim_transaction` 194、`session_on_cancel` 174）；五态事务核心
`TxnStage`/`TransactionContext` 在 `model/virtual_bag_transaction_rules.inc:222-245`。
host 测试覆盖为 `tests/test_host.cpp` 的 `test_p52_drag_session`（引用见
`rulebook.md:339`）。

### 2.5 H-14 / H-15 / H-16 wrapper 细节

**H-14 `move_item_wrapper`**（`native_inventory_hook.cpp:238-285`）：锁内经
`virtual_bag_capture_move_item_observation`（`extension_bag_public_runtime.inc:75`）
采集身份与物理摘要，随即放锁；扩展对象且非装备交换例外时记录
`MoveItem GUARD reject` 并返回 0、不调 backup（`247-261`）；原版对象记录 pre/post 或
passthrough 后调 backup（`263-284`）。

**H-15 `virtual_bag_handle_equip_control_event`**（`game_ui_virtbag.cpp:248-388`）：
仅 `0x04` 参与（`252-254`）；锁内校验扩展宝石源、descriptor、session 与 token
（`262-309`），失败 Blocked 并取消 session；放锁调原版 proc（`320`）；回锁 finish
（`326-341`）；成功路径把 session 推进到 Committed（`356-381`）。wrapper 返回值分流
在 `native_inventory_hook.cpp:359-369`。

**H-16 `refresh_item_area_wrapper`**（`native_inventory_hook.cpp:205-213`）：
`g_refresh_depth > 0` 时直达 `inventory_native_hook_call_refresh_item_area_original`
（`589-593`）；否则 depth+1 后进 `virtual_bag_refresh_item_area_with_gate`
（`game_ui_virtbag.cpp:237-246`）：锁内 trampoline 调原版刷新，投影安装且非
restore 抑制时 `refresh_projection_if_overwritten_locked`。
`restore_module_view_locked` 通过 `RefreshRestoreSuppressScope` 设置同线程抑制
（`extension_bag_render.inc:569-579`、`581-612`）。模块内部主动刷新点统一走
raw-original dispatcher `inventory_native_hook_call_refresh_item_area_original`
（`native_inventory_hook.cpp:589-593`；规则 `rulebook.md` R-32）。

### 2.6 指令 patch 的安装与恢复

全部 BL 指令 patch 的安装集中在 `inject_locked`（`extension_bag_lifecycle.inc:510-820`）：
item draw 调用点（`534-571`）、bag draw 调用点（`573-608`）、draw-end 调用点
（`610-646`）、save callsites（`648-690`，含 `patch_all_save_callsites` `55-69`）、
drop gate `0xb8cc0`（`692-730`）、draw gate `0xaaf28`（`732-770`）、desc gate
（`772-814`）。每处均校验原指令字后才写替换 BL，地址记录在 `g_*_patch_addr`。
当前代码没有运行时把替换字写回原指令的路径；还原以「写回对应 `kOriginal*Call`
常量」为可逆操作，进程重启后内存 patch 自然消失。

### 2.7 投影同步与 moving 保留门

H-16 的 post-projection 在一次原版 `RefreshItemArea` 返回后逐槽比较控件 `data[0]` 与
`g_module_objects[bag][slot]`，不同才 `SetItem`，空槽同样覆盖
（`extension_bag_render.inc:533-555`；H-16 入口
`game_ui_virtbag.cpp:237-245`）。draw-end 和 draw wrapper 不再逐帧调用该路径。
无刷新事务的变化只调用 `sync_projected_slot_locked(bag, slot)` 定点同步
（`extension_bag_render.inc:557-568`；原版→扩展缺口调用
`extension_bag_transaction.inc:115-116`）；H3 回滚仍保留整袋同步入口，不作为帧级兜底。

draw-end 的 moving 保留门由 `clear_stale_projected_moving_locked` 实施
（`extension_bag_input.inc:34-77`）。只有以下表达式全真才保留当前投影 moving，保留时不清
任何字段：

```text
phase ∈ {Pressed, NativeMoving, TargetResolved, TransactionInFlight}
&& g_module_view_installed
&& g_virtual_bag_state.mode == Module
&& g_module_view_index == session.source_bag
&& session.view_generation == g_extension_tab_generation
&& g_extension_tab_generation == g_inventory_generation
&& live_root == g_projected_item_root
&& moving == valid_child(g_projected_item_root, session.source_slot)
```

任一条件不成立即清 `TouchState+0x30`，并清 moving 控件及 release source 控件的
`+0x0a/+0x0b` flag；该门不改变 `0x18` owner 或 release 五出口。

### 2.8 H3 物理快照守卫

`call_original_event_with_inventory_guard`（`extension_bag_lifecycle.inc:116-167`）
在 `g_orig_event` 调用前后捕获 96 槽物理快照（`79-94`），session 活动且槽数组变化时
记录 `ERROR physical inventory mutation` 并按 before 数组恢复，随后
`virtual_bag_sync_projected_bag()`（`143-165`）。规则锚 `rulebook.md` R-20。

## 3. 侵入面清单

本册记录触摸链相关的全部改动点；「与 vanilla 差异」栏描述模块启用时的行为。

| 改动点 | 方式 | 目的 | 与 vanilla 差异 | 可逆性 |
|---|---|---|---|---|
| 库存 state entry `+0x10/+0x28/+0x38` | 内存函数指针覆盖（`extension_bag_lifecycle.inc:815-820`） | 事件流进入 `virtual_bag_event`；F3/进入库存走 wrapper | 启用后全部触摸/按键事件先入模块；未启用分支原样调 `g_orig_event` | 进程内可逆：写回保存的原值（读取于 `523-531`）；当前无运行时还原路径 |
| item proc GOT 槽 | GOT 写替换（`game_patch_move_merge.inc:122-163`） | C1/C2 事件层路由与 `0x02` 三态门 | 扩展源 drop 被吞或转扩展事务；原版源放行原版 proc | 有卸载路径 `uninstall()`（`159-161`） |
| H-01..H-16 Native Hook | LSPosed `hook_func` 安装（`native_inventory_hook.cpp:507-566`） | 库存函数层分流；触摸链相关为 H-14/H-15/H-16 | 每个被 Hook 函数多一层 wrapper；原版对象 original-first | 安装失败自动回滚（`384-406`）；无运行期单独卸载 API |
| `0xb8cc0` drop gate patch | BL 指令替换（`extension_bag_lifecycle.inc:692-730`） | bag proc `0x04` 落袋写入改经 `save_item_on_empty_gate` | 投影 session 命中时返回 0，原版不清同号物理槽；真实移动延迟到 `0x18` 路由（`extension_bag_render.inc:442-462`） | 可写回原字 `0x94012fc8`；无运行时还原路径 |
| MakeDesc desc gate patch | BL 指令替换（`extension_bag_lifecycle.inc:772-814`） | 详情打开时装详情操作 hook（Path A） | `0x80` 详情路径多一层 gate；触摸落点无事务行为 | 可写回原字 `0x97fffdfe`；无运行时还原路径 |
| 事件吞并（capture） | `virtual_bag_event` 内返回 1（`extension_bag_lifecycle.inc:274-304`、`348-389`、`432-450`） | 旧 overlay/网格空位交互与点击-拖动分类 | 被 capture 的序列不达原版 TouchHandle | 逻辑开关：`g_extension_touch_capture` 清除即恢复放行 |
| 事件吞并（projected owner） | `0x18` owner 返回 1（`extension_bag_lifecycle.inc:340-347`） | 扩展事务唯一提交，原版不得二次移动 | 该次 `0x18` 原版五步清理由等价清理补齐（`extension_bag_input.inc:34-71`） | 逻辑开关：session 不活动即不触发 |

模块另有 draw/save 类指令 patch 与 store/save panel hook（`extension_bag_lifecycle.inc:534-690`），
不属于触摸落点链，登记于架构册与本册 §2.6；此处不展开。

## 4. 实现目标与验证状态

**目标**（三项）：

1. 扩展物品在 vanilla 触控链可达：投影物品借用原版 `TouchHandle` 建立 moving 状态，
   经 §1.4 起拖判定与 §1.7/§1.8 的 proc 事件进入模块路由（实现见 §2.2）。
2. 原版路径零变化：扩展未启用、非世界态、非扩展源的事件全部按 §1 基线行为放行
   （bypass 段 `extension_bag_lifecycle.inc:211-253`；C1 放行原版源
   `game_patch_move_merge.inc:24-48`）。
3. release 状态机等价：模块吞掉 `0x18` 的每个出口都以
   `complete_original_release_cleanup_locked` 完成 §1.5 五步清理的等价动作
   （§2.3；规则 `rulebook.md` R-33）。

扩展源 drop 的目标解析先判页签、后判网格；重叠坐标按页签优先处理。

**验证状态**（截至本稿）：

- 触摸机械部分（按下/拖动/松手/残留清理）已由用户在真机确认正常。
- 触摸触发的落点问题中，宝石拖装备仍是反例 S-03（`rulebook.md:47-48`、
  `verification-matrix.md` VM-B03），本批不处理；背包类物品拖空标签与跨页签提交已按
  §2.2.1 完成 R1/R2 代码修复，真机证据由 `verification-matrix.md` 的 S-05 卡补齐。
- 触摸链实施按会话口径分四步：①H-16 refresh gate（已完成，R-32）、②release 等
  价清理（已完成，R-33，`control-plane.md` §3.3 第 4 条）、③heal 退役（代码已完成，
  真机待验，R-34）、④VM-B01..VM-B05 四象限真机回归（未做，
  `verification-matrix.md:358-420`）。moving 六条件门随第③步落地。
- VM-28（release 清理变体选择）与 VM-B 真机证据均未采集；Overall 仍为
  `NOT_ACCEPTED`（`control-plane.md` §3.2）。

**后续计划**（事实性条目）：

1. VM-B 四象限回归：按 `verification-matrix.md` §2.1 五卡采集真机证据。
2. S-03 仍按 `control-plane.md` §4 登记；R1/R2 的页签落点修复按本册 §2.2.1 与验收册
   S-05 取证。

## 5. 设计决策与未决事项

本章只放裁定结论、理由与出处；事实记录在 §1–§4。

| 决策 | 理由 | 出处 |
|---|---|---|
| `INVEN_MoveItem` 纯函数层不做扩展分流（「函数层全放行/全接管均不成立」的落点）：采用方案 A，只按 item 扩展身份拒绝 | 该函数只有 item/count/target bag/slot 四参，不足以区分物理/显示目标、无法还原扩展意图；在此分流会与 `0x18` owner 形成双提交 | `inventory-integration-decision-plan.md:72`（方案 A/B 裁决）；`rulebook.md` R-31（`rulebook.md:538-545`） |
| 保留 `0x18` 单一 drop owner，事务失败也吞掉原版 | 原版 release 会继续调 `INVEN_MoveItem`/`SaveItemOnEmpty`，投影对象不在 `g_inven`；先放行后补偿会把物理槽交给原版链改写。R-33 的等价清理只补 TouchHandle 状态，不重新派发 `0x18` | `rulebook.md` R-02（`rulebook.md:333-340`）；`control-plane.md` §5 决策索引 2026-09-06 行 |
| Refresh 采用函数级关卡（H-16）而非 15+ 主动刷新点+帧 heal | 模块刷新点多持有 `g_virtual_bag_mtx`，经被 Hook 地址自调会重复加锁死锁；raw-original dispatcher 保留 Hook 未就绪回退；H-16 承担 post-projection，帧级 heal 退役 | `rulebook.md` R-32、R-34；`control-plane.md` §5 决策索引 2026-09-09 行 |
| moving 只在六条件全真时保留 | draw-end 不再逐帧写投影控件；无刷新事务定点同步受影响槽，stale moving 清 TouchState 与控件 flags | `rulebook.md` R-34；`verification-matrix.md` VM-B01～VM-B05 |
| 吞掉 `0x18` 必须做原版等价清理且放锁派发 | 只清扩展 session 会遗留 TouchState、moving/on 标志与选中控件，下一次点击进入幽灵拖拽；原版 UI 回调可重入模块，持锁派发会死锁 | `rulebook.md` R-33（`rulebook.md:552-562`）；实现 `extension_bag_input.inc:34-71` |
| 物理快照守卫只做「比较+恢复」，不承诺覆盖全部写点 | 它包住 `g_orig_event` 调用，无法观察不经过该调用的写入；问题 B 仍按「未解决+已布防」判定 | `rulebook.md` R-20（`rulebook.md:461-467`）；`control-plane.md` §4.1 |
| R-27/R-26 窗口袋号与 direct/GOT 成对恢复；R-30 root 重建门禁；R-19 generation 递增 | 视图切换与控件重建后旧事件不得提交到新视图；具体正文的编号引用归规则册 §5 | `rulebook.md:454-466`、`503-537`；本册 §2.2/§2.4 为实现锚 |

### 5.1 未决事项

1. S-03（宝石拖装备触发交换）的落点根因仍未定位；本批 R1/R2 仅覆盖背包类空页签装备
   与跨页签提交，不能替代 S-03 结论。
2. 问题 B（ext↔ext swap 的原版同号槽丢失）独立于触摸机械，登记与取证 SOP 归
   `control-plane.md` §4.1；本册 §2.8 的 H3 守卫是其防御层之一。
3. `control-plane.md` §4.1 取证字段仍指向旧稿 `drag-protocol.md §7.4`；旧 §7.4
   SOP 随本次重构移出本册，该引用需由 Hub 维护者修订（见附录 A）。

## 附录 A. 旧稿章节映射（2026-09-09 重构）

本册由「拖拽协议全册」收窄为「触摸链事实册」。旧稿章节去向：

| 旧稿章节 | 去向 |
|---|---|
| §1 当前裁决与术语、§2 状态机、§3 三态门、§4 三方向事务、§5 投影生命周期、§6 H3 | 事实部分收窄进本册 §2；事务/投影/H3 细节仍以旧稿内容为准的，改引 `rulebook.md`（R-01..R-34）与 `runtime-architecture.md`；H3 保留为 §2.8 |
| §7 问题 B 登记章、§8 候选写点清单 | `control-plane.md` §4.1（登记）与 `rulebook.md` R-20/R-31（防御层）；候选写点清单随旧稿移除，取证时按 `control-plane.md` §4.1 的证据缺口重新对码 |
| §7.4 复现取证 SOP | `control-plane.md` §4.1「取证」字段为其唯一引用点；SOP 正文随旧稿移除，需时从 git 历史恢复 |
| §9 旧稿结论迁移清单、§10 维护和验收引用 | R 编号索引归 `rulebook.md` §5；旧稿已废结论不再重复登记 |

引用本册的外部锚点漂移：`control-plane.md` §5 决策索引中 `drag-protocol.md §5.2`、
`§1.1`、`§3.1–§3.2`、`§3.3`、`§7.5`、`§8` 均指向旧稿编号，需由 Hub 维护者按本册
新结构（§2.5、§2.2、§4）修订。
