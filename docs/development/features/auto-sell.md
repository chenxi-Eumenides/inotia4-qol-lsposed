# 自动出售（auto-sell）设计与实现

> 状态：无 UI 版本代码已实现，但**宿主（每帧 BL patch）已按用户 2026-09-13 要求停用，游戏恢复原生状态**；待统一 frame 派发宿主完成后接入。host 通过、debug 构建通过；整体 `NOT_ACCEPTED`（缺真机证据）。本册是「自动出售」功能的范围、设计、风险与验收计划权威。
> 来源：`idea.md` 第 3 项 + 2026-09-12 多轮目标对齐。
> **功能定位**：独立于扩展背包的功能，与扩展背包为「支持」关系（可扫描/处置扩展袋物品），不归属扩展背包七册。
> 关联：扩展背包七册入口 `../extension-bag/control-plane.md`（仅「扩展袋支持」部分引用其契约）；游戏 UI 机制 `../../reference/game/ui.md`。
> **外部依赖（并行开发中，勿修改）**：属性范围 feature（`../attribute-range-display.md`、`module/app/src/main/cpp/feature/attribute_range/`、`tests/test_attribute_range.cpp`）。见 §3.6。

## 1. 范围与目标

### 1.1 功能定位

自动出售是一个独立功能：按可配置规则扫描背包，自动处置低价值物品。它不是扩展背包的子功能；只是**支持扩展背包**——扫描范围可含模块扩展袋，处置时走扩展背包既有桥接，但不改变扩展背包的所有权/事务契约。

### 1.2 面板规则清单（对齐结论）

面板自上而下：

1. **总开关**：启用/关闭整个自动出售。
2. **简要说明**：一行描述文本（总开关下方）。
3. **装备规则（3 条，每条可「关闭」，均为 `≤ 所选档`）**：
   - 品质：阈值单选（白/绿/蓝/黄/紫），出售 `rarity ≤ 阈值` 的装备。
   - 强化次数：阈值选择，出售 `总强化次数 ≤ 阈值` 的装备（`I_ENCHANT` bits6-10；新装备为 0）。
   - 镶嵌空位：阈值选择，出售 `总孔数 ≤ 阈值` 的装备。
4. **宝石规则（2 条，每条可「关闭」，均为 `≤ 所选档`）**：
   - 宝石等级：档位单选（低级/中级/高级/顶级/混沌），出售 `category ≤ 所选档`（28+档位）的宝石。
   - 宝石属性范围：复用属性范围 feature，阈值单选（30/60/75/90/100%），出售属性百分位 `≤ 阈值` 的宝石。
5. **特殊类型（多选）**：如背包类、英雄徽章类等；命中任一勾选类型即出售。
6. **返回**：关闭面板。

### 1.3 执行目标

- 总开关开启后立即清扫一次，其后每 60 游戏帧扫描并处置一次。
- 扫描范围：原版袋 `0..4` + 模块扩展 5 袋；排除原版任务袋 `5`。
- 处置优先复用原版单件销毁/出售链的结算语义，由模块循环（即用户所述「批量销毁」）。

## 2. 非目标

- 不改动原版存档结构、不改动 `ITEMDATABASE` 静态数据。
- 不新增对外 API 端点（首版只做游戏内面板）。
- 不接管原版任务袋 `5`。
- 不修改正在并行开发的属性范围 feature 的任何文件（§3.6）。
- 不引入新的入口 Hook 机制：沿用现有 `PtrHook` / PopupState 改写 / 函数指针调用。

## 3. 事实基础（已核实，含 `文件:行`）

### 3.1 装备品质 / 强化 / 孔位

- 品质：`ITEMSYSTEM_GetRarity(void* item)`，`F_GET_RARITY_VMA = 0x10d700`（`game_symbols.h:377`），返回 `0..4`；模块口径 **0 白 / 1 绿 / 2 蓝 / 3 黄 / 4 紫**（`StaticData.kt:60-69`、`api-reference.md:150-151`）。`raw_rarity` 亦为 `I_TYPE` bit2-5（`game_inventory_read.inc:116,120`）。
- 强化等级/次数：`I_ENCHANT = 0x1A`（u16，`game_symbols.h:61`）；**bits6-10 = 当前强化/附魔等级**、bits11-15 = 附魔 ID（真机权威 `attribute-range-display.md:279`）。native 输出 `enchant_level`（`game_inventory_read.inc:18-22`），公开对象 `enchant:{id,level,effect}`（`api-reference.md:156`）。无读取 getter，只能读位域。
  - **规则口径（用户 2026-09-13 裁决）**：强化次数 = **当前总强化次数（`I_ENCHANT` bits6-10）**。新获得的装备一定为 0，已强化装备由既有保护规则排除，故总次数与剩余可强化次数一致；采用唯一可读的 bits6-10。**无每件「强化上限」字段**，上限是常量（vanilla 30 / 改版 12），不用于比较。
  - 注：`game_symbols.h:61` 现有注释（bit5-6/bit10-15）与实现/实测不一致，属过时注释。
- 孔位：`I_SOCKET = 0x19`（u8，`game_symbols.h:60`）；**bits0-3 = 已镶数、bits4-7 = 总孔数**（`game_inventory_read.inc:16-17`），空位数 = 总孔数 − 已镶数，无专用字段。公开对象 `gem:{total_slots, slots[]}`（`api-reference.md:154`）。`game_symbols.h:60` 注释（bit0-2/bit4-6）过时。
- 装备判定：`item_is_equip(item) == (item_count_encoding(item)==kNotEncoded)`（`game_inventory_read.inc:83-85`），基于 `ITEMDATABASE[category].+6 bit0`（不可堆叠=装备）。
- 强化写入函数 `ITEMSYSTEM_EnchantItem 0x10b330`（`game_symbols.h:395`）；镶嵌函数 `ITEMSYSTEM_PutJewel 0x10bcb4`（`:393`，返回 2=无孔）；无「读等级/读空位」谓词函数。

### 3.2 宝石（等级与属性）

- 宝石判定：`ITEMSYSTEM_IsJewel(category)`，`F_IS_JEWEL_VMA=0x10b964`（`game_symbols.h:394`）；反汇编确认 category 闭区间 **28..32**。运行时 `category = (I_TYPE >> 6) & 0x3FF`（`game_inventory_read.inc:71-72`）。
- 档位名（`ITEMDATABASE.json` 记录 28..32）：**28 低级 / 29 中级 / 30 高级 / 31 顶级 / 32 混沌**（实为 5 档，非用户口述的 3 档）。
- 宝石实例随机等级：`item+0x10` 位域 **bits11-17**（`ITEMSYSTEM_MakeJewel@0x10b974`；`attribute-range-display.md:76,109`）；但更稳的档位判据是 category（§3.2 上）。
- 宝石自身属性：`item+0x10` **bits0-10 = 属性值、bits11-17 = 随机等级、bits18-23 = 属性类型**（`attribute-range-display.md:76`、`game_symbols.h:658-674`）。**现有 inventory JSON 不输出独立宝石自身的 `+0x10`**（只输出装备链表的 `options`，`game_inventory_read.inc:26-63`），自动出售宝石侧需新增读取。
- 属性取值范围：**运行时随机函数，不落表**。`ITEMSYSTEM_GetJewelOptionValue` `0x108f90`（`game_symbols.h:461`）→ `MATH_GetRandom(min,max)` `0xa8bcc`（`:462`），宝石区间 `[X,2X]`；`MATH_GetRandom` 为闭区间（`attribute-range-display.md:83`）。静态表无 min/max。

### 3.3 特殊类型判定（运行时 category 谓词）

| 类型 | 判定符号 | VMA / 范围 | 证据 |
|---|---|---|---|
| 背包类 | `ITEMDATABASE[category].+2 == 0x1f`（原生判定，`category_is_extension_backpack`） | category 1..4（手提包/背包小中大） | `extension_bag_runtime.inc:903-916`；`bag.md:88-99` |
| 英雄徽章 | `ITEMSYSTEM_IsMercenarySeal(category)` | `0x10be70`；category 42..50 / 928..933 | `game_symbols.h:574,761`；`control-plane.md:170` |
| 强化卷轴 | `ITEMSYSTEM_IsEnchantScroll` | `0x10b2f0`；16-20/946、21-25/947 | `game_symbols.h:396` |
| 宝石 | `ITEMSYSTEM_IsJewel` | `0x10b964`；28..32 | `game_symbols.h:394` |
| 骰子 | `ITEMSYSTEM_IsDice` | `0x10be60`；0x34-0x38 | `game_symbols.h:519` |
| 可解封 | `ITEMSYSTEM_IsSealed` | `0x10be50`；0x3a6-0x3ab | `game_symbols.h:521,518` |
| 开箱 | `ITEMSYSTEM_IsItemBox` | `0x10cda0`；0x3ef-0x3f1 | `game_symbols.h:522` |
| 不可售 | `ITEMDATABASE_IsNoSell(category)` | `0x105864` | `game_symbols.h:511`；`game_inventory_use.inc:194` |

- 注意：徽章用途类型 `+2=0x1d` 在 `StaticData.itemType` 落 `consumable`，**不能靠 itemType 识别**。
- 未决：普通袋内任务物品无专用判定函数；「不可销毁」是否由 `IsNoSell` 覆盖未确认。

### 3.4 处置链（出售/销毁）

- 模块出售：`data_op_sell_item`——`IsNoSell` 校验 → 算价 → `INVEN_AddMoney` → `INVEN_RemoveItem`，失败退款（`game_inventory_use.inc:180-225`）。
- 模块销毁：`data_op_discard_item`——仅 `INVEN_RemoveItem`（`:168-179`）。
- 原版详情页链：`UIEquip_ButtonDestroyExe 0xb6240`（`game_symbols.h:655`）→ `UIEquip_OKDestroyItem 0xb83d0`（`:657`，无参，读袋/槽上下文）→ 结算 `F_UIEQUIP_SELL_SETTLE_VMA=0x1261c4`（`:670-675`）。H-22/H-23 已接管该链（`native_inventory_hook.cpp:833-838,887-969`，R-55）。
- 底层符号：`INVEN_RemoveItem 0x104044`（`game_symbols.h:455`）、`INVEN_RemoveItemData 0x1040a8`（`:528`）、`ITEMPOOL_Free 0x108160`（`:315`）、`INVEN_AddMoney 0x1044e4` / `INVEN_MinusMoney 0x104780`（`:449-450`）。

### 3.5 面板 / 配置 / 帧驱动范式

- 外部 `config.json` 为唯一配置来源，`ModuleConfig.kt`（`load`/`apply`/`toJson`，原子写）；native↔Kotlin 桥 `ModuleConfigUiBridge.kt:13-38`（当前只支持布尔翻转，`BOOL_KEYS`）。
- 面板范式：`PtrHook` 覆盖按钮 ExecuteProc（`game_ui_settings_injection.inc:12-29`）→ `fn_ui_set_popup_process_info(1, state_id)`（`:6-10`）→ 改写 PopupState 死条目 5 回调（`:75-94`）→ PROCESS 渲染 + EVENT 触摸（`game_ui_settings_panel.inc:72-116,152-192`）→ 配置反调 `game_ui_settings_config.inc:1-62`。
- 空闲 PopupState 死条目：设置占 `F_PANEL_UNK1_ENTER`（GEMSHOP）、存档占 `F_PANEL_UNK2_ENTER`（GOODS）（`save-backup.md:118`）。
- 可复用控件/绘制：`game_ui_kit.h:16-63` + `game_ui_components.h`。
- 帧派发：统一帧任务管理器（core/native/frame_task.* + frame_host.*，architecture.md §2.1）在主线程渲染开始前（GAMESTATE_DrawPlay+0x20 bl MAP_DrawBase）派发；旧 FrameTaskManager（后台线程、单任务）已删除。
- 扩展袋：原版袋 `0..5`（5=任务袋，`virtual_bag_state.h:20-26`）；扩展逻辑袋 5 个（`:16`）；页签动态定位 `extension_bag_runtime.inc:110-187,138-140,169-174`。

### 3.6 外部依赖：属性范围 feature（并行开发中）

- 该 feature（`idea.md` 第 2 项）负责把属性值按其在随机范围内的百分位着色（金100/紫90/蓝75/绿60/白30/灰0）。
- 工作树现状（未提交 WIP，**auto-sell 不得修改**）：`feature/attribute_range/`（纯逻辑 `attribute_range.h/.cpp:5-50` + UI/探测 `game_ui_attr_range.cpp`）、`tests/test_attribute_range.cpp`、设计册 `docs/development/features/attribute-range-display.md`；并已改动 `CMakeLists.txt`、`gamebridge.cpp`、`game_symbols.h`、`symbol_registry.h`、`game_access.*`、`docs/INDEX.md`。
- 对自动出售的可用性：纯逻辑 `attr_range::classify(value,min,max)` / `color_code(Tier)` 可复用，但**无对外 range provider**——范围探测函数在匿名命名空间内（`game_ui_attr_range.cpp:14-145`），自动出售无法直接调用；且其 `MATH_GetRandom` wrapper 使用 thread_local 单发捕获（`:29-31,39-48`），与详情渲染存在同线程重入/竞争。
- 结论：**宝石属性范围规则依赖该 feature 暴露稳定 range API（或复制探测逻辑）后才能真正落地**；在此之前该规则只能先做 UI 占位/禁用。

## 4. 设计

### 4.1 入口按钮

- 位置：背包（EQUIP）页「任务背包右侧、扩展背包下方」，**动态锚定原版任务袋（袋 5）标签所在行**。
- **默认挂载（用户 2026-09-13）**：入口按钮始终随背包页安装，不依赖任何前置开关；总开关在面板内。
  - 模仿扩展背包页签的动态计算：以原版袋标签实际坐标为基准 + 行偏移（参考式 `x = 袋0绝对x + 68`、`y = 袋0绝对y + 2 + 5×70`，尺寸 `57×57`；`extension_bag_runtime.inc:138-140,169-174`）。实现时动态计算，不写死像素。
  - 最终偏移量需真机校准（风险 R2）。
- 控件：挂在原版袋容器，新增 ControlItem（`type=3` + `SetControlProc` + `SetUserType(2)`），点击回调打开配置面板。
- 绘制：复用扩展页签 DrawProc/图组风格或 `ui_custom::draw_button`。

### 4.2 配置面板

- 打开：从背包页按钮 `fn_ui_set_popup_process_info(1, <state_id>)` push 面板（同设置页机制）。
- PopupState 占用：需一个未占用死条目。设置/存档已占 GEMSHOP/GOODS，需从其余 IAP 死场景侦察一个（风险 R3），或扩展背包状态机内嵌套 push。
- 布局（行数较多，需分页/分组，复用 `game_ui_kit`）：

```text
[ 自动出售 ]  (总开关 toggle)
  说明：开启后按下方规则自动出售低价值物品，每 60 帧扫描一次。
────────────────────────────
 装备规则
   品质      [关闭] [白][绿][蓝][黄][紫]        (单选)
   强化次数  [关闭] [-]  N  [+]                  (阈值)
   镶嵌空位  [关闭] [-]  N  [+]                  (阈值)
────────────────────────────
 宝石规则
   宝石等级  [关闭] [低][中][高][顶级][混沌]      (单选)
   属性范围  [关闭] [30%][60%][75%][90%][100%]   (单选；依赖 §3.6)
────────────────────────────
 特殊类型（多选）
   [ ] 背包类  [ ] 英雄徽章  [ ] 强化卷轴
   [ ] 骰子    [ ] 解封类    [ ] 开箱类
[← 返回]
```

- 交互：每条规则首列「关闭」与档位互斥（关闭时档位灰显）；特殊类型为独立多选框。
- 渲染/事件：PROCESS 每帧渲染 + EVENT 触摸命中（同设置面板）。行数超出时按组或分页处理。

### 4.3 规则语义

- 装备（仅 `equip==true`）：**同类内 OR**——任一已启用装备规则命中即出售：品质 `rarity ≤ 阈值`、总强化次数 `enhance_count ≤ 阈值`、总孔数 `socket_total ≤ 阈值`。
- 宝石（`IsJewel(category)`）：**同类内 OR**——任一已启用宝石规则命中即出售：档位 `category ≤ 所选档`（28+档位）、属性 `百分位 ≤ 阈值`。
- 特殊类型：命中任一勾选类型即出售。
- 跨类独立：装备、宝石、特殊类型各自判定，命中任一即出售（全局等价 OR）。
- 比较方向统一为 `≤ 所选档`（用户 2026-09-12）。

### 4.4 配置项与持久化

配置**按存档独立**，持久化到**模块存档 sidecar** 的独立 section `autosell`（version=1，UTF-8 JSON，**直写**，不进 participant/journal），**不写入 `config.json`**（用户 2026-09-13 裁决；sidecar 机制见 `../extension-bag/module-save-store.md`）。

section `autosell` v1 payload：

```json
{"v":1,
 "enabled":false,
 "rarity":{"enabled":false,"threshold":0},
 "enhance":{"enabled":false,"threshold":0},
 "socket":{"enabled":false,"threshold":0},
 "gemTier":{"enabled":false,"threshold":0},
 "gemRange":{"enabled":false,"threshold":30},
 "specialMask":0}
```

| 字段 | 类型 | 默认 | 语义 |
|---|---|---|---|
| `enabled` | bool | `false` | 总开关（按存档） |
| `rarity.enabled` / `.threshold` | bool / int `0..4` | `false` / `0` | 出售 `rarity ≤ 阈值` |
| `enhance.enabled` / `.threshold` | bool / int `≥0` | `false` / `0` | 出售 `总强化次数（I_ENCHANT bits6-10）≤ 阈值` |
| `socket.enabled` / `.threshold` | bool / int `0..15` | `false` / `0` | 出售 `总孔数 ≤ 阈值` |
| `gemTier.enabled` / `.threshold` | bool / int `0..4` | `false` / `0` | 出售 `宝石 category ≤ 28+阈值` |
| `gemRange.enabled` / `.threshold` | bool / int enum | `false` / `30` | 依赖 §3.6；仅入 schema，不接线 |
| `specialMask` | int 位掩码 | `0` | 特殊类型多选（位与 `autosell_rules.h::SpecialType` 一致） |

- 读写：`ModuleSaveStore.readSection/writeSection(slot, "autosell", 1, payload)`；`slot = current_save_slot()`。
- 加载：`autosell_tick` 顶部按 `slot` 变化惰性加载（与扩展背包同构）；主菜单/无存档保持默认且不扫描。
- 兼容：`v` 缺失或 ≤1 按 v1 解析；`v>1` fail-closed 用默认且**不删 section**；越界值钳制。
- 该组配置**不进入 `ModuleConfig` / 模块设置页 / `ConfigApiService` 全局下发**；由自动出售面板写入当前存档 sidecar，随存档加载/保存。
- 无 UI 阶段：由 debug JNI 应用并写入当前存档 sidecar，待面板接入。
- 默认值取安全侧：总开关默认关；各规则默认关。
- **无 `opEnabled` 门禁**（用户 2026-09-13 裁决）：自动出售为独立功能开关，与扩展背包同类。
- **风险（已裁决规避）**：`ModuleSaveCoordinator` 当前只支持单 participant 一次性 prepare/commit，第二个 participant 会覆盖 `module.save.journal`；因此自动出售采用**独立 section 直写**，不并入扩展背包 participant，也不另造 journal（`module-save-store.md` 双 journal 不可混用）。

### 4.5 扫描与执行

- **线程约束（强制）**：扫描与处置必须在游戏主线程执行；常驻帧宿主复用 `UIEquip_Draw` draw-end patch 或等价主循环回调；不得在 detached 线程直接调游戏函数。
- 节流：`data_frame_count()` 帧计数；启用时立即执行一次，之后每 60 帧一次。
- 单次扫描（原版袋 `0..4` + 扩展 5 袋逐槽）：
  1. 取物品指针；空槽跳过。
  2. 分类：装备 / 宝石 / 特殊类型。
  3. 按 §4.3 规则判定（装备 `equip`+rarity/强化上限/总孔数；宝石 `IsJewel`+category/属性百分位；特殊类型谓词）。
  4. 保护过滤（§4.6）。
  5. 处置（§4.5.1）。
  6. 每次处置后重读槽位，避免遍历失效（原版删除返回值不可信，`game_symbols.h:500`）。
- 遍历复用 data 层 `for_each_bag_slot`；扩展袋经扩展背包既有遍历/桥接。

#### 4.5.1 处置路径（复用单件链 + 模块循环）

- **批量语义（已定，2026-09-12）**：原版无批量入口；「批量销毁」= 复用原版单件销毁/出售链，由模块对每个命中物品循环调用。
- **首选**：复用原版单件链结算语义（`ButtonDestroyExe → OKDestroyItem → 0x1261c4`），但**不弹确认 UI、不依赖面板选中上下文**，由模块循环驱动。
  - 待验证（R1）：`0x1261c4` 是否可无 UI 安全调用；若强依赖 UI，退化为等价实现（模块算价 + `INVEN_AddMoney` + `INVEN_RemoveItem`，即 `data_op_sell_item` 口径），并与详情页售价对拍。
  - 与 H-23 / R-55 的 canonical 接管关系必须核对，不得绕过或冲突。
- **兜底**：`INVEN_RemoveItem`（销毁，不给收益）。
- **扩展袋物品**：原版删除/出售函数作用于原版物理槽；扩展袋物品必须走扩展背包桥接（H-04 / R-56 / 扩展 API），不得让扩展对象进入原版物理释放路径。

### 4.6 保护规则

**硬保护（不可配置）**：

1. 已穿戴装备（不在背包；装备槽不扫描）。
2. 任务物品（任务袋 `5` 已排除；普通袋任务物品判定来源待定）。
3. 不可售 / 不可销毁（`ITEMDATABASE_IsNoSell`；「不可销毁」判定待确认）。
4. 扩展背包事务中/锁定/投影中的物品。

**规则侧排除**：特殊类型多选中若勾选某类，该类物品按规则出售；未勾选的类型默认不因特殊类型规则出售，但仍可能因装备/宝石规则命中。

### 4.7 模块分层与文件规划

| 层 | 规划内容 |
|---|---|
| data/symbols | 复用 `F_GET_RARITY_VMA`、`I_ENCHANT`/`I_SOCKET`、`IsJewel`/`IsMercenarySeal` 等；宝石属性读取需新增字段输出；如需新 VMA 登记 `game_symbols.h`+`symbol_registry.h` 并跑 `check_symbols.py` |
| native feature | 新增独立 `feature/autosell/`（纯逻辑：规则判定/组合/档位映射，可编 host 单测）+ `feature/ui/game_ui_autosell*`（入口/面板） |
| bridge/JNI | 面板配置桥接与扫描状态导出（同 `gamebridge_settings.cpp` 模式） |
| Kotlin | `AutoSellConfigStore`（`ModuleSaveStore` section `autosell` 读写 + debug JNI 桥）；**不**改 `ModuleConfig` / `ConfigApiService` |
| 扩展背包支持 | 经扩展背包稳定端口/API 读写扩展袋，不直接依赖其内部实现 |

## 5. 风险与验证方案

| 编号 | 风险 | 验证方案 | 判定标准 |
|---|---|---|---|
| R1 | 单件链结算 `0x1261c4` 无法无 UI 安全调用 | 反汇编 `0x1261c4` 及调用点；不可行则以 `data_op_sell_item` 口径等价实现并与详情页售价对拍 | 价格/结果一致；否则降级销毁并记录 |
| R2 | 任务袋右侧动态坐标/触摸命中未校准 | 真机截图量取 + 点击验证 | 与任务袋标签同行、可点、不遮挡 |
| R3 | 无可用第三个 PopupState 死条目 | 侦察其余 IAP 死场景可达性；或验证嵌套 push | 面板可开可关，不影响 IAP 屏蔽 |
| R4 | 常驻帧回调与移动/拖动/保存并发 | 战斗/寻路/拖动/存档界面下开启 | 无死锁/崩溃/物品丢失；必要时暂停 |
| R5 | 扩展袋物品处置误入原版物理释放 | 物理槽 pre/post 快照 | 物理槽零变化、扩展账本一致 |
| R6 | 扫描性能 | 60 帧一次、满包场景计时 | 无可感知掉帧 |
| R7 | 宝石属性范围依赖并行 WIP feature，无稳定 range API | 与属性范围 feature owner 协调暴露 provider 或复制探测；复核 §3.6 重入风险 | 有稳定只读 range API 后再落地；否则该规则禁用占位 |
| R8 | 面板行数多、分页/滚动与触摸命中复杂 | 原型真机验证分组/分页交互 | 所有控件可命中、不越界 |
| R9 | 强化口径（已裁决：用总强化次数 bits6-10；无每件上限字段） | 阶段 0 已完成反汇编裁决 | 按 bits6-10 实现；已接线 |

## 6. 分阶段实施计划

1. **阶段 0（先决侦察）**：R1 反汇编 + R2 真机量位 + R3 死条目侦察；确认 §3.6 依赖边界。
2. **阶段 1（面板与配置）**：入口按钮 + 面板 + 按存档 sidecar 持久化；规则 UI 可配但可先不接线扫描。
3. **阶段 2（装备/特殊类型扫描处置）**：帧节流 + 装备三规则 + 特殊类型 + 保护过滤 + 处置（按阶段 0 结论）。
4. **阶段 3（宝石规则）**：宝石等级规则（可独立落地）；宝石属性范围规则待属性范围 feature 提供 API 后接线。
5. **阶段 4（真机验收）**：按 §7 取证；缺真机证据一律 `NOT_ACCEPTED`。

## 7. 验收标准（VM 草案）

- VM-AS1：入口按钮显示/可点/开关面板，不影响任务袋与扩展页签。
- VM-AS2：配置按存档持久化，重启/切档保持；不同存档互不影响。
- VM-AS3：装备品质规则只出售 `rarity ≤ 阈值` 装备。
- VM-AS4：装备强化规则只出售 `总强化次数（I_ENCHANT bits6-10）≤ 阈值` 装备。
- VM-AS5：装备孔位规则只出售 `总孔数 ≤ 阈值` 装备。
- VM-AS6：每条规则可单独「关闭」，关闭后不影响该维度。
- VM-AS7：宝石等级规则只出售 `category ≤ 所选档` 的宝石（5 档正确）。
- VM-AS8：宝石属性范围规则只出售 `百分位 ≤ 阈值` 的宝石（依赖 R7 解锁）。
- VM-AS9：特殊类型多选命中即出售，未勾选类型不因该规则出售。
- VM-AS10：硬保护生效（已穿戴/任务/不可售/不可销毁）。
- VM-AS11：开启后立即清扫一次、之后每 60 帧一次（日志帧号可核）。
- VM-AS12：扩展袋物品处置物理槽与扩展账本一致。
- VM-AS13：空背包/满包/拖动中/存档界面/战斗中等边界无崩溃、无物品丢失。
- VM-AS14：同类内任一启用规则命中即出售（OR 语义）；跨类命中任一即出售。

## 8. 未决项

1. R1：`0x1261c4` 无 UI 可调用性；不可则改等价模块 sell 口径。
2. R9（已裁决）：强化规则用「当前总强化次数（`I_ENCHANT` bits6-10）」；每件「强化上限」不存在。残余：bits2-5 精确语义（不影响本规则）。
3. 宝石属性范围 feature 的稳定 API 与重入隔离（R7）。
4. 宝石 5 档命名与用户口述 3 档的差异确认。
5. 普通袋内任务物品与「不可销毁」判定函数。
6. 面板关闭后常驻扫描宿主回调；与扩展背包锁/投影/拖动事务隔离。
7. 扩展袋被处置物品的 generation/handle 失效路径。
8. 自动出售 section 与扩展背包 journal/协调器的隔离（单 participant 限制 → 直写）。

## 9. 决策与理由（思考，非事实）

- 功能独立于扩展背包：用户 2026-09-12 明确；扩展背包仅作为被支持的扫描/处置范围，不改变其契约与所有权。
- 品质读取用 `ITEMSYSTEM_GetRarity`；强化/孔位无 getter，直接读 `I_ENCHANT`/`I_SOCKET` 位域。
- 宝石档位判据用 category（28..32）而非实例随机等级：category 是稳定静态语义，实例等级是掷值。
- 属性范围规则不复制属性范围 feature 的内部实现：作为外部依赖等待稳定 API，避免双份维护与重入竞争。
- 扫描放游戏主线程逐帧回调（统一帧任务管理器 kFramePointRenderPre 点位）。
- 处置首选复用原版单件链结算语义，保留销毁兜底；「批量」= 模块循环单件链。
- 入口按钮模仿扩展背包页签动态定位，锚定任务袋标签右侧，不写死像素。
- 配置按存档持久化到模块存档 sidecar，不用 `config.json`/`ModuleConfig`，不加 `opEnabled` 门禁（用户 2026-09-13）；入口按钮默认挂载，总开关在面板内启用。
- 规则组合（用户 2026-09-12）：同类内多条启用规则取 OR（命中一条即出售）；跨类独立，命中任一即出售。
- 比较方向统一 `≤ 所选档`；孔位用总孔数；强化次数用「当前总强化次数（bits6-10）」（用户 2026-09-13 裁决；已强化装备由保护规则排除，总次数与剩余次数一致）。

## 10. 影响契约与保持证据（实现时必填）

自动出售触及扩展背包出售/删除与主循环，实现变更说明必须逐项引用并给出保持证据（Host 测试名或真机 VM 卡）：

| 受影响项 | 关系 | 保持证据 |
|---|---|---|
| R-44 持锁原版调用纪律 | 扫描/处置可能在持锁或主线程内调用原版删除 | 无锚-待补 |
| R-55 原版详情出售 H-23 canonical 接管 | 首选处置口径与详情页一致，需确认不绕过/不冲突 | 无锚-待补 |
| R-56 `INVEN_RemoveItemData` 扩展补扣 | 扩展袋物品批量处置的物理不足路径 | 无锚-待补 |
| 受影响 VM-B（扩展背包原版基线走查） | 处置过程对原版物理槽零变化 | 无锚-待补 |
| 属性范围 feature（并行） | 宝石属性范围规则的外部依赖；不得修改其文件 | 无锚-待补 |

## 11. 可行性验证结论（2026-09-13）

依据：`llvm-objdump`（NDK r26d）反汇编 `apk/decoded/overhaul/lib/arm64-v8a/libgame.so` + 源码核对。

### 11.1 处置链（R1）

- `0x1261c4` 入参 `(bag, slot)`，只读 `g_inven[bag*16+slot]`（GOT `0x2f31c8`→`0x7131c0`），**不依赖面板上下文**；结算 = `ITEM_GetSellPrice` × `unit*count*7/10`（70%）→ `INVEN_AddMoney` → `INVEN_RemoveItemDirect`。
- **隐式 preview 门**：`126298 cmp x23,#0x666` —— `x23` 不是 ABI 参数、不在 prologue 保存；直调需 asm shim 保证 `x23!=0x666`，否则只算价不删除。
- 唯一 `bl` 调用点 `UIEquip_OKDestroyItem@0xb83d0`；`0xb83d0` 才读面板 `desc_type/bag/ctrl`（`G_UIEQUIP_PANEL_VMA` 等），**直调它会误用残留面板态**。
- 直调 `0x1261c4` 不经过 `0xb83d0`，**不触发 H-23，也不与 R-55 冲突**；但只覆盖物理袋 0..4、无 `IsNoSell`、绕过 H-04（扩展袋物品在 `g_inven` 为空指针直接早退）。
- **结论**：首选不走 `0x1261c4`；改用模块等价原语（`IsNoSell` → 取价 → `INVEN_AddMoney` → `INVEN_RemoveItem`(H-04)），同时覆盖物理+扩展袋，且已被 API 真机路径使用；销毁兜底 `INVEN_RemoveItem`。扩展袋处置走 `extension_bag_sell_price` + `extension_bag_remove_native_item` + `extension_bag_sync_projected_slot`（`core/native/extension_bag_port.h:29-36`）。

### 11.2 定时宿主（R4）

- `FrameTaskManager` **不可用**：回调运行在后台线程（`game_motion.cpp:51,75-98`），且 `frame_task_register` 会 `g_tasks.clear()`（`:45`）顶掉移动/寻路任务。（现状：旧 FrameTaskManager 已删除，自动出售经统一帧任务管理器注册到 kFramePointRenderPre，与移动槽互不影响。）
- 截至验证时**不存在世界态通用每帧宿主**；现有 draw-end wrapper 只在对应面板绘制时触发（背包/合成/商店）。
- **结论**：需新增主线程 draw-end tick（复用 `allocate_draw_thunk` + BL patch 范式，`extension_bag_lifecycle.inc:610-644`），用 `data_frame_count()`（`G_FRAME_COUNT_VMA`）做 60 帧节流；若需面板外常驻，需在世界绘制路径另挂一个 tick。
- 未决：背包面板打开时 `g_gamestate==0` 未真机证实（不影响「不用 FrameTaskManager」的结论）。

### 11.3 强化「次数」口径（R9）

- **无按稀有度/静态表派生的每件强化上限**（`ITEMGRADEBASE`/`ITEMRARITYGRADEBASE`/`ITEMDATABASE` 均无该字段）。
- `ITEMSYSTEM_EnchantItem@0x10b330`：**bits6-10 = 当前强化层数**（成功 +1，上限常量 `0x1e`=30）；**bits2-5 = 失败时 −1 的字段**（疑为耐久/剩余次数，未证实）。
- 改版混沌补丁把耐久上限设为常量 **12**（`inotia4_a.txt:1292-1293`）。
- **结论/裁决（2026-09-13）**：无每件「强化上限」字段（上限是常量 vanilla 30 / 改版 12）；规则采用**当前总强化次数** `I_ENCHANT` bits6-10（= `enchant_level`）作 `≤ 阈值` 比较。

### 11.4 宝石 / 特殊类型

- 宝石自身属性 `item+0x10`：bits0-10 值 / bits11-17 随机等级 / bits18-23 类型；现有 JSON **未读取**（`game_inventory_read.inc` 对 `+0x10` 按装备混沌解读，对宝石错误），需新增读取。档位用 category 28..32（`IsJewel`，5 档）正确。
- 宝石属性范围依赖属性范围 feature（WIP，无对外 range API）——本阶段不实现该规则。
- 6 个特殊谓词（`IsMercenarySeal`/`IsEnchantScroll`/`IsJewel`/`IsDice`/`IsSealed`/`IsItemBox`）均已解析可用；`ITEM_IsRealEquip` 未解析。

### 11.5 裁决记录

1. **强化规则语义（2026-09-13）**：采用「当前总强化次数 ≤ 阈值」（`I_ENCHANT` bits6-10）。理由：新装备为 0、已强化装备由保护规则排除，总次数与剩余次数一致；无每件上限字段。
2. **出售价格（2026-09-13）**：70%（与详情页/`data_op_sell_item` 默认一致）。
3. **配置存储与门禁（2026-09-13）**：按存档持久化到模块存档 sidecar（不用 `config.json`）；不加 `opEnabled` 门禁；入口按钮默认挂载，开关在面板内。

> 本册为设计稿；实现落地前先按 `control-plane.md` §2.2「增加操作」路由补读架构册/库存册，并在验收册新增操作契约卡。属性范围依赖变更时须与对应 owner 协调。
