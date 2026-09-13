# 合成器宝石合成操作优化（gem-craft-optimization）设计书

> 状态：**已实现并真机验证通过（2026-09-13）**。阶段1（放料解绑 + 自动选中格 + 开关）与阶段2（合成行为：同档校验 + 错误提示 + 产物档位推导）均已落地；真机 VM-GC1/2/3/5/6/11 与 VM-GC7/8/9/10 全部通过（见 §7）。本册是本功能的范围、目标行为、技术方案与验收标准的权威。
> 来源：2026-09-13 用户需求 + 同日 6 项决策对齐（§8）。
> 功能定位：独立的合成器**交互优化**功能，不归属扩展背包；但与扩展背包的合成器宿主（`module/app/src/main/cpp/feature/extension_bag/extension_bag_mix.inc`）共用同一 UIMix 面板，实现时必须协调。
> 关联：游戏合成机制 `../../reference/game/game-systems.md` §6.4、游戏 UI 机制 `../../reference/game/ui.md` §3；扩展背包合成器宿主 `../extension-bag/runtime-architecture.md` §2.5.1、规则 `../extension-bag/rulebook.md` R-56/R-57、验收 `../extension-bag/verification-matrix.md` VM-22。
> 参考（已删除的旧研究稿，仅追溯）：`git show 94ed927:docs/features/craft-batch-ui.md`（批量宝石合成，与本次「单次操作优化」不是同一功能）。

## 1. 范围与目标

### 1.1 用户原始需求

> 新增功能：合成器宝石合成操作优化。在宝石合成界面，不管选择的是合成哪种配方，都可以填入任何宝石，只要三个都是同等级宝石，就允许合成到下一个等级，不同等级弹出错误提示框。打开宝石合成面板时，自动选中第一个填入格。当选中格子被填入时，自动选择下一个未填入的格子，全部填满了就不选中了。点击合成后，自动选中第一个格子。

### 1.2 目标行为（对齐结论，2026-09-13）

在 UIMix 宝石合成视图（type 1）内：

1. **放料解绑配方**：不再要求放入的宝石与当前所选配方的材料档位一致；任何宝石都可放入空格。
2. **放料不校验等级**：放料阶段允许多档宝石共存，**不弹错、不拒绝**（错误只在合成时给出，见 3）。
3. **合成时同档校验**（原版「不同等级 = 不同配方」，本功能让**任选其中一个配方都支持**任意档材料）：
   - 三格已填满且**同一档位**（同 category，28..31）→ 合成为其**上一档**（低→中→高→顶级）。
   - 三格已填满但**档位不一致** → 用**游戏原生提示框**提示，取消合成。
   - 三格已填满且 `category == 32`（混沌）→ 用**游戏原生提示框**提示「已是最高等级」，取消合成。
   - 三格未填满 → 沿用原版行为（不进入本功能的改写分支）。
4. **产物按材料档位推导**：合成使用 `mixType = 12 + (category - 28)`，**与当前选中的配方无关**。
5. **自动选中格**：
   - 打开宝石合成视图时，自动选中第一个填入格。
   - 当前选中格被填入后（放料成功），自动选中下一个未填格。
   - 三格全部填满时，清除选中（不选中任何格）。
   - 点击合成后（填入格被清空），自动选中第一个填入格。
   - **手动点击选中填入格不触发自动跳转**。
6. **配置开关**：新增开关控制本优化，**默认关闭**；关闭时完全走原版行为。

### 1.3 达到的效果

- 玩家无需先理解「哪档宝石配哪档配方」，只要凑齐三个同档宝石即可合成，减少误操作与来回切换配方。
- 连续放入宝石时选中格自动前进，凑料更顺手。
- 档位不一致或放入混沌宝石时在点击合成时得到游戏内错误提示，而不是静默失败。

## 2. 非目标

- 不新增 HTTP API 端点。
- 不改动原版存档结构、`ITEMDATABASE`/`RECIPEBASE`/`MIXTUREBASE` 静态数据。
- 不实现批量合成（旧的 `game_patch_craft.inc` 批量旁路保持现状，不在本功能范围内复活）。
- 不改变宝石合成的 3:1 比例、费用计算、词条继承规则（沿用游戏 `MIXSYSTEM_*`）。
- 不改变其他 4 个合成类型（混沌/打孔/其它/传说）的行为。
- 不接管扩展背包页签/投影（沿用 `extension_bag_mix.inc` 既有契约）。
- 不引入新 Hook 机制：沿用现有 `PtrHook`（数据段函数指针覆盖）、PopupState 回调包装、单指令 BL patch、直接调用游戏函数四种手段（ShadowHook / inline trampoline 在本项目环境不可用）。

## 3. 事实基础（已核实）

依据：`llvm-objdump` 反汇编 `apk/decoded/overhaul/lib/arm64-v8a/libgame.so`（大修版 20260704）+ 静态表 `apk/static-data/json/tables/`。VMA 为大修版；三版本（大修/monster/原版）**符号名一致、VMA 不同**，实现必须走动态符号解析（`fn_resolve`），禁止在域文件写裸 VMA。

### 3.1 宝石合成界面结构（UIMix type 1）

- 合成类型存于 UIMix 实例 `+0x38`（`game_symbols.h` 记 `[0x305000+0x588]`）：`0=混沌`、`1=宝石合成(3:1)`、`2=打孔`、`3=其它配方`、`4=传说/Unique`。
- 宝石合成视图有 **3 个填入格**：`UIMix_InitMixingState@0xc0038` 的 type==1 分支（`0xc017c`）执行 `[g_uimix+0xf0] = 3`，并按 `3×16` 字节分配 stuffList（`[g_uimix+0xe8]`）。填入格是材料组 `[g_uimix+0xc8]` 的前 3 个子控件（`UIMix_ResetStuffItemControl@0xc0240` 依 `[+0xf0]` 显示 1..4 格）。
- **当前选中填入格下标：`[g_uimix+0x128]`（i64，`-1` = 未选中）**。写入点：
  - 点击填入格：`UIMix_StuffItemControlEventProc@0xc0ec8`（`UI_GetChildIndex` → 写入 `+0x128`）。
  - 选择配方/进入 type 1：`UIMix_ButtonRecipeExe@0xc0390` 与 `UIMix_ButtonMenuListExe@0xc05c0` 置 `+0x128 = -1`。
- **放料流程（原版）**：点填入格选中 → 点背包宝石（选中态存于背包组 `[g_uimix+0xd8]` 的控件 cursor，非全局变量）→ 点详情菜单按钮 `[g_uimix+0x130]`（`ExecuteProc = UIMix_ButtonInvenItemSelectExe@0xc2328`）确认放入。**不是拖拽**（材料格与背包格创建时已 `UnuseControlEventMove/Drop`）。
- **放入实现**：`UIMix_ButtonInvenItemSelectExe` type 1 分支 `GetChild([g_uimix+0xc8], [+0x128])` → `ControlItem_SetItem(slot, gem)`（`0xc2524`）。

### 3.2 原版放料校验（本功能要改写的位置）

`UIMix_ButtonInvenItemSelectExe@0xc2328` 的 type 1 分支（`0xc23bc`）：

1. `category = UTIL_GetBitValue(*(u16*)(item+0x08), 15, 6)`；`ITEMSYSTEM_IsJewel(category)` 为假 → 弹文本 `0x61`（`0xc23d0`）。
2. 取 `stuffList[0]`（`[g_uimix+0xe8]` 首项 = 当前配方材料 itemId，即材料档位 category）与放入宝石的 `category` 比较，**不等 → 弹文本 `0x62` 并拒绝**（`0xc24c0`–`0xc24cc`）。**这就是「配方不匹配」校验，本功能要移除它（改为放料只看是否宝石）。**
3. `UIMix_FindJewelUpgradeStuff@0xc2164` 判该宝石是否已放入 → 已放 → 弹文本 `0x63`（保留）。
4. `[g_uimix+0x128] < 0`（未选中填入格）→ 直接返回，不放（保留）。
5. 通过后 `ControlItem_SetItem(GetChild([+0xc8], [+0x128]), gem)`。

> 注意：原版**没有**独立的「三个宝石必须同档」检查——它靠「配方只对应一个材料档位」间接保证三格同档。移除配方约束后，原版不再有任何档位一致性保证；本功能在**合成时**自行校验（§4.2）。

### 3.3 宝石数据模型

- 物品结构：`I_TYPE = 0x08`（u16）；`category = (I_TYPE >> 6) & 0x3FF`（宝石 = 28..32）。判定 `ITEMSYSTEM_IsJewel@0x10b964`。
- 宝石档位（`ITEMDATABASE` 记录 28..32）：**28 低级 / 29 中级 / 30 高级 / 31 顶级 / 32 混沌**。
- 宝石实例随机字段 `item+0x10` 位域：bits0-10 = 属性值、bits11-17 = 随机等级、bits18-23 = 属性类型（力量/敏捷/智力…），见 `../attribute-range-display.md`。
- **本需求中的「同等级」= 宝石档位 category（28..32）**，不是 `+0x10` 的随机等级（游戏合成校验用的就是 category）。
- 同档内允许不同**属性类型**（力量/敏捷/…）混合，产物属性类型沿用游戏 `ITEMSYSTEM_CreatePerfectItem` 继承规则（材料同类型 ≥2 → 继承，否则随机）。

### 3.4 配方与产物

`RECIPEBASE`（69 条 × 12B，大修版实测）：

| mixType | 产物 itemId | 产物 category | 材料（`MIXTUREBASE` 下标） | 材料档位×数量 | 费用文本 |
|---|---|---|---|---|---|
| 12 | 1163 | 29 中级 | 19 | 低级 28 ×3 | 188 |
| 13 | 1164 | 30 高级 | 20 | 中级 29 ×3 | 189 |
| 14 | 1165 | 31 顶级 | 21 | 高级 30 ×3 | 190 |
| 15 | 1166 | 32 混沌 | 22 | 顶级 31 ×3 | 191 |

- 即 **同档 3 个 → 上一档**；混沌（32）无上级（`category==32` 无对应 mixType 16，视为不可合成）。
- 当前 mixType 存于 `[g_uimix+0x48]`，由所选配方决定。
- 产物词条/属性类型继承由游戏 `MIXSYSTEM_MakeItem` → `ITEMSYSTEM_CreatePerfectItem` 处理，模块不复制。

### 3.5 合成执行与合成后清理

- 合成按钮 `[g_uimix+0x98]`，`ExecuteProc = UIMix_ButtonMixingExe@0xc21ec`：
  - 读 `[g_uimix+0x48]`（mixType）与 `[g_uimix+0xf8]`（费用，`UIMix_InitMixingState` 依配方费用文本经 `CAL_Calculate` 算出）；金币不足弹文本 `0xa`；
  - 弹 `UIPopupMsg_CreateYesNoFromTextData` 确认 → `UIMix_StartMix@0xc0870`。
- `UIMix_StartMix` type 1：遍历材料组 `[+0xc8]` 子控件逐个 `INVEN_RemoveItem` 扣料 → `MIXSYSTEM_MakeItem(mixType)` → `INVEN_SaveItem` → `INVEN_MinusMoney` → `UIMix_InitMixingState` → `UIMix_ResetStuffItemControl`（清空填入格并重排）→ `UIMix_RefreshInvenItem` → 音效 → 弹文本 `0x6a`（成功）。
- **合成成功后会重建 stuffList、清空填入格**——这是「点击合成后自动选中第一个格」的触发时机。

### 3.6 可复用机制

- `extension_bag_mix.inc` 已包装 UIMix 的 popup ENTER/F3 回调，并 BL patch 了 `Scene_Draw_POPUP_SC_MIX` 的 `bl GRPX_End` 挂 **draw-end wrapper**（`mix_draw_end_wrapper`）；这是面板打开期间每帧执行、且在游戏主线程的现成宿主。
- `PtrHook`（`module/app/src/main/cpp/game_ptr_hook.h`）可覆盖数据段函数指针（如动态创建的按钮 `+0x50 → +0x20 ExecuteProc`），先例：`game_patch_craft.inc`、`extension_bag_store.inc`。
- 直接调用游戏函数：`fn_control_item_set_item`（`extension_bag_mix.inc` 已有先例）、`fn_ui_mix_refresh_inven_item`。
- 读取填入格内容：`ControlItem_GetItem`（`UIMix_StartMix` type 1 遍历材料格即用它）。
- 原生弹窗：`UIPopupMsg_CreateOKFromTextData`（游戏文本表 id）与模块 `data_exp3_custom_dialog`（`feature/ui/game_ui_exp.cpp`，支持自定义文本缓冲）两条路径。

## 4. 设计

### 4.1 放料解绑配方（仅移除档位匹配校验）

**接入点：覆盖详情菜单按钮 `[g_uimix+0x130]` 的 `ExecuteProc`（数据段函数指针覆盖，即 `PtrHook` 语义）。**

- 该按钮由 `UIMix_SetDescMenu`@0xc0be4 在选中背包物品时经 `UIDesc_AddMenuButton`@0xb5338 动态创建，`ExecuteProc` 初值 `UIMix_ButtonInvenItemSelectExe`@0xc2328。
- **安装点：创建时 gate（不逐帧重挂）**：BL patch `UIMix_SetDescMenu` 内 `0xc0c4c bl UIDesc_AddMenuButton`（偏移 `+0x68`，原字 `0x97ffd1bb`）；gate 复刻原调用后，对该次新建按钮装 wrapper。之所以不用逐帧懒安装：`game_memory_accessible` 每次 open + 扫描 `/proc/self/maps`（`game_access.cpp:22-45`），逐帧重挂约 180 次扫描/秒，代价过高（§10）。
- 自定义处理函数 `gemcraft_place_execute(void* ctrl)`（`ExecuteProc` 签名 `void(void*)`；`type != 1` 或功能关闭时直接转调原函数）：
  1. 取当前背包选中物品：`fn_control_object_get_cursor(*(void**)(g_uimix+0xd8))` → `GetData` → `*data`（与原函数 prologue 同路）。
  2. 非宝石 / 取不到物品 / 符号未解析 → **直接转调原函数**：原函数对非宝石弹文本 `0x61` 拒绝，重复放入弹 `0x63`，未选中格直接返回——**这些原版校验全部保留**。
  3. 是宝石 → 把 `stuffList[0]`（`*(uint32_t*)(g_uimix+0xe8)`，当前配方材料档位）**临时改为该宝石 category**，转调原函数（其 `0xc24c8 cmp w0,w2 / 0xc24cc b.ne` 档位比较必然通过，放入目标格 = `[g_uimix+0x128]`），随后还原。
- 语义：**放行任意宝石、不放行非宝石物品**；`UIDesc_SetOff` 等副作用由原函数执行。
- **风险 R1**：`[g_uimix+0x130]` 删除后不置空（陈旧指针），创建时 gate / 一次性安装路径必须先过 `game_memory_accessible` 守卫再读写按钮数据块（照抄商店 `store_make_desc_gate`）。

### 4.2 合成时同档校验 + 产物档位推导 + 错误弹窗

**接入点：覆盖合成按钮 `[g_uimix+0x98]` 的 `ExecuteProc`（`PtrHook`）。**

自定义处理函数 `gem_craft_exec()`（`type != 1` 时直接转调原函数）：

1. 读取三格（`[+0xc8]` 前 3 个子控件）的已放宝石，取各自 `category`。
2. **未填满（<3）** → 转调原 `UIMix_ButtonMixingExe`（沿用原版行为，含材料不足判定）。
3. **填满且档位不一致** → 弹错误「请放入相同等级的宝石」，**不进入合成**。
4. **填满且 `category == 32`（混沌）** → 弹错误「混沌宝石已是最高等级」，**不进入合成**。
5. **填满且同档 `category ∈ 28..31`** → 推导 `mixType = 12 + (category - 28)`，写 `[g_uimix+0x48]`，调用 `UIMix_InitMixingState` 重算 stuffList 与费用 `[+0xf8]`，然后转调原 `UIMix_ButtonMixingExe`。
   - **必须重算 `[+0xf8]`**：否则金币校验与扣费仍按旧配方档位（如配方 12 费用 188、实际材料为 30 → 应 190）。
   - 确认弹窗（`CreateYesNoFromTextData`）与 `UIMix_StartMix` 沿用原版，不改扣料/产物语义。

**错误弹窗形式（用户裁决）**：**复用游戏原生提示框**——调 `UIPopupMsg_CreateOKFromTextData`，文案复用游戏文本表既有项 `0x62`（原版即在「宝石与配方材料档位不符」时弹出，语义与「三格必须同档」一致），零新增文本资源；混档与混沌两种情形都走同一原生提示路径（如后续发现更贴切的既有文本项再替换）。

- **风险 R4**：改写 `[+0x48]`/`[+0xf8]` 后费用与产出需真机对拍。

### 4.3 自动选中格（事件驱动，避免覆盖手动选择）

统一 helper：

```text
select_first_empty_or_none():
  若三格全满 → [g_uimix+0x128] = -1
  否则        → [g_uimix+0x128] = 索引最小的空格 (0..2)
```

触发时机（**只在事件发生时调用**，不做每帧强制，以免覆盖手动选择）：

| 时机 | 触发方式 | 结果 |
|---|---|---|
| 进入宝石合成视图（选类型/选配方） | 覆盖类型按钮 `UIMix_ButtonMenuListExe`@0xc05c0 与配方按钮 `UIMix_ButtonRecipeExe`@0xc0390 的 `ExecuteProc`（二者均在函数尾部写 `[+0x128]=-1`，即链终点），调原函数后写选中 | 选中第一个空格（index 0） |
| 放料成功 | `gemcraft_place_execute` 调原函数后比较已填格数增加 | 选中下一个未填格；全满则 `-1` |
| 点击合成、填入格被清空 | BL patch `UIMix_StartMix+0x258` 的 `bl UIMix_ResetStuffItemControl`@0xc0ac8，调原函数后写选中 | 选中第一个空格（index 0） |
| 手动点击填入格 | 不触发 | 保持手动选择 |

- 顺序规则采用**升序第一个空格**（用户确认「顺序随意，以最容易开发、最不会出错的方式」）。
- 三个按钮 `ExecuteProc` 初值由 GOT 槽一次性改写（desc `0x2f4598` / 菜单 `0x2f6658` / 配方 `0x2f40d0`，照 `game_patch_move_merge.inc` 的 mprotect + PtrHook 先例，槽值校验 fail-closed）；**无逐帧 tick、无每帧 maps 扫描**。

**可见性风险 R2**：`[+0x128]` 只决定「放入目标格」；选中格的**高亮绘制**可能另由材料组 `[+0xc8]` 的控件 cursor 表示（原版点击填入格走 `UIMix_StuffItemControlEventProc`，具体高亮通路需真机确认）。若仅写 `+0x128` 没有可见高亮，则需同时设置 `[+0xc8]` 的 cursor / `ControlItem_SetFocus`。实现时以真机表现定案。

### 4.4 配置开关

新增 `ModuleConfig` 布尔键（沿用 `config.json` 唯一配置来源 + `ModuleConfigUiBridge` 下发）：

| 键 | 类型 | 默认 | 语义 |
|---|---|---|---|
| `gemCraftOptimize` | bool | **`false`** | 启用本优化；关闭时完全走原版（配方约束、无自动选中、无合成期改写） |

运行时关闭：wrapper 由 `g_gemcraft_enabled` 门控 no-op（**不回写已覆盖的指针**，避免按钮已销毁时的 use-after-free）；两个 BL patch 永久驻留、门控生效。键名与默认值已定（`gemCraftOptimize` / `false`）。

### 4.5 模块分层与文件规划

| 层 | 规划内容 |
|---|---|
| data/symbols | 新增登记：`UIMix_GetType(0xbf47c)`、`UIMix_SetType(0xbfe44)`、`UIMix_ButtonInvenItemSelectExe(0xc2328)`、`UIMix_FindJewelUpgradeStuff(0xc2164)`、`UIMix_InitMixingState(0xc0038)`、`UIMix_ButtonMixingExe(0xc21ec)`、`UIMix_StuffItemControlEventProc(0xc0ec8)`、`ControlObject_GetCursor`、`ControlItem_GetItem`、`ControlObject_SetCursor`/`ControlItem_SetFocus`、`MIXSYSTEM_GetResultItemCount(0x11aa5c)` 等；跑符号校验脚本 |
| native feature | 新增 `feature/gemcraft/`（纯逻辑：档位→mixType 映射、三格同档判定、下一空格计算，可编 host 单测）+ `feature/ui/game_ui_gemcraft*`（UIMix 接入） |
| bridge/JNI | 配置下发与启停（同既有 `set_*_enabled` 模式） |
| Kotlin | `ModuleConfig` 新键 + `ModuleConfigUiBridge` |
| 复用 | UIMix draw-end wrapper、PtrHook、`fn_control_item_set_item`、`fn_ui_mix_refresh_inven_item`、`extension_bag_mix.inc` 的字段读写 helper |

### 4.6 与扩展背包合成器宿主的协调

- 两者在同一 UIMix 面板：本功能只操作 `[+0x38]`(type)、`[+0xc8]`(材料格)、`[+0x128]`(选中)、`[+0x48]`(mixType)、`[+0x130]`/`[+0x98]`(按钮回调)；扩展背包宿主操作 `[+0xd8]`(物品网格)、`[+0xe0]`(袋组)、`[+0x20]`(state)。**字段不重叠**。
- 二者都挂 draw-end wrapper：实现时合并为同一 wrapper 内的顺序调用，或确认多处 BL patch 不互斥（同一 `bl GRPX_End` 调用点只能挂一个 thunk）。**风险 R3**。
- 扣料路径 `UIMix_StartMix → INVEN_RemoveItem`/`INVEN_RemoveItemData` 的扩展袋补扣属 R-56；本功能不改扣料语义，保持证据。

## 5. 风险与验证方案

| 编号 | 风险 | 验证方案 | 判定标准 |
|---|---|---|---|
| R1 | 详情菜单按钮重建 / `[+0x130]` 陈旧指针 | 创建时 gate 即时包装新按钮 + `game_memory_accessible` 守卫；真机反复选中/切换物品后放料 | 覆盖始终生效；无崩溃 |
| R2 | 仅写 `[+0x128]` 无可视选中高亮 | 真机观察填入格高亮 | 选中格可见高亮，与放料目标一致 |
| R3 | 与扩展背包 draw-end wrapper 冲突 | 同时启用扩展背包与本功能，进合成器 | 页签/投影/本功能都正常；无重复 BL patch 报错 |
| R4 | 改写 `[+0x48]`/`[+0xf8]` 后费用与产出不一致 | 真机放入不同档材料，比对扣费与产物 | 扣费 = 实际档位费用；产物 = 上一档 |
| R5 | 混沌/混档在合成时的拦截与提示 | 三格放混沌、放混合档，点合成 | 各自弹对应错误框；不扣料、不产物、不崩溃 |
| R6 | 关闭开关无法还原 | 运行时开关切换 | 关闭后完全恢复原版行为（配方约束、无自动选中） |
| R7 | 合成确认弹窗取消后选中态 | 点合成后取消确认 | 填入格与选中态保持一致，无越界 |

## 6. 分阶段实施计划（均已完成）

1. **阶段 0（先决侦察）**：已确认 `[+0x128]` 为高亮源（写它即可见选中，无需 cursor）、并选定各链终点锚点。
2. **阶段 1（放料解绑 + 自动选中格）✅**：实现 §4.1 与 §4.3 + 开关；真机验证 VM-GC1/2/3/5/6/11 通过。
3. **阶段 2（合成行为：同档校验 + 错误提示 + 产物档位推导）✅**：实现 §4.2 步骤 1–5（原设计的阶段2/阶段3 合并）；真机验证 VM-GC7/8/9/10 通过。
4. **阶段 3（真机验收）✅**：真机取证完成（§7 尾注）。

## 7. 验收标准（VM 草案）

- VM-GC1：打开宝石合成视图，自动选中第一个填入格。
- VM-GC2：向选中格放入一个宝石后，自动选中下一个空位。
- VM-GC3：三格填满后，无任何格被选中。
- VM-GC4：点击合成（填入格被清空）后，自动选中第一个填入格。
- VM-GC5：任何档位宝石都能放入空格（不再受所选配方限制）；放料阶段不因档位不同弹错。
- VM-GC6：手动点击某个填入格，选中态保持该格，不被自动逻辑跳走。
- VM-GC7：三格同档（28..31）合成成功，产物为上一档；扣费按实际材料档位（与所选配方无关）。
- VM-GC8：三格混档点合成 → 弹「请放入相同等级的宝石」，不扣料、不产物。
- VM-GC9：三格均为混沌(32) 点合成 → 弹「混沌宝石已是最高等级」，不扣料、不产物。
- VM-GC10：同档内不同属性类型宝石可混合放入，产物属性类型走游戏继承规则。
- VM-GC11：关闭开关后，原版配方约束与选中行为完全恢复。
- VM-GC12：扩展背包同时启用时，合成器页签/投影与本功能均正常（R3）。

> **真机验证记录（2026-09-13）**：设备 `192.168.3.54:5555`；阶段1 debug APK `2609131007-5edbeb3737b8`，阶段2 debug APK `2609131024-e2e695cfc15f`。阶段1 通过 VM-GC1/2/3/5/6/11；阶段2 通过 VM-GC7/8/9/10（同档产物上一档、扣费按实际档位；混档/混沌弹原生提示且不扣料不产物）。开关 `gemCraftOptimize` 默认关闭，测试时经 `/api/config/set` 开启。

## 8. 决策记录（2026-09-13 用户对齐）

| 编号 | 问题 | 用户裁决 |
|---|---|---|
| D1 | 「同等级」定义 + 混属性 | **按档位 category**（低/中/高/顶/混沌）；**允许**同档内不同属性类型混合，产物走游戏继承规则 |
| D2 | 顶级(31)→混沌(32) | **允许** 31→32；混沌本身不可再合成 |
| D3 | 错误提示时机与形式 | **点合成时才弹**（放料阶段不校验等级）；**复用游戏原生提示框**（`UIPopupMsg_CreateOKFromTextData`，文案复用既有文本项 `0x62`） |
| D3b | 原版「不同等级不同配方」 | **任选其中一个配方都支持**任意档材料；合成产物由实际放料档位决定，与所选配方无关 |
| D4 | 三格未满点合成 | 沿用原版行为（本设计不处理） |
| D5 | 配置开关 | **新增开关，默认关闭**（键名 `gemCraftOptimize`，已实现） |
| D6 | 自动跳格顺序 / 手动选择 | 填入后跳走、手动选中不触发；顺序随意，取**升序第一个空格**（最易实现、最不易出错） |

## 9. 影响契约与保持证据（实现时必填）

| 受影响项 | 关系 | 保持证据 |
|---|---|---|
| R-56 `INVEN_RemoveItemData` 扩展补扣 | 本功能不改扣料语义，但改写了进入 `UIMix_StartMix` 的 mixType | 无锚-待补（Host 测试 / 真机 VM-GC7） |
| R-57 原版袋列高亮遮蔽 | 与扩展背包宿主共用 draw-end wrapper（R3） | 无锚-待补（VM-GC12） |
| 扩展背包 VM-22 合成回滚 | 合成链未被本功能替换，仅前置改写 mixType/费用与拦截 | 无锚-待补 |
| `game_patch_craft.inc` 批量旁路（死代码） | 本功能不复活该旁路；若未来接线需重新评估 | 保持不接线 |

## 10. 决策与理由（思考，非当前事实）

- **为什么用 PtrHook 覆盖按钮回调而不是改指令**：校验内联在 `UIMix_ButtonInvenItemSelectExe` 的函数体里（`0xc24c8` 的 `b.ne`），改指令属 inline patch、跨版本 VMA 不同且需硬编码原指令字；覆盖数据段函数指针更安全、跨版本只需符号 + 固定偏移。
- **为什么错误校验放在合成时（而非放料时）**：用户 2026-09-13 明确「点合成时才弹」。好处是放料阶段完全自由，玩家可先放入再调整；代价是原版放料时的即时反馈消失，需在合成入口补校验。
- **为什么错误提示复用原生提示框**：用户 2026-09-13 明确「复用游戏原生的提示框」。原版放料校验本就用 `UIPopupMsg_CreateOKFromTextData` + 文本项 `0x62` 表达「宝石与配方档位不符」；本功能把该语义平移到「三格必须同档」，复用同一原生组件与文本项，零新增资源、风格一致。
- **为什么产物按材料推导**：用户的意图是「配方不再决定能放什么」，若仍用所选配方的 mixType 产出，则放入高级宝石却选了低级配方会产出错误档位；必须由实际材料决定。
- **为什么自动选中用事件驱动而非每帧强制**：每帧强制会在玩家手动点击已填格时立即把选中跳走，违反「手动选中不触发」；事件驱动只在打开/放料成功/合成清空三个时机跳转。
- **为什么先做阶段 1**：放料解绑 + 自动选中是纯状态读写，不触碰合成执行与费用，先落地可最快获得真机反馈。
- **为什么安装点用「链终点挂钩」而非逐帧采样**：详情按钮每次选物品都重建、逐帧重挂需反复调用 `game_memory_accessible`（每次 open + 扫描 `/proc/self/maps`，`game_access.cpp:22-45`），代价约 180 次扫描/秒。改为：按钮 `ExecuteProc` 初值在 GOT 槽一次性改写（新按钮自动带 wrapper），合成清空点用一条 BL patch；自动选中全部由「链终点」事件触发，无逐帧 tick。
- **为什么用「覆盖 ExecuteProc」而非 NOP 拦截分支**：`0xc24cc` 的 `b.ne` 是拦截本体，NOP 它最直接，但属 inline 指令 patch（函数内偏移 + 原字，跨版本靠 fail-closed）；覆盖数据段函数指针只需符号，且能用 wrapper 精确表达「放行任意宝石、不放行非宝石」并保留原函数全部其余校验，故采用后者。
