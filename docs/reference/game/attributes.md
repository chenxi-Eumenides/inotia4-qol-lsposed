# 角色属性体系

> 日期：2026-11-10 ｜ 版本口径：C18L v588（`libgame.so`）与静态数据 `apk/static-data/json/`
> 用途：说明角色属性的存储位置、完整 id 清单、面板可见集合，以及不属于角色属性的全局加成（掉宝率、经验、金币）。属性取值与公式引用处的唯一权威仍是 `docs/reference/api-reference.md`；本文补充隐藏区与静态表证据。

## 1. 存储与读取入口

角色（`CHAR`）上有两套独立的属性存储：

| 种类 | 存储 | 读取入口 | 说明 |
|---|---|---|---|
| 主属性 | `ch+0x250..0x26B` | `CHAR_GetStat(ch, i)`，`i=0..4` | 四项分量：Base `0x250+i`、Main `0x256+i*2`、Bonus `0x260+i`、Sub `0x266+i*2`；总属性 = 四者之和 |
| 数值属性 | `ch + 0x24 + id*4`（int32） | `CHAR_GetAttr(ch, id)`，VMA `0xdfd18` | 扁平数组，无符号名、无边界检查 |

`CHAR_GetAttr` 反汇编为 `ldr w0, [ch + id*4 + 0x24]`，未做 id 范围校验。数组实际覆盖 **115 槽（id 0..114）**：`CHAR_ResetAttrUpdatedAll` 的清零循环以 `cmp w19,#0x73` 收尾（0x73 = 115）。其中 id113 是总攻击缓存（`C_TOTAL_ATTACK`），id114 存在但语义未知。对外契约只暴露前 32 项（`contracts/game_ports/character_state_port.h:13` 的 `kQolCharacterStateStatCount=32`），**32 号以后的面板不可见属性同样可读**，只是没有走契约。

证据：`module/app/src/main/cpp/game/symbols/game_symbols.h:19,70-87,493`；`module/app/src/main/cpp/game/ops/character_state_read.cpp:107-118`。

## 2. 主属性（5 项）

| i | 名称 | 主要贡献 |
|---|---|---|
| 0 | 力量 | 物攻（系数随主手武器类型：×0.5 / 0.6 / 0.7） |
| 1 | 敏捷 | 物攻 ×0.5、命中 ×4、总敏捷 attr13 |
| 2 | 体力 | HP 上限 ×80、防御 ×1 |
| 3 | 智力 | 法攻（×0.6 / 0.7 / 1.0） |
| 4 | 精力 | 法攻（×0.6 / 1.0）、命中 ×4、MP |

- 属性贡献只与**主手武器类型**有关，与职业和等级无关；总物攻 =（基础 + 武器）×（1 + Σ加成），每步向下取整（`game-systems.md` §6.1「精确版」）。
- 每级 3 点自由分配，等级上限 105。
- 文本 id：力量 12/13、敏捷 15/16、体力 18/19、智力 21/22、精力 24/25。

## 3. 数值属性 attr 0..31（契约前缀，面板与 API 使用）

| id | 名称/含义 | 确认度与证据 |
|---|---|---|
| 0 | 暴击率（×10，默认 30 = 3.0%） | 实测，`api-reference.md:424`；面板反汇编 |
| 1 | 暴击抵抗率 | 词缀表对照，未真机逐项确认 |
| 2 | 暴击伤害抵抗率（默认 0） | 词缀表对照，未真机确认；原写默认 8 有误（8 是 attr16 的默认值） |
| 3 | 暴击伤害（×10，默认 1000 = 100%） | 实测，`api-reference.md:425`；面板反汇编 |
| 4 | 物理攻击 | 实测，`api-reference.md:426` |
| 5 | 普攻伤害增幅（千分比） | 旧版逆向注释 |
| 6 | 增伤相关 | 旧版逆向注释，名称未确认 |
| 7 | 物理防御基础 | 旧版逆向注释；与 attr17 的关系未确认 |
| 8 | 魔法攻击 | 实测，`api-reference.md:427` |
| 9 | 魔法增伤 | 旧版逆向注释 |
| 10 | 专属增伤开关 | 旧版逆向注释 |
| 11 | 魔法抵抗（M.RES，上限 750 = 75%） | 实测，`api-reference.md:428`；面板反汇编 |
| 12 | 闪避 | 词缀「回避率」高字节 12 + 旧注释 |
| 13 | 总敏捷 | `api-reference.md:429` |
| 14 | 命中率基数 | `api-reference.md:430` |
| 15 | 命中率百分比 | `api-reference.md:431` |
| 16 | 默认 8，疑为「搜寻范围」 | 未确认 |
| 17 | 防御（DEF 数值） | 实测，`api-reference.md:432`；面板反汇编 |
| 18 | 防御增幅系数：DEF = attr17 × (attr18+1000)/1000 | 面板反汇编确认除数 1000 |
| 19 | 武器格挡/武器减伤（×10，默认 30 = 3%，格挡减伤 30%） | `api-reference.md:434`；面板反汇编；词缀表 |
| 20 | 盾牌格挡 | v588 面板把 35196 绑定到 attr20；`api-reference.md:435` 记为「副手武器攻击」，两者冲突，面板证据更直接 |
| 21 | 默认 1000，千分比类 | 未确认具体含义 |
| 22-27 | 火 / 风 / 寒气 / 神圣 / 黑暗 / 毒 元素增伤 | 词缀表 idx22-27 |
| 28 | 等级驱动属性 | `api-reference.md:436` |
| 29 | MP 恢复（默认 8） | `api-reference.md:438`；词缀表 |
| 30 | HP 上限（缓存于 `ch+0x9C`） | `game_symbols.h:76` |
| 31 | MP 上限（缓存于 `ch+0xA0`，默认 200） | `game_symbols.h:77` |

`ATTRINITBASE` 的 22 条记录是「有默认值的属性」，不是数组长度；数组长度见 §1 的 115 槽。

## 4. 隐藏属性：attr ≥ 32 扩展区

同一数组、同一 getter，面板与 `/stats` 都不暴露。按功能分组：

### 4.1 吸血与回复

| id | 含义 |
|---|---|
| 33 | 吸血百分比（千分比，伤害 × 值 / 1000） |
| 36 | 固定吸血值 |
| 60 / 61 | 高额吸血触发概率 / 高额吸血数值 |
| 39 | 召唤物回蓝（旧注释 0x27=39；27 本身是毒） |
| 63 / 112 | 命中回蓝 / 命中固定回蓝 |
| 40 | 友军受击回血 / DOT 层数 |
| 42 | 治疗增幅百分比 |
| 44 | 魔法伤害削减 / 护盾反伤 |

### 4.2 触发类

| id | 含义 |
|---|---|
| 52 | 双倍攻击触发概率（千分比） |
| 66 | 重击触发概率 |
| 72 / 98 | 命中嘲讽概率 / 嘲讽概率 |
| 69 | 命中附加持续状态层数 |
| 70 | 命中重置召唤物 CD 概率 |
| 58 | 命中生成护盾 buff |
| 76 | 反伤百分比 |
| 110 | 反击附加 debuff 概率 |
| 111 | 命中沉默概率 |

### 4.3 增减益与行为参数

| id | 含义 |
|---|---|
| 32 | 蓝耗增减百分比 |
| 83 | 蓝耗减免百分比（与词缀表「魔力专家」冲突，未确认） |
| 71 | 增伤阶段属性 |
| 106 / 109 | 额外普攻开关 / 额外普攻增幅值 |
| 75 | 行为距离加成 |
| 91 | **移动速度**（默认 8） |
| 93 | 武器攻击帧（默认 10） |
| 94 | 仇恨系数（默认 1000） |
| 95 | 千分比倍率，疑为冷却缩减，名称未确认 |
| 81 | 防御减益百分比（debuff 侧） |
| 103 | 持续伤害相关 |
| 107 | 受击获得持续增益 |

### 4.4 元素与状态抗性

| id | 含义 |
|---|---|
| 22-27 | 火、风、寒气、神圣、黑暗、毒 |
| 78-84 | 重力摆、冰霜、治愈气息、狂战士、瞬间恢复、魔力专家、减少敌意值 |
| 85 | 失明抗性 |
| 86 | 睡眠抗性 |
| 87 | 眩晕抗性 |
| 88 | 恐惧抗性 |
| 89 | 沉默抗性 |
| 90 | 减速抗性 |

元素与抗性条目来自装备词缀表 `ITEMOPTINFOBASE`，每条抗性按能力等级每级 +1。

### 4.5 掉落与产出

| id | 含义 |
|---|---|
| 92 | 掉宝率（千分比）；全队 3 名队员求和后乘入掉落阈值 `w28` |

`PARTY_GetItemChance` 最多累加 3 名出战队员的 attr92；该值不直接改掉落表，而是线性放大 `CHARSYSTEM_DropItem` 的阈值（见 §7.1）。

### 4.6 无确认名称的 attr

以下 43 个 id 没有确认名称，面板统一显示 `attr<id>`，不得按旧注释或猜测命名：

`6, 16, 21, 34, 35, 37, 38, 39, 41, 43, 45, 46, 47, 48, 49, 50, 51, 53, 54, 55, 56, 57, 59, 62, 64, 65, 67, 68, 73, 74, 77, 95, 96, 97, 99, 100, 101, 102, 103, 104, 105, 108, 114`

其中 23 项（34、35、37、38、39、41、43、45、46、47、48、49、50、51、53、54、55、56、57、59、62、64、65）仅有旧版注释，其余 20 项为完全未知；旧注释不足以作为命名依据。

## 5. 面板可见集合（v588 反汇编）

面板入口 `F_SCENE_DRAW_POPUP_SC_CHARACTER_INFO`（`0x14977c`），属性行绘制子函数 `CharacterInfo_InfoDraw @ 0x1494fc`。

标签索引数组（`.rodata 0x24c2c0`）：`41,42,45,46,47,43,48,49,50,51,52`——直接跳过 44（即 text id `35188`）。

| 行 | text id | 标签 | 取值函数/字段 |
|---|---|---|---|
| 0 | 35185 | 物攻 DMG | `CHAR_GetNormalDamage` |
| 1 | 35186 | 法攻 M.DMG | `CHAR_GetNormalMagicDamage` |
| 2 | 35189 | 暴击率 CRT | `CHAR_GetAttr(ch, 0)` |
| 3 | 35190 | 命中率 H.RATE | `CHAR_GetHitRate1000` |
| 4 | 35191 | 爆伤 C.DMG | `CHAR_GetAttr(ch, 3)` |
| 5 | 35187 | 防御力 DEF | `attr17*(attr18+1000)/1000` |
| 6 | 35192 | 物抗 P.RES | `CHAR_GetDisplayDamageReduceRate` |
| 7 | 35193 | 法抗 M.RES | `CHAR_GetAttr(ch, 11)` |
| 8 | 35194 | 闪避率 EVD | `CHAR_GetNormalEvasionRate1000` |
| 9 | 35195 | 武器格挡 W.D.R | `CHAR_GetAttr(ch, 19)` |
| 10 | 35196 | 盾牌格挡 S.D.R | `CHAR_GetAttr(ch, 20)` |

面板另绘制 5 条主属性（`CHAR_GetStat` 0..4）与 HP/MP/EXP 条。`35188`（M.DEF）有文案但不在标签数组中，面板不显示；其 attr id 未证实（候选 10/16）。

自 `QolCharacterReadApi` v2.13 起，物抗/命中率/闪避率不再需要调用方自行反汇编取 getter，可直接经宿主只读面获取（均为**千分比**，含装备与 buff 修正，仅游戏主线程）：

| 面板行 | 宿主接口 | 说明 |
|---|---|---|
| 命中率 | `QolCharacterReadApi::hit_rate_1000(ch, &v)` | `CHAR_GetHitRate1000`，按自身等级计算，忽略目标参数 |
| 闪避率 | `QolCharacterReadApi::evasion_rate_1000(ch, &v)` | `CHAR_GetNormalEvasionRate1000`，内部把临时值夹到 980 |
| 物抗 | `QolCharacterReadApi::display_damage_reduce_rate(ch, &v)` | `CHAR_GetDisplayDamageReduceRate`，内部使用 CAL 全局计算器栈，只能主线程串行调用 |

**右侧战斗属性框内部文本区**的屏幕绝对矩形由 `QolUiAdapter::character_info_content_rect`（Adapter minor 36）给出：`{inset_x+741, inset_y+153, 205×411}`，已含屏幕 inset。几何链：`M = B + (0,133)`、`A = M + (496,0)`、右框外框 `A + (226,0,224,431)`、内部文本起点 `A + (245,20)`。该矩形避开左侧 5 维属性框、队伍页签、关闭 X、属性分配入口与 +/−/重置；消费方不得自建布局常量。

佣兵团技能（全队生效、不占角色属性数组）是否已学由 `MERCENARYGROUPSKILLSYSTEM_CheckCondition(actId, NULL)` 查询（VMA `0x118918`，actId 119..142，返回 1=已学/0=未学）。该函数是叶函数、只读、无副作用；v2.14 起经 `QolCharacterReadApi::group_skill_learned(act_id, &v)` 暴露，越界返回 InvalidArgument。

## 6. 装备词缀表 `ITEMOPTINFOBASE`

`apk/static-data/json/tables/ITEMOPTINFOBASE.json` 是唯一显式有序命名的属性表，37 条；记录 `+2` 的 u16 编码为「高字节 attr id + 低字节操作类型」，数值公式取 `apk/static-data/json/text/formula-e.json` 的 `+4` 索引（139-174）。名称文本为 `zh-Hans.json` 的 text id 1114-1150。

词缀顺序：力量、敏捷、体力、智力、精力、暴击率、命中率、暴击伤害抵抗率、魔法抵抗率、回避率、盾牌格挡率、武器格挡率、MP 增加、MP 恢复、暴击抵抗率、暴击伤害增加率、HP 吸收，以及元素、抗性与占位项。

注意：词缀**编号不是角色 attr id**，只能用编码高字节对照（已知 id 全部吻合），不可直接把词缀序号当 attr 索引。

## 7. 不属于角色属性的全局加成

以下概念在角色属性数组中不存在，原版通过函数入口或数据表参数生效：

| 概念 | 实际机制 |
|---|---|
| 掉宝率 | `CHARSYSTEM_DropItem` 逐条读 `DROPINFOBASE` 概率 × 全队倍率，再叠加佣兵团技能 |
| 经验加成 | 入口 `CHAR_AddExperience`；原版来源为佣兵团技能与消耗品 buff |
| 金币获得 | 金币由怪物表 `MONSYSTEM_GetMoney` 决定，另有 `INVEN_AddMoney`；无「金币获得率」属性 |
| 幸运 | 不是属性，是 `ITEMSYSTEM_MakeItem(category, key, luck)` 的第 3 个参数（品质门槛） |

原版中这些加成的唯一持有者是**佣兵团技能**（全队生效、不占角色属性数组）：卷轴掉率 +3%、材料掉率 +2%、装备掉落 +1%、开箱好道具概率提升、稀有佣兵概率提升、经验 +2%、出售价 +3%、购买价 −2% 等。模块侧的倍率通过 hook `CHARSYSTEM_DropItem`、`CHAR_AddExperience`、`INVEN_AddMoney` 实现，见 `docs/reference/plugins/world_tuning.md`。

### 7.1 佣兵团掉落加成的数值语义

`CHARSYSTEM_DropItem` 的掉落判定是阈值比较，而不是把面板文案的百分比直接加到概率上。

**基础概率 `base`**

- `base = (raw × 2 × level) / (playerLevel + level)`，其中 `raw` 是 `DROPINFOBASE` 槽位的原始概率，`level` 是怪物等级、`playerLevel` 是玩家等级。
- 公式来自 `EXPRESSBASE` 第 13 条记录 → text id 13 → `formula-e[13] = "a2*b*cb+/"`（`CAL_Calculate` 的 RPN 语义：`a2` 为第 2 个入参、`b` 为第 1 个入参、`cb` 为常量表项，`+/` 为除法）。
- 同级时 `base == raw`（`2×level/(level+level) = 1`）。

**全队掉宝倍率与判定**

- `w28 = (1000 + Σ attr92) × base / 1000`；`Σ attr92` 由 `PARTY_GetItemChance` 最多累加 3 名出战队员。
- `roll = MATH_GetRandom(1, 9999)`，`roll < w28` 时该槽位掉一件。
- 技能加值在乘法之后叠加：`130`（卷轴）与 `138`（材料）为 `+20`，`142`（装备）为 `+10`。
- 原版骰子 1..9999：约 `+0.2pp` / `+0.2pp` / `+0.1pp`；monster/overhaul 骰子上限为 2999/5999，约 `+0.67pp` / `+0.67pp` / `+0.17pp`。
- 加值只控制该槽位**本次是否掉落**，不影响掉落数量与物品类别；每次击杀跑 1 或 3 轮，多个技能之间不叠加。
- 技能文案写 2%/3%/1%，与实现的实际增量不自洽；面板按实际增量（`+0.2pp` / `+0.2pp` / `+0.1pp`）显示，不采用文案百分比。

**等级来源**

- 普通怪：`CHARSYSTEM_GetMonsterLevel(id, MAP_nID)`；地图 12 使用玩家等级。
- 宝箱/NPC：`MAPINFOBASE[+6]`。

**特殊槽位**

- 槽 8 `ITEMSYSTEM_MakeMonster(玩家等级)`：随机等级 `rand(player, player+3)` 匹配 `MONDATABASE` 的普通类别，再 `CHARSYSTEM_Produce(1, id)` 生成真实怪物；生成后等级改写为地图 `+6`、坐标取死亡点、`situation=4`。原版 `t7=0` 不触发；`t7>0` 时存在链式生成风险。
- 槽 9 `CHARSYSTEM_Produce(1, +0x1b 固定 id)`：直接按记录尾部的 `produce_monster_id` 生成。

**`DROPINFOBASE` 记录（29 字节）**

| 偏移 | 字段 | 含义 |
|---|---|---|
| `0x00..0x11` | `t0..t8` | 9 个掉落槽的原始概率/阈值；`t8` 关联槽 8 的怪物生成 |
| `0x12..0x17` | `f0..f2` | 3 个槽位标志/修正（按分支参与判定） |
| `0x18` | `detail_start` | 掉落明细表起始索引（u16） |
| `0x1a` | `detail_count` | 明细条数（u8） |
| `0x1b` | `produce_monster_id` | 槽 9 生成怪物 id（u16，`0xffff` = 无） |

#### 掉落率控制点

| 方式 | 说明 |
|---|---|
| `attr92`（推荐） | 全队求和后线性放大 `w28`，不改表、不重抽，副作用最小 |
| hook `CHARSYSTEM_DropItem` 重跑 | 用于 `drop_quantity`（改数量/追加掉落），需自行处理去重与轮次 |
| hook `ITEMSYSTEM_MakeItem` / `MakeEquipDetail` 重抽 | 用于 `drop_reroll`（品质/词条重掷），消耗游戏 RNG |
| `DROPINFOBASE` `t2/t4/t5` | 定向修改具体槽位概率，影响面大且需重载表 |
| 不建议改 `MATH_GetRandom` | 全游戏共用，截获会污染战斗/合成等所有随机 |

证据：`CHARSYSTEM_DropItem` 的三条技能分支地址 `0xf50c4`（130）/`0xf510c`（138）/`0xf517c`（142），随机数入口 `MATH_GetRandom`（`0xa8bcc`），技能是否已学查询 `MERCENARYGROUPSKILLSYSTEM_CheckCondition`（`0x118918`），全队掉宝 `PARTY_GetItemChance`（`0x1200b8`），怪物生成 `ITEMSYSTEM_MakeMonster`（`0x10859c`）/`CHARSYSTEM_Produce`（`0xf3880`，c18-v520/monster/overhaul 同址）。

## 8. 未确认项

1. attr 1/2/5/6/7/9/10/16/21/29 的名称来自旧版逆向注释或词缀表对照，未在 v588 真机逐项验证。
2. attr 95 只确认是千分比倍率，语义（冷却缩减？）未确认。
3. attr 20 的「盾牌格挡」与 `api-reference.md:435` 的「副手武器攻击」冲突，需 `CHAR_GetAttr(20)` 真机复核。
4. `35188` M.DEF 的 attr id 未证实。
5. 抗性穿透在伤害公式内部按倍率连乘，未发现独立 attr id。

## 9. 证据索引

| 内容 | 位置 |
|---|---|
| 属性数组基址、getter、主属性偏移 | `module/app/src/main/cpp/game/symbols/game_symbols.h:19,70-87,493` |
| 属性数组 115 槽（id 0..114） | `CHAR_ResetAttrUpdatedAll`（`0xd9f0c`）清零循环 `cmp w19,#0x73`；id113=总攻击缓存，id114 语义未知 |
| 默认值记录口径 | `apk/static-data/json/tables/ATTRINITBASE.json` 22 条 = 有默认值的属性，不是数组长度 |
| 面板选中角色 | `PARTY_GetMenuCharacter`（`0x120338`）、`PARTY_nMenuIndex`（`0x728ed8`） |
| 32 项属性读取与契约 | `module/app/src/main/cpp/game/ops/character_state_read.cpp:107-118`、`contracts/game_ports/character_state_port.h:13` |
| id → 名称实测映射 | `docs/reference/api-reference.md:415-445` |
| 面板标签与 35188 占位 | `docs/reference/plugins/character_panel.md`、`plugins/character_panel/feature.h:30-39` |
| 命中/闪避/物抗 getter VMA | `module/app/src/main/cpp/game/symbols/game_symbols.h:501-503`（`F_CHAR_GET_HIT_RATE_1000_VMA`、`F_CHAR_GET_NORMAL_EVASION_RATE_1000_VMA`、`F_CHAR_GET_DISPLAY_DAMAGE_REDUCE_RATE_VMA`）；按名解析登记 `game/symbols/symbol_registry.h:72-74` |
| 面板派生只读面（v2.13 起） | `contracts/game_host.h:67-86`（`QolCharacterReadApi::main_stat`/`normal_damage`/`normal_magic_damage`/`hit_rate_1000`/`evasion_rate_1000`/`display_damage_reduce_rate`）、`contracts/game_host.h:317-318`（`kGameHostCharacterReadMinor=14`）、`game/adapter_ports/game_host_port.cpp:996-1112` |
| 佣兵团技能已学查询（v2.14） | `game/symbols/game_symbols.h:914-916`（`F_MERCENARYGROUPSKILL_CHECK_CONDITION_VMA=0x118918`）、`game/symbols/symbol_registry.h:430`（→ `MERCENARYGROUPSKILLSYSTEM_CheckCondition`）、`contracts/game_host.h:83-85`（`group_skill_learned`）、`game/adapter_ports/game_host_port.cpp:1063-1078` |
| 佣兵团掉落加值（130/138/142） | `CHARSYSTEM_DropItem` 分支 `0xf50c4`/`0xf510c`/`0xf517c`（`game/symbols/game_symbols.h:815` 为函数入口 `0xf4d30`）；随机数 `MATH_GetRandom`（`game/symbols/game_symbols.h:661`、`game/symbols/symbol_registry.h:130`） |
| 全队掉宝倍率 | `PARTY_GetItemChance`（`0x1200b8`，c18-v520/monster/overhaul 同址，最多累加 3 名队员 attr92） |
| 掉落基础概率公式 | `EXPRESSBASE` 第 13 条 → text id 13 → `formula-e[13] = "a2*b*cb+/"`；RPN 求值 `CAL_Calculate`（`CAL_Calculate` 的 `a2`/`b`/`cb` 语义见 §7.1） |
| 道具生成怪物 | `ITEMSYSTEM_MakeMonster`（`0x10859c`）、`CHARSYSTEM_Produce`（`0xf3880`）；`DROPINFOBASE` 29 字节记录字段见 §7.1 |
| 右侧战斗属性框内部文本区矩形 | `contracts/ui.h:831-833`（`character_info_content_rect`）、`contracts/ui.h:17`（`kUiAdapterMinor=36`）、`host/ui_adapter.cpp:613-628`（`inset+(741,153)`、`205×411`） |
| 属性贡献与格挡结论 | `docs/reference/game/game-systems.md` §6.1 |
| 词缀表与公式 | `apk/static-data/json/tables/ITEMOPTINFOBASE.json`、`apk/static-data/json/text/formula-e.json` |
| 默认属性值 | `apk/static-data/json/tables/ATTRINITBASE.json`（`[attr_id, class_mask, default_u16]`） |
