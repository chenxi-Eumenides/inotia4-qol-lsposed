# API 参考手册

> **本文档 = API 规格（面向调用方）**：每个 API 的路径、用途、请求格式、返回格式与注意事项。
> 技术实现细节（VMA/函数签名/调用链/游戏内机制）见 `docs/development/architecture.md` 与 `docs/development/planning/refactor-plan.md`（原 `docs/research/` 系列已于 2026-08-16 清理）。
> 状态：**v0.5.13**。全部域（character/world/item/quest/ui/system/op/debug/health）端点已与 controller 真实路由对齐：v0.5.13 完成端点重构（36 处路径按本文档修正、废弃端点删除、缺失端点补齐或占位），本文档为唯一权威路由来源。
>
> 通用约定：
> - 服务地址：`http://<设备IP>:8088`（局域网，模块监听 0.0.0.0）
> - 请求/响应均为 JSON；写操作（POST）的 body 是 JSON 字符串
> - `role` = 出战槽位 0..2；`bag` = 背包袋 0..5 + 扩展逻辑袋 6..10（`extensionBagEnabled=true` 时并入读取与移动，见 §4.1/§4.2）；`slot` = 袋内槽位 0..15
> - 写操作成功返回 `{"ok":true,...}`；失败返回 `{"ok":false,"error":"<原因>"}`（错误信封格式 A，v0.5.45 统一）
> - **错误信封与 HTTP 状态码（v0.5.45 统一）**：所有错误响应统一 `{"ok":false,"error":"<原因>"}` + 语义状态码——**400** 参数错误（路由参数解析异常/body 解析失败/参数校验不过）、**403** 权限不足（OP 门禁未开启 `op disabled`）、**404** 未找到、**500** 内部错误、**501** 未实现（OP 占位端点）、**503** 未就绪（native 初始化中）。实现机制：controller 抛 `ApiException(code,msg)` → `GlobalExceptionResolver`（@Resolver）统一转响应
> - 写操作返回会附带 `state` 字段 = 操作后的最新状态快照（类型见各端点说明）

---

## 0. 分组总览

> API 按游戏实体/领域分 7 组，不再按读写性质划分（info/data/action 前缀已废弃）：
> 每组内 GET（读）与 POST（写）混合，读写同域，GET/POST 由 HTTP 方法区分。OP（越权操作）独立保留。
>
> **character 域端点按实体化重设计**（第二章为设计草案，代码待实现；其余域为现状）。

| 分组 | 顶层路径 | 覆盖实体 | 端点数 |
|---|---|---|---|
| **character**（角色与队伍） | `/api/character/*` | 出战角色 party、佣兵 mercenary、战斗 combat、角色成长 grow | 29 |
| **world**（地图与移动） | `/api/world/*` | 当前地图 map、移动操作 movement、静态地图 maps（含瓦片矩阵） | 15 |
| **item**（物品与背包） | `/api/item/*` | 背包 inventory、商店 shop | 17 |
| **quest**（任务） | `/api/quest/*` | 任务 | 6 |
| **ui**（界面与对话） | `/api/ui/*` | 界面状态 ui、对话/弹窗 dialog | 9 |
| **system**（系统与会话） | `/api/system/*` | 游戏整体 game、事件流 events、存档 save、静态数据表 tables（含 text/story-events）、帮助文档 help | 16 |
| **config**（模块配置） | `/api/config/*` | 模块配置读取与修改（list/set） | 2 |
| **op**（越权操作） | `/api/op/*` | 改数据/强行操作（全局开关门禁，默认关闭，见 §8） | 10 已实现 + 11 定稿 |
| debug（调试） | `/api/debug/*` | 开发期调试（含扩展背包视图/状态，见 §九） | 7 |
| health（顶层） | `/api/health` | 服务健康检查 | 1 |

> 全量端点 = 112（character 29 + world 15 + item 17 + quest 6 + ui 9 + system 16 + config 2 + op 10 + debug 7 + health 1；其中 GET 60 / POST 52）。

---

## 一、数据模型

各端点返回的 JSON 结构引用以下数据模型。

### Player（玩家）

```json
{
  "money": 72503,
  "map_id": 30,
  "x": 304,
  "y": 376,
  "active_quest": 0,
  "leader_slot": 0,
  "party_count": 2
}
```

| 字段 | 说明 |
|---|---|
| `money` | 金币（实时） |
| `map_id` | 实时地图 ID（MAPINFOBASE 记录下标，0-415） |
| `x`/`y` | 玩家实时坐标（像素） |
| `active_quest` | 当前激活任务 ID |
| `leader_slot` | 当前主控角色对应出战槽 |
| `party_count` | 出战人数 |

### Role（出战角色）

> 字段呈现顺序：可读字段在前（name/type_name/class_name/血量/魔力/经验/主属性），原始字段归尾部（type/class_idx/name_id/stats）。`main_stats` 为结构化列表（`stat_name`/`base_stat`/`additional_stat`，additional=总属性-基础属性，F_GET_STAT=Base+Main+Bonus+Sub）。

```json
{
  "name": "凯恩",
  "type_name": "主角",
  "class_name": "忍者",
  "level": 27,
  "hp": 10598, "max_hp": 10664,
  "mp": 200, "max_mp": 250,
  "exp": 12000, "exp_next": 15000,
  "main_stats": [
    { "stat_name": "力量", "base_stat": 9, "additional_stat": 87 },
    { "stat_name": "敏捷", "base_stat": 15, "additional_stat": 124 },
    { "stat_name": "体力", "base_stat": 11, "additional_stat": 90 },
    { "stat_name": "智力", "base_stat": 7, "additional_stat": 47 },
    { "stat_name": "精力", "base_stat": 5, "additional_stat": 33 }
  ],
  "status_point": 78,
  "equipment": [
    { "slot": 0, "name": "光荣的火冠", "category": 333, "count": 1, "item_type": "equipment",
      "need_level": 5, "rarity": 3, "rarity_tier": "紫",
      "base": { "damage": 0, "defense": 37, "magic_rate": 0.0 },
      "bonus": [ { "id": 1, "name": "敏捷", "value": 18 } ],
      "gem": { "total_slots": 1, "slots": [] },
      "chaos": { "is_chaos": false, "level": null, "rate": null },
      "enchant": { "id": 0, "level": 0, "effect": null } },
    null
  ],
  "type": 0,
  "class_idx": 1,
  "name_id": 2210,
  "stats": { "0": 130, "1": 0, "2": 0, "30": 10664, "31": 212 }
}
```

| 字段 | 说明 |
|---|---|
| `name` | 角色名（CHAR_GetName） |
| `type` | 角色类型（`[ch+0x09]` int8）：0=英雄（主控） 1=佣兵（非英雄成员）；地图单位上下文另有 2=装饰/场景单位（见第二章 units 模型） |
| `type_name` | 角色类型名（service 层注入：0→主角 1→佣兵；type==2 装饰物不注入） |
| `class_idx` | 职业索引 0-5（`[ch+0x0D]` int8，CHARCLASSBASE 记录下标；`type==2` 装饰物该字段存 type 值非职业索引） |
| `class_name` | 职业名（service 层注入，`class_idx` → CHARCLASSBASE 联查：黑暗骑士/忍者/黑魔导/祭司/暗影猎手/狂战士；仅 `class_idx∈[0,5]` 时注入） |
| `level` | 等级 |
| `hp`/`max_hp`/`mp`/`max_mp` | 当前血量魔力 + 最大血量魔力。`max_hp`/`max_mp` **直读属性缓存字段 `[ch+0x9c]`/`[ch+0xa0]`**（`char_max_hp`/`char_max_mp`），不调用 `CHAR_GetAttr(0x1e/0x1f)`——其 0x1e 分支在 HP>上限时会写回 HP，离线程调用会与游戏主线程属性重算竞争钳低 HP（见 `docs/history/hp-clamp-offthread-incident.md`）；缓存值 ≤0 时回退当前 `hp`/`mp` |
| `exp`/`exp_next` | 当前经验/升级所需经验。`exp` 直读 `[ch+0x318]`（等价 `CHAR_GetExperience` 纯读）；`exp_next` 取自**游戏线程帧缓存**（`char_next_exp_cached`，`kFramePointLogicPre` 每帧对 3 名队员缓存 `CHAR_GetNextExperience` 结果，见 `docs/development/features/frame-dispatch-host.md`），未命中缓存时回退直读游戏自身惰性缓存 `[ch+0x320]` |
| `main_stats` | 主属性列表（0-4=力量/敏捷/体力/智力/精力），每项 `{stat_name, base_stat, additional_stat}`；`base_stat`=基础属性（[ch+0x250+i] s8），`additional_stat`=总属性-基础（含分配/加成/动态）。总属性由 `char_stat_total` **直读 `C_STAT_BASE/MAIN/BONUS/SUB` 求和**（= `CHAR_GetStat` 的 Base+Main+Bonus+Sub，无 clamp），不调用 `CHAR_GetStat`（经 `CHAR_GetStatSub` 在动态派生脏位置位时会重算写回角色对象） |
| `status_point` | 剩余能力点 |
| `equipment` | 10 装备槽数组（每件为物品统一结构：`slot`/`name`/`category`/`item_type`/`need_level`/`rarity`/`rarity_tier`/`base`/`bonus`/`gem`/`chaos`/`enchant`，见 Inventory 段物品字段表），空槽为 `null`；位置映射见第二章 |
| `name_id` | 角色名字文本 ID |
| `stats` | 战斗属性聚合（角色 +0x24 数组 32 项，**数字 id 为键**：0-29 属性位 + 30=HP上限 + 31=MP上限）。属性名映射见第二章 stats 端点 |

### Inventory（背包）

```json
{
  "bags": [
    { "bag": 0, "items": [
      { "slot": 3, "name": "基础短剑", "category": 462, "count": 1, "item_type": "equipment",
        "need_level": 1, "rarity": 0, "rarity_tier": "白",
        "base": { "damage": 5, "defense": 0, "magic_rate": 1.1 },
        "bonus": [ { "id": 3, "name": "力量", "value": 18 } ],
        "gem": { "total_slots": 0, "slots": [] },
        "chaos": { "is_chaos": false, "level": null, "rate": null },
        "enchant": { "id": 0, "level": 0, "effect": null } }
    ], "capacity": 16, "slot_count": 13 }
  ]
}
```

**物品字段（读端点统一结构）**：

| 字段 | 说明 |
|---|---|
| `slot` | 槽位（背包槽 0-15 / 装备槽 0-9） |
| `name` | 物品名（品级前缀 + ITEMDATABASE 名称，Kotlin 联查注入） |
| `category` | 类别索引 = ITEMDATABASE 记录下标（`UTIL_GetBitValue(flags,15,6)`；物品 id = category+30） |
| `count` | 数量（`ITEM_GetCumulateCount`：可堆叠读 bit22-31 实际数量，装备返回 1） |
| `item_type` | 物品类型五类（ITEMDATABASE +2 字节用途类型）：`equipment`（0-21 装备）/ `potion`（药水）/ `scroll`（卷轴）/ `gem`（宝石）/ `consumable`（其余消耗/材料/任务/货币） |
| `need_level` | 所需等级（`ITEM_GetAbilityLevel`(0x1091f4)：读 ITEMCLASSBASE 记录 +3 int8；无等级概念的消耗品归 0） |
| `rarity` | 稀有度档位 0-4（GetRarity，白绿蓝黄紫） |
| `rarity_tier` | 档位名（白/绿/蓝/黄/紫，Kotlin 注入） |
| `base` | 基础属性对象：`{damage 物攻, defense 物防, magic_rate 魔法伤害倍率}`；`magic_rate` 为原始值/100 显示（110 → 1.1） |
| `bonus` | 词缀列表（type==0 节点，ITEMOPTINFOBASE 联查）：每项 `{id 词缀索引, name 词缀名, value 词缀值}` |
| `gem` | 宝石对象：`{total_slots 总槽位(插槽等级), slots[{id, name, value}] 已镶宝石}`（type==1 节点） |
| `chaos` | 混沌对象：`{is_chaos 是否混沌, level 混沌等级, rate 混沌成功率}` |
| `enchant` | 附魔对象：`{id 附魔ID, level 附魔等级, effect 附魔名(ITEMENCHANTBASE 联查)}` |

> 物品对象为统一结构（装备/消耗品/材料同构）：**缺失的标量字段置 `null`，列表字段恒为数组（无内容时空列表）**；`base`/`bonus`/`gem`/`chaos`/`enchant` 为重组后的可读对象，替代原位域拆解字段（`type_flags`/`raw_rarity`/`socket`/`enchant`/`chaos_*`/`options`/`option_ids`/`option_names`/`options_detailed`/`static_options`/`socket_info`/`enchant_info`/`chaos_info`）。`capacity` 袋容量 16 格；`slot_count` 占用数。

### Skills（角色技能）

> 直接提供技能列表，每项技能带名称与最大等级。

```json
{
  "skills": [
    { "action_id": 3, "name": "血之复仇", "level": 1, "max_level": 10 },
    { "action_id": 5, "name": "致命一击", "level": 3, "max_level": 10 }
  ],
  "unlock_bitmap": 65535,
  "active_skill_id": 3
}
```

- 来源：技能链表（`[ch+0x2A0]`，节点 `action_id`/`level`）、+0x2B0 解锁位图、+0x280 当前技能
- `name` 由 SKILLDESCBASE 联查（该表 `[0]`=action_id、`[3]` 起为技能名文本）
- `max_level` **不在链表节点、不在 SKILLDESCBASE**：来源为技能信息表 `*(0x2f4000+0x9e0)` 记录 +0x1D → 角色 +0x2B2 打包 nibble 数组解码（`CHAR_GetActMaxLevel` 0xe9560）。**等级规则**：常规最高 **4 级**，使用技能书（CHAR_ProcessSkillBook 0xe2488）可提升至最高 **8 级**。**API 当前未输出该字段，待实现**

### Mercenaries（全部佣兵）

```json
[
  { "slot": 0, "type": 0, "flags": 219, "in_party": true,
    "name": "凯恩", "level": 27, "x": 320, "y": 480 },
  { "slot": 1, "type": 2, "flags": 77, "in_party": false,
    "name": "西雷斯", "level": 26, "x": 2048, "y": 2048 }
]
```

- `flags` bit0=占用 bit1=在队伍；未上场佣兵坐标 2048 = 未激活哨兵

### Path（寻路）

```json
{ "target": { "x": 600, "y": 400 },
  "start": { "x": 40, "y": 168 },
  "in_map": true,
  "found": true,
  "distance": 72,
  "nearest": null,
  "path": [ { "x": 40, "y": 168 }, ... ] }
```

- 自研 BFS 导航（v0.4.29，替代游戏 CHAR_SearchPath）：瓦片矩阵（bit3=阻挡 bit7=出口）+ 单位占用（NPC/怪物不可穿越，v0.4.30）
- `found=false` 时返回 `nearest`（最近可达 tile）与最近可达路径；路径节点 = tile 中心像素（tile×16+8）

### GameState（游戏界面状态）

```json
{ "screen": "dialog_npc", "dialog": { "type": "npc", "active": true, "speaker": "杂货商人", "text": "……", "options": [...] } }
```

| 字段 | 说明 |
|---|---|
| `screen` | 当前界面（v0.5.42 起统一枚举，`GET /api/ui/screen` 同值）：`"loading"` / `"main_menu"` / `"world"` / `"tutorial_pause"`（药水教学）/ 对话框 `dialog_*`（`dialog_popup` 弹窗 / `dialog_story` 剧情 AVG / `dialog_npc` NPC 对话 / `dialog_quest` 任务完成面板 / `dialog_wipeout` 死亡面板 / `dialog_choice` 选择框 / `dialog_input_count` 数量输入）/ 面板 `panel_*`（`panel_character_info`/`panel_inventory`/`panel_skills`/`panel_mercenary`/`panel_quests`/`panel_settings`/`panel_shop`/`panel_craft`/`panel_npc_rest`/`panel_npc_revive`/`panel_save_slot`/`panel_character_select`/`panel_options`/`panel_shortcut`/`panel_world_map`/`panel_daily_reward`/`panel_in_app`/`panel_ui_panel`）/ 主菜单面板 `main_menu_*`（`main_menu_save_slot`/`main_menu_character_select`/`main_menu_daily_reward`/`main_menu_options`/`main_menu_settings`）/ `"agreement"`（Android Java 层同意页 `AgreementUIActivity`，独立于 native dialog_*/panel_*；仅主菜单前触发，`POST /api/ui/dialog/select` `{"action":"ok"}` 关闭）。 |
| `story` | 仅 screen=dialog_story：`active`/`speaker`/`text`/`index`/`count` |
| `dialog` | 仅 UI 被占据时存在：`<DialogContent 模型>`（type/title/text/options）。注意：type 残留时 `displayed` 字段为 false（数据残留，非实际 UI），以 `screen` 为准 |

> **v0.5.42 变更**：`dialog_active` 字段已移除——`screen` 精确表达 UI 占据状态（`dialog_*`/`panel_*`/`main_menu_*` 前缀即 UI 占据）。不再有数据残留导致的误报（旧版：关闭 NPC 对话框后 dialog_active 残留 true）。
>
> **v0.5.43 变更**：世界操作（移动 `move_to`/`walk_dir`/`stop_move`、战斗 `attack_target`/`cast_skill`/`stop_combat`、交互 `interact_with`、技能、物品使用、传送、`start_interact`）在 UI 占据时返回 `ui occupied: <screen>` 而非继续执行——UI 占据时游戏输入被接管，直接调 CHAR_Move 等会与 UI 竞争破坏控制态。面板内操作（装备/出售/整理/佣兵等）、对话操作（dialog/select）、菜单操作不受影响。

### Snapshot（快速状态快照）

```json
{
  "frame": 12345,
  "screen": "world",
  "money": 72503,
  "map_id": 30, "x": 304, "y": 376,
  "leader_slot": 0,
  "party": [ { "type": 1, "type_name": "佣兵", "class_idx": 1, "class_name": "忍者",
    "level": 27, "hp": 10598, "mp": 200,
    "max_hp": 10664, "max_mp": 250, "main_stats": [96, 139, 101, 54, 38],
    "name": "凯恩" } ]
}
```

一站式聚合：UI 状态 + 玩家全局 + 队伍摘要（角色类型/职业/等级/HP MP/主属性/角色名）。**不含装备明细（party 每角色无 equipment 字段）与佣兵列表**——装备明细走 `GET /api/character/party/{slot}/equipment`，佣兵走 `GET /api/character/mercenary`。party 角色含 `class_idx`/`class_name`（职业索引/职业名注入）与 `type_name`（0 主角/1 佣兵）；不含 `party_count`（出战人数走 `GET /api/character/party/count`）。

### DialogContent（对话/弹窗内容）

`GET /api/ui/dialog` 返回的统一结构（一个检测函数覆盖多种类型）：

```json
{
  "type": "sell",
  "active": true,
  "title": "出售物品",
  "text": "确定要出售 治疗药水 ×5 吗？",
  "options": [ { "id": "confirm", "label": "确定" }, { "id": "cancel", "label": "取消" } ]
}
```

| 字段 | 说明 |
|---|---|
| `type` | 弹窗/对话种类：`save`/`sell`/`quest`/`npc`/`story`/`popup`/`wipeout`/`agreement`/`none`（随逆向扩展；`agreement` 为 Java 层同意页，非 native 对话） |
| `active` | 是否有对话/弹窗 |
| `title` | 标题（任务对话框/出售弹窗等有标题的类型；无标题为 null） |
| `text` | 内容文本 |
| `options` | 可选动作列表 `{ id, label }`——供 `POST /api/ui/select_option` 选择；`id` 为动作标识（confirm/cancel/next/skip/quit/shop/revive/... 或具体选项），`label` 为按钮文案 |

各类型 options 见第六章 dialog 端点说明。

### 静态数据表

每条记录结构见 `apk/static-data/json/tables/<表名>.json`，已验证主要表：
- **ITEMDATABASE**（物品，1,018 条）：记录 = `{ "hex", "u16", "text_0" }`，+0 名称 text_id
- **MONDATABASE**（怪物，553 条）：+0 名称、+0x0f~0x14 六项成长属性、+0x22 技能起点、+0x23 技能数
- **QUESTINFOBASE**（任务，507 条）：+0 u16[0] **任务链/组 ID**（非 quest_id！如 13=导入战斗链[19/20/21/22/381]、21=突破军用仓库链[61/62]）、+2 标题、+6 高字节 bit5=任务菜单隐藏（战斗/教学任务）、+12 职业需求、+14 详情、+16 接取后对话、+18 交付对话、+26/+28 奖励起止；**任务真实标识 = 记录下标（0-506），运行时 QUESTSYSTEM 槽数组 questId 即按下标索引**（2026-08-16 定案）；**主线/支线判定见 QUESTGROUPBASE**
- **QUESTREWARDBASE**（任务奖励，394 条）：+0 item_id、+2 数量、+4 职业掩码
- **MAPINFOBASE**（地图，416 条）：+0 地图 ID（text_id 3513-3928）、名称
- **CHARCLASSBASE**（职业，6 条）：+2 描述文本
- **SKILLDESCBASE**（技能，114 条）：+2 技能文本

> ⚠️ 两套地图编号：静态表 MAPINFOBASE 的 `map_id` 是 **text_id**（3513-3928）；运行时 `/api/world/map/id` 返回 **MAPINFOBASE 记录下标**（0-415，如 30=影子丛林1）。`/api/world/maps/{map_id}` 对 0-415 按下标查询，其余按 text_id 兼容。

---

## 二、character（角色与队伍）— GET/POST /api/character/*

> ⚠️ **本节为设计草案（文档先行，代码待实现）**。实现后按本节验收，本节端点与当前代码不一致属预期。

**角色实体全生命周期**：出战角色（party）状态与操控、佣兵（mercenary）管理、战斗行为（combat）、角色成长（grow）。

**命名约定**：
- POST 动作用「动词+宾语」两词组合命名（如 `set_auto_attack`、`cast_skill`）
- 静态数据中有名称的字段一律注入名称（职业名 `id_name`、角色名 `name`、装备名、属性名、技能名）
- 单值端点「请求什么，返回的主字段就是什么」，可附加名称字段（如 `id` + `id_name`）

### 2.1 出战角色 party

#### 出战角色复合

`GET /api/character/party`

**用途**：获取 3 槽出战角色完整状态（含职业名/装备名/技能名注入）。

**返回格式**：`[ <Role>, <Role>, ... ]`（Role 模型数组，空槽为 `null`）

#### 出战人数

`GET /api/character/party/count`

**用途**：获取出战人数。

**返回格式**：`{ "count": 2 }`

#### 主控角色

`GET /api/character/leader`

**用途**：获取当前主控角色（转发到 party 中正在操控的那个角色，`leader_slot` 对应出战槽）。

**返回格式**：`<Role 模型>` 或 `{"error":"not found"}`


#### 指定出战槽

`GET /api/character/party/{slot}`

**用途**：获取指定出战槽（0..2）完整状态。

**返回格式**：`<Role 模型>` 或 `{"error":"not found"}`

#### 角色类型

`GET /api/character/party/{slot}/id`

**用途**：获取指定出战槽角色类型（职业索引）。

**返回格式**：`{ "id": 1, "id_name": "忍者" }`

**注意**：返回 `id`（职业索引 0-5）+ `id_name`（职业名，CHARCLASSBASE 记录 +0x00 文本联查：0 黑暗骑士 / 1 忍者 / 2 黑魔导 / 3 祭司 / 4 暗影猎手 / 5 狂战士）。

#### 角色名

`GET /api/character/party/{slot}/name`

**用途**：获取指定出战槽角色名。

**返回格式**：`{ "name": "凯恩" }`

#### 等级

`GET /api/character/party/{slot}/level`

**用途**：获取指定出战槽等级。

**返回格式**：`{ "level": 27 }`

#### 状态聚合

`GET /api/character/party/{slot}/status`

**用途**：获取指定出战槽基本状态聚合（血量/魔力/经验/技能点/能力点）。

**返回格式**：

```json
{
  "status": {
    "hp": 10598, "max_hp": 10664,
    "mp": 200, "max_mp": 250,
    "exp": 12000, "exp_next": 15000,
    "skill_points": 6,
    "attribute_points": 78
  }
}
```

#### 战斗属性聚合

`GET /api/character/party/{slot}/stats`

**用途**：获取指定出战槽全部战斗属性聚合（一次取全量）。

**返回格式**：

```json
{
  "stats": {
    "crit_rate": 60, "crit_damage": 400, "attack": 300,
    "magic_attack": 150, "dexterity": 139, "defense": 200,
    "wdr": 100, "max_hp": 10664, "max_mp": 212
  }
}
```

**属性名字段映射**（角色 +0x24 数组 32 项，以属性名为字段名；✅ v0.5.1 实机 frida 实测补全）：

| 字段 | 属性 id | 说明 |
|---|---|---|
| `crit_rate` | 0 | 暴击率 ×10（默认 30） |
| `crit_damage` | 3 | 暴击伤害 ×10（默认 1000） |
| `attack` | 4 | 攻击（力量×0.5-0.7 + 敏捷×0.5 + 主手武器） |
| `magic_attack` | 8 | 魔攻（智力/精力×0.6-1.0 + 主手武器） |
| `magic_resist` | 11 | **魔法抵抗**（面板 M.RES 35193；clamp 750=75.0%） |
| `dexterity` | 13 | 敏捷（总，=主属性敏捷） |
| `hit_base` | 14 | **命中率基数**（CHAR_GetHitRate1000 加数；默认 0，LV1 黑魔导 80） |
| `hit_rate` | 15 | **命中率百分比**（敏捷×4+精力×4） |
| `defense` | 17 | 防御（体力×1 + 装备） |
| `phys_reduce` | 18 | **物理减伤系数**（P.RES=attr17×(attr18+1000)/10000） |
| `wdr` | 19 | 武器伤害减免率 ×10（默认 30） |
| `sub_weapon_attack` | 20 | 副手武器攻击（slot6） |
| `level_attr` | 28 | 等级驱动属性 =(960+36×等级)/10（黑魔导；职业各异） |
| `max_hp` | 30 | HP 上限（属性缓存字段 `[ch+0x9c]`，`char_max_hp`；不调用 `CHAR_GetAttr(0x1e)`——该分支有 HP 写回副作用；=640+72×(等级+10) 黑魔导） |
| `max_mp` | 31 | MP 上限（属性缓存字段 `[ch+0xa0]`，`char_max_mp`；默认 200） |
| `total_attack` | 113 | 总攻击 = max(物攻,魔攻)（扩展 id，不在 32 项数组） |

**属性公式表**（CHAR_UpdateAttrFromStat 映射，frida 实测）：力量→攻击(a×700/600/500÷1000，职业条件 4/2/3/5/6/18/1)、敏捷→攻击(a×500÷1000)+命中(a×4)+敏捷(a×1)、体力→HP上限(a×80)+防御(a×1)、智力→魔攻(a×600/700/1000÷1000，条件 5/6/16)、精力→魔攻(a×600/600/1000÷1000)+命中(a×4)。

**注意**：其余 id（1,2,5,6,7,9,10,12,16,21,22-27）仍输出 `attr_<id>` 占位（LV1 实测全 0；21 默认 1000、16 默认 8、29 默认 8 来源未定性，待高等级/词缀样本）。⚠️ 不可直接套用 ITEMOPTINFOBASE 词缀属性编码（如 MP增加 编码 31，而 stats[31] 是 MP上限）——该编号与 stats 数组索引不一致。

#### 装备列表

`GET /api/character/party/{slot}/equipment`

**用途**：获取指定出战槽装备列表（每件含位置与名称注入）。

**返回格式**：`{ "equipment": [ <Item 对象>, null, ... ] }`（10 槽，空槽 `null`）

**装备位置映射**（每件装备含 `position` 字段。依据：CHAR_FindEquipSlot(0xe4fd0) 反汇编 + ITEMCLASSBASE 记录 +4 字节槽位表）：

| slot | position | 位置 | 物品类别（ITEMCLASSBASE 类名） |
|---|---|---|---|
| 0 | `head` | 头部 | 帽子/头盔/头环/黄金之冠 |
| 1 | `glove` | 护手 | 护手 |
| 2 | `cloak` | 斗篷 | 斗篷 |
| 3 | `armor` | 铠甲 | 布甲/皮甲/板甲 |
| 4 | `boots` | 鞋 | 鞋 |
| 5 | `weapon1` | 武器·主手 | 短剑/长剑/斧头/钝器/双手武器/水晶球/法杖/弓/弩 |
| 6 | `weapon2` | 武器·副手 | 盾牌；忍者/狂战士（职业 1/5）双持副手武器 |
| 7 | `necklace` | 项链 | 项链 |
| 8 | `ring` | 戒指 | 戒指 |
| 9 | `unused` | 未使用 | 无任何装备类映射（保留槽） |

**注意**：游戏只分配 1 个戒指槽（slot 8），无第二戒指槽；槽位由静态表驱动而非固定约定，slot 9 预留无装备可穿。

#### 指定装备槽

`GET /api/character/party/{slot}/equipment/{equip_slot}`

**用途**：获取指定出战槽指定装备（0..9）。

**返回格式**：`<Item 对象>`（含 position/name）或 `null`

#### 技能列表

`GET /api/character/party/{slot}/skills`

**用途**：获取指定出战槽技能列表。

**返回格式**：

```json
{
  "skills": [
    { "action_id": 3, "name": "血之复仇", "level": 1, "max_level": 10 },
    { "action_id": 5, "name": "致命一击", "level": 3, "max_level": 10 }
  ],
  "unlock_bitmap": 65535,
  "active_skill_id": 3
}
```

**注意**：`skill_points` 已移入 status 聚合；`name` 由 SKILLDESCBASE 联查；`max_level` 来源为技能信息表 `*(0x2f4000+0x9e0)` 记录 +0x1D → 角色 +0x2B2 打包 nibble 数组解码（CHAR_GetActMaxLevel 0xe9560），常规最高 4 级、技能书可提升至 8 级，**API 当前未输出该字段，待实现**。

#### 佣兵入队

`POST /api/character/party/include`

**用途**：把待命佣兵编入出战队伍（MERCENARYSYSTEM_IncludeParty）。

**请求格式**：`{ "mercenary_slot": 1 }`

**返回格式**：`{"ok":true,"state":<Party 模型>}`

**注意**：已在队→`already in party`；满员→`party full`。⚠️ 参数索引见 2.2「两套索引」说明。

#### 佣兵离队

`POST /api/character/party/exclude`

**用途**：把出战佣兵移回待命（MERCENARYSYSTEM_ExcludeParty）。

**请求格式**：`{ "mercenary_slot": 1 }`

**返回格式**：`{"ok":true,"state":<Party 模型>}`

**注意**：主控→`cannot exclude leader`；任务 NPC→`cannot exclude quest npc`。

#### 佣兵遣散

`POST /api/character/party/discharge`

**用途**：遣散佣兵（MERCENARYSYSTEM_Release）。

**请求格式**：`{ "mercenary_slot": 27 }`

**返回格式**：`{"ok":true,"state":<Party 模型>}`

**注意**：无该槽角色→`mercenary not found`；主控/任务 NPC→不可遣散。

#### 取出佣兵装备

`POST /api/character/party/withdraw`

**用途**：取出佣兵指定装备槽到背包（CHAR_UnequipItemToInven）。

**请求格式**：`{ "mercenary_slot": 0, "equip_slot": 3 }`

**返回格式**：`{"ok":true,"state":<Party 模型>}`

**注意**：无佣兵→`mercenary not found`；装备槽越界→`bad slot`。

### 2.2 佣兵 mercenary

#### 全部佣兵

`GET /api/character/mercenary`

**用途**：获取全部佣兵（含未上场）。

**返回格式**：`[ <Mercenary>... ]`（Mercenaries 模型数组）

#### 佣兵槽列表

`GET /api/character/mercenary/list`

**用途**：获取非空佣兵槽 id 列表。

**返回格式**：`{ "slots": [0, 1, 3] }`

#### 指定佣兵槽

`GET /api/character/mercenary/{slot}`

**用途**：获取指定佣兵槽信息。

**返回格式**：`<Mercenary 对象>` 或 `{"error":"not found"}`

#### ⚠️ 两套索引说明

`GET /mercenary*` 端点返回的 `slot` 与 `POST /party/include|exclude|discharge|withdraw` 请求中的 `mercenary_slot` **不是同一套索引**：

| 概念 | 值示例 | 来源 |
|---|---|---|
| 读端点 `slot`（槽数组索引） | 27/32/58 | 佣兵槽数组 `MERCENARYSYSTEM_pSlotList` 下标（每槽 20B 记录，G_MERC_SLOTLIST_GOT_VMA） |
| 写参数 `mercenary_slot`（角色槽 ID） | 0/1/255 | 角色对象 +0x352 字段（角色池中标记归属槽） |

**为什么会这样**：游戏内部有两套并行的佣兵管理机制——佣兵**记录**存于槽数组（管理占用/在队状态），佣兵**角色对象**在角色池中用 +0x352 字段标记归属槽。API 读端点直接暴露槽数组下标，写端点则透传角色槽 ID 参数（凯恩 +0x352=0，其余角色 +0x352=255 无效），两套编号自然对不上。

**如何改进（待实现）**：API 层统一为「槽数组索引」一套编号——写操作也接收读端点返回的 `slot`，模块内部完成 槽数组索引 ↔ +0x352 槽 ID 的映射转换后再调用底层；`mercenary_slot` 参数改名 `slot` 与读端点对齐。

### 2.3 战斗 combat

> 战斗动作统一「动词+宾语」两词命名：`set_auto_attack` / `set_skill_usage` / `switch_player` / `cast_skill` / `attack_target` / `stop_combat`。

#### 切换主控

`POST /api/character/combat/switch_player`

**用途**：切换主控角色到指定出战槽（PARTY_SetActivePlayer 0x11f584）。

**请求格式**：`{ "slot": 1 }`（目标出战槽 0/1/2）

**返回格式**：`{"ok":true,"state":<Player 模型>}`

**注意**：目标槽越界/不可切换→`switch failed`；主控切换后 `/api/character/leader` 返回相应角色。

#### 自动反击开关

`POST /api/character/combat/{role}/set_auto_attack`

**用途**：开关指定出战槽的**自动反击**（CHAR_SetAutoAttack 写 [ch+0x3a0] bit7-10）。

**请求格式**：`{ "on": true }`

**返回格式**：`{"ok":true,"state":<Party 模型>}`

**注意**：⚠️ 此开关只控制「**被怪物攻击后的自动反击**」——角色受击后自动还击；**不是**自动攻击附近怪物的挂机行为，角色不会主动索敌。开启后仍需先通过 `attack_target` 进入战斗。

#### 技能使用开关

`POST /api/character/combat/{role}/set_skill_usage`

**用途**：设置指定出战槽**单个已学技能**是否被战斗 AI 自动使用及其使用频率（每个技能独立设置）。

**请求格式**：`{ "action_id": 3, "mode": "normal" }`

**返回格式**：`{"ok":true,"state":<Party 模型>}`

**mode 三档**：`off` 不使用 / `normal` 一般频率 / `high` 高频率

**注意**：`action_id` 必须为已学技能（未学→`skill not learned`）；`off` 等价于关闭该技能自动使用。底层为 CHAR_SetSkillUsage 写 [ch+0x3a0] bit0-2 总开关 + 技能链表节点 +0x07 单技能 AI 等级（待需）。

#### 释放技能

`POST /api/character/combat/{role}/cast_skill`

**用途**：指定出战槽立即释放技能（CHAR_GetEnemyTarget + CHAR_SetActionID）。

**请求格式**：`{ "action_id": 5 }`

**返回格式**：`{"ok":true,"state":<Party 模型>}`

**注意**：当前校验 = 技能链表成员资格（未学→`skill not learned`）。⚠️ 0-7 基础动作（普攻）也在技能链表中，`cast 5` = 普攻（2026-08-09 真机实测 -403 伤害）；「真技能 vs 基础动作」白名单与 MP 校验待实现（见 backlog 攻击/cast 校验条目）。无目标→`no target`。

#### 攻击目标

`POST /api/character/combat/{role}/attack_target`

**用途**：指定出战槽**进入战斗状态并对指定目标开始自动普攻**（CHAR_SetTarget + CHAR_MakeDefaultAttack）。

**请求格式**：`{ "target_slot": 5 }`

**返回格式**：`{"ok":true,"state":<Party 模型>}`

**注意**：目标无效→`target not found`；缺参→`target_slot required`。此操作使角色进入战斗姿态，随后持续普攻该目标直至目标死亡/离开/`stop_combat`。

#### 停止战斗

`POST /api/character/combat/{role}/stop_combat`

**用途**：停止指定出战槽战斗（CHAR_StopCombat）。

**请求格式**：无 body

**返回格式**：`{"ok":true,"state":<Player 模型>}`

**注意**：非战斗态调用安全（清标志幂等）。

### 2.4 角色成长 grow

> grow 下直接四个动词动作：`add_skill` / `add_stat` / `reset_skill` / `reset_stat`。

#### 技能加点

`POST /api/character/grow/add_skill`

**用途**：主角学习/升级技能（CHAR_LearnAction，消耗技能点）。

**请求格式**：`{ "action_id": 3, "level": 1 }`

**返回格式**：`{"ok":true,"state":<Skills 模型>}`

**注意**：主角专用，无 role 路径段。

#### 属性加点

`POST /api/character/grow/{role}/add_stat`

**用途**：指定出战槽分配属性点（属性+1/能力点-1，StatDivide 语义）。

**请求格式**：

```json
{ "attrs": { "strength": 1, "agility": 2 } }
```

`attrs` 为字典：字段名=主属性英文名（`strength`/`agility`/`vitality`/`intelligence`/`spirit`，对应索引 0=力量/1=敏捷/2=体力/3=智力/4=精力，**可只传部分**），值为分配数量。

**返回格式**：`{"ok":true,"state":<Player 模型>}`

**注意**：⚠️ 分配数量只能为正整数；各属性分配数量**总和不能超过剩余能力点**，否则报错（`no status point`）；属性名非法→`bad attr`。

#### 属性重置

`POST /api/character/grow/reset_stat`

**用途**：重置主角分配属性（CHAR_InitializeStatus：分配点归零+能力点按公式还原）。

**请求格式**：无 body

**返回格式**：`{"ok":true,"state":<Player 模型>}`

**注意**：⚠️ 只能对主角使用，无 role 路径段。

#### 技能重置

`POST /api/character/grow/reset_skill`

**用途**：重置主角技能（CHAR_InitializeSkill：移除非基础技能+技能点还原）。

**请求格式**：无 body

**返回格式**：`{"ok":true,"state":<Player 模型>}`

**注意**：⚠️ 只能对主角使用，无 role 路径段。

### 2.5 待补齐数据（逆向/探索缺口）

> 本节罗列 character 域设计草案中**尚缺的数据**：实现上述端点前需补齐的逆向结论与代码能力。按「运行时逆向 / 静态数据资产 / 文档修正」三类组织。

#### 运行时逆向缺口

> ✅ **v0.5.1 实机 frida 验证已全部闭环**（2026-08-13 真机，证据见 `docs/development/planning/backlog.md` 对应条目）。

| # | 缺口 | 状态与结论 |
|---|---|---|
| R1 | stats 属性名 22 项 | ✅ 已确认 15 项（见第二章属性映射表：新增 11 魔法抵抗/14 命中基数/15 命中率/18 物理减伤/20 副手攻击/28 等级驱动），其余 12 项 LV1 实测全 0 暂占位 |
| R2 | skill max_level 权威来源 | ✅ 技能信息表 `*(0x2f4000+0x9e0)` 记录 +0x1D int16（负=无）→ 表2 `*(0x2f3758)` +9 角色偏移 → `[ch+0x2B2]` **bit1-4**（4 位）= 最终 max_level（常规 4 / 技能书 8，实测切换） |
| R3 | set_skill_usage 单技能档位编码 | ✅ 修正：`CHAR_SetSkillUsage` 写 [ch+0x3a0] **bit0-3**（非 bit0-2）；技能链表节点 +0x07=1 恒为激活标志，**native 无单技能档位**，只有全局位域 |
| R4 | merc 两套索引映射规则 | ✅ 槽数 = **21**（data-sources 88 为 GOT 双层误读）；读=槽数组下标、写=+0x352，经 CHARSYSTEM_FindAsMercenarySlot(0xf4254) 匹配 |
| R5 | 装备槽位表运行时验证 | ✅ 实机确认：基础法杖→主手槽5、漆黑之皮甲→身体槽3，与 equipment 数组一致；槽位 = ITEMCLASSBASE+2 → 槽位表+4 |

#### 静态数据资产缺口

| # | 缺口 | 状态与结论 |
|---|---|---|
| S1 | ITEMOPTINFOBASE.json 未打包 | ✅ **已修复并复验**（v0.5.1）：`package_assets.py` INCLUDE_TABLES 加 ITEMOPTINFOBASE → 重打包 → option_names 非空（实测 `["敏捷","体力","瞬间恢复","武器格挡率"]` 等） |
| S2 | className 联查缺失 | ✅ 实现依据：CHARCLASSBASE u16[0]=职业名 text_id=class_idx×2；StaticData.kt 待实现 `className()` |
| S3 | skillName / skillMaxLevel 联查缺失 | ✅ 权威路径修正（非 SKILLDESCBASE）：技能信息表 recN↔action N，技能名 = rec+0 u16 text_id（=1220+rec），max_level = 角色 +0x2B2 bit1-4；StaticData.kt 待实现 |
| S4 | 佣兵名联查缺失 | ✅ 实现依据：MERCENARYINFOBASE +0x04=佣兵名 text_id（35752+idx）；name=null 槽成因 = 无联查函数 |

#### 文档修正项

| # | 缺口 | 状态与结论 |
|---|---|---|
| D1 | static-data.md §7.2 职业名字段错误 | ✅ 修正：+0x00=职业名（u16[0]，text_id=class_idx×2），已修 static-data.md |
| D2 | backlog L63「修复 buildOptionNames 恒空」与资产包矛盾 | ✅ 由 S1 修复消除（v0.5.1 打包 ITEMOPTINFOBASE） |
| D3 | backlog merc 两套索引条目对齐 | ✅ 槽数 21 实测确认，data-sources 88 误读已修正 |

---

## 三、world（地图与移动）— GET/POST /api/world/*

**空间实体**：当前地图感知（map）、移动操作（movement）、静态地图查询（maps，含静态瓦片矩阵）。位置感知与位置操作一体。

**命名约定**：POST 动作用「动词+宾语」两词命名（`move_to`/`walk_dir`/`stop_move`/`interact_with`）；静态数据中有名称的字段注入名称（地图名）。

### 3.1 当前地图 map

#### 地图 ID

`GET /api/world/map/id`

**用途**：获取当前地图 ID 与地图名。

**返回格式**：`{ "id": 30, "id_name": "影子丛林1" }`（`id_name` 由 MAPINFOBASE 联查，与 character `id`+`id_name` 规则一致）

#### 出口区域

`GET /api/world/map/exits`

**用途**：获取当前地图全部出口区域（**静态数据源**：`maps/exits.json`，含瓦片坐标与目标地图 id；替代原 native 瓦片矩阵扫描）。

**返回格式**：`{ "exits": [ { "x": 24, "y": 19, "targetMapId": 30, "targetX": 2, "targetY": 10 }, ... ] }`

**字段**：`x`/`y` = 出口瓦片坐标（像素 = ×16）；`targetMapId` = 目标地图 id（MAPINFOBASE 索引）；`targetX`/`targetY` = 目标点瓦片坐标。

#### 场景单位

`GET /api/world/map/units`

**用途**：获取当前地图全部场景单位（队伍/NPC/怪物/装饰物，含 level/hp/mp/name）。type 0=队伍 1=怪物/NPC 2=装饰物（路障/宝箱/火把/出口等）。过滤全部在 native 层完成（type 0-2 / status≤2 / 坐标范围）。

**返回格式**：
```json
{ "units": [ { "slot": 0, "status": 1, "type": 1, "level": 27, "hp": 10598, "mp": 200,
  "x": 320, "y": 480, "name": "凯恩", "distance": 0, "nearest_distance": -1 },
  { "slot": 7, "status": 2, "type": 2, "level": 1, "hp": 792, "mp": 200,
  "x": 312, "y": 152, "name": "路障", "func_display": 120, "interactable": true, "distance": 1 }, ... ] }
```

**注意**：
- `distance` = 玩家到「能紧贴该对象的相邻可达格」的最短 BFS 路径长度（对象自身 tile 被单位阻挡标记恒不可达，故取上下左右 4 邻格中可达者的最小深度）；4 邻格全部不可达 → `distance=-1` 且附 `nearest_distance`（最近可达点距离）。
- type==2 装饰物额外输出 `func_display`（npc+0xa u16，NPCSYSTEM_CheckFunctionDisplay 入参）+ `interactable`（funcDisplay 非不可交互，即 CheckFunctionDisplay≠2）。可交互装饰物如路障（120）/宝箱（36）；纯装饰如火把（127）/地图出口（371）/巨石。

#### 敌人/召唤物

`GET /api/world/map/enemies`

**用途**：获取当前地图敌人/召唤物（native 独立构建，过滤 type==1，v0.5.35 起由 Java 过滤下沉 native）。

**返回格式**：`{ "units": [ ... ] }`

#### 可交互对象

`GET /api/world/map/interactives`

**用途**：获取当前地图可交互对象（native 独立构建，过滤 type==2 且 `interactable==true`，v0.5.35 起由 Java 过滤下沉 native）。

**返回格式**：`{ "units": [ ... ] }`

#### 掉落物

`GET /api/world/map/drops`

**用途**：掉落物列表。

**返回格式**：`{ "drops": [] }`

**注意**：数据源未探索，恒返回空数组（⏳ 占位，见 backlog P2 掉落物条目）。

#### 寻路距离

`GET /api/world/map/distance?tx=600&ty=400`

**用途**：玩家→目标的 BFS 距离（只计算不移动）。

**返回格式**：
```json
{ "target": { "x": 600, "y": 400 }, "start": { "x": 40, "y": 168 },
  "in_map": true, "found": true, "distance": 72, "nearest": null }
```

### 3.2 移动操作 movement

> 动作统一「动词+宾语」两词命名。`move_to`/`walk_dir` 均为**正常走路**（后台线程逐帧移动，非瞬移），到达出口区域自动切图。

#### 寻路移动

`POST /api/world/movement/move_to`

**用途**：寻路移动玩家到目标像素坐标（自研 BFS 导航，后台线程逐帧移动，正常走路非瞬移）。

**请求格式**：
```json
{ "x": 600, "y": 400 }
```

**返回格式**：`{"ok":true,"state":<Player 模型>}`

**注意**：
- 目标不可达时走到最近可达点并转身面向目标（face-target）
- 单位/墙阻挡自动绕行；到达出口区域自动切图
- 剧情/切图状态（GAMESTATE_nState!=0）时操作自终止

#### 持续移动

`POST /api/world/movement/walk_dir`

**用途**：方向键持续移动（后台线程每帧 CHAR_Move，正常走路）。

**请求格式**：`{ "direction": 1 }`（0=下 1=左 2=上 3=右）

**返回格式**：`{"ok":true,"state":<Player 模型>}`

**注意**：撞墙即停；direction 越界返回 `direction 0-3 required`；持续移动到达出口区域自动切图。

#### 停止移动

`POST /api/world/movement/stop_move`

**用途**：停止所有移动（停后台线程+清路径）。

**请求格式**：无 body

**返回格式**：`{"ok":true,"state":<Player 模型>}`

#### 场景交互

`POST /api/world/movement/interact_with`

**用途**：场景交互/攻击键（复现官方 EVTSYSTEM_DoCheckAllEvent(2) 事件触发链）。

**请求格式**：无 body

**返回格式**：`{"ok":true,"state":<Player 模型>}`

**注意**：与 `start_interact` 等价（内部同链）。

### 3.3 静态地图 maps

> 静态数据查询域：地图信息与静态瓦片矩阵（assets maps/tiles.json，416 图）。**瓦片数据只从静态数据获取，不再从运行时内存读取**；原运行时端点 `/api/world/map/tile`（单格瓦片）与 `/api/world/map/tiles`（矩阵）已移除。

#### 地图列表

`GET /api/world/maps/list`

**用途**：获取地图列表（id+名称）。

**返回格式**：`{ "maps": [ { "map_id": 3513, "name": "黑暗骑士团营地" }, ... ] }`

#### 指定地图

`GET /api/world/maps/{map_id}`

**用途**：获取指定地图静态信息。

**返回格式**：
```json
{ "map_id": 30, "text_id": 3513, "name": "影子丛林1", "raw": { "u16": [...], "text_0": "..." } }
```

**注意**：0-415 按下标查询（text_id 为对应 text_id）；其他值按 text_id 反向匹配（兼容旧语义）。

#### 地图瓦片矩阵

`GET /api/world/maps/{map_id}/tiles`

**用途**：获取指定地图完整瓦片矩阵（寻路/切图判定用），数据源为静态 assets（maps/tiles.json）。

**返回格式**：
```json
{ "map_id": 30, "src": "static", "width": 64, "height": 64, "size": 64,
  "encoding": "array", "tiles": [ [0, 64, 0, ...64 列...], ...64 行 ] }
```

**注意**：
- `tiles` 为**双层数组**（64×64），每个元素为 1 字节瓦片值（0-255），已解码：
  - **bit6 (0x40)**：阻挡（不可通行）
  - **bit7 (0x80)**：出口（切图区域）
  - 其余 bit：瓦片类型（`byte1 >> 4`）
- `width`/`height` 为该地图实际有效尺寸（有效区域外的瓦片值为 0）
- 瓦片数据只从静态数据获取，缺失时返回 `{"error":"no tiles"}`；由原 `/api/world/map/tiles` 移入本端点（v0.5.24 起由 base64 改为双层数组）

#### 地图出口

`GET /api/world/maps/{map_id}/exits`

**用途**：获取指定地图全部出口区域（**静态数据源**：`maps/exits.json`，387 图/3077 出口，与 tiles.json 同源生成）。

**返回格式**：
```json
{ "map_id": 30, "src": "static", "exits": [ { "x": 24, "y": 19, "targetMapId": 30, "targetX": 2, "targetY": 10 }, ... ] }
```

**字段**：`x`/`y` = 出口瓦片坐标（像素 = ×16）；`targetMapId` = 目标地图 id（MAPINFOBASE 记录下标）；`targetX`/`targetY` = 目标点瓦片坐标。

**注意**：出口条目 6 字节逆向完成（2026-08-16）：byte5=目标地图 ID、byte2-3=目标点坐标；当前地图出口见 §3.1 `/api/world/map/exits`。
---

## 四、item（物品与背包）— GET/POST /api/item/*

**物品实体全生命周期**：背包读写（inventory）、商店交易（shop）。物品从获得到消耗/处置全在一个组；穿脱装备/镶嵌宝石属于物品操作，保留在本域。

**命名约定**：POST 动作用「动词+宾语」两词命名（`use_item`/`discard_item`/`sell_item`/`equip_item`/`put_jewel`/`buy_item` 等）；静态数据中有名称的字段一律注入名称（物品名、词缀名经 `bonus[].name`）。

### 4.1 背包读 inventory

> **扩展逻辑袋并入（v0.6.16）**：`extensionBagEnabled=true` 且处于 world 时，全部背包读端点自动并入扩展逻辑袋 `6..10`（schema 与原版袋一致：`bag/items/capacity/slot_count`，物品项为 `slot/category/count`）；`false` 或非 world 时仅返回原版 `0..5`。原型阶段容量固定 `16/8/4/0/0`；扩展物品不进入原版 `g_inven`（模块自持）。原版第 6 袋（索引 `5`，任务物品专用袋）不属于扩展域。

#### 背包复合

`GET /api/item/inventory`

**用途**：获取背包完整信息（原版 6 袋 × 16 槽 + 扩展逻辑袋 6..10（启用时），含名称注入）。

**返回格式**：`<Inventory 模型>`

#### 金币

`GET /api/item/inventory/money`

**用途**：获取当前金币。

**返回格式**：`{ "money": 72503 }`

#### 全部物品展平

`GET /api/item/inventory/items`

**用途**：获取全部物品展平列表（每项附 bag 字段；启用时含扩展袋物品 bag=6..10）。

**返回格式**：`{ "items": [ { "bag": 0, "slot": 3, "category": 1, "count": 1, "name": "治疗药水" }, ... ] }`

#### 袋信息

`GET /api/item/inventory/bag/{bag}/info`

**用途**：获取指定袋（0..5 原版；6..10 扩展，启用时）信息（容量/占用）。

**返回格式**：`{ "bag": 0, "capacity": 16, "slot_count": 13 }`

#### 袋内指定槽

`GET /api/item/inventory/bag/{bag}/{slot}`

**用途**：获取指定袋内指定槽物品（**按 `slot` 字段匹配，不跳过空格**）。

**返回格式**：`<Item 对象>`（含 name）或 `null`（空槽）

**注意**：按实际格号（物品的 `slot` 字段）匹配，不做数组下标偏移——native 输出跳过空格后数组下标 ≠ 实际格号，查询以 `slot` 为准。

### 4.2 背包操作 inventory

> 写操作统一「动词+宾语」两词命名。

#### 使用物品

`POST /api/item/inventory/use_item`

**用途**：使用指定背包物品。按物品类别分派四条路径：骰子（STATUSDICE 掷骰）/ 解封（ReleaseSealed）/ 开箱（OpenItemBox）/ 常规物品（CHAR_UseItemEx 效果链：药水回血、卷轴、技能书、佣兵卡等，内部成功时自动消耗）。

**请求格式**：`{ "bag": 0, "slot": 3 }`

**返回格式**：`{"ok":true,"state":<Inventory 模型>}`

**注意**：骰子→掷骰预览返回 `base/pending/delta`（不应用，由 `accept_dice`/`reject_dice` 处理）；非消耗品→`item not usable`；常规物品冷却中/状态不符→`on cooldown`（不消耗）。

#### 接受掷骰

`POST /api/item/inventory/accept_dice`

**用途**：接受未确认的掷骰结果（应用 pending 到基础属性）。

**请求格式**：无 body

**返回格式**：`{"ok":true,"state":<Player 模型>}`

**注意**：无未确认结果→`no dice result pending`。

#### 拒绝掷骰

`POST /api/item/inventory/reject_dice`

**用途**：拒绝掷骰结果（仅清 flag，骰子不退回）。

**请求格式**：无 body

**返回格式**：`{"ok":true}`

**注意**：无未确认结果→`no dice result pending`。

#### 丢弃物品

`POST /api/item/inventory/discard_item`

**用途**：丢弃指定背包物品。

**请求格式**：`{ "bag": 0, "slot": 5 }`

**返回格式**：`{"ok":true,"state":<Inventory 模型>}`

**注意**：按槽位清空判定成功。

#### 移动/整理物品

`POST /api/item/inventory/move_item`

**用途**：移动物品或堆叠合并。按 bag 域自动分派：原版↔原版走 `INVEN_MoveItem`（支持部分数量）；任一端为扩展逻辑袋（6..10）时走扩展事务路径（整堆移动、序列化/重建、自动选槽）。

**请求格式**：`{ "bag": 0, "slot": 3, "count": 1, "to_bag": 0, "to_slot": 4 }`

**返回格式**：`{"ok":true,"state":<Inventory 模型>}`（原版路径）；`{"ok":true,"state":<扩展状态,含 items>}`（扩展路径）

**注意**：
- 源空槽→`slot empty`；count≤0→参数错；同槽→`same slot`；目标越界→`bad target`。
- **扩展路径（bag 含 6..10）**：整堆移动（`count` 忽略）；同一扩展逻辑袋内同类可堆叠物品才尝试合并，跨扩展袋不合并；原版→扩展属于跨域移动，也不合并；放置槽自动选择（优先同袋可合并槽/空槽，`to_slot` 仅扩展→扩展生效）；`bag`/`to_bag`=5（任务物品袋）→`task bag excluded`；扩展开关关闭→`extension bag disabled`；跨域移动写入模块 unsaved journal，显式保存（`/api/system/save`）成功后清零。
- 原版→原版不可经扩展路径分派；扩展内部事务失败自动回滚，可安全重试。

#### 出售物品

`POST /api/item/inventory/sell_item`

**用途**：出售指定背包物品（价格=ITEM_GetPrice 静态表）。

**请求格式**：`{ "bag": 0, "slot": 5 }`

**返回格式**：`{"ok":true,"price":15,"state":<Inventory 模型>}`

**注意**：空槽→`slot empty`；价格由静态表决定（防刷钱）。

#### 穿装备

`POST /api/item/inventory/{role}/equip_item`

**用途**：指定出战槽穿上装备（背包位置或类别；装备操作为物品操作，保留在本域）。

**请求格式**（二选一）：
```json
{ "bag": 0, "slot": 3 }
```
或
```json
{ "category": 512 }
```

**返回格式**：`{"ok":true,"state":<Party 模型>}`

**注意**：目标槽占用自动替换（先卸后穿）；不可装备（等级/职业/类型不符）按 CHAR_CanEquipItem 校验失败。

#### 脱装备

`POST /api/item/inventory/{role}/unequip_item`

**用途**：指定出战槽脱下装备。

**请求格式**：`{ "slot": 2 }`

**返回格式**：`{"ok":true,"state":<Party 模型>}`

**注意**：空槽返回 `unequip failed`。

#### 镶嵌宝石

`POST /api/item/inventory/{role}/put_jewel`

**用途**：把背包宝石镶嵌到指定装备槽（ITEMSYSTEM_PutJewel）。

**请求格式**：`{ "bag": 0, "slot": 3, "equip_slot": 3 }`

**返回格式**：`{"ok":true,"state":<Party 模型>}`

**注意**：无孔→`no socket`；非宝石→`not jewel`；空装备槽→`equip slot empty`；**镶嵌后自动消耗背包宝石（防刷）**。

#### 强化装备

`POST /api/item/inventory/{role}/enchant`

**用途**：用背包中的强化卷轴强化指定装备槽（v0.5.12，复现 UIEquip_ApplyStuff 成功分支：ITEMSYSTEM_EnchantItem 0x10b330 + 成功消耗 INVEN_ConsumeItem 0x1047bc）。

**请求格式**：`{ "bag": 0, "slot": 3, "equip_slot": 5 }`

**返回格式**：`{"ok":true,"state":<Party 模型>}`

**注意**：装备槽空→`equip slot empty`；背包物品非强化卷轴→`not enchant scroll`（IsEnchantScroll 0x10b2f0：武器卷轴 16-20/946、防具卷轴 21-25/947）；不可强化装备→`cannot enchant`（ITEMCLASSBASE bit1/bit2）；失败→`enchant failed`；成功写回 +0x1A bit6-10 新等级且消耗卷轴 1 张（防刷）。

### 4.3 商店 shop

#### 商店商品列表

`GET /api/item/shop/items`

**用途**：获取当前商店商品列表。

**返回格式**：`{ "items": [ { "slot": 0, "category": 5, "count": 1, "price": 15, "name": "恢复药水" }, ... ] }`

**注意**：price = ITEM_GetBuyPrice。

#### 购买商品

`POST /api/item/shop/buy_item`

**用途**：购买当前商店指定商品（绕过 cursor：DEALSYSTEM 表定位 + ITEM_GetBuyPrice + INVEN_SaveItem + MinusMoney）。

**请求格式**：`{ "slot": 0 }`

**返回格式**：`{"ok":true,"state":<Inventory 模型>}`

**注意**：金币不足→`not enough money`；无商品→`item not found`。
---

## 五、quest（任务）— GET/POST /api/quest/*

**任务链路**：接受→进度→交付→放弃。静态数据（QUESTINFOBASE）提供任务文本，动态数据（QUESTSYSTEM）提供接受/进度/完成状态。

#### 任务复合

`GET /api/quest`

**用途**：获取任务信息复合（active/details/completed）。

**返回格式**：`{ "active": [ ... ], "details": [ ... ], "completed": [] }`（active/details/completed 结构见下）

#### 已接任务（active）

`GET /api/quest/active`

**用途**：获取**所有已接任务**（含进度与主线/支线判定）。v0.5.37 起数据源为 QUESTS.json 解析产物（文本已去色）；**v0.5.38 起默认排除游戏面板不显示的任务**（QUESTS.json `hidden=true`，战斗/教学类，与游戏任务面板可见性一致）。

**返回格式**：
```json
{
  "quests": [
    { "slot": 3, "quest_id": 6, "state": 2, "id_name": "去找费罗赛普妮", "name": "去找费罗赛普妮",
      "group_id": 4, "detail": "……", "is_mainline": true, "is_side": false },
    { "slot": 4, "quest_id": 181, "state": 1, "id_name": "弱化的原因", "name": "弱化的原因",
      "group_id": 196, "detail": "黑魔法师卡茵委托你去消灭5只刀针蚊子。……", "is_mainline": false, "is_side": true }
  ]
}
```

**字段说明**：
- `quest_id`：QUESTINFOBASE 记录下标（运行时索引）
- `deliverable`：**可交付判定（v0.5.40）**——`G_NPC_QUEST_STATE[quest_id]==2`（可完成/可交付）为 true，否则 false（0 未接/1 进行/3 已完成均 false，实时读取）；原 `state` 字段 v0.5.40 起不再暴露
- `group_id`：任务组 ID（QUESTGROUPBASE 下标）；`name`/`detail`：任务名/详情（去色文本）
- `is_mainline`：**主线判定（✅ 2026-08-16 定案）** = 任务组 QUESTGROUPBASE 记录 +2 字节 bit0（1=主线，面板显示 main 标识）；`is_side` = 非主线（含支线/重复任务）
- **过滤规则（v0.5.38）**：`hidden=true`（QUESTS.json，= QUESTINFOBASE u16[6] 高字节 bit5，游戏任务面板跳过显示）的任务从结果中剔除
- ⏳ `progress.detail` 进度详情字段**待逆向**（见 backlog Q2）

#### 已接受任务详情（details）

`GET /api/quest/details`（v0.5.41 起，原 `/api/quest/list` 已弃用）

**用途**：获取已接受任务**全量静态字段 + 可交付判定**（QUESTSYSTEM 槽数组 + G_NPC_QUEST_STATE + QUESTS.json 解析产物，v0.5.39 起；v0.5.40 起以 `deliverable` 替代 `state`）。与 `active` 对照：active=面板可见+精简字段，details=全部已接受+完整静态数据。

**返回格式**：`{ "quests": [ { "slot", "quest_id", "deliverable", "group_id", "group_name", "name", "detail", "accepted_dialog", "delivered_dialog", "class_req", "reward_hint", "rewards", "is_mainline", "is_side", "hidden" }, ... ] }`

**字段说明**：
- `deliverable`：**可交付判定**（`G_NPC_QUEST_STATE[quest_id]==2` 为 true，实时读取；v0.5.40 起不再暴露原始 state）
- `group_name`：任务链标题（面板实际显示名，游戏面板按组显示）
- `accepted_dialog`：**接取后对话**（原「进度」text_16）；`delivered_dialog`：**交付对话**（原「完成」text_18）——2026-08-16 定案命名
- `rewards`：奖励数组 `[{item_id, item_name, quantity, class_mask}]`（仅含静态奖励，支线/重复任务奖励多来自事件/其他机制，见 quest.md §2.2）
- `hidden`：任务菜单隐藏标志（u16[6] 高字节 bit5，战斗/教学任务不显示在面板）

**注意**：`quest_id` 为 QUESTINFOBASE **记录下标**（0-506，2026-08-16 定案）；⚠️ 勿与静态表 u16[0] 字段（任务链 ID）混淆——P0 误报即源于此（见 quest.md §2.1/§2.2）；`/api/quest/details` 优先于 `/api/quest/{id}` 匹配。

#### 任务详情（静态）

`GET /api/quest/{id}`

**用途**：获取指定任务**静态数据**（QUESTS.json 解析产物）。只提供可能的静态数据，不提供动态完成状态/进度（动态见 active）。

**返回格式**：
```json
{ "quest_id": 181, "group_id": 196, "group_name": "弱化的原因", "name": "弱化的原因", "detail": "……",
  "accepted_dialog": "……", "delivered_dialog": "……", "rewards": [...], "is_mainline": false, "is_side": true }
```

**注意**：⏳ 当前为 NOT_FOUND 占位（未接线，见 backlog）；数据源已就绪（QUESTS.json）。

#### 已完成任务

`GET /api/quest/completed`

**用途**：获取**所有已完成**任务列表（状态表 state==3 过滤 + QUESTS.json 字段注入，v0.5.37 起）。

**返回格式**：`{ "quests": [ { "quest_id": 181, "name": "弱化的原因", "group_id": 196, "detail": "……", "is_mainline": false, "is_side": true }, ... ] }`

#### 放弃任务

`POST /api/quest/quit_quest`

**用途**：放弃指定任务（QUESTSYSTEM_Find + RemoveSlot）。

**请求格式**：`{ "quest_id": 381 }`

**返回格式**：`{"ok":true,"state":<Player 模型>}`

**注意**：无任务→`quest not found`。
---

## 六、ui（界面与对话）— GET/POST /api/ui/*

**界面交互**：界面状态感知（ui）、对话/弹窗统一检测（dialog）、选项选择（select_option）、NPC 交互（start_interact）。操作前置检查统一看 `dialog.active`（有阻塞弹窗时操作被 UI 阻塞）。

### 6.1 界面状态

#### 界面状态复合

`GET /api/ui`

**用途**：获取界面状态复合（screen/dialog）。

**返回格式**：`<GameState 模型>`

#### 当前界面

`GET /api/ui/screen`

**用途**：获取当前界面名（v0.5.42 枚举：loading/main_menu/world/tutorial_pause/dialog_*/panel_*/main_menu_*）。

**返回格式**：`{ "screen": "world" }`

#### 当前面板

`GET /api/ui/panel`

**用途**：获取当前面板（screen 为 `panel_*` 或 `main_menu_*` 时，返回带前缀值）。

**返回格式**：`{ "panel": "panel_inventory" }` 或 `{ "panel": null }`

### 6.2 对话与弹窗

#### 对话/弹窗状态检测

`GET /api/ui/dialog`

**用途**：统一检测当前对话/弹窗状态（**一个函数覆盖多种类型**），返回类型、标题、内容与可选动作。

**返回格式**：`<DialogContent 模型>`（`type` + `title`/`text` + `options`）

**支持类型**（options 随类型给出；✅ v0.5.6 实机验证，面板态已纳入）：

| type | 场景 | options 示例 |
|---|---|---|
| `popup` | 普通确认弹窗 | `[ok 确认, cancel 取消]` |
| `agreement` | Android 同意页（主菜单前，Java 层） | `[ok 同意]` |
| `story` | 剧情对话 | `[next 下一句, skip 跳过]` |
| `npc` | 商人/村民对话 | 分支选项 `[0..n + close 关闭]`（选择框型，v0.6.6）或 `[next 下一句]`（线性型） |
| `npc_quest` | NPC 任务完成面板 | `[complete 完成任务, close 关闭]` |
| `wipeout` | 死亡面板 | `[revive 复活, special_revive 特殊复活, game_over 游戏结束]` |
| `save_slot` | 存档槽面板 | `[save 存档, close 关闭]` |
| `character_info`/`inventory`/`skills`/`mercenary`/`quests`/`settings`/`shop`/`craft`/`npc_rest`/`npc_revive`/`options`/`shortcut`/`world_map`/`input_count`/`choice`/`daily_reward`/`in_app` | 各可交互面板 | `[close 关闭]` |
| `save` | 保存弹窗 | `[confirm 确认]` |
| `sell` | 出售弹窗 | `[confirm 确认, cancel 取消]` |
| `quest` | 任务对话框（含标题） | `[confirm 确认, quit 退出]` |
| `none` | 无对话 | 空 options |

**注意**：type 检测与 options 生成见 backlog U1/U2（✅ v0.5.6 已实现：面板态经 popup 栈顶 enter 识别，`save_slot` 额外暴露 save；`select_option` 的 close 走 panel/close 官方流程3）。

#### 选择选项

`POST /api/ui/dialog/select`

**用途**：从 `GET /api/ui/dialog/content` 返回的 `options` 中选择一项（**唯一选择端点**，替代确认/取消/按钮/选项选择全部逻辑）。

**请求格式**：`{ "action": "close" }`（`action` 取 dialog.content options 中的一项 id；`index` 可选，NPC 对话选项用 `{"index":n}`）

**返回格式**：`{"ok":true}` 或 `{"ok":false,"error":<原因>}`

**支持动作**（✅ v0.5.6 实机验证）：
- agreement（Java 层同意页，主菜单前）：`ok`（同意——定位同意页内的「同意」元素并点击，返回 `{"ok":true,"result":"tap_dispatched"}`，关闭异步生效，随后轮询 `GET /api/ui/screen` 直到 `main_menu`；若在页面刚出现瞬间点击可能因未就绪被吞掉，重试 `ok` 即可）；其他动作→`no such option in agreement`
- popup：`ok`/`cancel`（UIPopupMsg 官方按钮）
- story：`next`（下一句）/`skip`（跳过）
- npc：`index`（选项选择，选择框型）/`next`（下一句，线性型）/`close`（关闭对话框，v0.6.6）
- npc_quest：`complete`（完成任务）/`close`（关闭）
- wipeout：`revive`/`special_revive`/`game_over`
- 面板态：`close`（关闭面板，panel/close 官方流程3）；save_slot 面板另接受 `save`（存档落盘）

**注意**：`action` 必须匹配当前对话态的 options（不匹配→`no such option in <type>`）；无对话→`no dialog`；Java 层同意页（screen=`agreement`）存在时走 agreement 分支，不进入 native 检测。

#### 开始交互

`POST /api/ui/start_interact`

**用途**：复现官方触摸交互链（v0.5.31）：PLAYER_DoCheckNearNPC 设 NearNPC（空时 type==2 装饰物 fallback 扫描）→ 读 npc+0xa funcDisplay → NPCSYSTEM_CheckFunctionDisplay（>1 报不可交互）→ UINpc_InitNPC → 分支：普通功能（返回值 0）弹 npc 对话框 / 任务交付接取（返回值 1）直接执行。

**请求格式**：无 body

**返回格式**：
- 弹 UI 对象（普通 NPC/商店等）：`{"ok":true,"result":"dialog_shown"}`
- 直接执行对象（路障/宝箱等）：`{"ok":true,"result":"task_executed"}`
- 不可交互对象（火把/出口等纯装饰）：`{"ok":false,"error":"not interactable"}`

**注意**：无 NPC 附近→`no npc nearby`；切图触发的剧情对话无需 interact（自动激活）。路障类「直接执行」对象交互后立即打开 npc_quest 面板（无需再 select index=0）；宝箱类单步直接开箱。

### 6.3 界面操作

#### 回到主菜单

`POST /api/ui/go_main_menu`

**用途**：从世界回到主菜单（GAMESTATE_SetState(4) 正规状态切换）。

**请求格式**：无 body

**返回格式**：`{"ok":true,"state":<Player 模型>}`

**注意**：非 world→`not in game`。**UI 守卫（2026-09-14）**：栈顶是面板时模块先走官方 Pop 关闭并等栈清空再切换；栈顶是非面板弹窗（对话/剧情/数量输入等脚本态）→ `ui occupied: close current dialog first`（fail-closed，不由模块代关）；面板关闭未完成→`panel close pending`。带面板直接切换会触发游戏 `ControlItem_Draw(NULL)` 崩溃，故必须走守卫。

#### 打开面板

`POST /api/ui/open_panel`

**用途**：打开指定面板（UI_SetPopupProcessInfo(1,id) 官方流程1 Push）。

**请求格式**：`{ "panel": "inventory" }`（v0.5.42 起兼容裸名与 `panel_` 前缀，如 `"panel_inventory"`，与 screen 输出对称）

**返回格式**：`{"ok":true,"state":<GameState 模型>}`

**注意**：面板白名单（character_info/choice/inventory/mercenary/quests/settings/skills/wipeout/world_map）真机验证；需游戏内上下文的面板→`panel requires in-game context`。

#### 关闭面板

`POST /api/ui/close_panel`

**用途**：关闭当前面板（UI_SetPopupProcessInfo(3,0) 官方流程3 Pop）。

**请求格式**：无 body

**返回格式**：`{"ok":true,"state":<GameState 模型>}`
---

## 七、system（系统与会话）— GET/POST /api/system/*

**会话/进程级能力**：游戏整体快照、事件流通知、存档（会话持久化）、静态知识库查询（tables，含 text/story-events）、帮助文档。与具体游戏系统解耦；服务健康检查为顶层 `/api/health`。

### 7.0 服务健康 health（顶层）

`GET /api/health`

**用途**：模块服务存活检查（v0.5.8 起只返回 ok，其余属性收编入 `/api/system/info`）。

**返回格式**：`{ "ok": true }`

> **注意**：version/game/base 等属性在 `/api/system/info` 提供（见下）；`version` 跟随构建版本（BuildConfig.VERSION_NAME）。

### 7.1 游戏整体 game

#### 游戏复合

`GET /api/system/game`

**用途**：获取游戏整体信息（snapshot+info）。

**返回格式**：`{ "snapshot": <Snapshot 模型>, "info": <info 对象> }`

#### 全量快照

`GET /api/system/snapshot`

**用途**：获取局内全量快照（含 name 注入）。

**返回格式**：`<Snapshot 模型>`

#### 帧计数

`GET /api/system/game_frame`（v0.5.42 起，原 `/api/system/game/frame` 已迁移）

**用途**：获取游戏帧计数（每帧 +1）。

**返回格式**：`{ "frame": 12345 }`

**注意**：源 [0x2f5648] GOT 槽 u64。

#### 软件信息

`GET /api/system/info`（v0.5.42 起，原 `/api/system/game/info` 已迁移）

**用途**：获取模块/软件信息。

**返回格式**：
```json
{ "version": "0.5.8", "game": "world", "package_name": "com.com2us.inotia4...", "base": 532410707968,
  "save_slots": [{ "slot": 0, "exists": true, "hero_level": 1, "hero_index": 0 }, ...],
  "current_save_slot": 0 }
```

**注意**：`version` 跟随构建版本（BuildConfig.VERSION_NAME）；`game` 为当前 UI 状态（loading/main_menu/world/story）；`save_slots` 为各槽存在性+英雄等级；`current_save_slot`（Int）当前加载存档槽（未加载 -1）。

### 7.2 事件流 events

`GET /api/system/events`

**用途**：轮询获取游戏事件（差异检测，零 hook）。

**返回格式**：
```json
{ "events": [
  { "type": "money", "old": 100, "new": 150 },
  { "type": "hp", "role": 0, "old": 10598, "new": 8000 },
  { "type": "level_up", "role": 1, "old": 10, "new": 11 },
  { "type": "inventory", "old": 13, "new": 12 }
] }
```

**注意**：事件类型 `money`/`hp`/`mp`/`exp`/`level_up`/`move`/`inventory`；需周期性轮询（500ms-1s）；首次调用仅建立基线返回空列表。

### 7.3 存档 save

#### 手动存档

`POST /api/system/save`

**用途**：手动保存当前游戏（SAVE_Save 无参静默保存：SV 校验→全量序列化）。

**请求格式**：无 body

**返回格式**：`{"ok":true,"state":<Player 模型>}`

**注意**：非 world→`not in game`。

#### 进入存档槽

`POST /api/system/enter_slot`

**用途**：直接进入指定存档槽（复现 SaveSlot_SlotButtonExe 链；进入/读档合一）。

**请求格式**：`{ "slot": 0 }`（0/1/2）

**返回格式**：`{"ok":true,"state":<Player 模型>}`

**注意**：非 world 才可调（world 中→`already in game`）；原生 `UIPopupMsg` 弹窗存在时返回 `ui occupied: dialog_popup`，Android 同意页（screen=`agreement`）存在时返回 `ui occupied: agreement`，需先 `dialog/select {"action":"ok"}` 关闭同意页或处理弹窗；进档流程期间（调用后 15s 宽限）同意页启动会被模块拦截，不会打断读档；UI 状态检测失败时 fail-closed 返回 `ui state unavailable`；slot 越界→`bad slot`；空槽、损坏或不兼容存档会在进入前由内部完整性门禁拦截，返回结构化错误，不触发原版进档崩溃路径。当前没有独立的存档预检 HTTP API。

#### 创建新存档

`POST /api/system/create_slot`

**用途**：创建新角色存档并自动进初始营地（复现 SaveSlot_GoToNewGame + SelectCharacter_ButtonStartExe 链）。

**请求格式**：`{ "slot": 1, "class_idx": 2 }`（slot 0/1/2；class_idx 0-5）

**返回格式**：`{"ok":true,"state":<Player 模型>}`

**注意**：创建后自动进初始营地（map_id=0）+ 剧情对话激活（dialog type=story，可 skip）；职业映射 0=黑暗骑士 1=忍者 2=黑魔导法师 3=祭司 4=暗影射手 5=狂战士；新档未保存前槽区 exists=false；与 enter_slot 相同的同意页/弹窗门禁（`ui occupied: agreement` / `ui occupied: dialog_popup` / `ui state unavailable`）及 15s 进档宽限拦截；损坏槽可直接 create_slot 重建覆盖（2026-08-28 save1 实测）。

#### 存档备份（导出 / 列表 / 导入 / 删除）

> 存档管理器：把「原版 `save{slot}.dat` 解密明文 + 模块 sidecar」打包为自包含 `.qol_save` 备份，可还原到任意槽位（原版 + 模块一起）。备份目录 `getExternalFilesDir(null)/save_backup/`。
>
> **备份标识 = `checksum`**：`sha256(origPlain ‖ module)` 的前 12 位小写 hex，即备份文件名 `<yyyyMMdd-HHmmss>_s<slot>_<checksum>.qol_save` 的末段。同一游戏内容的备份 `checksum` 恒相同；导入/删除均按 `checksum` 定位备份，不再使用文件名。
>
> 导入导出时机与责任由调用方负责，模块只保证原子写、CRC 校验与内容完整。

##### 导出备份

`POST /api/system/backup/export`

**用途**：把指定槽导出为 `.qol_save` 备份（原版存档解密为明文 + 模块 sidecar 原始字节 + 元数据 + CRC32）。

**请求格式**：`{ "slot": 0 }`（0/1/2）

**返回格式**：

```json
{ "ok": true, "backup": <BackupMeta> }
```

去重命中（同 `checksum` 备份已存在）时不写新文件，返回既有备份的 meta 并附加 `"deduplicated":true`：

```json
{ "ok": true, "backup": <BackupMeta>, "deduplicated": true }
```

**注意**：先算出 `checksum`，再扫描 `save_backup/*.qol_save`：已存在同 `checksum` 备份 → 不写入新文件直接返回；不存在 → 按命名规则写入。读取已落盘的 `save{slot}.dat` 与 sidecar，**不触发游戏保存**，任意时刻可调；备份内原版明文的槽位字段归一化为 `0xFF`，导入时写入目标槽。slot 越界→`bad slot`；槽为空或读取失败→`load failed`；写盘失败→`bundle write failed`。

##### 备份列表

`GET /api/system/backup/list`

**用途**：列出 `save_backup/` 下全部备份，按导出时间倒序。

**返回格式**：`{"ok":true,"backups":[<BackupMeta>,...]}`

**注意**：只读文件头元数据，不解密正文；`checksum` 从 bundle 的 metaJson 读取（文件名解析兜底）；损坏文件跳过并记日志。

##### 导入备份

`POST /api/system/backup/import`

**用途**：把备份还原到指定槽位：原版存档按目标槽改写槽位字段并重加密写回 `save{slot}.dat`，模块 sidecar 一并写入。

**请求格式**：`{ "checksum": "8f03df248aec", "slot": 1 }`

**返回格式**：`{"ok":true,"backup":<BackupMeta>}`

**注意**：按 `checksum` 定位 bundle。校验值非法→`bad checksum`；无此备份→`backup not found`；文件损坏→`backup unreadable or corrupt`；重加密失败→`encrypt failed`；原版写盘失败→`save file write failed`；sidecar 导入失败→`module container import failed`。写入前把目标槽现存原版文件与 sidecar 复制到 `save_backup/.rollback/`，任一步失败自动还原，成功后清理回滚副本；导入不自动刷新游戏内存态，主菜单下一次 `enter_slot`/`system/info` 会重载。

##### 删除备份

`POST /api/system/backup/delete`

**用途**：按 `checksum` 删除备份文件（bundle 自包含，删除单文件即同时移除原版 + 模块两部分）。

**请求格式**：`{ "checksum": "8f03df248aec" }`

**返回格式**：`{"ok":true}`

**注意**：校验值非法→`bad checksum`；无此备份→`backup not found`；删除 IO 失败→`delete failed`（记日志）。

**BackupMeta 字段**：`file_name`（含导出时间与 `checksum` 前缀）、`size_bytes`、`source_slot`（导出源槽）、`export_time`（ms）、`map_id`、`hero_level`、`hero_index`、`save_version`、`save_time`、`original_sha256`、`module_sha256`、`checksum`（备份标识，12 位小写 hex）。

### 7.4 静态数据表 tables

> 静态知识库：游戏静态表 + 多语言文本（text）+ 剧情事件（story-events）统一作为 table 查询。

#### 静态表列表

`GET /api/system/tables`

**用途**：获取可用静态表列表。

**返回格式**：`{ "tables": [ "ITEMDATABASE", "MONDATABASE", "TEXT", "STORY-EVENTS", ... ] }`

#### 指定静态表

`GET /api/system/tables/{table}`

**用途**：获取任意静态表全量数据（表名大写，如 `ITEMDATABASE`）。

**返回格式**：`{ "records": [ <表记录>... ] }`

**注意**：表名自动转大写；不存在的表返回 `{"error":"not found"}`；特殊表 `text`（需 lang 参数）与 `story-events`。

#### 表内搜索

`GET /api/system/tables/{table}/search?q=治疗`

**用途**：表内名称模糊搜索。

**返回格式**：`{ "items": [ { "index": 788, "name": "鑫迪的治疗药", "raw": { "u16": [818, ...], "text_0": "鑫迪的治疗药" } } ] }`

**注意**：参数 `q` 必填；搜索字段为表记录名称字段（text_0）。

#### 下载静态表

`GET /api/system/tables/{table}/download`

**用途**：下载静态数据表文件（原始 JSON，供离线使用）。

**返回格式**：静态表 JSON 文件内容

**注意**：⏳ 占位（暂不实现，与 /api/system/download 一致）。

#### 多语言文本

`GET /api/system/tables/text?lang=zh-Hans`

**用途**：获取指定语言的全部文本（text 作为 table 一员）。

**返回格式**：`<text/{lang}.json 内容>`

**注意**：参数 `lang` 必填；支持 `zh-Hans`/`en`。

#### 剧情事件

`GET /api/system/tables/story-events`

**用途**：获取剧情事件静态数据（命令/条件/文本，story-events 作为 table 一员）。

**返回格式**：`<events.json 内容>`（608 事件 + 28598 命令）

### 7.5 帮助文档 help

#### 帮助文档

`GET /api/system/help`

**用途**：获取模块使用帮助文档（API 概览/示例）。

**返回格式**：`{ "help": "……" }`

**注意**：⏳ 占位（帮助文档内容待提供）。

#### 下载帮助文档

`GET /api/system/download`

**用途**：下载帮助文档文件。

**返回格式**：帮助文档文件

**注意**：⏳ 占位（帮助文档内容待提供）。

### 7.6 模块配置 config（v0.5.22）

> 模块级配置的读取与修改。配置为纯 Kotlin 层能力，**不走 ControllerGuard**——
> native 未就绪时配置端点同样可用（如修改监听端口解决端口冲突）。
> **外部存储 `getExternalFilesDir(null)/config.json` 为唯一配置来源**（v0.5.21 起，
> 与日志同目录，`/sdcard/Android/data/<游戏包>/files/config.json`，用户可见可编辑）：
> - 外部文件存在 → 读取生效
> - 外部文件不存在/损坏 → 使用默认值，并**立即写入外部 config.json**
> - 每次 `POST /api/config/set` 修改立即持久化到该文件；删除外部文件即恢复出厂默认
> - `stackLimitIncrease` 变化时通知 native 生效（切换运行时操作视图与 99/999 clamp），无需重启；sidecar 数量位解码永远按 S2 布局、与该开关无关；sidecar 读档按绝对 `0..999` 校验，不改写、不因当前配置截断已有数量；可堆叠 payload 仅在与 descriptor 不一致时同步
> - **数量编码 S2（当前实现，无版本标识）**：sidecar `extensionbags.items` 状态 JSON 不携带编码版本字段；数量位只有 S2 布局（高位段 `a`=bits22-24、低位段 `b`=bits25-31，`count=128a+b`，业务上限 999）一种，持久化与 descriptor 读写统一 `s2_read_count`/`s2_write_count`，运行时操作面统一经模式感知层 `effective_read_count`/`effective_write_count`/`effective_clamp`/`effective_view_count`（R-47 决策 b）；历史 sidecar 若携带 `encodingVersion` 字段，解析时宽容忽略（不报错、不迁移）；无 S1→S2 迁移代码，原版袋 `+0x10` 无迁移扫描；非可堆叠类别（宝石选项/袋容量/装备 marker）不参与数量位段改写。`/api/item/*` 的 `count` 字段为当前模式的运行时视图：启用态 S2 全量 `0..999`、关闭态低 7 位 `b`（`count mod 128`，`a` 保留、重开恢复完整值）；API 结构不变。详见 `docs/development/features/extension-bag/module-save-store.md` §6.4 与规则册 R-45..R-49
> - `moveMergeEnabled` 控制背包内拖拽同类可堆叠物品时的自动合并，默认 `false`；变化即时安装或还原 native GOT hook，无需重启
> - `autoSellEnabled` 自动出售全局开关，默认 `false`；变化即通知 native（进档后由存档内总开关共同决定扫描任务）
> - `apiEnabled` API 全局开关，默认 `true`；`false` 时不启动 HTTP 服务与 native 缓存预取线程（`nativeSetApiEnabled(false)` → `frame_cache_stop()`），`GET/POST /api/config/*` 随之不可达。**`apiEnabled=false` 不影响 feature 初始化**——`ApiServer.bootstrap` 已把 feature 初始化（设置页/扩展背包/存档/自动出售等）与 HTTP 启动拆开，关闭仅停 HTTP 与预取线程。关闭后唯一恢复通道为**游戏内设置页第一项「API服务」开关**（走 `ModuleConfigUiBridge` JNI，不依赖 HTTP；false→true 经 `ApiServer.startFromConfig` 重启 HTTP）；由 HTTP 置 `false` 时延迟约 500ms 停止以先送回本响应
> - `debugLogEnabled` 统一日志 debug 级别运行时开关，默认 `false`；变化即时经 `nativeQolLogSetDebugEnabled` 下发 native 门控，无需重启。info/warn/error 不受该开关影响（常开）。接入游戏内**设置页第 8 行「调试日志」**；日志格式与规则见 `docs/development/logging.md`

#### 读取配置

`GET /api/config/list`

**用途**：获取当前生效的模块配置。

**返回格式**：

```json
{
  "listenAddress": "0.0.0.0",
  "listenPort": 8088,
  "stackLimitIncrease": false,
  "moveMergeEnabled": false,
  "opEnabled": false,
  "autoSellEnabled": false,
  "apiEnabled": true,
  "debugLogEnabled": false
}
```

#### 设置配置

`POST /api/config/set`

**用途**：设置模块配置。**只更新请求体中出现的字段**，未出现的字段保持不变；校验失败时整体不生效。
成功时立即持久化到 config.json。

**请求格式**：

```json
{ "listenAddress": "0.0.0.0", "listenPort": 9090, "stackLimitIncrease": true, "moveMergeEnabled": false, "debugLogEnabled": true }
```

**返回格式**：

```json
{
  "ok": true,
  "restart": true,
  "listenAddress": "0.0.0.0",
  "listenPort": 9090,
  "stackLimitIncrease": true,
  "moveMergeEnabled": false,
  "opEnabled": false,
  "autoSellEnabled": false,
  "apiEnabled": true,
  "debugLogEnabled": true
}
```

**注意**：
- `listenAddress` 非空必填；`listenPort` 合法范围 1-65535，越界返回 `{"ok":false,"error":"listenPort must be 1-65535"}`；body 非法返回 `{"error":"bad request"}`；持久化失败返回 `{"ok":false,"error":"config save failed"}`（内存不提交）
- `restart` 字段：`listenAddress`/`listenPort` 有变化时为 `true`（并自动重启 HTTP 服务生效，延迟约 500ms 先让本响应送达）；仅改 `stackLimitIncrease`/`moveMergeEnabled`/`opEnabled`/`autoSellEnabled`/`apiEnabled`/`debugLogEnabled` 时 `restart=false`
- `apiEnabled=false` 时延迟约 500ms 停止 HTTP 服务（先让本响应送达），此后 `GET/POST /api/config/*` 不可达；重新开启须经游戏内设置页（HTTP 已不可达）
- 重启后服务按新地址/端口监听，**旧端口的连接会断开**——改端口后请用新端口访问
- **端口被占用（v0.5.22 修复）**：新端口绑定失败时自动**回退默认端口 8088** 重建服务，并同步把配置写回默认端口（config.json 同步修正，重启进程不会再次失败）；回退日志见 `inotia4-export.log`。`listenAddress` 非法时回退通配绑定（0.0.0.0，端口用配置值）
---

## 八、op（越权操作）— POST /api/op/*

游戏内做不到的操作（改数据/强行操作）。**OP 全局开关（v0.5.47）**：默认关闭；`config.json` 中 `opEnabled=true` 或 `POST /api/config/set {"opEnabled":true}` 开启后，§8.1 各端点才可用；开关关闭时所有 OP 端点返回 **403 + `{"ok":false,"error":"op disabled"}`**。门禁在 OpApiService 统一入口（controller 无法绕过）。OP 端点与合法操作物理隔离。

### 8.1 已实现

#### 设置血量

`POST /api/op/character/{role}/hp`

**请求格式**：`{ "hp": 8000 }`

**返回格式**：`{"ok":true,"state":<Party 模型>}`

**注意**：截断到 0..max_hp。

#### 设置魔力

`POST /api/op/character/{role}/mp`

**请求格式**：`{ "mp": 200 }`

**返回格式**：`{"ok":true,"state":<Party 模型>}`

**注意**：截断到 0..max_mp。

#### 设置经验

`POST /api/op/character/{role}/experience`

**请求格式**：`{ "exp": 12000 }`

**返回格式**：`{"ok":true,"state":<Party 模型>}`

**注意**：CHAR_SetExperience 直接写经验，不触发升级结算。

#### 设置等级

`POST /api/op/character/{role}/level`

**请求格式**：`{ "level": 30 }` 或 `{ "level": 200, "force": true }`

**返回格式**：`{"ok":true,"state":<Party 模型>}`

**注意**：CHAR_SetLevel 完整升级结算（写等级+重算 nextExp+InitializeFromLevel+升级加点+回满血蓝）；降级→`level down not allowed`。
- **默认校验 1-105**（游戏等级上限，EXP 表 105 级；超出报 `level 1-105`）
- **`force:true` 跳过校验**（v0.5.11）：⚠️ 等级存储字段 `[ch+0xe]` 是 **s8（int8）**，>127 会溢出为负（实测 200 → -56）；force 只放开校验，溢出结果由调用方自担

#### 批量设置基础属性

`POST /api/op/character/{role}/set_attr`

**请求格式**：`{ "stats": { "strength": 10, "agility": 7 } }` 或 `{ "stats": { "0": 10, "3": 7 } }`（键可用主属性英文名 `strength`/`agility`/`vitality`/`intelligence`/`spirit` 或索引 0-4，**可只传部分**）

**返回格式**：`{"ok":true,"set":[{"attr":1,"value":12},{"attr":4,"value":15}]}`

**注意**：v0.5.8 由 `/attr/{index}` 单值端点改版；CHAR_SetStatBase 直写基础属性（0..255，骰子同路径，持久化已验证）；总属性 = 基础+分配+加成+动态；非法键/越界值报 `bad attr`/`bad value`。

#### 添加物品

`POST /api/op/inventory/add`

**请求格式**：`{ "category": 1, "count": 5 }`

**返回格式**：`{"ok":true,"state":<Inventory 模型>}`

**注意**：ITEMSYSTEM_CreateItem + INVEN_SaveItem；`stackLimitIncrease=false` 时可堆叠上限 99，开启时上限 999；两种状态都使用固定 bit22-31 格式；背包满→`inventory full`。

#### 修改金币

`POST /api/op/inventory/money`

**请求格式**：`{ "money": 99999 }`（绝对值设置，非增量）

**返回格式**：`{"ok":true,"state":<Inventory 模型>}`

**注意**：nativeOpSetMoney 直写金币绝对值（v0.5.47 接线，D4）。

#### 设置属性点

`POST /api/op/character/{role}/status-point`

**请求格式**：`{ "points": 5 }`

**返回格式**：`{"ok":true,"state":<Party 模型>}`

**注意**：nativeOpSetStatusPoint 直写可分配属性点（v0.5.47 接线，D4）。

#### 队伍换位

`POST /api/op/party/swap`

**请求格式**：`{ "a": 0, "b": 1 }`（两个队伍槽位）

**返回格式**：`{"ok":true,"state":<Party 模型>}`

**注意**：nativeOpPartySwap 交换两槽位角色（v0.5.47 接线，D4）。

#### 传送/切图

`POST /api/op/movement/teleport`

**请求格式**：`{ "map_id": 20, "x": 15, "y": 10 }`

**返回格式**：`{"ok":true,"state":<Map 模型>}`

**注意**：nativeOpTeleport 传送至指定地图坐标（v0.5.47 接线，D4）；`map_id`/`x`/`y` 均必填。**坐标语义（P1-47 真机验证）**：`map_id>0` 时 `x`/`y` 为**瓦片索引**（走 fn_change_map 切图）；`map_id=0` 时 `x`/`y` 为**像素坐标**（走 fn_set_position 原地传送）。

### 8.2 未实现

以下端点结构已定稿，待开发（需权限获取机制 + 端点组）：

| 路径 | 用途 | body |
|---|---|---|
| `POST /api/op/quest/accept` | 接取任务（绕过 NPC） | `{"quest_id","force":false}` |
| `POST /api/op/quest/complete` | 完成任务（无视条件） | `{"quest_id"}` |
| `POST /api/op/character/{role}/skill-point` | 设置技能点 | `{"points":N}` |
| `POST /api/op/character/{role}/skill-level` | 设置技能等级 | `{"action_id","level":N}` |
| `POST /api/op/inventory/set-slot` | 格子设物品+数量 | `{"bag","slot","itemId","count"}` |
| `POST /api/op/inventory/set-equip` | 修改装备属性 | `{"bag","slot"}`+属性参数 |
| `POST /api/op/craft/mix-direct` | 免配方机直合成 | `{"recipeId","resultCount"}` |
| `POST /api/op/combat/{role}/heal` | 回血回蓝 | `{"hp","mp"}` |
| `POST /api/op/combat/{role}/rest` | 休息恢复 | — |
| `POST /api/op/combat/{role}/revive` | 复活 | — |
| `POST /api/op/combat/{role}/hate` | 仇恨操作 | `{"targetId","value"}` |

---

## 九、debug（调试）

`GET /api/debug/ui`

**用途**：获取调试用 UI 状态原始 JSON。

**返回格式**：`<原始 gamestate JSON>`（含全部 native 字段）

**注意**：DebugController，不走 ControllerGuard（native 未就绪时直接返回原始数据）。

---

`GET /api/debug/path?tx=&ty=`

**用途**：直接调用 `nav_bfs` 返回玩家到目标 tile 的**完整寻路结果 + 阻挡信息**（排查 BFS 导航/尸体阻挡/模块瓦片建模 vs 引擎碰撞差异的观测手段）。

**参数**：`tx`/`ty` 为目标 **tile 坐标**（非像素；tile = 像素 ÷ 16）。

**返回格式**：

```json
{ "start": { "tx": 7, "ty": 19, "x": 120, "y": 312 },
  "target": { "tx": 30, "ty": 30, "x": 488, "y": 488 },
  "found": true,
  "distance": 23,
  "path": [ { "tx": 8, "ty": 19, "dir": 3 }, "..." ],
  "nearest": { "tx": 24, "ty": 19, "distance": 18 },
  "unit_blocks": [ { "tx": 10, "ty": 19, "slot": 9, "type": 1, "hp": 0 }, "..." ],
  "static_block_count": 123 }
```

- `path`：路径 tile 序列（含每步方向 `dir`，0=下 1=左 2=上 3=右）
- `nearest`：不可达时的最近可达 tile（`found=false` 时非 null，供排查重规划 resume 格）
- `unit_blocks`：被单位占用的 tile 列表（`hp=0` 为尸体，供排查尸体阻挡）
- `static_block_count`：静态瓦片阻挡总数（全量 4096 tile 不逐一输出）

**注意**：DebugController，不走 ControllerGuard。

---

#### 扩展背包调试端点（v0.6.16 自 `/api/extension_bag/*` 迁入）

> 扩展背包**数据与移动**的正式操作面是 §4.1/§4.2 的原版背包 API（bag 6..10 并入）；以下端点仅服务**视图/选中**控制与内部状态诊断（`mode/selected/inspected/pending/recovery_action` 等），供开发期与控制面阶段验收使用，不构成发布操作面。编号约定与原型阶段声明见 §4.1。

`GET /api/debug/extension_bag/status`

**用途**：扩展背包内部状态全量读取（使能/注入/控件旗标、视图模式、全部物品、pending 事务与恢复动作预测）。

**返回格式**：`{ "enabled": bool, "injected": bool, "extension_tab_button": bool, "inventory_frame_active": bool, "recovery_action": "none|complete|rollback", "state": { "mode": "original|module|exiting_module", "selected": -1|0..4, "items": [[{slot,category,count,payload(base64)}]], "pending": {...} } }`；非 world 态 `state:{}`。

`POST /api/debug/extension_bag/enter_view`　body `{ "bag": 6..10 }`

**用途**：进入扩展逻辑袋视图（等价点击扩展 tab）。错误：`already in extension view` / `exit in progress` / `extension bag disabled` / `not in game`。

`POST /api/debug/extension_bag/exit_view`　body 可省略

**用途**：退回原版背包视图。错误：`not in extension view` / `exit in progress`。

`POST /api/debug/extension_bag/select_bag`　body `{ "bag": 6..10 }`

**用途**：扩展视图内切换逻辑袋。错误：`not in extension view` / `bag already selected`。

`POST /api/debug/extension_bag/click_item`　body `{ "bag": 6..10, "slot": 0..15 }`

**用途**：选中扩展物品并返回信息（含完整 payload）；空槽 → `{"ok":true,"item":null}`。错误：`bag not selected` / `not in extension view`。

`POST /api/debug/extension_bag/equip`　body `{ "index": 0..4, "bagType": N }`

**用途**：开发期测试注入：设置扩展袋装备类型（`set_test_equipped`）。

`POST /api/debug/extension_bag/item`　body `{ "index": 0..4, "slot": 0..15, "category": N, "count": N }`

**用途**：开发期测试注入：写入扩展袋测试物品（`set_item`）。

---

## 十、部署形态差异

| 项 | 手机版（LSPosed 模块） | 服务器版（LSPatch 集成） |
|---|---|---|
| 数据获取 | 同进程 Hook | 同进程 Hook（LSPatch 注入） |
| API 访问 | 监听 0.0.0.0，局域网 Wi-Fi IP | Waydroid NAT，需端口转发 |
| minSdk | Android 11+（Zygisk-LSPosed） | 随游戏 targetSdk 29 |
| 静态数据 | JSON 库随模块打包 | JSON 库随集成 APK 打包 |
