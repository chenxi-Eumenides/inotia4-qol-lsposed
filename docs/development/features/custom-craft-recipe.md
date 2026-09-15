# 合成器自定义配方（custom-craft-recipe）功能设计

> 状态：**设计稿（未实现，未写任何功能代码）**。本文经用户确认后方可进入实现。
> 来源需求：`idea.md` 第 7 项「宝石升级」；用户 2026-09-15 指令（注入配方 + 选中时改写按钮/放料 + 配方 1 定义 + 配方框架须可复用且支持多条）。
> 本文是「向合成器注入模块自定义配方，并改写该配方的放料与合成行为」的唯一设计册。
> 依赖事实册：`gem-craft-optimization.md`（UIMix 面板内部结构）、`attribute-range-display.md`（档位定义与宝石范围探测）。

## 1. 范围与职责

- 在**合成器 → 混沌合成面板**（UIMix type 3）注入**模块自定义配方**，使其成为该面板的追加配方按钮。
- 自定义配方由**配方目录（catalog）驱动**：目录是唯一真源，表注入、放料改写、产物改写全部由目录派生；**新增配方只需向目录追加一项，不改动注入与 hook 代码**。
- 选中自定义配方时改写两条行为：**放料**（目标槽接受宝石）与**合成按钮**（产物按配方种类计算）。
- 不影响原版 2 条配方（混沌合成 / 深渊混沌合成），不影响其余 4 个面板。
- 不改动原版属性数值公式；只写回目标宝石自身的数值。

## 2. 需求规格（已对齐，冻结）

### 2.1 注入落位

混沌合成面板（UIMix type 3）。依据：该面板的目标槽是「单个物品」且合成时对目标物品**原地改写**（原版即改装备本体），与「提升宝石自身数值」语义一致；材料由游戏自动扣除，改造面最小。（用户 2026-09-15 裁决）

### 2.2 配方框架（本轮冻结）

| 要求 | 规格 |
|---|---|
| 可复用 | 配方的「显示项 / 材料表 / 费用 / 行为种类」由**目录声明**定义；注入与 hook 均为目录驱动，不含任何具体配方的硬编码分支 |
| 支持多条 | 目录长度为 N（N ≥ 1）；表注入按目录顺序为每条配方追加 1 条 `RECIPEBASE` 记录与对应 `MIXTUREBASE` 材料条目 |
| 动态下标 | 注入记录的 mixType 由**运行时原版记录数**推导（`base + i`），不写死 69 |
| 本轮注册数 | 1 条（配方 1）；框架保证再加配方只需向目录追加一项 |
| 开关粒度 | 本轮为**单一总开关**；不做逐配方独立开关 |

### 2.3 配方 1 定义（冻结）

| 项 | 规格 |
|---|---|
| 行为种类 | `kJewelTierUp`（目标宝石数值提升一档） |
| 目标槽 | 1 个独立宝石物品 |
| 材料 | 卓越灵药（itemId 15）×N，界面上仍以 **1 个材料格**显示；**N 由宝石档位与角色等级决定**（§2.6） |
| 费用 | **0**（不扣金币；只消耗卓越灵药） |
| 产物 | 目标宝石本身的数据提升一档；新数值在「高一档」区间内均匀随机 |
| 已是最高档 | 弹原生提示并拒绝合成：不扣材料、不扣宝石 |

（材料格显示、「已是最高档拒绝」、费用 0 均为用户裁决。）

### 2.4 档位定义（引用属性显示范围）

按 `attribute-range-display.md` §2.1 的 6 档百分位：数值在其动态范围 `[min,max]` 内的百分位决定档位。

| 档 | 判定 | 颜色码 |
|---|---|---|
| 金 | `value == max`（满值） | `Y` |
| 紫 | ≥90% 且 <100% | `V` |
| 蓝 | ≥75% 且 <90% | `L` |
| 绿 | ≥60% 且 <75% | `C` |
| 白 | ≥30% 且 <60% | `W` |
| 灰 | ≥0% 且 <30% | `G` |

**档位区间**（`R = max − min`，用于在新档内随机取值）：

| 档 | 区间 |
|---|---|
| 灰 | `[min, min+0.30R)` |
| 白 | `[min+0.30R, min+0.60R)` |
| 绿 | `[min+0.60R, min+0.75R)` |
| 蓝 | `[min+0.75R, min+0.90R)` |
| 紫 | `[min+0.90R, max)` |
| 金 | `{max}` |

「提升一档」= 取当前值所在档的**上一档**区间，在该区间内均匀随机取整数。

**硬约束**（host 测试必须断言）：`classify(new_value, min, max)` 等于目标档；金档不可再升。

### 2.5 明确不做（本轮冻结）

- 不做逐配方独立开关（仅总开关）。
- 不处理镶嵌在装备上的宝石（对齐 `attribute-range-display.md` E7 裁决）。
- 不做数值 `(min~max)` 括号显示。
- 不引入新的本地化文本（复用原生文本项）。

### 2.6 材料需求数量规则（冻结）

用户需求原话：「配方是否可以根据填入的宝石类型来变化？低级宝石1瓶，中级2瓶，类推」「配方是否可以根据角色等级变化？数量*（1-等级/105），向上取整」。

**数量 = ceil(1 × 宝石档位 × (105 − 角色等级) / 105)**，全程整数运算。

- **宝石档位**由目标宝石的类别（`item+8` 的 bits6-15）得出：**28=低级宝石(1)、29=中级宝石(2)、30=高级宝石(3)、31=顶级宝石(4)、32=混沌宝石(5)**（静态数据实证见 §3.10）。名称与「低级/中级/类推」逐字对应。
- **角色等级**取主角等级（`PARTY_GetMember(0)` 的 `C_LEVEL`，int8），上限 105。
- 边界：105 级 → **0 瓶**（对应 `idea.md` §7 原文「当前角色等级为 105 级（满级）后，无需消耗卓越灵药」）；1 级 → 原量（1..5 瓶）；非零需求向上取整后**绝不会变成 0**。
- 读取失败或档位无法识别（不在 28..32）→ **保持原需求数**（fail-closed），不会退化成免费合成。

## 3. 现状事实（逆向与代码依据）

> 本节只记录当前状态。地址基于大修版 `apk/decoded/overhaul/lib/arm64-v8a/libgame.so`。

### 3.1 面板与类型映射

- 类型存于 `[g_uimix+0x38]`（`UIMIX_SLOT_TYPE`）：`0=药水合成 1=宝石合成(3:1) 2=打孔 3=混沌合成(混沌/深渊) 4=传说/Unique`。
- 依据：`UIMix_ButtonMenuListExe@0xc05c0` 跳转表（偏移字节表 @`0x24a6ec` = `32 1d 16 00 45` → `SetType 0/1/2/3/4`，配方组 2/3/(硬编码 mixType 16)/1/5），与文本表 `35290-35294` 连续五项「药水合成/宝石强化/宝石孔生成/混沌合成/传说装备」一致。
- 混沌合成面板的配方组 = group 1。

### 3.2 配方数据表

| 表 | 记录 | 读取路径（GOT 槽 → 指针全局 → 数据） |
|---|---|---|
| `RECIPEBASE` | 69 条 × 12B | 基址 `*(0x2f3158)`；记录大小 `*(0x2f55e8)`（u8）；记录数 `*(u16*)*(0x2f6960)` |
| `MIXTUREBASE` | 189 条 × 3B `{u16 itemId, u8 count}` | 基址 `*(0x2f6a98)`；记录大小 `*(0x2f4898)`（u8）；**无独立记录数校验**（只受 `RECIPEBASE` 的 b4-5/b6 约束） |

`RECIPEBASE` 记录字段：

| 偏移 | 语义 |
|---|---|
| b0-1 | **配方名称 wordId**：配方按钮文本，draw proc 读 `MEMORYTEXT_GetText(b0-1)`。实证：记录 0/1 → 1151「混沌」/1152「深渊混沌」；记录 16 → 1167「宝石孔生成」；记录 12-15 → 1163-1166「中级宝石…混沌宝石」（`zh-Hans.json` 索引） |
| b2-3 | 结果物品 id（`IsNeedEuip == 0` 时喂 `ITEMSYSTEM_CreatePerfectItem`）。混沌/打孔 = 64「装备」占位；宝石记录 12-15 = 29-32（类别） |
| b4-5 | `MIXTUREBASE` 材料起始下标 |
| b6 | 材料条目数 |
| b7 | 1（与原版一致，语义未定） |
| b8-9 | 费用公式 wordId（`MEMORYTEXT_GetText_E` → `CAL_Calculate`，参数 = 目标物 `ITEM_GetAbilityLevel`） |
| b10 | 解锁门槛：**`CalcRecipeListCount` 只按 b11 掩码计数、不检查 b10**，而 `MakeRecipeList` 用 `flag >= b10` 过滤 → 两者不一致会造成配方数组尾部为 `MEM_Malloc` 垃圾（幻影按钮 + 越界读）。运行时 flag = `u8[*(0x2f6a68)+0xe]` |
| b11 | 面板组位图（bit g = group g）\| bit0 = 需装备（`MIXSYSTEM_IsNeedEuip`）\| bit5 = 配方书条目 |

- group 1 现有记录：0（按钮文本 1151「混沌」）/ 1（1152「深渊混沌」），均 `b2-3=64`、`b11=0x03`、`b6=4`、`b10=1`。
- **`MIXTUREBASE` 189 条全部被 `RECIPEBASE` 引用（下标 0..188），无空闲条目；无 `itemId==15` 条目** → 注入配方必须同时扩展两张表。
- `ITEMDATABASE` 记录下标 15 = itemId 15「卓越灵药」（`ITEMDATABASE` +2 u8 = 0x17，可消耗类）。

### 3.3 配方列表与配方按钮构建

- `MIXSYSTEM_CreateRecipeList(group, flag)@0x11b7e0`：释放旧数组（`*(0x2f4fd8)` 指向的指针全局）→ 计数 = `MIXSYSTEM_CalcRecipeListCount@0x11b470`（`mask=1<<group`，遍历记录测 `b11`）→ 计数写入 `*(0x2f4160)` → `MEM_Malloc(count*8)` → `MIXSYSTEM_MakeRecipeList@0x11b5ac`。
- `UIMix_CreateRecipeGroupControl@0xbfb24`：按计数创建等量配方按钮（ExecuteProc 取 GOT `0x2f40d0`；draw proc 取 GOT `0x2f52a8`）。
- ⇒ **配方按钮数量完全由 `RECIPEBASE` 的 `b11` 位图决定，无硬编码**。
- 选中配方：`UIMix_ButtonRecipeExe@0xc0390` → `mixType = recipeList[idx]` 写 `[+0x48]` → `UIMix_InitMixingState@0xc0038`。该函数**无数组长度边界检查**（槽号天然小于按钮数）、**无 mixType 合法性校验**（不比较 {0..4}）→ 注入的任意 mixType 均可被选中。
- 配方按钮 draw proc = `UIMix_ButtonRecipeDraw@0xbef40`：槽号取 `UI_GetChildIndex`，`idx = (*(0x2f4fd8))[slot]`，`p = *(0x2f3158) + idx * recsize`，`MEMORYTEXT_GetText(*(u16*)p)` 居中绘制；**不读 `[+0x100+type*8]`、不绘制物品图标**。
- `UIMix_CreateRecipeGroupControl@0xbfb24` 对配方数 ≤6 与 >6 两种分支生成的几何完全相同（仅 ≤6 多一个 `count>0` 守卫），**无 6 条上限**；超出可视区由滚动控件处理。按钮数量 = `*(0x2f4160)`。

### 3.4 选中配方后的界面构建

- `UIMix_InitMixingState@0xc0038`：type ∉ {1,4} 走通用路径，材料格数 = `RECIPEBASE[mixType]`.b6；`MEM_Malloc(16*N)` 建 stuffList 写 `[+0xe8]`；`MIXSYSTEM_MakeStuffSlot@0x11b20c` 依 `MIXTUREBASE` 填材料（仅显示）；费用 `[+0xf8]` 先清 0。type==3 额外调 `MIXSYSTEM_GetResultItemCount@0x11aa5c`。
- 混沌面板的材料**不需玩家逐格放入**，由 `MIXSYSTEM_UseStuff@0x11b300` 按 stuffList 批量扣。
- 目标槽 `[+0x40]` 为单个 `ControlItem`。

### 3.5 放料（type 3）

`UIMix_ButtonInvenItemSelectExe@0xc2328` type==3 分支 `0xc24ec`：校验全部委托 `MIXSYSTEM_CheckMixture(mixType, item)@0x11ac34`。

**`CheckMixture` 的实际语义**（第一参 = 配方记录号）：`IsNeedEuip == 0` → 直接返回 0（跳过一切校验）；`IsNeedEuip != 0` 且 item 为空 → 2；否则**仅对 idx ∈ {0, 1, 0x10} 做装备类别/混沌值/上限校验**（返回 3..8）；**其它任何 idx（含注入的 69+）一律返回 0**。
⇒ 注入配方的放料**天然通过** `CheckMixture`，不需要绕过；模块的限制逻辑独立于它。

- 返回码映射（type 3）：3 → 弹 `0x66`；6 → `0x68`；7 → `0x67`；8 → `0x69`；0/2 → 通过。
- 通过分支 `0xc2418` → `0xc2424-0xc2430`：`ControlItem_SetItem([+0x40], item)`（`0xaad60`）→ 若 `type == 3`（`0xc2440-0xc24a8`）：`ITEM_GetAbilityLevel` + `MEMORYTEXT_GetText_E(RECIPEBASE[mixType]+8)` → `CAL_Calculate` → 写费用 `[+0xf8]`。
- **该函数内不刷新任何费用显示控件**（显示由后续绘制读取 `[+0xf8]`）→ 在 hook 里于原函数返回后把 `[+0xf8]` 置 0，即可让显示与实扣同时为 0。

### 3.6 合成（type 3）

`UIMix_ButtonMixingExe@0xc21ec` type==3 分支 `0xc22dc`：读 `[+0x40]` 目标 → `UTIL_GetBitValue(item+0x1a, 9, 2)` 与 `0xb` 比较（混沌阶上限）→ 费用 `[+0xf8]` > 0 时校验金币 → `UIPopupMsg_CreateYesNoFromTextData(0x12)@0xca7d4` → `UIMix_StartMix@0xc0870`。

`UIMix_StartMix` type 2/3 共用段 `0xc08c8`：`ControlItem_GetItem([+0x40])` → `MIXSYSTEM_MakeItem([+0x48], &item)@0x11af58`；返回 0 走成功路径 `0xc0a8c`：`MIXSYSTEM_UseStuff(mixType, [+0xe8])` → `INVEN_MinusMoney([+0xf8])@0x104780` → `UIMix_InitMixingState` → `ControlItem_SetItem([+0x40], 0)` → `UIMix_RefreshInvenItem@0xc04fc` → 音效 → `UIPopupMsg_CreateOKFromTextData(0x6a)@0xca778`。**无 `INVEN_SaveItem`**：物品被原地改写，槽位只清显示。

- `MIXSYSTEM_MakeItem@0x11af58` 分派：`IsNeedEuip = RECIPEBASE[mixType].b11 & 1`。为真 → `mixType==0` 调 `ITEMSYSTEM_MakeChaos(item,0)`、`==1` 调 `MakeChaos(item,1)`、`==0x10` 调打孔、其它调 `Flurry_EventItemMix` 后返回 0。为假 → `ITEMSYSTEM_CreatePerfectItem(RECIPEBASE[mixType].b2-3)`。**返回 0 = 成功，1 = 失败。**
- `MIXSYSTEM_UseStuff@0x11b300`：读 `RECIPEBASE[mixType]`.b6（N）；为 0 直接返回；否则遍历 stuffList（步长 16B；条目 +0 = u32 itemId，+6 = u16 数量）调 `INVEN_RemoveItemData(itemId, count)@0x1040a8` → 扩展背包补扣（R-56）自动生效。
- 费用为 0 时，`UIMix_ButtonMixingExe` 跳过金币校验、`INVEN_MinusMoney(0)` 无副作用。

### 3.7 文本与原生提示

- `TEXTDATABASE`（190 条 × 2B）：**记录下标 = 游戏 wordId**，记录 +0 的 u16 = 本地化串下标（即 `zh-Hans.json` 索引）。例：下标 105 → u16 35421 → `zh-Hans.json[35421]`。
- 可复用既有文本项：**97**「只有宝石道具才可以。」、**105**「已经达到合成最大值，无法再次进行合成。」
- 原生提示函数：`UIPopupMsg_CreateOKFromTextData(wordId)@0xca778`、`UIPopupMsg_CreateYesNoFromTextData(wordId)@0xca7d4`。

### 3.8 内存可写性与 GOT 槽解析（`readelf -rW` 实测）

- 段表：`.got` = `0x2f2980`–`0x2f7000`，含在 `GNU_RELRO`（`0x2eea00`–`0x2f7000`）内 → 启动后只读；`.bss` = `0x2faf90`–`0x734e88`（可写，不在 RELRO）。
- 表数据经「GOT 槽 → 指针全局 → 数据」二级访问。`readelf -rW` 解得各槽 addend：

| GOT 槽 | addend | 落点 | 语义 |
|---|---|---|---|
| `0x2f3158` | `0x3019b0` | `.bss` | `RECIPEBASE` 基址指针全局 |
| `0x2f55e8` | `0x3019b8` | `.bss` | `RECIPEBASE` 记录大小（u8）全局 |
| `0x2f6960` | `0x3019ba` | `.bss` | `RECIPEBASE` 记录数（u16）全局 |
| `0x2f6a98` | `0x3019a0` | `.bss` | `MIXTUREBASE` 基址指针全局（`MIXSYSTEM_GetStuffItem@0x11b0ec` `ldr x1,[x1,#0xa98]` → `0x11b0f4` `ldr x1,[x1]` 解引用） |
| `0x2f4898` | `0x3019a8` | `.bss` | `MIXTUREBASE` 记录大小（u8）全局（`0x11b0f0` `ldr x0,[x0,#0x898]` → `0x11b0f8` `ldrb w0,[x0]`） |
| `0x2f4fd8` | `0x307768` | `.bss` | 配方数组指针全局 |
| `0x2f4160` | `0x728c10` | `.bss` | 配方数量（int）全局 |
| `0x2f40d0` | `0xc0390` | 函数 | 配方按钮 ExecuteProc = `UIMix_ButtonRecipeExe` |
| `0x2f4598` | `0xc2328` | 函数 | desc 按钮 ExecuteProc = `UIMix_ButtonInvenItemSelectExe` |
| `0x2f6658` | `0xc05c0` | 函数 | 菜单按钮 ExecuteProc = `UIMix_ButtonMenuListExe` |
| `0x2f6d60` | `0xc21ec` | 函数 | 合成按钮 ExecuteProc = `UIMix_ButtonMixingExe` |
| `0x2f52a8` | `0xbef40` | 函数 | 配方按钮 draw proc |

- ⇒ **改写表基址 / 记录数只需写 `.bss` 指针全局，无需 mprotect**；本功能不改动任何 GOT 槽。

### 3.9 可复用既有能力

- `feature/ui/game_ui_gemcraft.cpp`：`install_got_hook`（:281-309）、`try_install_execute_wrapper`（:233-247）、`uimix_slot`（:32）、`read_filled_slots`（:48）、`selected_inven_item`（:119）。
- `feature/attribute_range/`：`attr_range_probe_jewel_range(type, item, out_min, out_max)`（`game_ui_attr_range.h:16-20`）、`attr_range::classify` / `percentile`（`attribute_range.h:17/24`）。
- `native_hook_func()`：函数级 hook（`feature/attribute_range/game_ui_attr_range.cpp` 已用）。
- `feature/patch/game_ptr_hook.h`（PtrHook）、`core/native/call_patch.h`（BL patch）。

### 3.10 材料需求数量的落点与依据（大修版 VMA）

**stuffList 条目布局**（`MIXSYSTEM_MakeStuffSlot@0x11b20c` 写 / `MIXSYSTEM_UseStuff@0x11b300` 读 / `UIMix_Draw` 画，三处一致）：步长 16B，`+0` itemId(u32)、`+4` 持有数(u16)、`+6` **需求数(u16)**。指针在 `[g_uimix+0xe8]`，条目数在 `[g_uimix+0xf0]`（= `RECIPEBASE[mixType]` 字节 6）。常量见 `game_symbols.h` 的 `UIMIX_STUFF_ENTRY_*`。

**需求数是扣料与显示的唯一真源**：

- **扣料**：`UseStuff@0x11b300` 循环体 `0x11b36c ldrh w1,[x19]`（取 `+6`）→ `0x11b370 cbz w1,#0x11b360` —— **需求数为 0 的条目直接跳过，不会调用 `INVEN_RemoveItemData`**。这使「满级 0 消耗」天然安全。
- **显示**：`UIMix_Draw` 每帧直读 `0xc1120 ldr x6,[x19,#0xe8]` / `0xc1154 ldrh w2,[x19,#4]`（持有）/ `0xc1158 ldrh w3,[x19,#6]`（需求）→ `0xc115c bl 0x1b6cec` 按 `0x24a6f8` 的 `%d/%d` 格式化。**显示格式是「持有/需求」，不是「×N」**；改 `+6` 下一帧即生效，**无需任何刷新调用**。
- `MakeStuffSlot` 是**一次性快照**（`0x11b2cc strh w0,[x20,#6]` 把 MIXTUREBASE 的数量抄进 stuffList），因此改源表不会自动生效，改 stuffList 才会。

**必须避开的两个坑**：

1. `INVEN_RemoveItemData@0x1040a8` 中 **`count == -1` 才是「删除全部」**（`0x104130 cmn w23,#1` → `b.eq #0x1041dc`）。需求数一经 cast 成 u16 写入，**绝不能出现 0xFFFF**。
2. **不能把 MIXTUREBASE 的 count 改成 0**：`MakeStuffSlot@0x11b2a4 cmp w0,wzr / b.le` 遇 `count<=0` 会**跳过写条目**，该条保持 `0x11a9d8` 初始化的 `itemId = -1`，而 `UIMix_Draw` 的图标绘制路径**没有负索引守卫**（由反汇编推断，真机表现未确认）→ 不采用该方案。

**宝石档位（类别）静态数据实证**（`ITEMDATABASE.json` 首 u16 = 名称 text_id；`zh-Hans.json` 同下标）：

| 类别/ itemId | 名称 text_id | 名称 |
|---|---|---|
| 28 | 58 | 低级宝石 |
| 29 | 59 | 中级宝石 |
| 30 | 60 | 高级宝石 |
| 31 | 61 | 顶级宝石 |
| 32 | 62 | 混沌宝石 |

`ITEMSYSTEM_IsJewel@0x10b964` 全函数仅 `sub w0,w0,#0x1c; cmp w0,#4; cset w0,ls` —— **只比较传入的类别**，完全不触碰 `item+0x10`，故档位与随机等级 bits11-17、属性类型 bits18-23 无关。

**角色等级读取入口**：`fn_get_member`（`game_access.h:47`，`F_GET_MEMBER_VMA=0x11f384` = `PARTY_GetMember`）→ `reinterpret_cast<int8_t*>(member)[C_LEVEL]`（`game_symbols.h:18`，`C_LEVEL=0x0E`）。既有同法先例 `core/native/game_system.cpp:61`、`api/native/game_character.cpp:226`。

## 4. 方案设计

### 4.1 分层落位

```
feature/custom_recipe/custom_recipe_catalog.{h,cpp}  配方目录（唯一真源）：配方声明 + mixType/材料下标映射（纯逻辑）
feature/custom_recipe/custom_recipe_rules.{h,cpp}    行为实现：档位 → 上一档区间 → 随机取值（可 host 单测）
feature/custom_recipe/custom_recipe_table.{h,cpp}    表注入：RECIPEBASE / MIXTUREBASE 扩展与指针改写（幂等）
feature/custom_recipe/game_ui_custom_recipe.{h,cpp}  UI 注入：菜单页签（确保注入）/ 放料 / 合成按钮 / 产物四处 hook 的安装与门控
```

符号一律登记进 `game_symbols.h` + `symbol_registry.h`；依赖方向 `feature → core/data`，不得反向。

### 4.2 配方目录（框架核心）

```cpp
namespace custom_recipe {

enum class Kind : uint8_t {
    kJewelTierUp,   // 目标宝石数值提升一档
    // 后续配方种类在此追加
};

struct Material { uint16_t item_id; uint8_t count; };

// 材料需求数量的计算规则（配方固有属性）。
enum class CountRule : uint8_t {
    kFixed,               // 直接用 Material::count
    kJewelGradeAndLevel,  // ceil(Material::count × 宝石档位 × (105 − 角色等级) / 105)，见 §2.6 / §4.10
};

struct Def {
    uint16_t label_word_id;        // 配方按钮文本 wordId（zh-Hans.json 索引空间；见 §3.2 b0-1）
    Kind     kind;
    const Material* materials;
    uint8_t  material_count;
    uint16_t cost_word_id;         // 0 = 免费
    CountRule count_rule;          // 材料需求数量规则
};

const Def* catalog(size_t* out_count);          // 静态目录
bool       catalog_ready();                     // 注入完成后可用
const Def* def_for_mix_type(uint32_t mix_type); // mixType → 配方（非自定义配方返回 nullptr）

}  // namespace custom_recipe
```

**下标映射**（全部由运行时原版记录数推导，不写死常量）：

| 量 | 推导 |
|---|---|
| 配方 `i` 的 mixType | `base_record_count + i` |
| 配方 `i` 的材料起始下标 | `base_material_count + Σ(material_count[0..i-1])` |
| `base_record_count` | 注入前读 `*(u16*)*(0x2f6960)`（原版 69） |
| `base_material_count` | 注入前遍历原版 `RECIPEBASE` 求 `max(b4-5 + b6)`（原版 189）——**不依赖静态常量，跨版本成立** |

配方 1 的目录项：`{label_word_id = 35291 /*「宝石强化」*/, kind = kJewelTierUp, materials = {{15, 1}}, material_count = 1, cost_word_id = 0, count_rule = kJewelGradeAndLevel}`。

### 4.3 表注入（配方与材料）

一次性执行（`bridge_ready` 后、首次启用时），幂等：

1. 读 `RECIPEBASE` 基址、记录大小、原版记录数 `base_record_count`；按 §4.2 求 `base_material_count`；以上三者与 `MIXTUREBASE` 基址**只解析一次并缓存**。
2. 分配模块自有缓冲并复制：
   - `RECIPEBASE`：`(base_record_count + N) * recsize`，前段复制原表，后段写入 N 条注入记录；
   - `MIXTUREBASE`：`(base_material_count + Σmaterial_count) * recsize`，前段复制原表，后段写入各配方的材料条目。
3. 写回指针全局（可写段）：`*RECIPEBASE_基址全局`、`*MIXTUREBASE_基址全局`；写 `RECIPEBASE` 记录数 = `base_record_count + N`。

注入记录取值（由目录 `Def` 派生）：

| 字段 | 值 |
|---|---|
| b0-1 | `Def.label_word_id`（配方按钮文本，默认 35291「宝石强化」，见 §7.2） |
| b2-3 | 0（结果物品 id；`MakeItem` 被 hook 拦截后仅可能影响详情面板，真机确认） |
| b4-5 | §4.2 推导的材料起始下标 |
| b6 | `Def.material_count` |
| b7 | 1（与原版一致） |
| b8-9 | `Def.cost_word_id`；**实际费用由 hook 强制 `[+0xf8] = 0`**（`formula-e.json` 无求值为 0 的公式，见 §7.9） |
| b10 | **0**（必须；见 §3.2 与 §7.8） |
| b11 | `0x02`（bit1 = group 1；bit0 清 0 → `CheckMixture` 直接返回 0、`MakeItem` 走通用路径由 hook 拦截；bit5 必须为 0） |

**幂等与重入（`custom_recipe_table_ensure()`）**：以「游戏侧指针是否仍指向模块缓冲」自校验，而非缓存计数——① 已注入且 `*(RECIPEBASE 基址全局) == 模块缓冲` → 直接返回，不重复追加；② 指针已被改写（**游戏重新装载静态表**，如进入存档时）→ 用新原表重新注入，旧缓冲**刻意不释放**（游戏可能仍持有指向其内部的局部指针，泄漏一份远优于悬空访问）；③ 表尚未装载或 5 个全局非法 → 返回 false（fail-closed）并允许后续重试。
调用点：开关启用时（`set_custom_recipe_enabled(true)`）、每次放料/合成 hook 命中时、以及**点任何类型/菜单按钮时（`UIMix_ButtonMenuListExe`，早于 `CreateRecipeList` 建按钮）**。健康路径仅一次指针比较。实测：启动期配置下发时表未装载 → 记 `QOL_LOG_ERROR` 后跳过，进程不崩溃；boot 后再次启用 → 注入成功；再次启用 → 无第二次注入（自校验生效，未重复追加）。

**fail-closed 窄域校验**：`recipe_size ∈ [1,64]`、`mixture_size ∈ [1,16]`、`count ∈ [1,4096]`、两个基址非空。这 5 个全局只能走 VMA 兜底，跨版本可能错位；宁可跳过注入也不要带着垃圾指针 `memcpy`（曾因 `MIXTUREBASE` 基址与大小两个全局互换实测崩溃，见 §7.12）。

### 4.4 放料改写

`native_hook_func` 在 `UIMix_ButtonInvenItemSelectExe@0xc2328` 装函数级 hook，采用**先转调原函数、再校验并纠正**的形式。原函数在该分支只做「`CheckMixture` → `ControlItem_SetItem([+0x40], item)` → 写 `[+0xf8]`」三件事，纠正成本极低，且不必复制任何控件/显示逻辑：

1. `type != 3` / `def_for_mix_type([+0x48]) == nullptr` / 功能未启用 → 仅转调原函数。
2. 否则：
   - `rc = call_orig(...)`（原版全部行为照常发生；`CheckMixture` 对注入 mixType 必然返回 0）。
   - `item = ControlItem_GetItem([+0x40])`（`0xaada8`）；`item == nullptr` → 返回 `rc`（本次未放入任何物）。
   - `!ITEMSYSTEM_IsJewel(*(u16*)(item + 8))` → `ControlItem_SetItem([+0x40], nullptr)` 撤销 + `UIPopupMsg_CreateOKFromTextData(97)`（「只有宝石道具才可以。」）→ 返回 `rc`。
   - `*(u64*)(g_uimix + 0xf8) = 0`（费用 0；必须在原函数返回后写，以覆盖其 `CAL_Calculate` 结果）。
   - 返回 `rc`。
3. 原函数返回值恒为 1，hook 原样透传。

### 4.5 合成按钮改写

`native_hook_func` 在 `UIMix_ButtonMixingExe@0xc21ec` 装函数级 hook（前置校验，避免无效确认框）：

- `type == 3` 且属于自定义配方且功能启用：
  - 目标槽非宝石 → 弹 97 并返回。
  - 目标宝石已是金档（按配方 `kind` 的前置条件）→ 弹 105 并返回。
  - 通过 → 转调原函数（原版 type 3 分支的费用/金币/确认框流程；费用 0 时自动跳过金币校验）。
- 其余情形 → 转调原函数。

### 4.6 产物改写

`native_hook_func` 在 `MIXSYSTEM_MakeItem@0x11af58` 装函数级 hook：

- 属于自定义配方且功能启用 → 按 `Def.kind` 分派执行：
  - `kJewelTierUp`：读宝石 `+0x10` bits0-10（数值）与 bits18-23（属性类型）→ `attr_range_probe_jewel_range` 求 `[min,max]` → `classify` 求当前档（金档则返回 1 失败，兜底）→ 在上一档区间内 `MATH_GetRandom(lo, hi)@0xa8bcc`（闭区间）取新值 → 写回 bits0-10，**保留 bits11-17 与 bits18-23**；返回 0（成功）。
  - 写回必须 read-modify-write：`bits = (bits & ~0x7FFu) | (new_value & 0x7FFu)`；禁止整体赋值 `item+0x10`（bits11-17 = 随机等级、bits18-23 = 属性类型必须原样保留）。
  - 金档兜底返回 1（失败）时原版 `UIMix_StartMix` 走失败收尾：不扣料、不扣钱、不清槽 —— 与 §4.5 的前置拦截互为双保险。
- 其余 → 转调原函数。

返回 0 后，原版 `UIMix_StartMix` 照常扣料 / 扣钱 / 清槽 / 刷新 / 提示。

### 4.7 与 gemcraft 的共存

- gemcraft 以 GOT 槽覆盖 4 个按钮的 ExecuteProc（desc `0x2f4598`、menu `0x2f6658`、recipe `0x2f40d0`、craft `0x2f6d60`），门控 `type == 1`，并在其 wrapper 内**调用原函数地址**。
- 本功能**不改动这 4 个 GOT 槽**，改为在**原函数入口**装函数级 hook，按 `type` / `mixType` 门控；gemcraft 转调原函数时会进入本 hook，`type == 1` 分支原样放行。
- 结论：两者无安装顺序依赖、无槽位争用，不需要修改 gemcraft 现有代码。

### 4.8 依赖与约束

- 禁止 `as any` / 空异常处理；类型与错误语义必须清晰。
- 不在渲染热路径做重 IO 或加锁阻塞。
- 表替换在启用时执行；**停用会把 `RECIPEBASE` 记录数收回原值**（`custom_recipe_table_deactivate()`），使任何配方查询都不再命中注入记录。
  - **保留模块缓冲作为表体**，不把指针改回原表：模块缓冲是原表的超集，故「已打开面板残留的配方数组仍持有注入下标」这一情形下，`UIMix_ButtonRecipeDraw`（**无边界检查**）读到的是缓冲界内数据而非原表之外，避免越界。
  - 停用后配方按钮的**消失需要重开面板**（`CreateRecipeGroupControl` 已建的子控件不会因记录数变化而销毁）；在已打开的面板上，放料被撤销、合成被拦截（下述 fail-closed），不会白扣材料。
- **停用期的 fail-closed 拦截**（`is_custom_recipe_now()` 只按目录识别 mixType，与开关无关）：放料 hook 转调后立即 `ControlItem_SetItem(null)`；合成按钮 hook 直接返回并记 `craft blocked reason=disabled`；产物 hook 直接返回 1 而**不回落原版**（注入记录号对原表是越界下标，原版通用路径会用它去读 `RECIPEBASE` 之外的字节）。
- 不写存档；宝石数值写回作用于既存库存物品（与原版混沌同语义）。

### 4.9 实现落位（当前状态）

| 文件 | 职责 |
|---|---|
| `feature/custom_recipe/custom_recipe_catalog.{h,cpp}` | 目录唯一真源；`mix_type_at` / `material_start_at` / `material_total` 纯映射 |
| `feature/custom_recipe/custom_recipe_rules.{h,cpp}` | 档位纯逻辑：`tier_ordinal` / `tier_from_ordinal` / `tier_lower_bound`（二分，与 `attr_range::classify` 严格互逆）/ `next_tier_interval` / `compute_tier_up_value` |
| `feature/custom_recipe/custom_recipe_table.{h,cpp}` | 记录字节构造 `build_record_bytes`、`derive_material_count`、纯注入 `inject_into_buffers`（host 可测）；`#ifdef __ANDROID__` 段为 `custom_recipe_table_ensure()` 实际写回 |
| `feature/custom_recipe/game_ui_custom_recipe.{h,cpp}` | **四处**函数级 hook + 总开关 + 安装入口（`install_one` / `g_attempted` CAS / `g_verify_log_budget{16}`） |
| `tests/test_custom_recipe.cpp` | host 单测（4 组，见 §5.1） |

| `custom_recipe_menu` | `UIMix_ButtonMenuListExe @ 0xc05c0` | 点页签时先确保注入完成（早于 `CreateRecipeList` 建配方按钮），再转调原函数 |
| `custom_place` | `UIMix_ButtonInvenItemSelectExe @ 0xc2328` | §4.4 放料改写 |
| `custom_mixing` | `UIMix_ButtonMixingExe @ 0xc21ec` | §4.5 合成按钮前置校验 |
| `custom_make_item` | `MIXSYSTEM_MakeItem @ 0x11af58` | §4.6 产物改写 |

四处均装在**原函数入口**（`native_hook_func`），与 gemcraft 的 GOT 槽覆盖分层共存（§4.7）。

对外契约（C++ 与 Kotlin 两侧同时实现）：配置 key `customRecipeEnabled`（默认 `false`）；JNI `nativeSetCustomRecipeEnabled(Boolean): Boolean`；native `set_custom_recipe_enabled(bool)` / `custom_recipe_enabled()` / `custom_recipe_ui_install_if_ready()`；日志 domain `custom_recipe`。设置项 `{"customRecipeEnabled", "自定义配方", 2}`（`feature/ui/game_ui_settings.cpp`），安装入口已接入 `nativeInit` 序列（`bridge/native/gamebridge.cpp`）。

新增游戏符号：`F_CONTROL_ITEM_GET_ITEM_VMA = 0xaada8`（已登记 `symbol_registry.h`，可名称解析）；`G_RECIPEBASE_DATA_VMA = 0x3019b0`、`G_RECIPEBASE_SIZE_VMA = 0x3019b8`、`G_RECIPEBASE_COUNT_VMA = 0x3019ba`、`G_MIXTUREBASE_DATA_VMA = 0x3019a0`、`G_MIXTUREBASE_SIZE_VMA = 0x3019a8`（无名 `.bss`，VMA 兜底，见 §7.12）；位域/槽常量 `UIMIX_SLOT_TARGET_ITEM = 0x40`、`UIMIX_SLOT_STUFF_NUM = 0xf0`、`UIMIX_STUFF_ENTRY_SIZE = 16`、`UIMIX_STUFF_ENTRY_ITEM_ID = 0`、`UIMIX_STUFF_ENTRY_HELD = 4`、`UIMIX_STUFF_ENTRY_NEED = 6`、`I_JEWEL_VALUE_WORD = 0x10`、`JEWEL_VALUE_MASK = 0x7FF`、`JEWEL_TYPE_SHIFT = 18`、`JEWEL_TYPE_MASK = 0x3F`、`I_TYPE_CATEGORY_SHIFT = 6`、`I_TYPE_CATEGORY_MASK = 0x3FF`（类别取法与既有 `data/native/item_class.h:39`、`feature/attribute_range/game_ui_attr_range.cpp:93` 一致）。角色等级复用既有 `fn_get_member`（`game_access.h:47`）+ `C_LEVEL`（`game_symbols.h:18`），未新增符号。

与 §4 的偏差（均不改变语义）：

1. **未登记 `MIXSYSTEM_CheckMixture` / `MIXSYSTEM_GetResultItemCount` / `UIPopupMsg_CreateYesNoFromTextData`**：§4.4 采用「先转调原函数再纠正」，放料校验与确认框全部由原函数承担，故无需这三个符号。
2. 实例指针复用既有 `g_uimix`（`G_UIMIX_VMA`），未新增全局。
3. `custom_recipe_table.cpp` 以 `#ifdef __ANDROID__` 分隔纯构造器与实际注入，使 host 单测不引入 `game_access` 链接依赖。

### 4.10 材料需求数量（§2.6 的落地）

**唯一写入点**：stuffList 条目 `+6`（u16 需求数）。依据 §3.10，该字段同时是扣料与显示的真源，故只需写它 —— **不必也不应改 MIXTUREBASE**（改源表既不会自动生效，把 count 置 0 还会触发 `MakeStuffSlot` 跳过条目留下 `itemId=-1`）。

**纯逻辑**（`custom_recipe_rules.{h,cpp}`，可 host 单测）：

```cpp
int jewel_grade_from_category(int category);            // 28→1 … 32→5；越界返回 0
int material_count_for(int base_count, int grade, int level);  // ceil(base×grade×(105−level)/105)
```

`material_count_for` 把 `level` 钳到 `[0,105]`、`grade<1` 或 `base<1` 返回 0；分子用 `long long` 防溢出；全程整数，避免浮点端点漂移。

**调用点**（`game_ui_custom_recipe.cpp` 的 `apply_material_count(def, jewel)`，三处同源同式）：

1. **放料 hook**：宝石通过校验后立刻写入 → 用户放料后下一帧就看到正确需求数。
2. **合成按钮 hook**：确认框弹出前再写一次（覆盖「放料后切换配方导致 `InitMixingState` 重置为源表值」的情形）。
3. **产物 hook**：`MakeItem` 返回 0 之后 `StartMix` 紧接着调 `UseStuff` 读 stuffList，此处写入的即**最终实际扣除量**（与显示同源，不会不一致）。

`apply_material_count` 在**档位无法识别（不在 28..32）时直接返回、保留原需求数**，不会退化成 0 消耗；等级读取失败（`fn_get_member` 空或成员空）返回 0 级，等价于「不减免」。

## 5. 验收计划

### 5.1 host 单测（`tests/test_custom_recipe.cpp`）

1. 档位区间边界：`R` 退化（`max == min`）→ 恒为金档且不可升；灰/白/绿/蓝/紫各档的上一档区间端点归属。
2. 随机取值后 `classify(new_value) == 目标档`（多轮随机）。
3. **目录映射**：对 N = 1 与合成的 N = 3 目录，断言 mixType = `base + i`、材料起始下标 = `base_material + Σ前缀`、材料条目数预算正确。
4. **表注入字节断言**（不依赖游戏内存）：给定伪造原表，断言复制后前段一致、后段记录字段逐个正确、记录数 = `base + N`、幂等重复调用不改变结果。
5. **材料需求数量**（§2.6）：类别 → 档位映射与越界返回 0；105 级为 0、超上限不为负；1 级为原量 1..5；向上取整边界（`ceil(5×53/105)=3`、`ceil(1×1/105)` 类非零值不得被取整成 0）；`base_count` 倍率；负等级钳制；非法输入返回 0；对等级**单调不增**且终值为 0。

**已执行（2026-09-15）**：`ctest --test-dir module/app/src/main/cpp/tests/build` 全量 14/14 通过（含 `custom_recipe_tests`）；`git diff --check` 通过；`scripts/verification/check_log_policy.py` 结果 PASS（`R3 = 0`、error = 0、本功能文件无 warn）；`scripts/build-debug.sh` BUILD SUCCESSFUL。

### 5.2 真机验收（VM 卡，草案；实现阶段补齐正式卡号与日志）

| 卡 | 场景 | 通过判据 |
|---|---|---|
| VM-C1 | 混沌合成面板配方数 | 出现 `2 + N` 个配方按钮（本轮 N=1 → 3 个）；其余 4 个面板配方数不变 |
| VM-C2 | 选中配方 1 | 材料格显示「卓越灵药 ×1」 |
| VM-C3 | 放料 | 放宝石被接受；放武器/防具弹出「只有宝石道具才可以。」并拒绝 |
| VM-C4 | 合成 | 宝石数值落入「高一档」区间（记录前后值 + classify）；扣 1 卓越灵药；**金币不变**（费用 0）；目标槽清空，宝石留在背包且数值已变 |
| VM-C5 | 已是金档 | 弹出「已经达到合成最大值」；不扣材料 / 金币不变 / 数值不变 |
| VM-C6 | 扩展背包扣料 | 卓越灵药仅存在于扩展背包时合成成功且扩展袋被扣（R-56） |
| VM-C7 | 开关关闭 | **重开面板后**混沌页签回到 2 个配方；已打开的面板上放料被撤销、合成无响应（fail-closed），不扣材料、原版行为不变 |
| VM-C8 | 回归 | 原版混沌 / 深渊混沌两条配方仍正常；其余 4 个面板配方面板正常 |
| VM-C9 | 材料数量随宝石档位 | 依次放低级/中级/高级/顶级/混沌宝石，材料格「持有/需求」的**需求数**依次为 1/2/3/4/5（角色等级非满级时应为按 §2.6 取整后的值） |
| VM-C10 | 材料数量随角色等级 | 同档位宝石在不同等级下需求数按 `ceil(档位×(105−等级)/105)` 变化；**105 级为 0**，且合成**不扣任何卓越灵药**（背包数量不变） |
| VM-C11 | 档位无法识别 | 若目标物类别不在 28..32（非宝石已在 VM-C3 覆盖），需求数保持原值、不会变成 0 消耗 |

**已取得的真机证据（2026-09-15，大修版，`Inotia4` 进程内，非 VM 卡）**：

- `custom_recipe game_ui_custom_recipe.cpp:264 custom recipe hooks installed` —— 四处 hook 全部安装成功，与 gemcraft 的四个 GOT 槽 hook 并存无冲突。
- 启动期配置下发（静态表未装载）：`table globals invalid recipe_size=0 mixture_size=0 count=0` + `table inject deferred reason=table_not_loaded`，**进程未崩溃**（fail-closed 生效）。
- boot 后经 `POST /api/config/set` 启用：`recipe table injected base=69 records=70 materials=190` —— 与 §4.2 的运行时推导预测一致（`base_record_count = 69`、`base_material_count = 189`），5 个 `.bss` 全局地址正确、注入与写回成功。
- 再次启用：**无第二次注入日志**（指针自校验生效，未重复追加记录）。
- 开关时序：`recipe table injected base=69 records=70 materials=190` → `recipe table deactivated records=69` → `recipe table reactivated records=70`，全程 `pid` 不变。
- 全程 `pid` 不变（未崩溃）。

**状态**：**VM-C1..VM-C8 已由用户真机确认通过**（2026-09-15，含第 3 个配方出现、放料校验、升档、金档拒绝、关闭后重开面板回到 2 个配方）。**VM-C9..VM-C11（材料需求数量）尚未执行**——需真机交互。

无法经 API 自动化的原因：`POST /api/ui/open_panel` 白名单不含 `craft`，且混沌合成页签切换需触摸输入。
辅助取证端点：`GET /api/debug/item/raw?bag=&slot=` 可读宝石数值位域，用于 VM-C4 合成前后对比。

交付定义遵循根 `AGENTS.md`：触及行为面必须有真机证据，缺少时只能报 `NOT_ACCEPTED`。

## 6. 决策与理由（思考，非事实）

> 本章为决策记录，不是当前实现状态。

- **为什么注入混沌合成面板**：该面板目标槽是「单物品 + 合成时原地改写」，与「提升宝石自身数值」语义一致；材料自动扣除，改造面最小。（用户 2026-09-15 裁决）
- **为什么用目录驱动而不是按配方写代码**：需求要求「可复用、支持多条」（用户 2026-09-15）。目录作为唯一真源，使表注入、下标推导、行为分派三者共用一份声明；新增配方只改目录，不触碰 hook 与注入逻辑。
- **为什么表注入而非新建面板**：配方按钮数量、材料格、费用全部由数据表驱动，注入后无需复制任何 UI 构建代码，且天然跨版本。
- **为什么材料起始下标从原表推导而非写死 189**：`MIXTUREBASE` 无运行时记录数来源，取「原表被引用的最大下标」既安全又跨版本；写死常量会在版本差异下越界。
- **为什么不用 GOT 槽覆盖按钮**：槽位已被 gemcraft 占用；函数级 hook 可按 type 门控并存，避免争用与安装顺序耦合。
- **为什么只改 bits0-10**：档位定义基于该数值；宝石等级与属性类型与本需求无关，保持最小改动。
- **为什么费用为 0**：与 `idea.md` §7 原文一致（需求只提卓越灵药）。（用户 2026-09-15 裁决）
- **为什么复用原生文本 97 / 105**：避免新增本地化串与多语言维护。

## 7. 风险与未决项

1. 表替换是**全局**的（影响所有面板的配方查询），需以 VM-C8 与 4 面板回归覆盖。
2. ~~配方按钮的显示来源未定~~ **已定案（§3.3）：按钮文本 = `MEMORYTEXT_GetText(RECIPEBASE[mixType].b0-1)`，无图标。** 默认复用 35291「宝石强化」；如需更贴切文案，在目录里换任意既有 wordId 即可（不新增本地化串）。
3. ~~注入记录 `b11` bit0 取值副作用未验证~~ **已定案：`b11 = 0x02`（bit0 清 0）** —— `CheckMixture` 直接返回 0（放料不拦，由 hook 兜住）、`MakeItem` 走通用 `CreatePerfectItem` 路径（被 hook 拦截）、`GetResultItemCount` 把结果数钳为 ≤99（原版混沌 bit0=1 时钳为 1）。
4. ~~表指针全局的可写性未实测~~ **已实测（§3.8）：`RECIPEBASE`/`MIXTUREBASE` 的基址与记录数全局均位于 `.bss`，无需 mprotect。**
5. 宝石范围是否依赖 bits11-17（等级）未定；以「写入后 classify 自检」兜底。
6. 与扩展背包 R-56 的扣料联动需真机验证（VM-C6）。
7. ~~多条配方时 `UIMix_CreateRecipeGroupControl` 的滚动/布局容量上限未验证~~ **已澄清（§3.3）：≤6 与 >6 两种分支几何完全相同、无 6 条上限**，超出可视区由滚动控件处理；多条配方仍需走一遍 VM-C1。
8. **注入记录 `b10` 必须为 0**：`CalcRecipeListCount` 只按 `b11` 掩码计数、**不检查 `b10`**，而 `MakeRecipeList` 用运行时 flag 过滤 `b10` → 两者不一致会让配方数组尾部成为 `MEM_Malloc` 垃圾（幻影按钮 + 越界读）。原版记录 `b10=1` 仅因运行时 flag ≥ 1 才安全。
9. **`formula-e.json` 中不存在求值为 0 的公式**（全表已枚举）→ 费用 0 只能由 hook 强制写 `[+0xf8] = 0`（§4.4），不能靠 `b8-9` 选一个「零公式」表项实现。
10. **`RECIPEBASE` 记录数从 69 变大无副作用**：全库无硬编码 69；3 处全量遍历（`GetRecipeCount`/`CalcRecipeListCount`/`MakeRecipeList`）均以运行时 u16 计数为界；存档只读写配方书位图（长度按 `GetRecipeCount(5)` 动态）。
11. **`MIXTUREBASE` 在 189 之后追加条目安全**：运行时读者 `GetStuffItem@0x11b05c`、`GetStuffCount`、`UIMix_Draw@0xc1fac` 只按下标取、不校验长度；计数全局 `*(0x2f6e60)` 无运行时读者。
12. **跨版本 VMA**：新增的 5 个表指针全局（`0x3019b0`/`0x3019b8`/`0x3019ba`/`0x3019a0`/`0x3019a8`）是无名 `.bss` 变量，只能走 VMA 兜底（与既有 `G_UIMIX_VMA = 0x305550` 同例）；函数类符号经 `symbol_registry.h` 名称解析。若三版本 `.bss` 布局不同，需对 3 个变体各验一次。
    **真机实证（2026-09-15）**：`MIXTUREBASE` 的基址与大小两个全局互换了位置会使 `mixture_data`/`mixture_size` 读到垃圾（实测 `memcpy` 源 `0xbd0003`、长度 19278）并直接崩掉游戏进程。`custom_recipe_table_inject()` 已加窄域 fail-closed 校验（`recipe_size ≤ 64`、`mixture_size ≤ 16`、`1 ≤ count ≤ 4096`），错位时只记 `QOL_LOG_ERROR` 并跳过注入，不再崩溃。**任何新增的表指针全局都必须先由真实读取点的反汇编确认「基址/大小/记录数」三者的对应关系，不能仅凭 `readelf` 的 addend 推断语义。**
13. 真机需确认：`b2-3 = 0` 时详情/结果面板是否出现异常占位（原版记录填 64「装备」），若异常则改为 64。
14. `ITEM_GetAbilityLevel` 的登记 VMA `0x1091f4` 与放料代码 `0xc2458` 实际调用的 `0x1091b4` 不同址，实现阶段需确认二者关系（本功能费用由 hook 强制为 0，不依赖该函数，仅登记）。
15. **需求数绝不可写成 `0xFFFF`**：`INVEN_RemoveItemData@0x1040a8` 中 `count == -1` 是「**删除全部**」（`0x104130 cmn w23,#1` → `b.eq #0x1041dc`）。`material_count_for` 的返回值域为 `[0, base×5]`，正常远小于 0xFFFF，但任何新增数量规则都必须保证非负且不触顶。
16. **不要通过改写 MIXTUREBASE 的 count 来实现 0 消耗**：`MIXSYSTEM_MakeStuffSlot@0x11b2a4` 遇 `count<=0` 会跳过写条目，该条保留 `itemId = -1`，而 `UIMix_Draw` 的图标绘制路径无负索引守卫（反汇编推断，真机未验）。本实现只改 stuffList `+6`，不碰源表。
17. **材料格显示格式是「持有/需求」而非「×N」**（`MIXTUREBASE` 无关，`UIMix_Draw` 按 `%d/%d` 格式化 stuffList 的 `+4`/`+6`）。需求数变化时「持有」不重算，故 105 级会显示成「5/0」之类的形式——属预期，不是错误。若后续要求也刷新「持有」数，需另行调用 `0x104260`（持有数查询）。
18. **角色等级口径**：§2.6 取 `PARTY_GetMember(0)` 即主角。若用户实际期望的是「出战队伍等级最高者」或「当前操作角色」，需改 `kPartyLeaderMemberIndex` 的取法；诊断日志会打印 `level=` 便于核对。

## 8. 关联

- 需求：`idea.md` 第 7 项。
- 依赖事实册：`gem-craft-optimization.md`、`attribute-range-display.md`。
- 游戏机制：`../../reference/game/game-systems.md` §6.3 / §6.4 / §6.8。
- 符号与常量：`module/app/src/main/cpp/data/native/game_symbols.h`、`symbol_registry.h`。
- 待办：`../planning/backlog.md`。
