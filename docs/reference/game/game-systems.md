# 游戏系统与数据结构总览（Inotia 4 盗版大修 20260704）

> 日期：2026-08-05 ｜ 来源：`libgame-symbols.txt`（readelf 导出符号，6626 函数 + 全局变量）+ `apk/decompiled/`（jadx）
> 用途：确定 API 能提供什么数据、M3 需要解析哪些静态表、M4 需要 hook/读取哪些运行时数据。

## 1. 游戏类型

暗黑破坏神风格 ARPG（点击移动 + 即时战斗 + 技能释放）。玩家操控 1 名主角色 + 2 名佣兵（共 3 人出战），地图上遇敌即时战斗，掉落装备/材料，任务驱动推进。

## 2. 玩法系统清单（模块证据）

| # | 系统 | 核心模块/符号 | 说明 |
|---|---|---|---|
| 1 | **角色养成** | CHAR(354)、CHARSYSTEM、CHARCLASSBASE、ATTRINITBASE、MAXLEVELBASE | 职业、等级、经验、属性（力量/敏捷等）、技能点、属性点分配 |
| 2 | **队伍/佣兵** | PARTY(36)、MERCENARYSYSTEM(17)、MERCENARYINFOBASE、MERCENARYSLOT | **3 人出战**（`PARTY_pChar` 3 指针）；佣兵招募/入队/离队（`MERCENARYSYSTEM_pSlotList`） |
| 3 | **战斗** | CHAR_Get*Damage、HATESYSTEM、STATUSDICE、CHECKTIME_*、MONSTERAI、**CHARLOCSYSTEM** | 物理/魔法/暴击/命中/闪避计算、仇恨、状态判定、怪物 AI；**所有单位（玩家+敌人+NPC）位置登记于 CHARLOCSYSTEM，可枚举坐标** |
| 4 | **状态/Buff** | CHARACTERSTATEBASE、BUFFDATABASE、BUFFSYSTEM、CONDITIONBASE | 中毒/眩晕/灼烧等状态与增益效果 |
| 5 | **装备** | ITEMENCHANTBASE、ITEMGRADEBASE、ITEMRARITYGRADEBASE、UIEquip、CHAR_FindEquipSlot | 多槽位装备、附魔/强化、**稀有度 5 档：白/绿/蓝/黄/紫（黄 = 特殊装备）**；颜色映射 `UIDesc_GetColorAsItemGradeByID`、取值 `ITEMSYSTEM_GetRarity` |
| 6 | **背包/物品** | INVEN(33)、ITEMSYSTEM(75)、ITEMDATABASE、ITEMCLASSBASE | 槽位背包、物品堆叠、物品分类（装备/消耗/材料/任务） |
| 7 | **货币** | MONEY_GetGold/GetSilver/GetCooper、INVEN_nMoney | **金/银/铜三币制** |
| 8 | **商店/交易** | UIStore、DEALSYSTEM、DEALINFOBASE、RarityShop | NPC 商店买卖、稀有度商店（金币抽奖） |
| 9 | **内购（已阉割）** | UIInApp(44)、CASHITEMBASE、CHARGEDITEMBASE、InApp* | 现金道具/宝石商店（盗版版功能已断，数据表仍在） |
| 10 | **合成/炼金** | MIXSYSTEM、MIXTUREBASE、RECIPEBASE、ITEMMIXLINKBASE | 配方合成、材料混合 |
| 11 | **任务** | QUESTSYSTEM(35)、QUESTINFOBASE、QUEST*BASE、UINpcQuest | 主线/支线 NPC 任务、任务链/前置/奖励/掉落 |
| 12 | **地图/场景** | MAPSYSTEM、MAPINFOBASE、PORTALINFOBASE、WORLDMAPBUILDER、WEATHERSYSTEM、**MAP_nBaseTile / MAP_nBaseInfo / MapBlockingcheck / ASTAR** | 区域地图、世界地图、传送门、天气；**当前地图瓦片矩阵 + 阻挡检测 + 引擎自带 A* 寻路** |
| 13 | **掉落/拾取** | MONSTERDROPBASE、DROPINFOBASE、DROPDETAILINFOBASE、MAPITEMDROP、OPENITEMBOXBASE | 怪物掉落表、地图物品、开箱 |
| 14 | **事件/剧情脚本** | EVTSYSTEM、EVTCMDBASE、ACTDATABASE、ANIMATION*、Scene(189) | 剧情事件驱动（对话/演出/条件分支） |
| 15 | **存档** | SAVE(68)、SAVESLOT、GAMELOADER、HubSave | 本地存档槽 + Hive 云存档（备份/恢复） |
| 16 | **教程** | Tutorial*(16) | 新手引导流程 |
| 17 | **奖励/每日** | DailyReward | 每日登录奖励 |
| 18 | **社交/平台** | Hub*、onHub*、NET(35)、C2S(31) | Hive 账号、云存档、分享（盗版版已断开） |
| 19 | **辅助** | SOUNDSYSTEM、TEXTINPUTSYSTEM、TEXTDATABASE、TIPBASE、HELPTEXTBASE | 音效、输入、文本/提示/帮助 |

## 3. 静态数据表（100 张，= game_res 需解析的全部内容）

来源：`*BASE_nRecordCount / *BASE_pData` 全局对象对。M3 解析优先级：

### 高优先级（直接对应 API 静态数据）

| 表 | 内容 |
|---|---|
| `CHARCLASSBASE` | 职业定义 |
| `ATTRINITBASE` | 属性初始值/成长 |
| `MAXLEVELBASE` | 等级上限 |
| `ITEMDATABASE` / `ITEMCLASSBASE` / `ITEMSTATICBASE` / `ITEMDESCBASE` | 物品主表/分类/静态属性/描述 |
| `ITEMENCHANTBASE` / `ITEMGRADEBASE` / `ITEMRARITYGRADEBASE` | 附魔/品级/稀有度 |
| `SKILLDESCBASE` / `SKILLTRAINBASE` / `SKILLTRAINPOINTBASE` | 技能描述/训练/技能点消耗 |
| `MERCENARYINFOBASE` / `MERCENARYSKILLBASE` / `MERCENARYGROUPSKILLBASE` | 佣兵信息/技能/群体技能 |
| `MAPINFOBASE` / `MAPFEATUREINFOBASE` / `PORTALINFOBASE` | 地图信息/特征/传送门 |
| `MONDATABASE` / `MONAIINFOBASE` / `MONSKILLBASE` / `MONSTERDROPBASE` | 怪物/怪物 AI/技能/掉落 |
| `QUESTINFOBASE` / `QUESTGROUPBASE` / `QUESTREWARDBASE` | 任务/分组/奖励 |
| `NPCINFOBASE` / `NPCDESCBASE` | NPC 信息/描述 |

### 中优先级

| 表 | 内容 |
|---|---|
| `MIXTUREBASE` / `RECIPEBASE` / `ITEMMIXLINKBASE` | 合成配方 |
| `DROPINFOBASE` / `DROPDETAILINFOBASE` / `OPENITEMBOXBASE` | 掉落明细/开箱 |
| `ITEMBUFFBASE` / `ITEMOPTINFOBASE` / `ITEMRECOVERBASE` / `ITEMPACKBASE` | 物品 Buff/词缀/回复/打包 |
| `STATUSBASE` / `STATUSINFOBASE` / `STATUSASSIGNBASE` / `STATUSDICEBASE` | 状态/状态信息/分配/判定 |
| `CHARACTERSTATEBASE` / `CHARACTERSTATECHANGEBASE` / `CONDITIONBASE` | 角色状态/变化/条件 |
| `BUFFDATABASE` / `BUFFUNITBASE` | Buff 定义 |
| `DEALINFOBASE` / `CASHITEMBASE` / `CHARGEDITEMBASE` / `CHARGEDITEMPRODUCTBASE` | 交易/内购商品 |
| `QUESTPREPAREBASE` / `QUESTCOMPLETEBASE` / `QUESTLINKBASE` / `QUESTDROPBASE` / `QUESTGENBASE` / `QUESTOBJECTCHANGEBASE` | 任务链/掉落/生成 |

### 低优先级（渲染/演出/辅助）

`ACTDATABASE`、`ACTUNITBASE`、`ACTTRANSMIT*`、`ANIMATION*`、`EFFECTINFOBASE`、`EVENTDATABASE`、`EVTCMD/EVTCOND/EVTINFO`、`CONSTBASE`、`CHOICEBASE`、`COLORRATE*`、`EXPRESSBASE`、`HELPTEXTBASE`、`IMAGEFILEBASE`、`INSTALLBASE`、`MANAGEMBASE`、`MAPCOLORBASE`、`NPCABILITY/NPCFUNC*`、`PORTRAIT*`、`SOUND*`、`SYMBOLBASE`、`TEXTDATABASE`、`TIPBASE`、`CHARACTERCOSTUME*`、`MONSTERCOSTUMEBASE`、`NPCCOSTUMEBASE`、`CHEATCHAR*`、`QUESTGENBASE`

## 4. 动态运行时数据（模块可读取）

| 数据 | 来源 | 说明 |
|---|---|---|
| 金币（金/银/铜） | `INVEN_nMoney` + `MONEY_GetGold/GetSilver/GetCooper` | 三币制 |
| 角色经验/技能点/属性点 | `CHAR_GetExperience` / `CHAR_GetSkillPoint` / `CHAR_GetStatusPoint` | |
| 角色属性 | `CHAR_GetStat` / `CHAR_GetAttr` | 力量/敏捷等 |
| 角色 HP/MP | 角色结构体 +0x1F0/+0x1F4（上限 = CHAR_GetAttr 0x1e/0x1f） | 已逆向，见 data-sources.md |
| 装备 | `CHAR_GetEquipItem` / `CHAR_FindEquipSlot` | |
| 技能列表 | 角色 +0x2A0 技能链表（`CHARSYSTEM_GetSkillList` 是 stub，勿用） |
| 队伍（3 人） | `PARTY_pChar` / `PARTY_GetMember(i)` | |
| 佣兵槽位 | `MERCENARYSYSTEM_pSlotList` / `SAVE_nPartyMercenarySlot` | |
| 背包 | `INVEN_pItem` / `INVEN_pBagSlot` / `INVEN_MakeItemList` | |
| 当前地图/坐标 | **`*(*(base+0x2f4000+0xe80))`**（地图ID=MAPINFOBASE 记录下标，实时，v0.4.28 修正）/ 角色结构体 **+0x02/+0x04**（坐标，实时） | ✅ 2026-08-05 真机实测 |
| 当前任务 | `QUESTSYSTEM_nActiveQuest` | |
| 传送目标 | `ACTTRANSSYSTEM_nCoordX/Y`、`nTargetIndex` | |
| 敌人坐标 | `CHARLOCSYSTEM_pPool` + `CHARLOCSYSTEM_Find` | 所有单位位置（含敌人），字段偏移待逆向 |
| 地图瓦片/阻挡 | `MAP_nBaseTile`（8192B）+ `MapBlockingcheck` | 当前地图通行矩阵 |
| 存档槽 | `SAVE_pSaveSlot`（87 字节） | 离线兜底 |
| 各类计时 | `CHECKTIME_*`（Buff/药水/单位/攻城道具 tick） | |

## 4.5 坐标与地图能力（外部程序可用）

### 敌人坐标：可以获得 ✅

- **CHARLOCSYSTEM**（角色位置系统：`CHARLOCSYSTEM_pPool` + `CHARLOCSYSTEM_nCount`）：游戏中所有单位的**位置登记池**（玩家 + 敌人 + NPC），`CHARLOCSYSTEM_Find` 按索引查找、`CHARLOCSYSTEM_Add` 登记
- 位置字段（x/y）在单位结构体，偏移待逆向（反汇编 `CHARLOCSYSTEM_*` + frida 运行时探测）
- 辅助函数：`CHAR_GetDistance`（角色间距）、`CheckCharLocation`、`MATH_GetDistance`
- 敌人单位枚举：`CHARSYSTEM_pPool`（角色对象池，含怪物）+ `MONSTERAI_RunAIProc`（AI 驱动单位）
- 坐标系：与 `MAP_nFocusX/Y` 相同的地图瓦片坐标系

### 地图数据：可以提供 ✅（可用于计算移动）

| 数据 | 来源 | 用途 |
|---|---|---|
| 瓦片矩阵 | `MAP_nBaseTile`（8192 字节，当前地图） | 地图布局（瓦片编码格式待逆向） |
| 地图基础信息 | `MAP_nBaseInfo`（4096 字节） | ⚠️ **实测为瓦片矩阵起点**（64×64，每字节 1 tile），**非地图 ID**（旧误读，v0.4.28 修正）；地图 ID 用 `*(*(base+0x2f4000+0xe80))` = MAPINFOBASE 记录下标 |
| 阻挡检测 | `MapBlockingcheck`（函数 0xf2860） | 判断坐标是否可通行 |
| 最近可达点 | `MAP_GetNearWaitCoord` / `MAP_GetNearestWaitCoord` | 找最近可站立坐标 |
| 寻路 | `ASTAR_GeneratePath` / `ASTAR_Step`（引擎自带 A*，13 函数） | 外部复算路径 |
| 特征区域 | `MAPFEATURE_GetAreaRect` / `MAPFEATURE_ApplyFIlter` | 特殊区域（传送点/事件区/碰撞） |

两种提供方式：
1. **导出通行矩阵**：模块读 `MAP_nBaseTile` + `MapBlockingcheck` 语义 → API 提供地图数据，外部程序自行寻路
2. **调用引擎寻路**：模块调用 `ASTAR_GeneratePath` → API 提供"路径计算"端点（需游戏运行时）

> 注意：`MAP_nBaseTile` 瓦片编码、`MAP_nDisplayBX/BTX`（显示起点）语义需逆向确认；地图随 `SAVE_nMapID` 切换而更新。

## 5. 结论：API 能提供什么（初步）

1. **玩家状态**：金币（三币）、等级/经验、HP/MP、属性、技能点、当前地图、坐标、任务
2. **队伍**：3 名角色完整状态（装备槽 + 稀有度 + 技能列表 + 属性）
3. **背包**：物品列表（静态表联查名称/属性/稀有度）
4. **静态全量**：100 张表可按需导出（优先：职业/物品/技能/佣兵/地图/怪物/任务/NPC）
5. **单位坐标**：玩家 + 敌人 + NPC 实时坐标（CHARLOCSYSTEM）
6. **地图数据**：当前地图通行矩阵 + 阻挡检测 + 可选寻路（供外部计算移动）
7. **事件**（可选）：hook `CHAR_AddExperience`/`INVEN_AddMoney`/`PARTY_AddHPMP`/`INVEN_MoveItem` 做变化推送

## 6. 机制细节（2026-08-07 网络资料补充，P0#4 游戏系统深入探索）

> 来源：网络攻略/wiki（见文末资料源）；逆向验证方向见 `../../development/planning/backlog.md` P2「游戏机制的深入探索」。

### 6.1 伤害与属性

| 机制 | 细节 |
|---|---|
| 属性贡献 | 力量=0.66 物攻；敏捷=0.5 物攻+0.1闪避+0.5命中；体力=80血+1防；智力=0.66 法攻；精力≈0.9-1.0 法攻+0.5命中（多站同源） |
| 精确版 | 属性贡献只与主手武器类型有关（与职业/等级无关）；总物攻=(基础+武器)×(1+Σ加成) 每步向下取整 |
| 普攻类型 | 狂战/忍者=物理；黑魔导=法攻；黑骑/祭祀/猎人=(物攻+法攻)×0.6；力敏+智精同现=混乱攻击 |
| 技能公式 | 如黑魔导【魔攻+智力×1.2+精力】×Z%（Z 随等级增）；忍者暗杀剑【物攻+1.2敏+力】×比率 |
| 暴击率 | 双手武器3%、弓8%、短剑8%、水晶10%、盾牌格挡12%；双持只算主手 |
| 格挡 | 武器格挡不挡魔法（盾牌可），武器格挡减伤30%；带武器3%格挡率、带盾12% |
| 暴击抵抗 | 暴击抵抗 > 怪物暴击率时不受暴击 |

### 6.2 成长与经验

- 每级 **3 属性点**自由分配；等级上限 **105**（EXP 表 105 级；等级存储字段 `[ch+0xe]` 为 s8，实测 set-level 200 溢出为 -56，安全上限 127）
- 怪物经验：比怪低1级 +10%（最高150%）；高1级 −10%（最低20%）
- 队伍惩罚：每带一个佣兵，怪物 ATK+20%/HP+20%；首领怪：等级+3、ATK×1.2、HP×3.6
- 完整 1-105 经验表见 wikiwiki（逆向可对比验证）

### 6.3 装备 / 强化 / 混沌

- 稀有度 6 级（白→红）：common 0属性 / uncommon 1 / rare 2 / unique 3 / epic 4（Boss）/ 混沌 / 深渊混沌
- 宝石孔：白4洞、绿3、蓝2、黄0、紫0
- 强化消耗耐久度（固定不可恢复）；**强化增量由卷轴决定**（顶级+4 物攻）；理论最大 15 次；特级卷轴 30% 概率+2
- 混沌卷轴：不耗耐久、失败归0；混沌合成主属性 ±50% 浮动、深渊 ±100%；攻击改攻击/护甲改护甲
- 宝石 MAX：CRT 6.1、暴击抵抗 3.0、C.DMG 11.1、主属性 17、MP增加 20

### 6.4 合成 / 任务 / 掉落

- 融合器 4 功能：药水/宝石合成、打孔、混沌合成、Unique 合成（Class D/C/B/A/S 五级配方）
- 任务：主线+支线（NPC 支线）；悬赏猎人 [!] 击杀红星级怪；融合器任务给配方；二周目记忆系列
- 掉落：紫装 200/1000、混沌之鳞 400/1000、脉动结晶 500/1000；玛娜宝石攻击掉落回 MP
- 影响掉率佣兵技能：寻宝猎人（开箱）、战利品收集家（装备+1%）、书籍收藏夹（卷轴+3%）等

### 6.5 职业 / 佣兵 / 其他

- **6 职业**：黑骑士/忍者/黑魔导/祭司/暗影猎手/狂战士；无转职；每职业 15 技能（全游戏 90+）
- 佣兵：人名佣兵自带 2 技能（特殊缺 3）；**佣兵技能不出战也对全队生效，同种不叠加**
- 元素属性：风（眩晕）/火（范围）/冰（冻结减速）/神圣（物攻归0）/暗黑（无视防御）/毒（累积）
- 背包：基础包+4 额外包；金币上限 9999金99银99铜（超限变负）
- 无限地下城：5-6 层，每层 3 图，45 只怪+30 记忆碎片开 Boss

### 6.6 逆向优先方向（交叉验证点）

伤害公式（×1.2/0.66/0.5 常量、逐项取整）> 经验表/成长曲线 > 强化/混沌（±50%/±100% 常量、耐久）> 宝石上限（6.1/11.1/17）> 掉落权重表（千分比分母 1000）

### 6.7 资料源

- Fandom Wiki：https://inotia4.fandom.com/
- wikiwiki（日文，数据最硬核）：https://wikiwiki.jp/inotia4/
- TapTap 实测帖：https://www.taptap.cn/moment/15205112674781805
- 我爱秘籍（属性计算）：https://m.52miji.com/v/f81
- DVG 掉落表：https://www.dvg.cn/yxxd/31841.html

### 6.8 盗版大修版修改点（P0-b，2026-08-08 改版说明图片提取）

> 来源：`apk/game-update-log/说明20260704.jpg` + `apk/game-update-log/附件20260704.jpg`（改版作者修改介绍）。**本版为盗版大修 20260704，以下修改相对原版 v1.3.2**，逆向验证机制时须对照（数值/机制与 wiki 原版资料可能不一致）。

**修改类**（影响数据结构的机制变更）：
1. 经验获取 **-30%**（经验增长逻辑与 wiki 原版不同）
2. 全部职业武器可装备（含副手；副手攻击默认取物防物理伤害，技能伤害按技能描述取物/魔/法攻）
3. 混沌合成无视材料提升强化卷轴阶数，**上限 12 阶**，仅限未强化武器防具；未操作成功强制存档
4. 强化卷轴失败不扣除强化次数（仍消耗卷轴）；混沌/深渊混沌合成一次满投且 **100% 成功**
5. 合成器可重复开孔（随机 1-4 孔）；掉落分品质随机开孔，上限 4 孔
6. 移除强制存档（混沌/宝石合成/强化套装/投掷陨子/生成佣兵等不再强制存档）
7. 移除首次进入强制网络验证
8. 重置属性/技能点/开启佣兵槽免费（断网重置：属性返还升级点但不返还陨子点，卓越级技能点消除）

**新增/调整类**：
9. **附魔系统新增**（原版无）：装备镶嵌满 4 宝石触发神秘力量附魔，按同类型属性派生新属性；25% 概率完美附魔加抗性（值 1=100%）；附魔属性显示颜色修改
10. **陨子（属性骰子）投掷**：初始属性最大，升级可随机增加点数（每 2 级上限 +1）；所有职业基础属性 1-127 点；佣兵可用陨子修正属性（仅基础属性）
11. 灵魂治疗师抽取黑曜：起始 15 银币每级 +30 铜币，奖励生成在角色脚下
12. 掉率提升：基础掉落 **5 倍**；蓝/黄/紫装概率（Lv60+ 装备 1/2%、6%、2%）；普装等级优化同地图等级 +20%；古老武器 99% 生成且紫装概率提升至 12%
13. 珍品/神灯/黑心商人调整：珍品商人售装概率 33%、紫装 16%（Lv60+）；三商人同图合计 6 种商品位；珍品商品价格 3 倍
14. 背包"粉碎"改"售卖"（售价为原 70%）
15. 双刃攻击取最高攻击力（物理/魔法）；副手攻击完整伤害
16. 提升拾取范围

**修复类**：古老武器拔不出/低等级生成（与玩家差 18 级时按玩家等级 -6 重新生成）、房间宝箱闪退、附魔 BUG（主附魔意外抗性、uid 错误）

**附魔宝石数值**（附件 20260704）：攻击/防御/魔攻宝石 1-5 阶 2/5/8、5-10 阶 11/14/17、11-15 阶 20/23/26、16-20 阶 29（三系相同）；另有宝石价格表、陨子属性表、装备炼化表、掉落表（图片分辨率有限，数值不全，需要精确值时参考原图）

**对逆向的影响**：
- 本版与 wiki 原版机制资料**存在偏差**（经验 -30%、强化上限 12、附魔系统新增、掉率 5 倍），§6.1-6.5 的网络资料结论须与改版实际反汇编交叉验证
- 新增附魔系统 → ITEMENCHANTBASE 表字段语义可能与本版相关（P3 B1 装备表字段逆向时注意）
- 经验 -30% → A2 经验/成长曲线逆向时需在真机实测对比
