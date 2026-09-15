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

### 2.7 挂载页签与配方形式（冻结）

用户裁决（2026-09-15，两次调整后定稿）：**不使用新增页签**，改为把模块配方挂在**宝石强化页（type 1，group 3）**；该页只保留 **1 个**宝石合成入口，自定义配方排在它后面。

- **形式是配方的属性，不是页面的属性**：`Def::form` ∈ {`kConsumeToCreate`（借 type 4：直接消耗材料生成物品）、`kTargetAndCreate`（借 type 3：填入一个物品 + 消耗材料 → 生成/原地修改）、`kMultiInputCreate`（借 type 1：填入多个物品 → 生成）}。选中配方后把 `[g_uimix+0x38]` 置为该形式的原版 type，后续放料/合成/产物全部由原版该 type 的路径处理（「走原版路径」）。
- **页签归属**取 `Def::group`（页签组位索引）→ 注入记录 `b11 = 1 << group`。本轮两条都是 **group 3**。
- **该页两条配方**（顺序 = 记录下标升序 = 按钮顺序）：`#69`「合成」（`Kind::kNativePassThrough` + `Form::kMultiInputCreate`，材料显示沿用原版 record 12 的 `{itemId=28, count=3}` 与费用文本 `188`，承担原「3 颗同档 → 高一颗」职能；实际合成由 gemcraft 改写 `[+0x48] = 12+(category−28)` 完成，故 1 个入口足够）；`#70`「宝石强化」（`Kind::kJewelTierUp` + `Form::kTargetAndCreate`，即本轮的自定义升阶配方）。
- **原版 record 12..15 从该页移除**：注入时对**模块自有缓冲**清掉它们的 group3 位，其它字段原样保留（gemcraft 仍按下标 12..15 走原版合成，与组位无关）。
- **不做**：新增/重排原版 5 个页签（§7.19）；自绘合成面板与自绘文案（§7.24）。

### 2.8 3 格隐式配方（冻结）

用户裁决（2026-09-15）：「不再限制物品的填入，允许任何物品填入，但是否可以合成，看 3 合 1 配方」＋「隐式匹配」＋「每格只放 1 个物品。不允许统一格放多个」。

- **页面与形态**：宝石强化页（type 1）的「合成」条目 = 3 格模式（`Kind::kThreeSlotCraft`）。**不新增配方按钮**：配方由 3 个填入格的**内容组合**隐式决定。
- **填入**：3 格接受**任何**物品，**一格一件**。
  - **不可堆叠物品（如宝石）** = 1 个对象：同一对象不得占两格，重复点弹「已放置」。
  - **可堆叠物品** = **1 个单位**：**同一个堆也可以占多格**（重复点添加），上限 = **该类别当前持有总数** —— 用**游戏自带的** `INVEN_GetItemCount(category)@0x104260`（「原版类别数量」，`MakeStuffSlot` 填材料格「持有」数用的就是它），与扣料口径一致。即「2 个物品不能添加 3 次」：**哪怕这 2 个分散在两个各 1 个的堆里，上限同样是 2 次**；超限弹「材料不足。无法进行合成。」。
- **匹配键**：物品类别（`item+I_TYPE` 的 bits6-15）；本项目用到的物品上**类别 == itemId**（文档类别表与 itemId 区间一致：5-8/15 药水、16-25 卷轴、28-32 宝石；`ITEMDATABASE` 记录内无独立类别字段）。空槽 = 0。
- **配方表**（顺序即表序；`ordered=true` 按槽位严格匹配，`false` 按多重集）：

| # | 三个槽 | ordered | 产物 |
|---|---|---|---|
| 1 | 皮革(35) + 空 + 魔法衣料(41) | 是 | 背包（大）(4) |
| 2 | 恢复药水（小）(5) ×2 + 低级武器强化卷轴(16) | 否 | 顶级宝石(31) |
| 3–6 | 3 个同级宝石 28/29/30/31 → 29/30/31/32 | 否 | 高一级宝石（含 顶级→混沌） |
| 7 | **任意宝石(28..32) + 混沌武器强化卷轴(20) + 混沌防具强化卷轴(25)** | **是** | **该宝石自身（同类别），数值 ×1.1** |

第 7 条的槽位是**通配宝石槽**（`kAnyJewelSlot`：任意 28..32）+ 两个固定卷轴类别，**顺序严格**（卷轴换位、缺格、第 1 格非宝石均不命中）。产物沿用源宝石的类别、随机等级（bits11-17）与属性类型（bits18-23），**只把数值位（bits0-10）按 ×1.1 向下取整**并钳到上限 2047。类别依据：`ITEMDATABASE.json` 实证类别 20 → text_id 50「混沌武器强化卷轴」、类别 25 → 55「混沌防具强化卷轴」（全表满足「类别 = 名称 text_id − 30」）。

- **扣料**：不可堆叠槽按对象整堆删；**可堆叠槽只扣 1 个单位**（不是整堆）；多格引用同一堆时逐格各扣 1。
- **拒绝**：未命中配方 → 弹「材料不足。无法进行合成。」且**不消耗任何材料**；扣料前复核库存，不足则中止（**不消耗、不产出**，fail-closed）。
- **不做**：为 3 格配方新增配方按钮；允许一格放多个物品。

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

### 3.11 面板控件与页签（大修版 VMA）

- **控件树一次性创建、全 type 共用**：`Scene_Init_POPUP_SC_MIX@0x14b330` → `bl 0xbf628 UIMix_CreateMainControl`（全库仅此一处调用）→ `bl 0xc0fc0 UIMix_Enter`（`SetState(0)` + 清零 `[+0x100..+0x120]`）。`UIMix_SetType@0xbfe44` 只写 `[g+0x38]` + `ResetActiveControl@0xbfd58`（切 `[+0x90]/[+0x40]/[+0xb0]/[+0xa0]` 的 active/show）+ 改合成按钮 rect，**不重建、不销毁任何控件**（唯一被重建的是 `[+0x18]` 配方按钮组，见 §3.3）。
- **菜单按钮**：组 `[g+0x10]`（rect `0xa,0x37,0x1ba,0x157`），背景 `[g+0x28]`（rect 高 `0x1a7`）；5 个按钮 `[+0x60+i*8]`（i=0..4，rect `(0, i*行高, 0x1ba, 行高)`，行高 = `GRPX_GetFontHeight(1)+0x29`），创建 = `ControlButton_Create@0xaa710(parent, execute_proc)` + `SetControlEventCallType(0x200)` + `SetDrawType(5)` + `SetDrawProc(GOT 0x2f5cd0 → UIMix_ButtonMenuListDraw@0xbeb9c)`。**`[+0x88]` 已被别的控件占用**（0xb9d28），第 6 个按钮指针只能模块自持。
- **页签文案**：`UIMix_ButtonMenuListDraw@0xbeb9c` 用 `UI_GetChildIndex@0xaeb4c` 得 idx → `SYMBOLBASE[*(GOT 0x2f6538→0x3016c0) + (idx+146)*recsize]` → `MEMORYTEXT_GetText@0x118674`；`SYMBOLBASE[146..150] = 35290..35294`（药水合成/宝石强化/宝石孔生成/混沌合成/传说装备）。该 DrawProc 画底部条图（图组 `0x13` loc `3`）+ 居中文本，按下态配色 `0xff823b`、常态 `0xe2cb9e`。
- **`ButtonMenuListExe@0xc05c0` 有上界守卫**：`0xc05e8 cmp x20,#4; b.hi 0xc05f8` → index ≥ 5 落到复位尾（清 `INVEN_nBagSlotSelected@0x7134f8` + `RefreshInvenBag@0xc0454` + `RefreshInvenItem@0xc04fc` + `DeleteCursor@0x9f6b4`）。原版**没有「当前页签选中」持久状态**（高亮即按下态 `priv+0x68`）。
- **`[+0x38]` 全库只有 2 处直接访问**（`GetType@0xbf47c` 读 / `SetType@0xbfe60` 写），其余 41 处均经 `GetType`（纯读、无副作用）；绘制在独立阶段（`Scene_Draw_POPUP_SC_MIX@0x14b3e0` → `UIMix_Draw@0xc1654`）→ 在 ExecuteProc 内「改 type → 调用 → 改回」对**读取**安全，需处理的**写**副作用见 §4.11。
- **`b11` 位语义全貌**（全库仅 5 个读点）：`IsNeedEuip@0x11aa40` 固定读 bit0；`GetRecipeCount@0x11b3f0`/`CalcRecipeListCount@0x11b4d4`/`MakeRecipeList@0x11b62c` 用 `1<<group` 掩码（**无上界钳制**）；`AddRecipeBook@0x11b968` 固定读 bit5。**bit6/bit7 无任何固定读取者** → group 6 可用。
- **`CreateRecipeList@0x11b7e0` 第二参 = 主角等级**：`u8[*(GOT 0x2f6a68 → 0x728fc8 PLAYER_pMainPlayer) + 0x0E]`，即记录 `b10` 是等级门槛（我们的注入记录 `b10 = 0` 恒通过）。
- **`UIMix_Draw@0xc1654`**：`void()`；`GetState()==0` 时只画背景/标题 + `[+0x60..+0x80]` 5 个按钮（循环界 `0xc1734 cmp x19,#5` **硬编码**）→ 第 6 个按钮必须模块自绘。
- **`ControlButton_Create@0xaa710` 第二参是 ExecuteProc**（存 `priv+0x20`；原版传 `GOT 0x2f6658 = UIMix_ButtonMenuListExe`），文本另经 `ControlButton_SetText@0xaa7dc` 写 `priv+0x00`（32B）。**注意 `game_symbols.h` 原先把它标注为 `char* text` 是错的，已更正**。
- **`ControlObject_SetRect@0x9de74` 禁止 C++ 直调**（第 5 参走 x8 sret，真机 SIGSEGV）→ 直写 `ctrl+0x18/0x20/0x28/0x30`。

### 3.12 属性/数值生成的调用链（大修版 VMA）

用户问「宝石、装备的属性随机值分别是哪个函数？有没有统一入口」。以下按 `.dynsym` 的 FUNC 边界逐个区间扫 `bl` 调用点得到（**扫描陷阱：capstone 从任意非指令边界开始会在第一条非法指令处停止，必须按函数逐个扫，否则会漏掉整片函数**）。

| 层 | 函数 | 结论 |
|---|---|---|
| 底层随机源 | `MATH_GetRandom@0xa8bcc` | **全库统一**（168 个调用点） |
| 宝石数值 | `ITEMSYSTEM_GetJewelOptionValue@0x108f90` | 全库**唯一调用者** = `ITEMSYSTEM_MakeJewel@0x10b974`（调用点 `0x10bc04`） |
| 装备词缀值 | `ITEMSYSTEM_GetOptionValue@0x109020` | 全库**唯一调用者** = `ITEMSYSTEM_MakeOptionEx`（调用点 `0x109554`） |
| **统一分派层** | `ITEMSYSTEM_CreatePerfectItem@0x10c600` | 8 个调用者（含 `MIXSYSTEM_MakeItem`、`ITEMSYSTEM_MakeItem`、`ITEMSYSTEM_ProcessUnpack`） |

`ITEMSYSTEM_CreatePerfectItem(category)` 内部：`CreateItem(category)` → `ITEMSYSTEM_IsJewel@0x10b964` 为真 ? `ITEMSYSTEM_MakeJewel(item)`（失败则 `ITEMPOOL_Free` 并返回 null）: (`0x10be70(category)` 为真 ? `MATH_GetRandom(0, *(GOT 0x2f3638) 指向的 u16 − 1)` → `SetBitValue(word, 7, 0, 值)` 掷品质 : 原样返回)。

**结论（回答该问题）**：属性值**没有单一生成函数** —— 宝石与装备各走各自的掷值函数；但**有单一的分派入口** `ITEMSYSTEM_CreatePerfectItem`，它按类别把两类掷值串起来。因此**凡是「按类别造一件成品」的路径都应调它，而不是只调 `ITEMSYSTEM_CreateItem`** —— 后者只给默认值（本功能实测：宝石数值恒为 `1024`、属性恒 0，真机报告见 §7.27 之后的本轮修复）。

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

### 4.11 挂载宝石强化页与形式分派（实现落位）

**决定性事实**（§3.11）：面板控件树在打开时一次性创建、**全 type 共用**；切页签只刷状态。因此用户设想里的「重新挂载页面控件」**没有对应需求** —— 实现只需按形式的原版 type 调用既有刷新序列。

| 部件 | 实现 |
|---|---|
| 页签归属 | `Def::group = 3` → `b11 = 0x08`；原版 12..15 的该位在注入时被清 → group 3 列表 = `[69, 70]`（`MakeRecipeList` 按记录下标升序写数组，故顺序即「合成」在前） |
| 配方点击 | `UIMix_ButtonRecipeExe` hook：转调**前**快照 `[+0x100+当前type*8]`；转调**后**若 `[+0x48]` 命中模块配方且 `form_borrowed_type(form) != 当前 type` → `SetType(该 type)` → `InitMixingState()` → `ResetStuffItemControl()` → **恢复快照**（防自定义下标污染原版该 type 的已选槽，否则切回原版页签会错选/越界读）。同型则不做任何事。 |
| 形式分派 | `Form → 原版 type`：`kMultiInputCreate = 1`、`kTargetAndCreate = 3`、`kConsumeToCreate = 4`。选「合成」时同型（已在 type 1）→ 不动作，gemcraft 的既有优化照常生效；选「宝石强化」时切到 type 3（目标槽 + 原地改写）。 |
| kind 门控 | 放料 / 合成按钮 / 产物 / `apply_material_count` **只对 `kind == kJewelTierUp` 介入**（helper `module_recipe_for_mix_type`）；`kNativePassThrough`（「合成」入口）一律转调原函数 —— 它的放料校验、费用与产物完全由原版 + gemcraft 处理。 |
| 表注入 | 保留原「指针自校验 + 需要时重新注入 + 停用只收记录数」机制；新增：注入时对模块自有缓冲清除目录各 `group` 位在原版记录上的占用（`b11 &= ~0x08`），使原 4 条不再出现。 |

**硬约束**：

- **禁止 `SetType(5)`**：`ResetActiveControl` 对 type ≥ 5 会用未初始化寄存器 → 按钮激活态不确定、`[+0x98]`/`[+0xa0]` rect 错乱。module 的 type 始终停在 0..4。
- **禁止调 `ControlObject_SetRect@0x9de74`**（x8 sret）→ 直写 `ctrl+0x18/0x20/0x28/0x30`。
- 绘制路径（`custom_tab_draw` / `custom_draw_wrapper`）**不得有任何级别日志**（R4）。
- 开关关闭时点该页签：只记 `QOL_LOG_WARN(... reason=disabled)` 并返回，不建列表、不改界面。
- 页签文案是模块自绘的 UTF-8 字面量（`"自定义配方"`），不进游戏文本表。

### 4.12 3 格隐式配方的实现落位

| 部件 | 实现 |
|---|---|
| 配方表 | `custom_recipe_catalog.{h,cpp}` 的 `ThreeSlotRecipe` / `three_slot_recipes()` / `match_three_slot()`（唯一真源；纯逻辑，host 可直接断言） |
| 放料 | `custom_place_wrapper` → `place_three_slot_item()`：type==1 且命中 `kThreeSlotCraft` 时**不转调原函数** —— 取选中背包物（`[+0xd8]` → `ControlObject_GetCursor` → `GetData` → `*`）→ 读 `[+0x128]` → 查重/上限判定 → `UIDesc_SetOff`（与原版放料前一致）→ `ControlItem_SetItem(ControlObject_GetChild([+0xc8], idx), item)` |
| 合成 | `custom_mixing_wrapper` → `mix_three_slot_craft()`：读 3 格类别 → `match_three_slot`；未命中弹 94 并返回；命中则记入模块全局 `g_pending_three_slot` 并弹**原生 YesNo**（文本 18；回调 = 模块的 `three_slot_craft_callback`；第 6 参 `param` **必须传 nullptr** —— 该位会被当钱数渲染，上下文只能走模块自有全局） |
| 产物与扣料 | 回调内**重读 3 格**（确认框停留期间可能被改动）→ **库存复核**（可堆叠堆的「需扣单位数 ≤ 当前数量」，不足弹 94 并**中止**）→ 逐格扣料（不可堆叠 `INVEN_RemoveItem`；可堆叠 `INVEN_RemoveItemData(category, 1)`）→ `ITEMSYSTEM_CreateItem(product,0,0,0)` → `INVEN_SaveItem(item, nullptr)`（唯一入包漏斗，**扩展袋 R-56/R-52 自动生效**）；入库失败按原版判据 `ITEMPOOL_Free` + 弹 5。**`ProductMode::kScaleFirstItem`（第 7 条配方）额外要求**：在**扣料前**抓取第 1 格物品的类别与宝石字（`item+0x10`）——扣料会销毁该对象、指针随即失效；第 1 格已非宝石（确认框停留期间被换）→ 弹 94 并**中止，不消耗不产出**；产物类别取第 1 格类别，创建后只把数值位（bits0-10）改写为 `scaled_jewel_value(value, 1100)`，等级/属性类型位原样搬用 |
| 收尾 | 照原版 `UIMix_StartMix` 成功序列：`UIMix_InitMixingState` → `UIMix_ResetStuffItemControl` → `UIMix_RefreshInvenItem` → `SOUNDSYSTEM_Play(9)` → 弹 106 |
| 与 gemcraft 共存 | gemcraft 的合成 gate 是「3 格填满且同档 28..31 → 改写 `[+0x48]=12+(cat−28)`；填满但混档/超界 → 弹 98 拦截」⇒ **会先拦掉本模式**。故在其 gate **最前面**加一条：`custom_recipe_three_slot_mode_active()` 为真 → 直接 `call_orig` 返回；gemcraft 其余逻辑一律不动 |
| 数量判定 | 上限与复核都用**游戏自带的类别持有总数** `INVEN_GetItemCount(category)@0x104260`（`fn_inven_get_item_count`）；判定式抽为 `custom_recipe_rules.{h,cpp}` 的 `slot_add_allowed(placed_slots, held_count)` 与 `stack_units_available(units, held_count)`（纯逻辑 + host 断言） |
| 可堆叠判定 | `data/native/item_class.h` 的 `item_count_encoding(item) == kEncoded`（ITEMCLASSBASE `+6` bit0） |

**硬约束**：本模式下放料与合成**都不转调原函数**；未命中或库存不足**绝不消耗**；关闭总开关后一律回落原版行为。

### 4.13 3 格填入格「不显示整堆数量」（实现落位）

**问题**：3 格允许放可堆叠物品，而 `ControlItem_SetItem` 放进去的就是**那一整堆对象** ⇒ 填入格被画成整堆数量（如「5」），与「一格 = 1 个单位」的语义不符。

**根因（反汇编定案）**：
- `ControlItem_Draw@0xaaedc` 画图标时调 `ITEM_DrawPorting@0x10644c(item, x, y, type, show_count)`，并**硬编码 `show_count = 1`**（`0xaaf24 mov w4,#1`）。
- `ITEM_DrawPorting` 入口 `0x106474 uxtb w22,w4` 保存该标志；`0x106550 cbnz w22, #0x106628` —— 置位才进数量分支；且 `0x106630 cmp w0,#1; b.le #0x106554` ⇒ **数量 ≤1 本来就不画数字**。数量分支 = 置白色 + `0xb0724` 在 `(x+0x43, y+0x2f)` 画数字。
⇒ 只要绘制期间把 `show_count` 压成 0，该格就不显示任何数字。

**实现（两个函数级 hook，均单独安装、失败只告警）**：
1. `custom_control_item_draw_wrapper`（hook `ControlItem_Draw@0xaaedc`）：当 `custom_recipe_three_slot_mode_active()` 且 `ctrl` 命中本模式 3 个填入格之一 → 置 `g_drawing_three_slot_slot = true` 后转调，随后**恢复原值**（保存/恢复而非直接清 0，天然可重入）。
2. `custom_item_draw_porting_wrapper`（hook `ITEM_DrawPorting@0x10644c`）：`g_drawing_three_slot_slot` 为真时把 `show_count` 改为 0 再转调。

**为什么不动调用点**：`0xaaf28` 那个 BL 调用点**已被扩展背包的 draw gate BL patch 占用**（`F_ITEM_DRAW_PORTING_CALL_VMA`），不能再打补丁；改函数入口则两者可共存。

**为什么按控件指针比较、且缓存控件指针**：绘制层若直接调 `stuff_slot_control(i)` 会解引用 `[g_uimix+0xc8]`，面板关闭后该槽可能悬空 ⇒ 崩溃风险。改为在**放料/合成 hook 内**（面板必然打开）刷新 `g_three_slot_ctrl[3]` 缓存，绘制层**只做指针比较、不解引用**：面板关闭后最坏是漏/误隐藏一次数量，绝不崩。

**作用域**：只影响本模式的 3 个填入格。背包/商店等界面画的是**同一个堆对象**，数量显示照常（已由「只在 `ControlItem_Draw` 命中的那一次绘制内压制」保证）。

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
| VM-C12 | 宝石强化页配方数 | 该页出现 **2 个**配方按钮：「合成」在前、「宝石强化」在后；原 4 个（中级/高级/顶级/混沌宝石）**不再出现**；其余 4 个页签配方数不变 |
| VM-C13 | 选「合成」入口 | 选「合成」→ 放 3 颗同档宝石（category 28..31）→ 合成升一颗（走 gemcraft 既有优化，行为与改造前一致）；材料格与费用显示与原版一致 |
| VM-C14 | 选「宝石强化」配方 | 选它 → 面板切到 type 3 形态（目标槽 + 材料格）；放宝石被接受、放武器/防具被拒、合成升档、金币不变（同 VM-C3/C4） |
| VM-C15 | 互切与回归 | 「合成」↔「宝石强化」↔ 其他页签反复切换：无残影/无错位/无崩溃；已选配方不错选；开关关闭时「合成」入口正常运作、「宝石强化」配方 fail-closed（无响应且不扣材料） |
| VM-C16 | 混沌页回归 | 混沌合成页仍是 2 条原版配方（混沌/深渊混沌），功能正常 |

**已取得的真机证据（2026-09-15，大修版，`Inotia4` 进程内，非 VM 卡）**：

- `custom_recipe game_ui_custom_recipe.cpp:264 custom recipe hooks installed` —— 四处 hook 全部安装成功，与 gemcraft 的四个 GOT 槽 hook 并存无冲突。
- 启动期配置下发（静态表未装载）：`table globals invalid recipe_size=0 mixture_size=0 count=0` + `table inject deferred reason=table_not_loaded`，**进程未崩溃**（fail-closed 生效）。
- boot 后经 `POST /api/config/set` 启用：`recipe table injected base=69 records=70 materials=190` —— 与 §4.2 的运行时推导预测一致（`base_record_count = 69`、`base_material_count = 189`），5 个 `.bss` 全局地址正确、注入与写回成功。
- 再次启用：**无第二次注入日志**（指针自校验生效，未重复追加记录）。
- 开关时序：`recipe table injected base=69 records=70 materials=190` → `recipe table deactivated records=69` → `recipe table reactivated records=70`，全程 `pid` 不变。
- 全程 `pid` 不变（未崩溃）。

**状态**：**VM-C1..VM-C8 已由用户真机确认通过**（2026-09-15，含第 3 个配方出现、放料校验、升档、金档拒绝、关闭后重开面板回到 2 个配方）。**VM-C1..VM-C11 已由用户真机确认通过**（2026-09-15：第 3 个配方出现、放料校验、升档、金档拒绝、关闭后重开面板回到 2 个配方；以及档位/等级材料数量两项）。**VM-C12..VM-C16（改挂宝石强化页）尚未执行**——需真机交互。

**换落位后须重跑的旧卡**：VM-C1（混沌页配方数现为 2 而非 3）、VM-C2..VM-C6（改由「宝石强化」配方覆盖，判据不变）。

无法经 API 自动化的原因：`POST /api/ui/open_panel` 白名单不含 `craft`，且混沌合成页签切换需触摸输入。
辅助取证端点：`GET /api/debug/item/raw?bag=&slot=` 可读宝石数值位域，用于 VM-C4 合成前后对比。

交付定义遵循根 `AGENTS.md`：触及行为面必须有真机证据，缺少时只能报 `NOT_ACCEPTED`。

### 5.3 3 格隐式配方真机验收（VM 卡草案）

| 卡 | 场景 | 通过判据 |
|---|---|---|
| VM-D1 | 任意物品可填入 | 宝石强化页选「合成」后，任何物品（宝石 / 药水 / 卷轴 / 皮革 / 衣料 / 装备）都能放进 3 格；一格一件 |
| VM-D2 | 顺序严格配方 | 皮革→格1、格2 留空、魔法衣料→格3 ⇒ 合成得背包（大）；把两件顺序对调 ⇒ 弹「材料不足」且不消耗 |
| VM-D3 | 顺序无关配方 | 恢复药水（小）×2 + 低级武器强化卷轴×1 任意排列 ⇒ 合成得顶级宝石 |
| VM-D4 | 宝石 3 同级 | 3 个同级宝石 ⇒ 高一级；3 个顶级宝石 ⇒ 混沌宝石 |
| VM-D5 | 未命中 | 组合不在表内 ⇒ 弹「材料不足。无法进行合成。」且材料 / 金币不变 |
| VM-D6 | 回归 | 「宝石强化」配方（宝石 + 卓越灵药 → 原地升档）不变；混沌 / 药水 / 传说 / 打孔四页的描述与配方数不变 |
| VM-D7 | 关闭开关 | 关闭总开关 ⇒ 3 格恢复原版校验（只收宝石、档位比对、重复放置弹「已放置」），无残留 |
| VM-D8 | 可堆叠多格与上限 | 持有 N 个（同一堆或分散成多个小堆效果相同）：前 N 次点击可占格（**允许在同一堆上反复点**），第 N+1 次弹「材料不足」；合成后**只扣实际占用的单位数**，剩余数量正确 |
| VM-D9 | 不可堆叠重复 | 同一颗宝石重复放入 ⇒ 弹「已放置」并拒绝 |

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
- **为什么「宝石强化」页代表原生 3:1 的按钮叫「合成」而不是「宝石合成」**：游戏全表 35811 条文案中不存在「宝石合成」（最接近的是 `zh[35216]`=「合成」、`zh[2798]`/`zh[35575]`=「道具合成」），而配方按钮文案只能取 `MEMORYTEXT` 表。要精确显示「宝石合成」须原地覆写某个未被静态表引用的现成串（即改游戏文本数据）。用户 2026-09-15 裁决：**用现成的「合成」**，不动文本表。
- **为什么该页只保留 1 条原生 3:1 记录**：原版 4 条（`RECIPEBASE[12..15]`）按宝石档位各占一个按钮，而 gemcraft 已按放入宝石的实际档位改写 mixType，4 个按钮功能等价 → 用 1 条 `kNativePassThrough` 记录代表即可。（用户 2026-09-15 裁决）

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
19. **不重排原版 5 个页签**：把它们后移会让 index→语义映射三处同时错位 —— `0x24a6ec` 的 5 字节跳转表、`UIMix_ButtonMenuListDraw` 的 `SYMBOLBASE[idx+146]` 文案、`[+0x100+type*8]` 已选槽与按钮几何；且 child index 来自父组子链表**尾插**，无「插队」手段。故第 6 页签**追加在末尾**。
20. **菜单按钮组没有第 6 行的几何余量（未确认，真机观察项）**：按钮绘制与命中**都不受**父组 rect 裁剪（`ControlButton_Draw` 无 scissor；命中用控件自身绝对 rect）→ 追加按钮功能上可行；但**视觉**上背景 `[g+0x28]` 高 `0x1a7`(423)、按钮组 `[g+0x10]` 高 `0x157`(343)，行高 = `GRPX_GetFontHeight(1)+0x29`（运行时值未确认）。若真机上背景裁掉第 6 行，需直写 `[g+0x28]`（必要时 `[g+0x10]`）rect 加高一行。
21. **自定义页签的配方列表依赖 `MIXSYSTEM_pRecipeList` 全局**：`CreateRecipeList(6, 等级)` 会顺带重写配方数全局 `*(0x728c10)`（该全局同时充当 type 4 页签的「配方书非空」旗标）。type 4 页签每次点击都会重新 `CreateRecipeList(5)`，故污染是瞬态的；**但不得在自定义页签上触发配方书逻辑**。
22. **第 6 页签按钮的居中口径**依赖模块 `ui_draw_text_centered` 的既有实现；若真机上文案未水平居中，需改为手工算宽后左对齐绘制（原版用 `GRPX_GetFontHeight(1)` 自行居中）。
23. **注入记录只挂 group 6 后，混沌合成页回到 2 条原版配方**（VM-C14 判据）。若希望两页都出现，把 `kRbGroupValue` 改为 `0x42`（bit1|bit6）即可 —— 但那时自定义页签的配方列表也会包含它，两条路径共用同一 mixType。
24. **第 6 页签方案已废弃（2026-09-15 用户改挂宝石强化页），但结论保留备查**：模块自建第 6 个菜单按钮后**功能可用（可点击进入）但视觉不可见** —— 说明「在游戏面板里自绘」这条路的绘制上下文有未定位的坑（模块 kit 的 `ui_draw_text_centered` 在模块自建控件上工作正常，在游戏 `ControlButton` 上未渲染；`ControlButton_Draw` 确实被调用、按钮也在触摸树内）。因此最终方案改为**不自绘**：配方文案一律取自游戏文本表（`MEMORYTEXT`）。
25. **`MEMORYTEXT` 与配方文案**（事实，本任务新查）：配方按钮文案 = `MEMORYTEXT_GetText(RECIPEBASE[mixType].b0-1)`；`MEMORYTEXT` 是**单一缓冲**，结构为 `[+0: 4 字节头][+4: 3 字节/条的 24 位偏移表][串区]`，串地址 = `base + offset`；基址在可写 `.bss` 全局 `*(0x2f59a0)`，**id 上限**在可写全局 `*(0x2f3c48)`（`MEMORYTEXT_GetText@0x118674`：`id >= count → 返回 NULL`）。**`MEMORYTEXT` id 就是 `zh-Hans.json` 下标**（实证：`zh[35290..35294]` = 5 个页签名；`zh[1163..1166]` = 中级/高级/顶级/混沌宝石）。
26. **游戏文本表里没有「宝石合成」这个字符串**（全表 35811 条已枚举），现有最接近的是 `zh[35216]`「合成」、`zh[2798]`/`zh[35575]`「道具合成」。**不能安全借用原 4 个按钮的文案 id**：静态全表扫描显示 `1163`/`1164` 还被 `BUFFUNITBASE`（rec 99-102，与药水/灵药同表的可使用道具名）与 `HELPTEXTBASE`（rec 17）引用，改它们会连带影响那些界面。**若要精确显示「宝石合成」，需要新增「MEMORYTEXT 文本注入」能力**（复制缓冲 → 尾部追写自造串 → 重指向某个 id → 提升 id 上限；注意偏移表 +3 字节的位移），并需真机与跨版本验证。当前实现用 `zh[35216]`「合成」。

27. **挂在宝石强化页（type 1）的记录必须带合法费用公式 wordId（不能为 0）**。`UIMix_ButtonRecipeExe@0xc0390` **内部自带一次 `UIMix_InitMixingState` 调用**，而此刻页面 type 仍是该页自身的 1（模块按形式换型发生在转调**之后**，来得及的是布局重建，来不及的是这次内部初始化）。`InitMixingState` 的 **type==1 专用分支**（`0xc00f0 cmp x0,#1; b.eq 0xc01f0`）会用本记录 `b8-9` 求值费用：`0xc021c MEM_ReadUint16(rec+8)` → `0xc0224 MEMORYTEXT_GetText_E(wordId)` → `0xc0230 CAL_Calculate(公式)`。`b8-9 = 0` 时 `GetText_E(0)` 返回空 → `CAL_Calculate(NULL)` 空指针解引用**直接崩溃**（真机 `SIGSEGV fault addr 0x0`，栈 `CAL_Calculate+436 ← UIMix_InitMixingState+504 ← UIMix_ButtonRecipeExe+64`）。
    **只有 type 1 会在 `InitMixingState` 里算费用**：type 3 分支只把 `[+0xf8]` 清零（`0xc0124 str xzr,[x19,#0xf8]`，另有 `GetResultItemCount`），type 2/4 走 `0xc01c0` —— 这正是「混沌页版本能正常工作」的原因。故本功能 `kJewelTierUpCostWordId = 188`（与原版 record 12 同一公式，其安全性由游戏自身行为证明）。**将来任何挂在 type 1/0/4 的记录都必须填合法公式；免费语义只能由 hook 强制 `[+0xf8] = 0`，不能靠记录字段为 0。**

28. **配方描述文案：由 `UIMix_Draw@0xc1654` 内部逐帧重建并写入**（真机日志定案；最终实现与审计见下方「⚠️ 定案」段）。
    > 以下三条是关于**另一个 UIDesc 实例（装备详情面板）**的事实，与合成器面板的描述无关，保留备考：
    - `UIDesc_MakeItemByID@0xb2eec` 按「结果 itemId」扫 itemdesc 表取描述 id（全库只有 2 个调用点，都在 `UIMix_MakeDesc@0xc0c6c` 内：`0xc0d50` 用 `RECIPEBASE[[g_uimix+0x48]].b2-3`、`0xc0de0` 用材料条目 itemId），把文本写进静态缓冲 `0x303dc0`，随后 `X_TEXTCTRL_SetTextControl@0xb181c` + `ControlScroll_SetOption@0xab130` + 尾跳 `UIDesc_SetOn@0xb2b0c`。`UIDesc_MakeItem@0xb36a0` 同构，但 x0 是**物品指针**（按物品类别 bits6-15 直接作行号查表）。
    - 模板 `MEMORYTEXT 35482` = `用3个$S%s$B合成一个$R%s$B。`（**34 字节**）；静态全表只有 `TEXTDATABASE[166].u16[0] == 35482`。
    - **输出方式**：写进 `0x303dc0` 的 512 字节共享缓冲（入口 `0xb2ef0 mov x2,#0x200` + `memset`），**返回前**即经 `X_TEXTCTRL_SetTextControl@0xb181c`（内部 `0xb1184` 解析至 NUL 拷贝进描述控件）→ `ControlScroll_SetOption@0xab130` → 尾跳 `UIDesc_SetOn@0xb2b0c`。两个调用点**都弃用返回值** ⇒ 事后改 `0x303dc0` 无效、替换返回值也不可行。
    - 模块两条记录的 `b2-3 = 0`（物品 0 = **「金币」**）⇒ 描述被拼成「用3个低级宝石合成**金币**」。静态 `RECIPEBASE` **无任何 `b2-3==0` 记录** ⇒ 该错误文案只可能由模块记录触发。
    - **处理（当前实现）**：在 `0xb2eec` 上装函数级 hook——**先原样转调**（保留其对 UIDesc 全局态的全部写入与刷新），随后在门控命中时**重放原函数同款刷新尾**，文本改传**模块自有静态 `char[]`** `用三个宝石合成高一级宝石`。**不改任何游戏文本数据**（不碰 `MEMORYTEXT`、不覆写 `35482`、不动 `*BASE` 表）。门控＝「`itemId == 0`」且「`[g_uimix+0x48]` 命中模块记录」；`static_assert` 锁文案长度上限（刷新链扫描上限 `0x166`）；符号未就绪时 fail-safe 保留原函数结果。
    - **为什么必须挂在 `0xb2eec` 而不是上层**（失败教训）：第一版把抑制挂在 `UIMix_MakeDesc` 的 `mode == 1` 分支上，真机**完全无效**——因为 type 1 下三条 mode 都到不了 `0xb2eec`（mode 0 type1 → `0xc0cc8 b.ne 0xc0c98` 直接返回；mode 1 type1 → 转 `0xb36a0` 走物品说明），被抑制的分支根本不是该文本的来源。**结论：要改这类「共享拼装器」的输出，就挂在那个唯一的拼装函数上，别赌上层分支。**
    - **为什么原地覆写模板不可行**：模板 34 字节（含 NUL 35），目标文案 36 字节；`MEMORYTEXT` 是紧凑打包的偏移 blob，串间无余量，覆写会冲掉下一个串。且该模板 id 是**数据表字段**（不是代码立即数），改它等于改游戏文本数据。
    - **为什么不改 `b2-3`**：它同时是产物 id，改 29 会把 `#69`（`kNativePassThrough`，真实行为是 gemcraft 按投入档位改写 mixType=12..15）的静态产物语义钉死成「中级宝石」，与真实行为不符。**保留 `b2-3 = 0`。**
    - **⚠️ 定案（真机日志指名）：宝石强化页这段描述由 `UIMix_Draw@0xc1654` 内部逐帧重建并写入。**
      - **写入点**：`UIMix_Draw` 内部直接调 `X_TEXTCTRL_SetTextControl@0xb181c`（诊断打印的返回地址 `ra=0xc20a4` 落在该函数区间 `0xc1654..0xc2124` 内）。因其在绘制路径上，**描述每帧重建**（一轮日志里同一调用重复 8～13 次）。
      - **目标是面板自己的堆对象**：实测 `ctrl=0x7a3087ebe8`、`text=0x7a3087f3e0`，**每次打开面板都不同**，**不是**静态全局 `0x302d98`/`0x303dc0`（后者属于**另一个 UIDesc 实例**：装备详情面板，由 `0xb2eec`/`0xb36a0` 使用；这两个函数在宝石强化页**零调用**）。
      - **错误文案成因**：模块两条记录 `b2-3 = 0`（物品 0 = 「金币」）⇒ 面板按该 id 取到模板 `MEMORYTEXT 35482` = `用3个$S%s$B合成一个$R%s$B。`（34 字节）⇒ 拼出「用3个低级宝石合成**金币**」。静态 `RECIPEBASE` **无任何 `b2-3==0` 记录** ⇒ 只可能由模块记录触发。
      - **最终实现**：只挂**一个** hook —— `X_TEXTCTRL_SetTextControl@0xb181c`。命中时先按内容门控，再把文本缓冲**原地**换成模块文案，然后转调原函数（宽度/滚动度量/点亮全由原调用点用自己的实参完成；模块不重放刷新链、不写任何游戏文本数据、不动 `MEMORYTEXT`/`35482`/`*BASE` 表）。门控 = ① `[+0x48]` 命中模块记录 ② 文本以原版模板前缀「用3个」（`e7 94 a8 33 e4 b8 aa`）开头 ③ 原文本不短于模块文案（**只做缩短替换**，绝不放长，避免越界写堆缓冲）。文案取目录常量 `kModuleRecipeDesc`（当前 = `用3个材料合成`）。
      - **审计结论（2026-09-15 真机一轮日志）**：模板前缀调用 8 条**全部** `mixType=69`；其余 `SetTextControl` 调用（「点击」`head=e782b9e587bb`、「注意」`e6b3a8e5868c`、「道具」`e98193e585b7`、「这里」`e8bf99e6898d`，均 `mixType=0`）**一次都没被触碰** ⇒「模板前缀且非模块 mixType」计数 = 0，**替换无副作用**。
      - **为什么前几轮全部无效（失败教训）**：第一次误判写入者为 `UIDesc_MakeItemByID@0xb2eec`（实际零调用），第二次误判为 `UIDesc_MakeItem@0xb36a0`（实际零调用），两次都白做；第三次虽挂对了函数（`0xb181c`），门控却写成「`ctrl == 0x302d98` 且 `text == 0x303dc0`」—— 而真实调用用的是面板自己的堆对象，**指针比对永远不成立**。**教训：定位「某段 UI 文本由谁写」时，先用有界诊断日志打印「调用者返回地址 + 目标控件 + 文本首字节」把写入者钉死，再写门控；门控要按内容，不要按指针（对象可能是每面板/每帧新建的堆对象）。**
      - **装饰性 hook 不得串进主安装链**：`UIDesc_MakeItem@0xb36a0` 已被 attribute_range 功能 hook → 对同一地址二次挂载 `native_hook_func` 返回 -1；早期把它串进 `&&` 链，导致 **5 个核心 hook 全部未安装**（真机 `install deferred reason=bridge_or_hook_not_ready` 实证，功能整体失效）。现在描述 hook **单独安装、失败只记 `QOL_LOG_WARN`**，安装日志为 `custom recipe hooks installed core=5 desc=%d`。

29. **「构建失败但脚本仍把旧包装上」必须双重确认**：`scripts/build-debug.sh` 在 C++ 编译失败时本任务曾出现「已报 BUILD FAILED、但 `output/` 里仍有可安装的旧包、脚本照样安装成功」，导致一次「修复」实际没生效（已向用户更正）。**判定新包是否生效，必须同时确认 `BUILD SUCCESSFUL` 与输出的 APK 文件名/hash 是本轮新产物**，不能只看「install Success」。
30. **静态数据 dump 不完整，不能当否定证据**：`apk/static-data/json/tables/ITEMCLASSBASE.json` 仅 36 条（缺尾部），据此曾误判「类别 ≠ itemId」，随后被真机 `GET /api/item/inventory` 读出的真实类别推翻 —— **类别 == itemId 成立**（实测：恢复药水（小）=5、低级武器强化卷轴=16、皮革=35、魔法衣料=41、低级宝石=28、卓越灵药=15、秘银=33、再生药水（小）=10、元气恢复药水=14、生命之叶=57）。凡「表里查不到 / 越界」的结论，都要先确认真机内存实际值再下判断。
31. **产物掷值的正确入口**：3 格配方的产物必须经 `fn_item_create_perfect_item`（`ITEMSYSTEM_CreatePerfectItem@0x10c600`）创建，否则宝石数值恒为默认值 `1024`、属性恒 0（详见 §3.12）。

## 8. 关联

- 需求：`idea.md` 第 7 项。
- 依赖事实册：`gem-craft-optimization.md`、`attribute-range-display.md`。
- 游戏机制：`../../reference/game/game-systems.md` §6.3 / §6.4 / §6.8。
- 符号与常量：`module/app/src/main/cpp/data/native/game_symbols.h`、`symbol_registry.h`。
- 待办：`../planning/backlog.md`。
