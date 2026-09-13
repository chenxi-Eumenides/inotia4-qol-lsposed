# 自动出售（auto-sell）设计与实现

> 状态：规则/视图/扫描（阶段 B）与游戏内 UI（背包入口按钮 + 只读占位面板）native 侧已实现；扫描任务由全局开关 `autoSellEnabled`（模块设置第 6 项）驱动，宿主为统一 frame 派发点。面板配置读写与关闭即保存尚未接线。host 通过、debug 构建通过；整体 `NOT_ACCEPTED`（缺真机证据）。本册是「自动出售」功能的范围、设计、风险与验收计划权威。
> 来源：`idea.md` 第 3 项 + 2026-09-12 多轮目标对齐。
> **功能定位**：独立于扩展背包的功能，与扩展背包为「支持」关系（可扫描/处置扩展袋物品），不归属扩展背包七册。
> 关联：扩展背包七册入口 `../extension-bag/control-plane.md`（仅「扩展袋支持」部分引用其契约）；游戏 UI 机制 `../../reference/game/ui.md`。
> **外部依赖（已实现，只读复用，勿修改其行为）**：属性范围 feature（`../attribute-range-display.md`、`module/app/src/main/cpp/feature/attribute_range/`、`tests/test_attribute_range.cpp`）。见 §3.6。

## 1. 范围与目标

### 1.1 功能定位

自动出售是一个独立功能：按可配置规则扫描背包，自动处置低价值物品。它不是扩展背包的子功能；只是**支持扩展背包**——扫描范围可含模块扩展袋，处置时走扩展背包既有桥接，但不改变扩展背包的所有权/事务契约。

### 1.2 面板规则清单（对齐结论）

面板自上而下：

1. **总开关**：启用/关闭整个自动出售。
2. **简要说明**：一行描述文本（总开关下方）。
3. **装备规则（3 条，值即开关，均为 `≤ 值-1`）**：
   - 品质：档位值（0=关；1..5 → 出售 `rarity ≤ 值-1`，1=白…5=紫）。
   - 强化次数：档位值（0=关；1..32 → 出售 `I_ENCHANT` bits6-10 `≤ 值-1`；新装备为 0）。
   - 镶嵌空位：档位值（0=关；1..16 → 出售总孔数 `≤ 值-1`）。
4. **宝石规则（2 条，均为 `≤ 值-1`）**：
   - 宝石等级：档位值（0=关；1..5 → 出售 `category ≤ 27+值`，即 28+档位）。
   - 宝石属性范围：复用属性范围 feature，阈值单选（30/60/75/90/99%；最高档 99 保留满分宝石）。
5. **特殊类型（多选）**：如背包类、英雄徽章类等；命中任一勾选类型即出售。
6. **返回**：关闭面板。

### 1.3 执行目标

- 全局开关（模块设置第 6 项）为「武装」标志：进入存档（读档/新档）后由 save-enter 回调按当前槽加载 sidecar 配置，仅在「已武装 + 已进档 + 存档总开关 `config.enabled` 为真」时注册 60 帧周期扫描任务；任一不满足即不持有任务。扫描每 60 帧一次（节流由 `frame_task` 的 `interval=60` 负责，注册后最多等待 60 帧）。存档总开关 `config.enabled` 变化时经 `autosell_apply_config` 同步任务（关→删除）；退出存档（world → 主菜单）由 save-exit 回调删除任务。
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
- 装备判定（自动出售，2026-09-14 真机修正）：用游戏自身的 `ITEM_IsRealEquip(item)`（`F_ITEM_IS_REAL_EQUIP_VMA=0x105ab8`，读 `item+8` 的 category → `ITEMDATABASE[category].+2` → 二级表 bit0）；`game_access` 增 `fn_item_is_real_equip`。比「不可堆叠」精确：`item_is_equip(item)==(item_count_encoding(item)==kNotEncoded)`（`item_class.cpp`）会把宝石/背包/徽章等非装备判为真，导致装备规则过匹配（真机 dry-run 实测每轮命中由 13 降为 2）。API 的 `equip` 字段仍用 `item_is_equip`，未改动。
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

### 3.6 外部依赖：属性范围 feature（已实现，已接线）

- 该 feature（`idea.md` 第 2 项）负责把属性值按其在随机范围内的百分位着色（金100/紫90/蓝75/绿60/白30/灰0）。
- 当前实现位于 `feature/attribute_range/`：纯逻辑 `attribute_range.h/.cpp` 提供 `classify(value,min,max)`、`color_code(Tier)` 与 `percentile(value,min,max)`（0..100 向下取整）；UI/探测 `game_ui_attr_range.h/.cpp` 提供对外稳定 API：
  - `attr_range_ready()`：MATH_GetRandom hook 是否已安装完成；
  - `attr_range_probe_jewel_range(type, item, out_min, out_max)`：探测独立宝石属性值的随机区间 `[X,2X]`（`type` = 宝石位域 bits18-23；不依赖详情渲染缓存 `t_current_item`，主线程调用）。
- 自动出售按 §4.3 复用上述 API 计算宝石属性百分位，**不复制其内部实现，不在域文件写裸 VMA**；hook 未安装或探测失败时 API 返回 false。
- 该 feature 的 `attr_range_ui_install_if_ready()` 仍由 `nativeInit` 安装；自动出售只读调用。

## 4. 设计

### 4.1 入口按钮

- 位置：背包（EQUIP）页「任务背包右侧、扩展背包下方」，**动态锚定原版任务袋（袋 5）标签所在行**。
- **开关门控（用户 2026-09-13）**：入口按钮由模块设置第 6 项全局开关 `autoSellEnabled` 门控，默认关闭；开启后随背包页安装，关闭时移除引用并关闭已打开面板（§13.2）。面板内另有按存档的规则总开关。
  - 模仿扩展背包页签的动态计算：以原版袋标签实际坐标为基准 + 行偏移（参考式 `x = 袋0绝对x + 68`、`y = 袋0绝对y + 2 + 5×70`，尺寸 `57×57`；`extension_bag_runtime.inc:138-140,169-174`）。实现时动态计算，不写死像素。
  - 最终偏移量需真机校准（风险 R2）。
- 控件：挂在原版袋容器，新增 `ControlButton`（`ui_create_button`：`type=3` + `ControlButton_ControlEventProc` + `ExecuteProc`），点击回调打开配置面板；生命周期强校验见 §13.1。
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
   属性范围  [关闭] [30%][60%][75%][90%][99%]   (单选；最高档保留满分宝石)
────────────────────────────
 特殊类型（多选）
   [ ] 背包类  [ ] 英雄徽章  [ ] 强化卷轴
   [ ] 骰子    [ ] 解封类    [ ] 开箱类
[← 返回]
```

- 交互：每条规则首列「关闭」与档位互斥（关闭时档位灰显）；特殊类型为独立多选框。
- 渲染/事件：PROCESS 每帧渲染 + EVENT 触摸命中（同设置面板）。行数超出时按组或分页处理。

### 4.3 规则语义

- **值即开关（用户 2026-09-14 裁决）**：规则组取消各自的 `*_enabled` 布尔开关，**用「值」当开关**——`0` = 关闭，从 `1` 开始的整数为 1-based 档位，出售阈值 = 值 − 1，比较方向统一 `≤`。
- 装备（仅 `equip==true`）：**同类内 OR**——任一值非 0 的装备规则命中即出售：
  - 品质 `rarity`：0=关；`1..5` → 出售 `rarity ≤ 值-1`（1=白…5=紫）。
  - 强化次数 `enhance`：0=关；`1..32` → 出售 `enhance_count(I_ENCHANT bits6-10) ≤ 值-1`。
  - 镶嵌空位 `socket`：0=关；`1..16` → 出售 `socket_total(I_SOCKET bits4-7) ≤ 值-1`。
- 宝石（`IsJewel(category)`）：**同类内 OR**——任一值非 0 的宝石规则命中即出售：
  - 宝石等级 `gem_tier`：0=关；`1..5` → 出售 `jewel_tier(category-28) ≤ 值-1`。
  - 属性范围 `gem_range`：0=关；`1..5` → 出售属性百分位 `percentile(value,min,max) ≤ 阈值`，阈值 1→30、2→60、3→75、4→90、5→100（即卖掉百分位 ≤ 阈值的低品质宝石）。百分位由 §3.6 的 `attr_range_probe_jewel_range` + `attr_range::percentile` 求得；探测不可用/失败时该物品 `jewel_percentile=-1`，规则不命中（fail-closed，不得默认 0 触发误售）。
- 特殊类型 `special_mask`：`0` = 关闭；非 0 位掩码与物品位标志按位与命中即出售（对任意物品生效）。
- 跨类独立：装备、宝石、特殊类型各自判定，命中任一即出售（全局等价 OR）。
- 保留按存档总开关 `enabled`（与全局开关独立；`enabled=false` 恒不售）。
- **开发期 dry-run（成品不得包含）**：命中规则时只打日志 + `wouldSell` 计数，**不扣物、不加钱**；由编译期开关控制（§4.5.2），不是运行时开关。
- 比较方向统一为 `≤`（用户 2026-09-12；值语义 2026-09-14）。

### 4.4 配置项与持久化

配置**按存档独立**，持久化到**模块存档 sidecar** 的独立 section `autosell`（version=1，UTF-8 JSON，**直写**，不进 participant/journal），**不写入 `config.json`**（用户 2026-09-13 裁决；sidecar 机制见 `../extension-bag/module-save-store.md`）。

section `autosell` v1 payload：

```json
{"v":1,
 "enabled":false,
 "rarity":0,
 "enhance":0,
 "socket":0,
 "gemTier":0,
 "gemRange":0,
 "specialMask":0}
```

| 字段 | 类型 | 默认 | 语义 |
|---|---|---|---|
| `enabled` | bool | `false` | 总开关（按存档） |
| `rarity` | int `0..5` | `0` | 0=关；1..5 → 出售 `rarity ≤ 值-1` |
| `enhance` | int `0..32` | `0` | 0=关；1..32 → 出售 `总强化次数（I_ENCHANT bits6-10）≤ 值-1` |
| `socket` | int `0..16` | `0` | 0=关；1..16 → 出售 `总孔数 ≤ 值-1` |
| `gemTier` | int `0..5` | `0` | 0=关；1..5 → 出售 `jewel_tier（category-28）≤ 值-1` |
| `gemRange` | int `0..5` | `0` | 0=关；1..5 → 出售属性百分位 `≤ 阈值`（30/60/75/90/99；最高档不出售满分宝石） |
| `specialMask` | int 位掩码 | `0` | 0=关；非 0 特殊类型多选（位与 `autosell_rules.h::SpecialType` 一致） |

- 键与 native `autosell::Config` 字段一一对应；**不再有** `rarityEnabled`/`rarityThreshold` 等成对开关与阈值键。
- 读写：`ModuleSaveStore.readSection/writeSection(slot, "autosell", 1, payload)`；`slot = current_save_slot()`。
- 加载：`autosell_tick` 顶部按 `slot` 变化惰性加载（与扩展背包同构）；主菜单/无存档保持默认且不扫描。
- 解析：缺失字段取默认 `0`；钳制 `rarity`/`gemTier`/`gemRange` 0..5、`socket` 0..16、`enhance` 0..32、`specialMask` 非负。
- 兼容：`v` 缺失或 ≤1 按 v1 解析；`v>1` fail-closed 用默认且**不删 section**；越界值钳制。
- 该组配置**不进入 `ModuleConfig` / 模块设置页 / `ConfigApiService` 全局下发**；由自动出售面板写入当前存档 sidecar，随存档加载/保存。
- 无 UI 阶段：由 debug JNI 应用并写入当前存档 sidecar，待面板接入。
- 默认值取安全侧：总开关默认关；各规则默认关。
- **无 `opEnabled` 门禁**（用户 2026-09-13 裁决）：自动出售为独立功能开关，与扩展背包同类。
- **风险（已裁决规避）**：`ModuleSaveCoordinator` 当前只支持单 participant 一次性 prepare/commit，第二个 participant 会覆盖 `module.save.journal`；因此自动出售采用**独立 section 直写**，不并入扩展背包 participant，也不另造 journal（`module-save-store.md` 双 journal 不可混用）。

### 4.5 扫描与执行

- **线程约束（强制）**：扫描与处置必须在游戏主线程执行；回调由统一帧任务管理器（`core/native/frame_task` + `frame_host`，锚点 `GAMESTATE_DrawPlay+0x20` 的 `bl MAP_DrawBase`）派发；不得在 detached 线程直接调游戏函数。
- 任务生命周期：**全局开关武装 + 进档注册 + 退档删除**——`nativeSetAutoSellEnabled` → `autosell_set_global_enabled` 置位/清位武装标志并同步任务；进入存档（读档/新档）回调 `autosell_register_save_enter` 按当前槽加载 sidecar 配置后同步任务，退出存档（world → 主菜单）回调 `autosell_register_save_exit` 清位后**无条件删除任务（兜底，不依赖存档配置）**。注册条件 = 已武装 && 已进档 && 存档配置 `config.enabled`（启动/主菜单不注册，存档开关关闭也不注册）；`autosell_apply_config` 写运行时配置，**仅存档总开关切换时**才同步任务。`autosell_tick` 顶部对全局开关与 `config.enabled` 双重防御；不常驻空转。
- 回调**无跨帧状态**：每次调用现取 `autosell_get_runtime_config()`，按规则扫描；不缓存、不在 tick 内读盘。
- 已接线（2026-09-13）：进入存档（读档/新档）由 `core/native/save_enter` 回调触发一次，按 `current_save_slot()` 读取 sidecar `autosell`（`autosell_store_ensure_loaded`）应用到运行时配置，再同步扫描任务。
- 单次扫描（原版袋 `0..4` + 扩展 5 袋逐槽）：
  1. 取物品指针；空槽跳过。
  2. 分类：装备 / 宝石 / 特殊类型。
  3. 按 §4.3 规则判定（装备 `equip`+rarity/强化次数/总孔数；宝石 `IsJewel`+category/属性百分位；特殊类型谓词）。
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

#### 4.5.2 开发期 dry-run（成品不得包含）

- 位置：`feature/autosell/autosell_scan.cpp` 顶部编译期宏 `#define AUTOSELL_DEV_DRY_RUN 1`（带醒目 TODO「发布前必须置 0 或删除该宏与分支」）。
- 行为：宏为 1 时，`process_ref` 在 `should_sell` 命中后只打日志并累加 `wouldSell`，**不扣物、不加钱、不调用出售链**；宏为 0 时走真实出售（NoSell 预过滤 → `inventory_trade::sell`）。
- 日志：`Inotia4AutoSell WOULD SELL bag=%d slot=%d category=%d rarity=%d enhance=%d socket=%d type=0x%x`（`type` = `ItemView.special_types` 位掩码）。
- 状态 JSON 增加累计 `wouldSell` 计数（成品应恒为 0；每次扫描重复计数属预期）。
- **不是运行时开关**，不得随成品发布；发布前必须移除/置 0。

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
- VM-AS11：开启时注册任务、扫描每 60 帧一次（日志帧号可核）；关闭时任务被删除（无回调）。
- VM-AS12：扩展袋物品处置物理槽与扩展账本一致。
- VM-AS13：空背包/满包/拖动中/存档界面/战斗中等边界无崩溃、无物品丢失。
- VM-AS14：同类内任一启用规则命中即出售（OR 语义）；跨类命中任一即出售。

## 8. 未决项

1. R1：`0x1261c4` 无 UI 可调用性；不可则改等价模块 sell 口径。
2. R9（已裁决）：强化规则用「当前总强化次数（`I_ENCHANT` bits6-10）」；每件「强化上限」不存在。残余：bits2-5 精确语义（不影响本规则）。
3. 宝石属性范围 feature 的稳定 API 与重入隔离（R7）。
4. 宝石 5 档命名与用户口述 3 档的差异确认。
5. 普通袋内任务物品与「不可销毁」判定函数。
6. 任务仅在开关开启期间存在；关闭后无扫描回调。与扩展背包锁/投影/拖动事务隔离。
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
- 配置按存档持久化到模块存档 sidecar，不用 `config.json`/`ModuleConfig`，不加 `opEnabled` 门禁（用户 2026-09-13）；入口按钮与扫描任务由模块设置第 6 项全局开关 `autoSellEnabled` 门控（默认关闭），面板内另有按存档的规则总开关。
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
- 截至验证时**不存在世界态通用每帧宿主**；后由并行功能补齐统一帧任务管理器（`core/native/frame_task.{h,cpp}` + `frame_host.{h,cpp}`，锚点 `GAMESTATE_DrawPlay+0x20` 的 `bl MAP_DrawBase`，真机已验证每帧派发）。
- **结论（已实现）**：扫描任务在「全局开关已武装 && 已进档 && 存档配置 `config.enabled`」时注册（`interval=60`，`frame_task` 负责节流）；无手写节流、无单次立即执行（`nativeAutoSellRunNow` 保留签名但不再单独触发扫描）。进档时按 sidecar 配置由 `save_enter` 回调自动应用（`autosell_register_save_enter`），存档开关变化由 `autosell_apply_config` 同步任务。
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

## 12. UI 可行性结论与原型（2026-09-13）

> 范围：本节只回答「入口按钮样式 / 类详情页弹窗 / 自绘内容 + 关闭即保存」三问，并记录已落地的**无功能原型**。原型不做真实配置读写、不做扫描接线、不做真机验证。

### 12.1 三个问题的结论

| # | 问题 | 结论 | 依据 |
|---|---|---|---|
| ① | 背包页入口按钮能否套用「设置」按钮样式 | **机制已确认可行；样式「完全复刻」未定稿** | 设置入口与背包入口同类：都是 `ControlButton` + `ExecuteProc`，点击回调里 `UI_SetPopupProcessInfo(1,id)` 打开面板（`game_ui_settings_injection.inc:6-28`）。但「设置按钮」本身是主菜单原生按钮，其贴图/DrawProc 只随主菜单控件树存在，**不是可搬到背包页的独立素材**；可复用的原版面板入口风格只有 Options 图组 `0x59`（返回箭头）与标题图组胶囊。原型采用自绘金色边框小按钮（不依赖图像单元加载），文字分两行「自动/出售」，待复核。 |
| ② | 点击后用「类物品详情页弹窗、双倍宽、遮住整页」 | **机制已确认可行；宽度基准未定** | 面板 = PopupState 死条目 push；`POPUPSTATE_Process/Event` 只走栈顶，push 后 EQUIP 的 process/draw/event 全部不执行（栈顶独占），触摸天然只归面板；全屏半透明遮罩 + 居中面板是设置/存档面板同款（`game_ui_settings_panel.inc:72-116`、`game_ui_savebackup_panel.inc`）。「两倍宽」的基准（详情面板实际宽度）**尚未真机量测**；原型暂取 2 × 0x180 = 0x300 = 768 ≤ 逻辑屏宽 1408。 |
| ③ | 内容自绘 + 关闭按钮即保存 | **自绘已确认；关闭即保存的接线点已存在，本轮未接** | 自绘用 `ui_begin_frame` + `GRPX_FillRect/FillRectAlpha` + `GRPX_SetFontColorFromRGB`+`GRPX_DrawStringWithFont`（settings/savebackup 真机先例）。保存接线点：`AutoSellConfigStore` / `autosell_store` / `ModuleSaveStore` section `autosell`（§4.4）。原型关闭只写日志。 |

### 12.2 原型实现（无功能）

- 入口按钮：独立 `ControlButton` 挂原版袋容器（与扩展背包页签同宿主，`G_UIEQUIP_PANEL_BAG_CONTAINER_VMA`），相对位置 `(68, 2+5×70)`、尺寸 `57×57`（袋列右侧、扩展页签之后）；点击回调 `UI_SetPopupProcessInfo(1, id)`。**不依赖扩展背包开关**；由全局开关门控（§13.2），默认关闭。生命周期强校验见 §13.1。
- 入口绘制宿主（关键选择）：用 `Scene_Draw_POPUP_SC_EQUIP + 0x1cc` 处 `bl UIDesc_Draw` 调用点的独立 BL patch（`call_patch_install_bl`，期望字 `0x97fdaadb`），wrapper 内先复刻 `UIDesc_Draw` 再绘制入口按钮。扩展背包占用的是同函数 `+0x210` 的 `bl GRPX_End`（`F_SCENE_DRAW_EQUIP_END_CALL_OFF`），两处地址不同，互不覆盖；扩展背包关闭时该 patch 也不安装，入口按钮仍显示。选此调用点是因为它在 `UIEquip_Draw`/`UIDesc_Draw` 之后、`GRPX_End` 之前，处于有效 GRPX 帧内，且不与扩展背包争用。
- 面板：改写 IAP 死条目 `F_PANEL_UNK3_ENTER`（`Scene_Init_POPUP_SC_INAPP_HOT`）的 enter/process/f3/f4/event 五回调；原型不做运行期还原。
- 面板几何：全屏半透明遮罩（`alpha=0x60`）+ 居中面板 `768×576`（`kPanelW=0x300`、`kPanelH=0x240`）。
- 面板内容：标题「自动出售」、副标题「原型占位：不读取、不保存配置」、7 行占位规则（总开关/装备×3/宝石×2/特殊类型，右侧灰框占位值）、金色边框「关闭」按钮。关闭按钮命中用本模块自算绝对矩形（`0x17` 按下、`0x18` 抬起且仍在按钮内 → 延迟 2 帧 `(3,0)` 关闭）。
- 涉及文件：`feature/ui/game_ui_autosell.{h,cpp}`；`game_symbols.h` 新增 `F_UIDESC_DRAW_VMA=0xb56f4`、`F_SCENE_DRAW_EQUIP_DESC_CALL_OFF=0x1cc`；`symbol_registry.h` 登记 `UIDesc_Draw`；`game_access.{h,cpp}` + `game_access_globals.inc` 增加 `fn_uidesc_draw`；`gamebridge.cpp` 在 `nativeInit` 调 `autosell_ui_install_if_ready()`；`CMakeLists.txt` 登记新 cpp。

### 12.3 未决与风险（原型不阻塞、定稿前必须处理）

1. **两倍宽基准未量测**：768 是「假定详情面板 384×2」；需真机截图量测 `UIDesc` 面板实际宽度后定稿宽度与是否真正「遮住所有背包页内容」。
2. **死条目选择**：`F_PANEL_UNK3_ENTER`（INAPP_HOT）现被原型长期占用且不还原；IAP 已被模块屏蔽，但仍需真机确认不影响其余 IAP 路径，或补「离开背包即还原」检测。
3. **入口绘制宿主冲突**：`+0x1cc` 与扩展背包 `+0x210` 地址不同（静态已确认），但需真机确认双开（扩展背包 + 自动出售）时绘制时序、点击、无互相遮挡/崩溃。
4. **触摸命中**：入口按钮命中依赖原生 `TouchHandle` 递归分发 `ExecuteProc`（扩展页签同宿主先例）；关闭按钮命中用自算绝对矩形。两者均需真机点按验证（含分辨率缩放下绘制与命中一致）。
5. **入口控件重建**：容器指针变化即判定旧控件失效；绘制前经「子控件 + 类型 + `ExecuteProc` 身份」强校验（§13.1），不通过即重建，不对未校验控件调用绘制。残余风险：容器与控件同址复用的极端情况，需真机回归覆盖多次开关背包。
6. **入口文案**：「自动出售」拆两行「自动/出售」，待主代理复核。
7. **API 屏幕枚举**：面板栈顶 enter 为模块函数，`data_ui_screen()` 会落到 `panel_ui_panel`（非 `in_app`）；原型不影响，若需 API 识别需另行登记。
8. **未做真机验证**：本轮仅静态审查 + host 测试 + Debug 构建通过；行为面（按钮显示/点击、面板开关、命中、扩展背包双开）一律待真机回归，当前整体 `NOT_ACCEPTED`。

## 13. 入口生命周期、全局开关与 JNI 契约（2026-09-13）

> 范围：native 侧入口按钮生命周期修复、全局开关门控、扫描任务生命周期、模块设置面板第 6 项、JNI 契约。真机验证由主代理执行。

### 13.1 入口按钮生命周期（防悬垂崩溃）

崩溃现象：控件树重建后对已失效控件调用 `ControlButton_Draw`，`pc=fault=容器地址`（把垃圾当函数指针跳转）。修复要点（`feature/ui/game_ui_autosell.cpp`）：

- `g_entry_container` 记录挂载容器；每帧读当前容器，**容器指针变化即判定旧控件失效并移除引用**（不销毁原版控件）。
- 绘制前强校验 `autosell_entry_valid(container)`：① `g_entry_ctrl` 必须是当前容器子控件；② `CO_TYPE == 3`；③ `CO_ACTIVE == 0x20`；④ `CO_DATA != null` 且 `CB_EXECUTE_PROC == &autosell_entry_clicked`。任一不符即置空并重建；**绝不**对未通过校验的控件调用 `fn_ctrl_btn_draw`。
- 校验顺序保证安全：容器指针比对与子控件指针扫描都不解引用 `g_entry_ctrl`；只有确认它仍在容器子链表中后才读取字段（此时控件必然存活）。
- 容器就绪守卫（child count ≥ 6）保持；`fn_ctrl_get_count`/`fn_ctrl_get_child` 为空或容器为空时安全跳过。
- 入口控件引用由 `g_entry_mtx` 保护（主线程绘制、JVM 线程开关写入）。

### 13.2 全局开关门控

- `autosell_ui_set_enabled(bool)` / `autosell_ui_enabled()`（`game_ui_autosell.h`）。
- 默认**关闭**（`g_enabled=false`）：关闭态不安装、不绘制入口按钮；开启后下一次 EQUIP 页绘制时安装。
- 关闭时：移除入口引用（不销毁原版控件）；`g_entry_ctrl` 残留控件即使仍被触摸命中，`autosell_entry_clicked` 也会因开关为关而直接返回；面板正打开则 `UI_SetPopupProcessInfo(3,0)` 关闭。
- 开启/关闭仅切换开关状态与任务/入口引用，不触碰游戏控件树销毁路径。

### 13.3 扫描任务生命周期（全局开关武装 + 进档注册）

- `autosell_set_global_enabled(bool)`（`autosell_scan.h/.cpp`）：置位/清位 `g_global_enabled` 武装标志后调 `sync_task()`；`sync_task` 在「已武装 && 已进档（`g_save_active`）&& 存档配置 `config.enabled`」时 `frame_task_add(kFramePointRenderPre, &autosell_tick, nullptr, kAutoSellScanIntervalFrames=60, 0)`，否则 `frame_task_remove`；幂等、`g_task_mtx` 保护。
- `autosell_apply_config(config)`：UI **关闭/销毁时提交一次**（面板内编辑不实时持久化）；写入运行时配置，**仅当存档总开关 `enabled` 切换时**才同步任务（开→按条件注册，关→`remove_task_if_any` 兜底删除），减少注册/删除时机、避免无关注册。
- `remove_task_if_any()`：无条件删除已注册任务，不读取全局/存档配置状态；用于退档、全局开关关闭、存档总开关关闭三条兜底路径。
- 进档注册：`autosell_register_save_enter()`（nativeInit 调用一次）注册 `save_enter` 回调；读档/新档进 world 后触发一次，`autosell_store_ensure_loaded(current_save_slot())` 应用 sidecar 配置、置位 `g_save_active`，再同步任务。
- 退档注册：`autosell_register_save_exit()`（nativeInit 调用一次）注册 `save_exit` 回调；world → 主菜单时触发一次，清位 `g_save_active` 并调 `remove_task_if_any()` **无条件删除任务（兜底，不读取配置/开关状态）**。全局开关关闭、存档总开关关闭同样走该兜底删除。
- `autosell_apply_config(config)` **只写运行时配置**，不再注册/删除任务（避免与全局开关两处争抢）。
- `autosell_tick` 顶部防御：`!g_global_enabled` 立即返回；随后仍检查 `config.enabled`（按存档规则总开关）与 `scan_gates_ok()`。

### 13.4 模块设置面板第 6 项

- `SETTINGS_ROW_COUNT` 5→6；`kRowKeys`/`kRowLabels`/`g_row_status` 各增一项：key `autoSellEnabled`、label `自动出售`。
- 布局：6 项填满网格前 3 行（2 列×3 行）；「存档备份」沿用 `settings_toggle_rect(SETTINGS_ROW_COUNT)`，现 index 6 → 第 4 行左侧，无重叠。
- 翻转走既有 `jni_toggle_config("autoSellEnabled")` → Kotlin；Kotlin 侧（并行 fixer）负责把变更下发到 `nativeSetAutoSellEnabled`。

### 13.5 JNI 契约（native 导出）

- 符号：`Java_com_inotia4_qol_NativeBridge_nativeSetAutoSellEnabled`（`bridge/native/gamebridge_autosell.cpp`）。
- 签名：`extern "C" JNIEXPORT jboolean JNICALL Java_com_inotia4_qol_NativeBridge_nativeSetAutoSellEnabled(JNIEnv*, jclass, jboolean enabled)`。
- 内部：`autosell_set_global_enabled(enabled == JNI_TRUE); autosell_ui_set_enabled(enabled == JNI_TRUE); return JNI_TRUE;`
- Kotlin 侧 `external fun nativeSetAutoSellEnabled(enabled: Boolean): Boolean` 由并行 fixer 声明。
- 已核验：debug 构建 arm64-v8a / armeabi-v7a 的 `libgamebridge.so` 均导出该符号。

按存档配置下发（**值即开关**，2026-09-14 起为 7 参）：

- 符号：`Java_com_inotia4_qol_NativeBridge_nativeSetAutoSellConfig`。
- 签名：`extern "C" JNIEXPORT jboolean JNICALL Java_com_inotia4_qol_NativeBridge_nativeSetAutoSellConfig(JNIEnv*, jclass, jboolean enabled, jint rarity, jint enhance, jint socket, jint gemTier, jint specialMask, jint gemRange)`。
- 入参：`enabled` 总开关；`rarity` 0..5、`enhance` 0..32、`socket` 0..16、`gemTier` 0..5、`specialMask` 非负（空=关）、`gemRange` 0..5（0=关）。native 侧按边界钳制（M-11），填 `autosell::Config` 后 `autosell_apply_config(cfg)`；当前存档槽合法时直写 sidecar `autosell` section。
- Kotlin 侧 `external fun nativeSetAutoSellConfig(enabled: Boolean, rarity: Int, enhance: Int, socket: Int, gemTier: Int, specialMask: Int, gemRange: Int): Boolean`（`NativeBridge.kt`）；开发期 API `AutoSellFeatureController` 以 `json.optInt("gemRange", 0)` 传入。
- `nativeAutoSellRunNow` / `nativeAutoSellStatusJson` 签名不变；状态 JSON 现含 `wouldSell`（开发期 dry-run 计数，成品应恒为 0）。

### 13.6 未决与风险

1. **真机回归**：反复开关背包、扩展背包开/关、切档、界面开启/关闭切换，验证无悬垂崩溃；全部待真机证据，当前 `NOT_ACCEPTED`。
2. **关闭态残留控件**：入口控件不销毁，触摸仍可能命中该位置（已由 `autosell_entry_clicked` 开关防御拦截功能影响）；是否需彻底禁用其触摸待真机评估。
3. **两倍宽基准**、**死条目占用**、**入口文案** 同 §12.3。
