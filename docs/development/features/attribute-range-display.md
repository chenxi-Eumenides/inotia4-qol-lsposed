# 属性显示范围（attribute-range-display）功能设计

> 状态：IMPLEMENTED + 真机验证（装备随机词缀着色端到端可用；独立宝石详情代码路径已实现，尚未取得 UI 截图证据）
> 来源需求：`idea.md` 第 2 项「属性显示范围」；关联 `../planning/backlog.md` P2「宝石随机属性优秀程度标注」。
> 本文是「按随机属性在其可能范围内的百分位着色」这一功能的唯一设计册。端点/UI 契约在定稿前不进入 `../../reference/api-reference.md`。
> 可行性测试记录见 §9；实现与真机证据见 §10。测试探针位于 `.tmp/attr-range-feasibility/`（临时，不入库）。

## 1. 范围与职责

- 在**宝石详情**与**装备详情**面板中，把随机属性的数值按其在该属性可能取值范围内的百分位重新着色。
- 本文只定义功能目标、着色规则、现状依据、待探索项与验收计划；**不含实现**。
- 不改动原版属性数值本身，只改详情文本的显示颜色；不新增对外 HTTP 端点（纯游戏内 UI 行为）。

## 2. 需求规格（已对齐，冻结）

### 2.1 颜色分档

| 颜色 | 百分位区间 | 下限（含） |
|---|---|---|
| 金 | 100%（满值） | `value == max` |
| 紫 | ≥90% 且 <100% | 90 |
| 蓝 | ≥75% 且 <90% | 75 |
| 绿 | ≥60% 且 <75% | 60 |
| 白 | ≥30% 且 <60% | 30 |
| 灰 | ≥0% 且 <30% | 0 |

### 2.2 纳入着色的数值类别（2026-09-12 最终裁决后）

1. **独立宝石详情**数值（E9 可行）；
2. **装备随机词缀** `bonus[]`（E1 可行）。

> **装备基础属性移出范围**：经真机测试为确定性值、无逐物品随机范围（§9.4）。
> **已镶嵌宝石移出范围（E7 裁决）**：镶嵌时宝石类别/等级被丢弃，无法精确求范围；按值归类仅 17%–39% 唯一。
> **混沌/附魔移出范围（E8 裁决）**：混沌为确定性值；附魔值 = `base + ITEMENCHANTBASE[等级]增量`（随机只决定成功/降级），均无逐物品范围。
> **超域固定值兜底**：数值超出其可计算范围时（特殊/固定物品，如「战神的斗篷」）**视为满值，着金色**。

### 2.3 范围获取原则

- **优先动态获取**：运行时由游戏自身的函数/公式算出每个数值的 min/max，以适应不同版本（大修 / monster / 原版）公式差异。
- 静态导出范围表仅作为动态路线不可行时的兜底，不在首选方案内。

### 2.4 明确不做（本轮冻结）

- 数值后追加 `(min~max)` 括号显示：不做。
- 独立功能开关：不引入，功能始终生效。

## 3. 现状事实（逆向与代码依据）

> 本节只记录当前状态。标「待复核/待探索」的条目尚未在本功能语境下独立验证。

### 3.1 物品与选项数据结构

来源：`module/app/src/main/cpp/data/native/game_symbols.h`（项目权威常量）+ 本次反汇编复核。

| 常量 | 偏移 | 语义 |
|---|---|---|
| `I_TYPE` | 0x08 | u16 类型位域（bit2-5 稀有度、bit6-15 类别） |
| `I_COUNT` | 0x10 | u32 数量位域（可堆叠物品 bit22-31；装备/jewel 另有位域语义，见 §3.2） |
| `I_MAGIC_RATE` | 0x18 | u8 魔法伤害倍率（物理伤害 ×此值/100） |
| `I_SOCKET` | 0x19 | u8 宝石/插槽位域（bit0-2 已镶数、bit4-6 插槽等级） |
| `I_ENCHANT` | 0x1A | u16 混沌/附魔位域（bit0 有混沌、bit5-6 附魔等级、bit10-15 附魔ID） |
| `I_OPTION_LIST` | 0x20 | 词缀链表头 |
| `O_INDEX` | 0x00 | u16 选项编码：bit0-6 选项索引、bit13-15 类型（0=词缀、1=宝石） |
| `O_VALUE` | 0x02 | s16 **已掷出的选项值**（`ITEM_GetOptionValue` 直接 `ldrsh [x1,#0x2]`） |
| `O_NEXT` | 0x08 | 下一节点指针 |

**关键事实**：选项节点只存「索引 + 已掷值」，**不存该值可能范围的 min/max**。

### 3.2 随机值的产生链（反汇编观察）

符号地址基于大修版 `libgame.so`（`apk/decoded/overhaul/lib/arm64-v8a/libgame.so`）：

| 符号 | 地址 | 作用 |
|---|---|---|
| `ITEM_AddOptionEx` | 0x105ec4 | 建选项节点：`SetBitValue(node, 13, 15, index)` 编码索引、`strh value,[node+2]` 写值；节点步长 16B |
| `ITEMSYSTEM_MakeJewel` | 0x10b974 | 生成宝石：先以 `MATH_GetRandom(表+3, 表+4)` 随机宝石等级，再按类别/等级匹配调用 `GetJewelOptionValue` 取值，结果写入物品 `+0x10` bits0-10（bits11-17=等级、bits18-23=类型） |
| `ITEMSYSTEM_GetJewelOptionValue` | 0x108f90 | **宝石值即范围随机**：由 `(category, type)` 算出 X 后 `MATH_GetRandom(X, 2X)`（已反汇编确认；唯一调用点 `MakeJewel` 0x10bc04） |
| `ITEMSYSTEM_MakeOptionEx` | 0x10928c | 装备词缀生成：随机挑选选项索引集合；对每个选项调 `GetOptionValue` 掷值。入口参数 `level=-1` 时取 `ITEM_GetAbilityLevel(item)` 作公式变量（0x10975c） |
| `ITEMSYSTEM_MakeOption` | 0x10dbe4 | 装备/宝石选项生成总入口：按稀有度决定词缀条数（首饰 +1），再调 `MakeOptionEx(item, count, -1)` |
| `ITEMSYSTEM_ChangeOption` | 0x10ecac | 选项变更（洗练/替换）路径，同样经 `MakeOptionEx` |
| `ITEMSYSTEM_GetOptionValue` | 0x109020 | 选项值计算：`CAL_Calculate(formula[optIdx], abilityLevel, flag)` → 修正 → `MATH_GetRandom(base/2, base)`；**掷值域 = [base/2, base]**（真机分布验证，见 §9.3） |
| `CAL_Calculate` | 0xd9968 | 公式求值：`CAL_Calculate(text, &arg, 1)`；公式表 = `formula-e`（`MEMORYTEXT_GetText_E`，1991 条），选项索引 → 公式经 `ITEMOPTINFOBASE` 记录 `+4` text id 查得 |
| `MATH_GetRandom` / `MATH_GetRandomAsLinear` | 0xa8bcc / 0xa8ba0 | 随机数原语：`MATH_GetRandom(min, max)` 返回 **[min, max] 闭区间**（已反汇编确认） |

### 3.3 详情渲染链（反汇编观察）

| 符号 | 地址 | 作用 |
|---|---|---|
| `UIDesc_MakeItem` | 0xb36a0 | 构造物品详情文本（入口，持有物品上下文） |
| `UIDesc_AddOption` | 0xb343c | 逐条格式化选项行：入参 `(buffer, optionType, optionIndex, value)`，按选项类型选颜色 id（1→0、2→7、其它→0xc），经 `TEXTCTRL2_GetCodeFromColorID` 把颜色码写入行文本 |
| `UIDesc_GetTextCtrlColorCode` | 0xb3224 | 按稀有度/混沌/附魔状态返回颜色 id |
| `UIDesc_GetColorAsItemGrade` | 0xb5490 | 按稀有度返回整行 ABGR 颜色 |
| `UIDesc_GetColorAsItemGradeByID` | 0xb55b4 | 同上，按物品 id |
| `TEXTCTRL2_GetCodeFromColorID` | 0x13e154 | 颜色 id（0..0xf）→ 文本颜色码（16 项表 @0x24bc10，越界回退 0x44）；**真机实测输出 `$<码>文本$B` 内联颜色串**（如 `$T暴击率: 1.0%$B`） |

**事实**：详情行的颜色由「选项类型 / 稀有度 / 混沌附魔状态」决定，当前**不感知数值本身的高低**；颜色以内联 `$<码>…$B` 形式写在行文本里，因此既可在取色函数处替换颜色码，也可在 `UIDesc_AddOption` 输出后改写该字符。真机已验证覆盖取色函数可改变输出（§9.5）。

### 3.4 模块既有能力（已产品化，可作为实现基础）

- PtrHook 覆盖游戏内存函数指针（`feature/patch/game_ptr_hook.h`），无 inline trampoline，已在设置页/合成器/UI 实验真机验证（见 `../../reference/game/ui.md` §2.2.1、§6）。
- 可复用原生 UI 组件层 `game_ui_kit` 与 `game_ui_components`（见 `../../reference/game/ui-kit.md`）。
- 物品 JSON 读取已暴露 `base`/`bonus`/`gem`/`chaos`/`enchant` 结构（`api/native/game_inventory.*`；契约见 `../../reference/api-reference.md` Inventory 段）。

## 4. 探索结论（可行性测试后）

| # | 问题 | 结论 | 状态 |
|---|---|---|---|
| E1 | 每类数值的动态 min/max 来源 | **装备词缀（type=0）已解**：`range = [floor(base/2), base]`，`base = ITEMSYSTEM_GetOptionValue(optIdx, ITEM_GetAbilityLevel(item), flag=1, item)` 掷值前上界；base 可由 `CAL_Calculate(formula-e[ITEMOPTINFOBASE[optIdx].textId], abilityLevel, 1)` + 同款修正动态复算。真机 8/8 词缀命中。宝石（`GetJewelOptionValue`）公式已解（`[X, 2X]`）。 | ✅ 装备词缀/独立宝石可行 |
| E2 | 随机值 vs 固定值判定 | **已裁决**：装备基础属性为确定性（无范围）→ 移出着色范围；特殊/固定物品词缀值超出公式域 → 视为满值着金色。 | ✅ 已定规则 |
| E3 | 颜色码 → 金/紫/蓝/绿/白/灰 | **已解**：16 色表已 dump，6 目标色均有对应码 —— 金 `$Y`(0xffe0)、紫 `$V`(0x981f，或 `$Q` 0xbced 浅紫)、蓝 `$L`(0x0099，或 `$T` 0x7bdf)、绿 `$C`(0x07e0)、白 `$W`(0xffff)、灰 `$G`(0x8410)。 | ✅ |
| E4 | 注入点上下文是否足够 | **已解**：`UIDesc_AddOption` 只有 (type, optIdx, value)，无物品指针。装备词缀分支前驱 `ITEM_GetOptionValue(item, node)` 紧邻调用可捕获 item；宝石分支（`UIDesc_MakeItem` @0xb4208 直接调 AddOption）需改为 hook `UIDesc_MakeItem` 入口缓存 item。 | ✅ |
| E5 | 显示值与已掷值差异 | **已解**：行文本显示值经格式化（`$T暴击率: 1.0%` 对应原始值 10；`$T冰霜: 48.0%` 对应 480），百分位必须用**原始值**，不可用显示串。 | ✅ |
| E6 | 版本差异 | **已测**：`memorytext_e` 公式表与 `ITEMOPTINFOBASE` 在**大修/monster/原版三版完全一致**（md5 相同、零差异）；`game.dat` 大修=原版，monster 仅多 2 条 ITEMDATABASE。函数地址：大修=monster23，原版 v1.3.2 全部不同（`GetOptionValue` 0x109020→0x1b7c78 等），三版均按符号名导出 → 必须走动态符号解析（项目现有机制），公式求值在运行时读当前版本数据即天然兼容。 | ✅ 结论：动态路线跨版本成立 |
| E7 | 已镶嵌宝石（node type=1）范围 | **负结论 + 重叠量化**：`ITEMSYSTEM_PutJewel` 只写入属性类型与值，宝石**类别与等级丢失**。范围依赖类别（X=3×(cat−27)，cat∈{28..32}）。真机 37 个属性类型的重叠统计：按值唯一归类仅 **17%–39%**。**已裁决：不处理镶嵌宝石**。 | ✅ 已裁决（不处理） |
| E8 | 混沌 / 附魔数值范围 | **分开结论**：混沌——产生/应用链内**无随机调用**，值为确定性。附魔——`EnchantItem` 内含 `CAL_Calculate` 阈值 vs `random(1,998)` 的**成功率判定**（公式 `1500/(5×等级)`，失败降级），但显示值仍由存储的 ID+等级确定。两者均无逐物品范围。**已裁决：都不处理**。 | ✅ 已裁决（不处理） |
| E9 | 宝石**详情**（独立宝石物品）着色 | **可行**：宝石 item 保留 category(+8)、type(bits18-23)、value(bits0-10)；`UIDesc_MakeItem` 宝石分支 `AddOption(buf, 1, type, value)` 直接可着色；范围 = `GetJewelOptionValue(type, gemItem)` 的 `[X,2X]`（真机 CreateItem+MakeJewel 验证：cat28→[3,6]、cat29→[6,12]…cat32→[15,30] 等按类别成比例）。 | ✅ 可行（需捕获 item 上下文） |

## 5. 方案设计（初步，待 §4 探索确认后定稿）

### 5.1 分层落位（拟定）

```
feature/attribute_range/attribute_range.{h,cpp}   纯逻辑：范围→百分位→颜色档判定（可 host 单测）
feature/attribute_range/game_ui_attr_range.{h,cpp} UI 注入：PtrHook 详情渲染取色点
feature/attribute_range/range_provider.inc         动态范围解析（按类别调用/复算游戏公式）
```

- 依赖方向遵守 `feature → core`、`feature → data` 与既有 `feature → patch`（PtrHook）先例；不得反向。
- 符号/VMA 一律进 `game_symbols.h` + `symbol_registry.h`，禁止域文件裸偏移。
- 纯逻辑（百分位与分档）与游戏依赖分离，编入 `module/app/src/main/cpp/tests/` host 单测。

### 5.2 数据流（按已验证结论）

```
详情渲染（UIDesc_MakeItem 选项循环）
  → hook ITEM_GetOptionValue(item, node)：缓存当前 item（选项循环内紧邻调用）
  → hook UIDesc_AddOption(buf, type, optIdx, value)：
       type==0 且 value 落在公式域内 → 动态求 base → pct → 目标颜色码
       否则（宝石/混沌/附魔/超域固定值）→ 默认颜色或按 §4 裁决
  → 以目标颜色码替换该行内联 `$<码>`（或包装取色函数）
```

- base 求取二选一：① 复算 `CAL_Calculate` + 修正；② 单次受控调用 `GetOptionValue` 并临时捕获 `MATH_GetRandom` 的 `max` 实参（主线程内安全）。实现阶段择优。

### 5.3 依赖与约束

- 遵循「直接读写内存 / 调游戏函数优先，hook 兜底」的核心实现原则（`../architecture.md` §2）；本功能确需拦截渲染取色，属允许的 PtrHook 使用场景。
- 不改原版数值、不改存档、不写回物品。
- 不可在渲染热路径做重 IO 或加锁阻塞；范围结果可按物品缓存（同一物品同一选项值 → 同一颜色）。

## 6. 验收计划（定稿后执行）

- host 单测：百分位→分档边界（0/30/60/75/90/100 及 max==min、负值边界）。
- 真机验收（VM 草案，需补正式卡号与日志）：
  - VM-A1：独立宝石详情，同一宝石多次重开详情颜色稳定且与百分位一致；
  - VM-A2：装备随机词缀着色正确；
  - VM-A3：超域固定值（战神的斗篷）着金色，且不崩溃；
  - VM-A4：无范围数值（基础属性/镶嵌宝石/混沌附魔）不着色；
  - VM-A5：大修→monster→原版切换后颜色仍正确（动态路线验证）。
- 交付定义遵循根 `AGENTS.md`：触及行为面必须有真机证据，否则记 `NOT_ACCEPTED`。

## 7. 决策与理由（思考，非事实）

> 本章为决策记录，非当前实现状态。

- **为什么优先动态获取范围**：idea 明确「不同版本范围可能不同，最好动态获取」；静态表会随版本漂移失效，且当前静态 JSON 未见 min/max 字段。
- **为什么不做独立开关**：用户 2026-09-12 决策「始终生效」。
- **为什么本轮不做 `(min~max)` 括号**：用户 2026-09-12 决策「先不做」。
- **为什么 5 类→最终收敛为 2 类**：用户 2026-09-12 先选择全部纳入；随后测试依次排除：基础属性（确定性，§9.4）、镶嵌宝石（范围不可恢复，§9.7）、混沌/附魔（无逐物品范围，E8）。最终只保留**独立宝石详情**与**装备随机词缀**。
- **为什么镶嵌宝石不处理**：用户 2026-09-12 裁决。类别/等级丢弃，按值归类不可靠，近似会误导。
- **为什么混沌/附魔不处理**：用户 2026-09-12 裁决。混沌值确定；附魔随机只决定成功/降级，显示值仍确定。
- **为什么超域值着金色**：用户 2026-09-12 裁决。特殊/固定物品的数值无法给百分位，视为满值最不误导。
- **为什么先写设计不实现**：用户要求先对齐目标、文档先行；核心范围来源（E1）未解，直接编码会返工。
- **为什么先做可行性测试**：用户 2026-09-12 要求。测试确认装备词缀范围与渲染注入可行（§9.3/§9.5），并推翻了两个假设（基础属性有范围、所有词缀均随机），据以修正本册 §2.2/§4。

## 8. 关联

- 需求：`idea.md` 第 2 项。
- 代码结构规范：`../architecture.md`。
- 游戏 UI 与注入能力：`../../reference/game/ui.md`、`../../reference/game/ui-kit.md`。
- 物品字段契约：`../../reference/api-reference.md` Inventory 段。
- 数据与符号：`module/app/src/main/cpp/data/native/game_symbols.h`、`symbol_registry.h`。
- 待办：`../planning/backlog.md`（P2「宝石随机属性优秀程度标注」）。

## 9. 可行性测试记录（2026-09-12，真机 192.168.3.54 / 大修版）

> 探针脚本：`.tmp/attr-range-feasibility/probe*.py`（frida 17.16.4 attach `Inotia4`）。
> 测试环境：模块 v0.7.0 已注入，存档 0，主角凯恩 Lv5。

### 9.1 颜色码表（`TEXTCTRL2_GetCodeFromColorID` 16 项 / `TEXTCTRL2_GetColorFromCode` 解色）

真机 dump：id0..15 → 码 `A B R S Y V O P W G D C T M L Q`，对应 RGB565：

| 目标色 | 码 | RGB565 | 说明 |
|---|---|---|---|
| 金 | `Y` | 0xffe0 | 黄/金 |
| 紫 | `V` | 0x981f | 紫红（备选 `Q` 0xbced 浅紫） |
| 蓝 | `L` | 0x0099 | 深蓝（备选 `T` 0x7bdf 浅蓝） |
| 绿 | `C` | 0x07e0 | 绿 |
| 白 | `W` | 0xffff | 白 |
| 灰 | `G` | 0x8410 | 灰 |

### 9.2 公式表与求值

- 选项表 `ITEMOPTINFOBASE`（count=37、recSize=12、data@`*(0x2f55b0)`）；记录 `+4` = formula-e text id。
- 真机取 `ITEMOPTINFOBASE[0]` textId=139 → `formula-e[139]="a16*100/1+"`；`CAL_Calculate` 求值：Lv1→1、Lv30→5、Lv100→17（`a` = ability level）。

### 9.3 装备词缀范围（关键结论）

对同一真实物品指针反复调用 `ITEMSYSTEM_GetOptionValue(optIdx, abilityLevel, flag=1, item)` 400 次，得到分布；与物品实际存储的 `O_VALUE` 比对：

| 物品 | optIdx | 存储值 | 计算分布 | 判定 |
|---|---|---|---|---|
| 钛金 狩猎弓 cat185 | 5 暴击率 | 18 | [9, 19] | ✅ 命中（近满值） |
| 钢铁 铁指环 cat408 | 9 回避率 | 9 | [5, 10] | ✅ 命中 |
| 皮甲/项链/手套等 | 0/1/2/4 | 1 | [1, 1] | ✅ 命中（base=1 区间退化） |

结论：**装备词缀（node type=0）的存储值确实落在 `[floor(base/2), base]`，且该 base 可用游戏函数在运行时得到**。

反例：存档内「战神的斗篷」cat874 的 7 条词缀存储值（20/78/1/20/20/480/20）**全部超出**公式域（对应 [5,10]/[30,60]…），判定为特殊/固定物品，非标准随机路径产物。

### 9.4 装备基础属性（负结论）

`ITEM_GetDamage` / `ITEM_GetDefense`（0x1099f0 / 0x109cc0）对同一物品重复调用返回相同值（狩猎弓 damage=25/25），**基础属性为确定性值，无逐物品范围**。因此原需求第 3 类「装备基础属性按范围着色」缺少可用的范围语义，需用户裁决（改为不处理 / 按稀有度整行着色 / 取消）。

### 9.5 渲染注入（真机演示成功）

直接调用 `UIDesc_AddOption(scratchBuf, type, optIdx, value)` 并读取其写入的内联串：

```
type=0 oi=5  val=10  -> "$T暴击率: 1.0%$B"
type=1 oi=2  val=5   -> "$A体力: 5$B"
type=0 oi=24 val=480 -> "$T冰霜: 48.0%$B"
```

随后 `Interceptor.replace(TEXTCTRL2_GetCodeFromColorID)` 强制返回码 `'R'`，同样调用得到：

```
type=0 oi=5  val=10  -> "$R暴击率: 1.0%$B"
type=0 oi=24 val=480 -> "$R冰霜: 48.0%$B"
```

证明**颜色可被运行时替换**，注入方案成立。

### 9.6 未完成项

- 未能在 UI 内自动打开物品详情（真机 2 无触摸坐标预案），§9.5 以直接调用 `UIDesc_AddOption` 完成注入验证；端到端「详情面板改色」截图留待实现阶段补真机证据。

### 9.7 E7/E8/E9/E10 追加测试（2026-09-12）

**E7 已镶嵌宝石（反汇编 `ITEMSYSTEM_PutJewel` 0x10bcb4）**

`PutJewel(equipItem, jewelItem)` 只做两件事：读宝石 `+0x10` bits18-23（属性类型）与 bits0-10（值），再 `ITEM_AddOptionEx(equipItem, type=1, attrType, value)` 写入装备选项节点。宝石的 **category（+8 bit6-15）与 level（+0x10 bits11-17）不写入节点**。而范围依赖 category：真机 `GetJewelOptionValue(type, fakeItem)` 显示 cat 28..32 的 X 分别 = 3/6/9/12/15（如 type 2：cat28→[3,6]、cat29→[6,12]、cat30→[9,18]、cat31→[12,24]、cat32→[15,30]）。

**重叠量化（真机 37 类型 × 5 类别）**：对每个属性类型枚举其值域内所有整数，统计能唯一确定 category 的比例——仅 **17%–39%**（如 type0 11/28=39%、type5 23/74=31%、type6 21/98=21%、type13 1/6=17%）。重叠区普遍存在（相邻区间共享端点 6/9/12/15）。**结论：镶嵌后无法精确得到范围，按值归类也不可靠。**

**E8 混沌 / 附魔（反汇编）**

- 混沌：`ITEMSYSTEM_ApplyChaosValue(base, rate) = base × rate / 100`；rate = `+0x10` bits8-15。产生方 `ITEMSYSTEM_MakeChaos`(0x10a5e0) 与调用链 `MIXSYSTEM_MakeItem`(0x11af58) 内**无随机调用**（用 `CAL_Calculate` + 公式/材料计算并 clamp 后写入）；`RestoreChaos`(0x10d604) 同样无随机。大修版混沌值为**确定性结果**。
- 附魔：`ITEMSYSTEM_EnchantItem`(0x10b330) 内含 **2 处 `MATH_GetRandom(1, 998)`**（0x10b584、0x10b764）：以 `CAL_Calculate` 公式算出的阈值与随机数比较，失败分支把附魔等级 −1（0x10b78c）。**附魔是随机结果**（成功率判定 + 失败降级）。

**E9 独立宝石详情（真机 `CreateItem`+`MakeJewel` 验证）**

宝石物品保留 category(+8)、type(+0x10 bits18-23)、level(bits11-17)、value(bits0-10)。`UIDesc_MakeItem` 宝石分支（0xb4208）读 type/value 后直接 `UIDesc_AddOption(buf, 1, attrType, value)`。真机 CreateItem+MakeJewel 得：cat28→value 6/range[3,6]、cat29→75/[45,91]、cat30→79/[53,106]、cat31→3/[2,5]、cat32→29/[21,42]，均与 `GetJewelOptionValue` 域一致。**结论：独立宝石可行，范围可在运行时求得。**

**E10 多版本（静态数据解码对比）**

| 版本 | libgame.so 目标函数地址 | memorytext_e | ITEMOPTINFOBASE | game.dat |
|---|---|---|---|---|
| 大修 | 0x109020（GetOptionValue）… | md5 4abd90de… | 37 条，零差异 | md5 64dd7dc9… |
| monster v23 | 与大修完全相同 | md5 4abd90de…（相同） | 与大修零差异 | md5 e5b0d27f…（多 2 条 ITEMDATABASE） |
| 原版 v1.3.2 | 全部不同（GetOptionValue 0x1b7c78、CAL 0x17fa28、AddOption 0x15076c…） | md5 4abd90de…（相同） | 与大修零差异 | md5 64dd7dc9…（与大修相同） |

三版函数均按符号名导出；公式表与选项表三版一致。**结论：公式层无需版本分支；VMA 必须走动态符号解析，公式读运行时当前版本数据即天然兼容。**

### 9.8 遗留裁决（2026-09-12）

- E7（镶嵌宝石）：**已裁决 = 不处理镶嵌宝石**，只对独立宝石详情着色（E9）。理由：类别/等级在镶嵌时丢弃，无法精确求范围，按值归类仅 17%–39% 唯一。
- E8（混沌/附魔）：**已裁决 = 都不处理**。混沌为确定性（无范围）。附魔经真机 hook `CAL_Calculate` 补充：其随机是**成功率判定**，公式为 `1500/(5×等级)`（真机捕获 `"1500a5+/"`，参数=附魔等级）再 ×2 与 `random(1,998)` 比较——等级越高成功率越低，失败降级。`I_ENCHANT` 位域：bits6-10=附魔等级（`ApplyEnchantValue` 乘数，模块 `enchant_level`）、bits11-15=附魔ID（模块 `enchant_id`）、bits2-5=失败时 −1 的独立字段（`ApplyEnchantValue` 不读，全库仅 `EnchantItem` 读写，疑为词条/阶数计数，未证实）。显示值仍由存储 ID+等级确定，无逐物品范围。

## 10. 实现与真机证据

### 10.1 实现文件与符号

| 层 | 内容 |
|---|---|
| 纯逻辑 | `module/app/src/main/cpp/feature/attribute_range/attribute_range.{h,cpp}`：`classify(value,min,max)` 百分位分档 + `color_code(Tier)`（`G/W/C/L/V/Y`）；host 单测 `tests/test_attribute_range.cpp`（33 断言，编入 `attribute_range_tests`） |
| 注入 | `module/app/src/main/cpp/feature/attribute_range/game_ui_attr_range.{h,cpp}`：`attr_range_ui_install_if_ready()`，用 `native_hook_func()` 安装 3 个 hook |
| 符号 | `game_symbols.h` 新增 `F_ITEMSYSTEM_GET_OPTION_VALUE_VMA`(0x109020)、`F_ITEMSYSTEM_GET_JEWEL_OPTION_VALUE_VMA`(0x108f90)、`F_MATH_GET_RANDOM_VMA`(0xa8bcc)、`F_UIDESC_ADD_OPTION_VMA`(0xb343c)、`F_UIDESC_MAKE_ITEM_VMA`(0xb36a0)、`G_UIDESC_TEXT_BUF_VMA`(0x303dc0)/`G_UIDESC_TEXT_BUF_SIZE`(0x200) + 对应 typedef；`symbol_registry.h` 登记 F_* 项；`game_access.{h,cpp}`/`game_access_globals.inc` 解析 `fn_item_system_get_option_value`/`fn_item_system_get_jewel_option_value`（复用已有 `fn_item_get_ability_level`、`fn_is_jewel`） |
| 接线 | `bridge/native/gamebridge.cpp` `nativeInit` 在 `bridge_init` 成功后调用 `attr_range_ui_install_if_ready()`；`CMakeLists.txt` 加入两个 cpp |

**范围获取机制**：`MATH_GetRandom` 上装一个 wrapper；probe 时置 thread_local 捕获标志，调一次游戏自身函数（词缀 `ITEMSYSTEM_GetOptionValue`、宝石 `ITEMSYSTEM_GetJewelOptionValue`），wrapper 截获其掷值区间 `[min,max]` 并返回上界（不调用原函数，不消耗游戏 RNG 状态）。这样范围完全用当前版本游戏自身算术，无 VMA/公式复制。

**颜色改写（只染数值）**：hook `UIDesc_MakeItem` 入口缓存当前物品；hook `UIDesc_AddOption`，调原函数后用缓存物品与 `(type, optIdx, value)` 求 `color_code`。该行格式为 `$<原色><标签>: <数值>$B`；游戏内联色约定是 `$<码>文本$B`（段内着色、`$B` 收束），直接插入 `$<码>` 会让标签段回落默认色（真机实测标签变白）。因此在数值前**插入 `$B$<目标色>`**（先关闭标签段、再开值段），标签保持原色、行尾 `$B` 收束。插入前用 `G_UIDESC_TEXT_BUF_VMA + 0x200` 做容量边界检查（+4 字节），不足则不改写。`type=1` 仅当当前物品自身是宝石（`ITEMSYSTEM_IsJewel(category)`）才处理，装备上已镶嵌宝石跳过（E7）。退化区间（如低等级基础属性 `[1,1]`）按有效范围处理，`classify` 判为满值金。

### 10.2 真机证据（2026-09-12，大修版，设备 `192.168.3.54`）

安装 `output/inotia4-qol-lsposed-debug-2609122247-d83a907444d9.apk`，进档后打开背包面板并选中「战神的斗篷」，触发详情构建：

```
attribute range hooks installed
color type=0 oi=5 value=20 range=[5,10] code=Y line=$T暴击率: $B$Y2.0%$B
color type=0 oi=13 value=1 range=[1,1]  code=Y line=$TMP恢复: $B$Y1$B
color type=0 oi=15 value=78 range=[30,60] code=Y line=$T暴击伤害增加率: $B$Y7.8%$B
color type=0 oi=24 value=480 range=[150,300] code=Y line=$T冰霜: $B$Y48.0%$B
```

截图核对（真机，`战神的斗篷` 详情）：标签「暴击率/暴击伤害增加率/MP恢复/命中率/HP吸收/冰霜/魔法抵抗率」均保持原生蓝色（`$T`），**只有数值**染金 `$Y`；`等级/宝石孔数量` 等非属性行不受影响；已镶嵌宝石无着色。超域固定值（该斗篷全部词缀越界）按裁决着金色。

- 早期版本直接插入 `$Y` 会导致标签段回落默认白（游戏解析器要求 `$<码>文本$B` 成段）；改为插入 `$B$Y` 后修复。
- 退化区间 `[1,1]`（低等级基础属性、MP恢复常量公式）此前被误判无效而不着色，已修正为有效范围 → 满值金。

### 10.3 残余与未验证

- 独立宝石详情（E9）代码路径已实现并复用同一 `UIDesc_AddOption`，尚无持有独立宝石的存档做真机截图；待补 VM-A1。
- 未取得「详情面板可见改色」的截图证据（真机 2 无触摸坐标预案，本节以真实 `UIDesc_MakeItem` 调用链 + 缓冲区改写日志为证）。
- `game_ui_attr_range.cpp` 保留**有界诊断日志**（每进程 16 条，`patch …`/`color …`），用于真机验收与后续排障。
- 多版本（monster/原版）仅静态核对公式与符号（§9.7 E10），未逐一真机运行；原版 VMA 不同但符号按名解析。
