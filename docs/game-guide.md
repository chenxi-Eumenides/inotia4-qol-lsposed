# 游戏指南与 API 操作手册（Inotia 4 远程操控）

> **本文档 = 游玩说明**：介绍《艾诺迪亚4》怎么玩，以及如何通过 HTTP API 远程操控游戏（端点/请求/参数/返回格式/用途）。只讲游玩，不讲实现。
>
> **AI 首次阅读建议（按顺序）**：① 第 1.5 节理解游戏核心 → ② 第 3.4 节记住游玩注意点 → ③ 第 4 章跑通基础流程 → ④ 第 11 章套用决策循环。端点细节用到时再查第 6 章。

---

## 目录

1. [游戏介绍](#1-游戏介绍)
2. [连接游戏](#2-连接游戏)
3. [通用约定](#3-通用约定)
4. [快速上手](#4-快速上手5-分钟从零开始)
5. [数据模型](#5-数据模型返回-json-结构)
6. [端点全表](#6-端点全表按域分组)
7. [玩法指南](#7-玩法指南用-api-完成游戏内目标)
8. [越权操作（OP）](#8-越权操作op-端点)
9. [静态数据表](#9-静态数据表查询)
10. [事件流](#10-事件流感知游戏变化)
11. [AI 代理决策建议](#11-给-ai-代理的决策建议)
12. [常见问题](#12-常见问题与注意事项)
13. [附录：参考表](#13-附录参考表)

---

## 1. 游戏介绍

### 1.1 基本信息

| 项 | 内容 |
|---|---|
| 游戏 | 《艾诺迪亚4》（Inotia 4），Com2uS，Android 单机 ARPG |
| 对接版本 | 盗版大修 20260704（离线、无网络验证） |
| 玩法类型 | 暗黑破坏神风格：点击移动 + 即时战斗 + 技能释放 |
| 队伍 | 1 名主控角色 + 2 名佣兵 = **3 人出战**，可随时切换主控 |
| 地图 | 64×64 瓦片矩阵，出口区域自动切图 |
| 等级上限 | 105 级 |
| 存档 | 3 个存档槽 |

### 1.2 六大职业

| 职业 | 特点 |
|---|---|
| 黑暗骑士 | 物理坦克 |
| 忍者 | 高暴击物理输出 |
| 黑魔导 | 魔法输出 |
| 祭司 | 治疗辅助 |
| 暗影猎手 | 远程物理 |
| 狂战士 | 近战爆发 |

> 无转职；每职业约 15 个技能（全游戏 90+）。

### 1.3 核心系统

| 系统 | 说明 |
|---|---|
| 角色养成 | 等级/经验/属性点/技能点 |
| 队伍/佣兵 | 3 人出战、佣兵入队/离队/遣散 |
| 战斗 | 普攻/技能/自动反击 |
| 装备 | 10 槽位、强化/镶嵌/附魔、稀有度 5 档 |
| 背包/物品 | 6 袋×16 槽、使用/丢弃/出售/移动 |
| 货币 | 金币 |
| 商店 | NPC 商店买卖（价格由游戏定） |
| 任务 | 主线/支线 NPC 任务 |
| 地图/移动 | 瓦片/出口/寻路/切图 |
| 对话/剧情 | NPC 对话树、剧情过场、弹窗 |
| 存档 | 3 槽、保存/进出档 |
| 合成/炼金 | 配方合成（未开放） |
| 内购/每日奖励 | 已屏蔽（不影响游玩） |
| 附魔 | 大修版新增：镶满 4 宝石触发神秘附魔 |

### 1.4 盗版大修 20260704 关键修改（相对原版）

- 经验获取 **-30%**
- 强化上限 12 阶、失败不扣强化次数；混沌/深渊合成 **100% 成功**
- 掉率提升（基础 5 倍；Lv60+ 装备紫装概率提升）
- **新增附魔系统**（原版无）：镶嵌 4 宝石触发附魔，25% 概率完美附魔
- 陨子（属性骰子）投掷：初始属性最大，升级随机加点
- 背包"粉碎"改"售卖"（售价原 70%）
- 内购/支付已断

> ⚠️ 数值/机制与原版 wiki 资料存在偏差，以游戏实际为准。

### 1.5 游戏核心理解（给第一次接触此游戏的 AI）

**一句话**：这是一个"接任务 → 打怪 → 升级/掉装备 → 换更强装备 → 推进地图与剧情 → 变强"的暗黑类 ARPG。你操控 3 人队伍（1 主控 + 2 佣兵）在俯视角地图上移动、遇怪即时战斗、捡装备、找 NPC 交任务。

**AI 必须理解的五个要点**：

1. **生存第一**：角色会**死亡**。全队死亡后回到主菜单，**未保存的进度全部丢失**（回到上次存档点）。所以：① 血少要喝药/逃跑；② 频繁存档。
2. **战斗是核心玩法**：怪会主动攻击你。**被攻击扣血时不还手会很快被打死**——要么开启自动反击（`set_auto_attack`），要么主动 `attack_target` 进入战斗。
3. **成长靠三件事**：**推进任务**（获得经验+奖励+解锁内容）、**打怪**（掉装备+升级）、**换装**（穿上更好的武器/防具变强）。三者都重要，别只做一样。
4. **游戏会随时打断你**：剧情对话（`screen=dialog_story`）、弹窗确认（`screen=dialog_popup`）、死亡面板（`screen=dialog_wipeout`）、药水教学暂停（`screen=tutorial_pause`）都可能突然出现，**阻塞你的操作**。每次行动前都要查当前 `screen`/`dialog`。
5. **移动是异步的**：`move_to` 只是发起寻路，角色需要几秒走完。**调用后要轮询确认到达**（对比坐标/距离），不能假设立即到达。

---

## 2. 连接游戏

### 2.1 服务地址

游戏运行时，HTTP 服务监听在手机局域网地址的 **8088 端口**：

```
http://<手机局域网IP>:8088
```

- 手机 IP 查看：设置 → WLAN → 详情
- 调用方（电脑/AI）与手机需在同一局域网

### 2.2 健康检查

```bash
curl http://<手机IP>:8088/api/health
```

成功返回：`{"ok":true}`

### 2.3 安全提示

- **无鉴权**：局域网内任何设备可访问全部**合法**端点。仅在可信局域网使用。
- **OP 越权端点受 `opEnabled` 全局开关门禁**（默认关闭，未开启返回 403）——见第 8 章；开启后同样无额外鉴权。
- **明文 HTTP**，勿在公共网络使用。

---

## 3. 通用约定

### 3.1 请求/响应

- 请求、响应均为 **JSON**（POST body 为 JSON 字符串，建议带 `Content-Type: application/json`）
- **GET** = 读数据；**POST** = 执行操作
- 操作成功：`{"ok":true,"state":<快照>}`（`state` 为操作后最新状态，类型见各端点）
- 操作失败：`{"ok":false,"error":"<原因>"}`
- 游戏未就绪：所有端点返回 `{"error":"not ready"}`（游戏刚启动，稍后重试）
- 不在游戏内：多数读端点返回 `{"error":"not in game"}`

### 3.2 索引约定

| 概念 | 范围 | 说明 |
|---|---|---|
| `role` | 0..2 | 出战槽位 |
| `bag` | 0..5 | 背包袋 |
| `slot` | 0..15 | 袋内槽位 |
| `equip_slot` | 0..9 | 装备槽（见 5.2 位置映射） |
| `class_idx` | 0..5 | 职业（见 1.2） |
| `direction` | 0..3 | 0=下 1=左 2=上 3=右 |
| `mercenary_slot` | — | 佣兵槽 ID（见 6.2 两套索引说明） |

### 3.3 操作前置检查（重要！）

- **UI 占据**：`GET /api/ui/screen` 返回 `dialog_*`（对话框类）/`panel_*`（面板类）/`main_menu_*`（主菜单面板）前缀时，UI 被占据，**世界操作（移动/战斗/交互/物品）被阻塞**，返回 `{"ok":false,"error":"ui occupied: <screen>"}`。操作前先查 `screen`，必要时先处理对话/关闭面板。
- **屏幕状态**：只有 `screen="world"` 才能执行战斗/移动/背包操作；`"main_menu"` 只能做存档操作；`"dialog_story"` 表示剧情对话中（先 `select action=skip`）。

### 3.4 游玩注意点（AI 必读）

> 这些是 AI 代理游玩时**最容易犯错**的地方，先读完再动手。

1. **随时留意游戏状态，查看当前 screen**：游戏可能随时插入突发事件——剧情对话（`dialog_story`）、确认弹窗（`dialog_popup`）、死亡面板（`dialog_wipeout`）、教学暂停（`tutorial_pause`）。**每次行动前都先 `GET /api/ui/` 看 `screen`**（`dialog_*`/`panel_*`/`main_menu_*` 前缀 = UI 被占据）；行动过程中被打断要立即处理，否则后续操作全部无效或被阻塞。

2. **被攻击扣血时，自动开始战斗**：怪会主动打你，**站着不动会被打死**。做法：
   - 开怪前先 `POST /api/character/combat/{role}/set_auto_attack {"on":true}`（受击自动还击）
   - 发现血量持续下降（`/api/system/events/` 有 `hp` 事件或 snapshot 中 hp 减少）→ 立即 `POST /api/character/combat/0/attack_target` 反打最近的敌人，而不是继续移动/发呆
   - 血低于 30% → 先 `use_item` 喝药，再考虑撤退

3. **推进任务、打怪掉装备与升级、装备更好的武器防具，都很重要**：三者互相促进——任务给经验和奖励、打怪掉装备给经验、换好装备才能打更强的怪。建议节奏：
   - 优先做任务（`/api/quest/active` 看当前任务 → 走到任务点 → `start_interact` → 对话推进）
   - 顺手清掉路径上的怪（升级 + 掉装备）
   - 定期看背包（`/api/item/inventory/items`），把 `rarity`/属性更好的装备 `equip_item` 换上

4. **记得保存，否则死亡后丢失进度**：死亡（`wipeout`）会回到**上次存档点**，中间所有操作白费。建议：
   - **每个里程碑后立即 `POST /api/system/save`**：升了级、换了装备、交完任务、捡到好装备之后
   - 死亡后：`dialog/select {"action":"game_over"}` 回主菜单 → `enter_slot` 重新进档（回到存档点）

5. **不要按次数循环调用，否则中途出状况会卡死在循环里**：不要写 `for i in range(100): move_to(...)` 这种固定次数循环。游戏状态随时会变（弹窗/死亡/切图/剧情），固定次数会一直撞墙。正确做法：
   - **状态驱动**：每轮循环先重新查询 `screen`/`dialog`/`snapshot`，根据**当前实际状态**决定下一步
   - **带超时与退出条件**：循环要有"目标达成"和"尝试上限/超时"两个出口，避免死循环
   - 见第 11 章伪代码模板

6. **留意寻路是否完成**：`move_to` 只是发起寻路，角色需要时间走完（正常走路，非瞬移）。**不要调用一次就以为到了**。正确做法：
   - 发起 `move_to` 后，轮询 `/api/system/snapshot` 的 `x`/`y` 或 `/api/world/map/units` 确认位置
   - 判定到达：坐标与目标点距离足够近（像素差 < 16，即一个格子），或 `move_to` 返回后再等几秒复查
   - 超时（如 10 秒）仍未到达 → 用 `/api/world/map/distance` 重新确认可达性，不可达则放弃换目标

---

## 4. 快速上手：5 分钟从零开始

> 把 `<手机IP>` 换成实际地址。全部命令可直接复制。

### 步骤 0：确认服务在线

```bash
curl http://<手机IP>:8088/api/health
```

### 步骤 1：查看游戏信息与存档槽

```bash
curl http://<手机IP>:8088/api/system/info
```

```json
{
  "version": "0.6.6",
  "save_slots": [{ "slot": 0, "exists": false }],
  "current_save_slot": -1,
  "package_name": "com.com2us.inotia4...",
  "base": 532410707968
}
```

### 步骤 2：创建新角色 或 进入已有存档

**新建**（槽 1，职业 2=黑魔导）：

```bash
curl -X POST http://<手机IP>:8088/api/system/create_slot \
  -H "Content-Type: application/json" \
  -d '{"slot":1,"class_idx":2}'
```

**进入已有存档**（先确认 `save_slots` 中该槽 `exists=true`，否则会崩溃）：

```bash
curl -X POST http://<手机IP>:8088/api/system/enter_slot -d '{"slot":0}'
```

### 步骤 3：看全局状态

```bash
curl http://<手机IP>:8088/api/system/snapshot
# {"frame":N,"screen":"world","money":N,"map_id":N,"x":N,"y":N,
#  "leader_slot":0,"party":[{name,type_name,class_name,level,hp,max_hp,mp,max_mp,exp,exp_next,main_stats,status_point,equipment,type,class_idx,name_id,stats}]}
```

### 步骤 4：移动

```bash
# 寻路移动到像素坐标（正常走路，非瞬移）
curl -X POST http://<手机IP>:8088/api/world/movement/move_to -d '{"x":320,"y":480}'

# 或方向键持续移动后停止
curl -X POST http://<手机IP>:8088/api/world/movement/walk_dir -d '{"direction":3}'
sleep 1
curl -X POST http://<手机IP>:8088/api/world/movement/stop_move
```

### 步骤 5：找敌人并攻击

```bash
# 开打前先开启自动反击（受击自动还击，防止被怪打不还手）
curl -X POST http://<手机IP>:8088/api/character/combat/0/set_auto_attack -d '{"on":true}'

# 查看周围单位（/api/world/map/enemies 按 type==1 过滤出战斗单位）
curl http://<手机IP>:8088/api/world/map/enemies
# {"units":[{"slot":12,"x":480,"y":400,"type":1,"status":2,"level":3,"hp":120,...}]}

# 攻击（target_slot 用上面的 slot）
curl -X POST http://<手机IP>:8088/api/character/combat/0/attack_target -d '{"target_slot":12}'

# 停止战斗
curl -X POST http://<手机IP>:8088/api/character/combat/0/stop_combat
```

> ⚠️ **被怪攻击时务必反打**：站着不动会被打死。血低于 30% 先喝药，再考虑撤退。

### 步骤 6：使用物品回血

```bash
curl http://<手机IP>:8088/api/item/inventory/
# 找到药水位置后：
curl -X POST http://<手机IP>:8088/api/item/inventory/use_item -d '{"bag":0,"slot":3}'
```

### 步骤 7：存档

```bash
curl -X POST http://<手机IP>:8088/api/system/save
```

> ⚠️ **养成好习惯**：每个里程碑（升级/换装/交任务/捡到好装备）后都存档，否则死亡会丢进度。

### 步骤 8：突发事件处理（随时可能遇到）

```bash
# 先看当前界面状态
curl http://<手机IP>:8088/api/ui/

# 剧情对话（screen=dialog_story）→ 跳过
curl -X POST http://<手机IP>:8088/api/ui/dialog/select -d '{"action":"skip"}'

# 启动同意页（screen=agreement）→ 同意关闭（异步生效，轮询 screen 直到 main_menu）
curl -X POST http://<手机IP>:8088/api/ui/dialog/select -d '{"action":"ok"}'

# 有阻塞弹窗（screen=dialog_popup）→ 查看内容并选择
curl http://<手机IP>:8088/api/ui/dialog
curl -X POST http://<手机IP>:8088/api/ui/dialog/select -d '{"action":"ok"}'

# 死亡（screen=dialog_wipeout）→ 回主菜单后重新进档（回到上次存档点）
curl -X POST http://<手机IP>:8088/api/ui/dialog/select -d '{"action":"game_over"}'
curl -X POST http://<手机IP>:8088/api/system/enter_slot -d '{"slot":0}'
```

> 完整应对表见 11.4「关键场景速查」。

---

## 5. 数据模型（返回 JSON 结构）

### 5.1 Player（玩家全局）

`GET /api/item/inventory/money` → `{"money":72503}`

| 字段 | 说明 |
|---|---|
| `money` | 金币 |
| `map_id` | 当前地图 ID（0-415，地图列表见 `/api/world/maps/list`） |
| `x`/`y` | 玩家坐标（像素，瓦片格×16） |
| `active_quest` | 当前追踪任务 ID |
| `leader_slot` | 主控角色对应出战槽 |
| `party_count` | 出战人数 |

### 5.2 Role（出战角色）

`GET /api/character/party` 返回 3 槽数组（空槽 `null`）。每项：

```json
{
  "name": "凯恩", "type_name": "主角", "class_name": "忍者",
  "level": 27,
  "hp": 10598, "max_hp": 10664, "mp": 200, "max_mp": 250,
  "exp": 12000, "exp_next": 15000,
  "main_stats": [
    { "stat_name": "力量", "base_stat": 9, "additional_stat": 87 },
    { "stat_name": "敏捷", "base_stat": 15, "additional_stat": 124 },
    { "stat_name": "体力", "base_stat": 11, "additional_stat": 90 },
    { "stat_name": "智力", "base_stat": 7, "additional_stat": 47 },
    { "stat_name": "精力", "base_stat": 5, "additional_stat": 33 }
  ],
  "status_point": 78,
  "equipment": [ { "...": "装备" }, null, ... ],
  "type": 0, "class_idx": 1, "name_id": 2210,
  "stats": { "0": 130, "4": 16, "30": 10664, "31": 212 }
}
```

| 字段 | 说明 |
|---|---|
| `name`/`type_name`/`class_name` | 角色名 / 类型名（0 主角 1 佣兵）/ 职业名（CHARCLASSBASE 联查） |
| `type`/`class_idx`/`name_id` | 原始字段：角色类型 0=主角 1=佣兵 / 职业索引 0-5（`party/{slot}/id` 端点返回 `{"id":class_idx,"id_name":"职业名"}`）/ 名字文本 ID |
| `hp`/`max_hp`/`mp`/`max_mp` | 当前/最大血量魔力 |
| `exp`/`exp_next` | 当前经验/升级所需经验 |
| `stats` | 战斗属性对象（键为字符串索引 0-31，关键索引见附录 13.2） |
| `main_stats` | 主属性列表（0-4=力量/敏捷/体力/智力/精力），每项 `{stat_name, base_stat, additional_stat}` |
| `status_point` | 剩余能力点 |
| `equipment` | 10 装备槽（含 `slot`/`position`/`name`/属性），空槽 `null` |

**装备位置映射（equipment 数组下标）：**

| slot | position | 位置 |
|---|---|---|
| 0 | head | 头部 |
| 1 | glove | 护手 |
| 2 | cloak | 斗篷 |
| 3 | armor | 铠甲 |
| 4 | boots | 鞋 |
| 5 | weapon1 | 主手武器 |
| 6 | weapon2 | 副手（盾牌/双持） |
| 7 | necklace | 项链 |
| 8 | ring | 戒指 |
| 9 | unused | 未使用 |

### 5.3 Item（物品）

```json
{
  "slot": 3, "name": "基础短剑", "category": 462, "count": 1, "item_type": "equipment",
  "need_level": 1, "rarity": 0, "rarity_tier": "白",
  "base": { "damage": 5, "defense": 0, "magic_rate": 1.1 },
  "bonus": [ { "id": 3, "name": "力量", "value": 18 } ],
  "gem": { "total_slots": 0, "slots": [] },
  "chaos": { "is_chaos": false, "level": 0, "rate": 100 },
  "enchant": { "id": 0, "level": 0 }
}
```

| 字段 | 说明 |
|---|---|
| `slot` | 槽位（背包槽 0-15 / 装备槽 0-9） |
| `name` | 物品名（品级前缀 + 名称联查，已直接给出） |
| `category` | 类别索引（ITEMDATABASE 记录下标，即物品 ID；`name` 已直接给出，用 `/api/system/tables/ITEMDATABASE/search?q=名称` 反查） |
| `count` | 数量（装备类恒 1） |
| `item_type` | 物品类型五类：`equipment` / `potion` / `scroll` / `gem` / `consumable`（ITEMDATABASE +2 字节用途类型判定） |
| `need_level` | 所需等级（ITEM_GetAbilityLevel；无等级概念的消耗品归 0） |
| `rarity`/`rarity_tier` | 稀有度档位 0-4 / 档位名（白绿蓝黄紫） |
| `base` | 基础属性：`{damage 物攻, defense 物防, magic_rate 魔法伤害倍率(原始值/100)}` |
| `bonus` | 词缀列表：`{id 词缀索引, name 词缀名, value 词缀值}` |
| `gem` | 宝石：`{total_slots 总槽位, slots[{id, name, value}] 已镶宝石}` |
| `chaos` | 混沌：`{is_chaos, level 混沌等级, rate 混沌成功率}` |
| `enchant` | 附魔：`{id, level, effect 附魔名(ITEMENCHANTBASE 联查，无附魔时 null)}` |

### 5.4 Skills（技能）

`GET /api/character/party/{slot}/skills`：

```json
{
  "role": 0,
  "skills": [ { "action_id": 3, "level": 1, "max_level": 4, "skill_name": "血之复仇" } ],
  "unlock_bitmap": 65535,
  "active_skill_id": 3,
  "skill_points": 6
}
```

> `max_level` 常规最高 4 级，技能书可提升至 8 级。

### 5.5 Mercenaries（佣兵）

`GET /api/character/mercenary`：

```json
[
  { "slot": 0, "type": 0, "flags": 219, "in_party": true, "name": "凯恩", "level": 27, "x": 320, "y": 480 }
]
```

- `in_party` 表示是否在队伍中；未上场佣兵坐标 2048 = 未激活（不在地图上）

### 5.6 Map（当前地图）

`GET /api/world/map/id`（地图 ID 与名称）与 `GET /api/world/map/units`（场景单位）：

```json
// /api/world/map/id
{ "map_id": 30, "id_name": "影子丛林1" }

// /api/world/map/units
{ "units": [ ... ], "char_loc": [ ... ] }
```

**Unit（场景单位）字段**：`slot/x/y/type/status/level/hp/mp/name/distance/nearest_distance`
- `status`：0=队伍，1=城镇 NPC/佣兵，2=怪物/召唤物
- `type`：0=队伍，1=怪物/NPC，2=装饰/场景（路障/宝箱/火把）
- `distance`：玩家到该单位的可达距离（-1=不可达，此时看 `nearest_distance`）

> **过滤语义（v0.5.35 起）**：`/api/world/map/enemies` 返回 `type==1` 的单位（含 NPC 与怪物）；`/api/world/map/interactives` 返回 `type==2` 且可交互的单位。不再按 `status` 过滤。
> **出口数据（v0.6.5 起）**：`/api/world/map/exits` 为**静态数据源**（`maps/exits.json`），返回 `{x, y, targetMapId, targetX, targetY}`（瓦片坐标 + 目标地图/落点），替代原 native 瓦片矩阵扫描。

### 5.7 GameState（界面状态）

`GET /api/ui`：

```json
{
  "screen": "world",
  "frame": 12345,
  "dialog": { "text": "...", "has_ok": false, "has_cancel": false, "buttons": [] }
}
```

`screen` 是**完整界面枚举**（v0.5.42 重构，`dialog_active` 字段已删除，用前缀判断 UI 是否被占据）：

| 前缀 | screen 取值 | 含义 |
|---|---|---|
| `main_menu_` | `main_menu_save_slot` / `main_menu_character_select` / `main_menu_daily_reward` / `main_menu_options` / `main_menu_settings` / `main_menu` | 主菜单面板 |
| `dialog_` | `dialog_popup`（弹窗） / `dialog_story`（剧情） / `dialog_wipeout`（死亡） / `dialog_quest`（任务完成） / `dialog_npc`（NPC 对话） / `dialog_choice`（选择框） / `dialog_input_count`（数量输入） | **对话框类占据** |
| `panel_` | `panel_character_info` / `panel_inventory` / `panel_mercenary` / `panel_craft` / `panel_npc_rest` / `panel_npc_revive` / `panel_options` / `panel_quests` / `panel_save_slot` / `panel_character_select` / `panel_shortcut` / `panel_skills` / `panel_shop` / `panel_settings` / `panel_world_map` / `panel_in_app` / `panel_daily_reward` / `panel_ui_panel` | **面板类占据** |
| 其他 | `world`（自由操作） / `loading`（加载） / `tutorial_pause`（药水教学暂停） / `agreement`（Java 层同意页，仅主菜单前，`dialog/select {"action":"ok"}` 关闭） | 特殊状态 |

**判定规则**（v0.5.42+）：
- `dialog_` 前缀 = 对话框类占据（`/api/ui/dialog` 的 `active=true` 即由此判断）
- `panel_` 前缀 = 面板类占据
- `world` = 可自由操作；`main_menu_*` = 主菜单场景（只能做存档操作）
- **世界操作（移动/战斗/交互/物品）在 UI 被占据时禁止**，返回 `{"ok":false,"error":"ui occupied: <screen>"}`（v0.5.43）

### 5.8 Snapshot（全量快照）

`GET /api/system/snapshot`（**不含装备明细与佣兵列表**，需要时分别查装备端点与佣兵端点）：

```json
{
  "frame": 12345, "screen": "world",
  "money": 72503, "map_id": 30, "x": 304, "y": 376,
  "leader_slot": 0,
  "party": [
    { "name": "凯恩", "type_name": "主角", "class_name": "忍者",
      "level": 27, "hp": 10598, "max_hp": 10664,
      "mp": 200, "max_mp": 250, "exp": 12000, "exp_next": 15000,
      "main_stats": [
        { "stat_name": "力量", "base_stat": 9, "additional_stat": 87 },
        { "stat_name": "敏捷", "base_stat": 15, "additional_stat": 124 }
      ],
      "status_point": 78, "type": 0, "class_idx": 1, "name_id": 2210
    }
  ]
}
```

> 装备明细走 `GET /api/character/party/{slot}/equipment`；佣兵走 `GET /api/character/mercenary`。

### 5.9 DialogContent（对话/弹窗内容）

`GET /api/ui/dialog` 统一检测返回：

```json
{
  "type": "npc", "active": true,
  "speaker": "商人", "text": "欢迎光临！",
  "options": [ { "id": "0", "label": "购买物品" }, { "id": "next", "label": "下一句" } ]
}
```

| type | 场景 | 可选动作（select 的 action） |
|---|---|---|
| `story` | 剧情对话 | `next` 下一句 / `skip` 跳过 |
| `npc` | NPC 对话 | `next` 下一句 / `index` 选项选择（`{"index":n}`） |
| `popup` | 确认弹窗 | `ok` / `cancel` |
| `npc_quest` | 任务完成面板 | `complete` / `close` |
| `wipeout` | 死亡面板 | `revive` / `special_revive` / `game_over` |
| `save_slot` | 存档槽面板 | `save` / `close` |
| 面板态 | 其他面板（背包/技能/设置等） | `close` 关闭面板 |
| `save`/`sell`/`quest` | 各类弹窗 | `confirm` / `quit` / `cancel` |
| `none` | 无对话 | 空 options |

---

## 6. 端点全表（按域分组）

> 全部可用端点按游戏领域分组，可用端点见下，未开放端点标注 ⏳。

### 6.1 system（系统与会话）

#### 服务与游戏整体

| 方法 | 路径 | 用途 | 返回 |
|---|---|---|---|
| GET | `/api/health` | 服务存活检查 | `{"ok":true}` |
| GET | `/api/system/game` | 游戏复合（快照+信息） | `{"snapshot":...,"info":...}` |
| GET | `/api/system/snapshot` | 全量快照（精简版） | `<Snapshot>` |
| GET | `/api/system/info` | 服务与游戏信息 | `{"version","save_slots","current_save_slot","package_name","base"}` |
| GET | `/api/system/game_frame` | 帧计数 | `{"frame":12345}` |

#### 存档

| 方法 | 路径 | 用途 | 请求 | 返回 |
|---|---|---|---|---|
| POST | `/api/system/save` | 手动存档 | 无 body | `{"ok":true,"state":<Player>}` |
| POST | `/api/system/enter_slot` | 进入存档槽（读档） | `{"slot":0}` (0-2) | `{"ok":true,"state":<Player>}` |
| POST | `/api/system/create_slot` | 创建新角色 | `{"slot":1,"class_idx":2}` | `{"ok":true,"state":<Player>}` |

**⚠️ enter_slot 注意事项**：
- 只能在非 world 状态调用（world 中 → `already in game`）
- 启动同意页（screen=`agreement`）→ `ui occupied: agreement`；先 `POST /api/ui/dialog/select {"action":"ok"}` 关闭后再进档；原生弹窗（screen=`dialog_popup`）→ `ui occupied: dialog_popup`
- 进入前会在 `enter_slot` 内执行只读完整性检查；缺失、损坏或不兼容存档返回结构化错误，不调用原版进档路径；当前没有独立预检 API。

#### 事件流

| 方法 | 路径 | 用途 | 返回 |
|---|---|---|---|
| GET | `/api/system/events` | 轮询游戏事件（差异检测） | `{"events":[...]}`（见第 10 章） |

#### 静态数据表

| 方法 | 路径 | 用途 | 状态 |
|---|---|---|---|
| GET | `/api/system/tables` | 表清单 | ✅ |
| GET | `/api/system/tables/{table}` | 单表全量（表名自动大写） | ✅ |
| GET | `/api/system/tables/{table}/search?q=` | 表内名称搜索（q 必填） | ✅ |
| GET | `/api/system/tables/{table}/download` | 下载静态表文件 | ⏳ `{"ok":false,"error":"not implemented"}` |
| GET | `/api/system/tables/story-events` | 剧情事件数据 | ✅ |
| GET | `/api/system/tables/text?lang=` | 多语言文本（zh-Hans/en） | ✅ |
| GET | `/api/system/help` | 帮助文档 | ⏳ `{"ok":false,"error":"not implemented"}` |
| GET | `/api/system/download` | 下载帮助文档 | ⏳ `{"ok":false,"error":"not implemented"}` |

### 6.2 character（角色与队伍）

#### 出战队伍读

| 方法 | 路径 | 用途 | 返回 |
|---|---|---|---|
| GET | `/api/character/party` | 3 槽出战角色完整状态 | `[<Role>,...]` |
| GET | `/api/character/party/count` | 出战人数 | `{"count":2}` |
| GET | `/api/character/leader` | 当前主控角色 | `<Role>` 或 `{"error":"not found"}` |
| GET | `/api/character/party/{slot}` | 指定出战槽完整状态 | `<Role>` |
| GET | `/api/character/party/{slot}/id` | 职业索引 | `{"id":1,"id_name":"忍者"}` |
| GET | `/api/character/party/{slot}/name` | 角色名 | `{"name":"凯恩"}` |
| GET | `/api/character/party/{slot}/level` | 等级 | `{"level":27}` |
| GET | `/api/character/party/{slot}/status` | 状态聚合 | `{"status":{hp,max_hp,mp,max_mp,exp,exp_next,attribute_points,skill_points}}` |
| GET | `/api/character/party/{slot}/stats` | 全部战斗属性 | `{"stats":{...}}` |
| GET | `/api/character/party/{slot}/equipment` | 全部装备（含名称） | `{"equipment":[...]}` |
| GET | `/api/character/party/{slot}/equipment/{equip_slot}` | 单装备槽 | `<Item>` 或 `{"error":"not found"}` |
| GET | `/api/character/party/{slot}/skills` | 技能（含名称） | `<Skills>` |

#### 佣兵读

| 方法 | 路径 | 用途 | 返回 |
|---|---|---|---|
| GET | `/api/character/mercenary` | 全部佣兵（含未上场） | `[<Mercenary>...]` |
| GET | `/api/character/mercenary/list` | 非空佣兵槽 id 列表 | `{"slots":[0,1,3]}` |
| GET | `/api/character/mercenary/{slot}` | 指定佣兵槽 | `<Mercenary>` 或 `{"error":"not found"}` |

#### ⚠️ 两套佣兵索引

| 概念 | 值示例 | 说明 |
|---|---|---|
| 读端点 `slot` | 27/32/58 | 佣兵槽编号（读 `/api/character/mercenary` 返回的 `slot`） |
| 写参数 `mercenary_slot` | 0/1/255 | 角色所属佣兵槽（写操作 include/exclude/discharge/withdraw 用） |

**两套编号不是同一个值**：读端点返回的 `slot` 不能直接用于写操作。多数角色写操作传 `0`；传错时返回 `mercenary not found` 属正常（安全拒绝）。

#### 队伍操作（POST）

| 方法 | 路径 | 用途 | 请求 | 返回 |
|---|---|---|---|---|
| POST | `/api/character/party/include` | 佣兵入队 | `{"mercenary_slot":0}` | `{"ok":true,"state":<Party>}` |
| POST | `/api/character/party/exclude` | 佣兵离队 | `{"mercenary_slot":0}` | `{"ok":true,"state":<Party>}` |
| POST | `/api/character/party/discharge` | 遣散佣兵 | `{"mercenary_slot":27}` | `{"ok":true,"state":<Party>}` |
| POST | `/api/character/party/withdraw` | 取出佣兵装备 | `{"mercenary_slot":0,"equip_slot":3}` | `{"ok":true,"state":<Party>}` |

**常见错误**：已在队 → `already in party`；满员 → `party full`；主控 → `cannot exclude leader`；任务 NPC → `cannot exclude quest npc`；无角色 → `mercenary not found`。

#### 角色成长（POST）

| 方法 | 路径 | 用途 | 请求 | 返回 |
|---|---|---|---|---|
| POST | `/api/character/grow/add_skill` | 主角学习/升级技能 | `{"action_id":3,"level":1}` | `{"ok":true,"state":<Skills>}` |
| POST | `/api/character/grow/{role}/add_stat` | 批量分配属性点 | `{"attrs":{"strength":1,"agility":2}}` | `{"ok":true,"applied":[...]}` |
| POST | `/api/character/grow/reset_stat` | 重置主角分配属性 | 无 body | `{"ok":true,"state":<Player>}` |
| POST | `/api/character/grow/reset_skill` | 重置主角技能 | 无 body | `{"ok":true,"state":<Player>}` |

**add_stat 属性名**：`strength`(0=力量)/`agility`(1=敏捷)/`vitality`(2=体力)/`intelligence`(3=智力)/`spirit`(4=精力)，也可用索引 `"0"`-`"4"`，值须为正整数。
**注意**：各属性分配数量总和不能超过剩余能力点 → `no status point`；属性名非法 → `bad attr`；值非法 → `bad value`。

#### 战斗（POST）

| 方法 | 路径 | 用途 | 请求 | 返回 |
|---|---|---|---|---|
| POST | `/api/character/combat/{role}/set_auto_attack` | 开关自动反击（受击后自动还击） | `{"on":true}` | `{"ok":true,"state":<Party>}` |
| POST | `/api/character/combat/{role}/set_skill_usage` | 设置技能 AI 使用档位 | ⏳ 未开放 | — |
| POST | `/api/character/combat/switch_player` | 切换主控角色 | `{"slot":1}` (0-2) | `{"ok":true,"state":<Player>}` |
| POST | `/api/character/combat/{role}/cast_skill` | 立即释放技能 | `{"action_id":5}` | `{"ok":true,"state":<Party>}` |
| POST | `/api/character/combat/{role}/attack_target` | 进入战斗并攻击目标 | `{"target_slot":5}` | `{"ok":true,"state":<Party>}` |
| POST | `/api/character/combat/{role}/stop_combat` | 停止战斗 | 无 body | `{"ok":true,"state":<Player>}` |

> ⚠️ `set_auto_attack` 只控制"受击后自动还击"，**不是**自动索敌挂机——需先 `attack_target` 进入战斗。
> `cast_skill` 的 `action_id` 必须是已学**技能**（非普攻）；未学 → `skill not learned`；无目标 → `no target`。
> `attack_target`：目标无效 → `target not found`；持续普攻该目标直至死亡/离开/stop_combat。

### 6.3 world（地图与移动）

#### 当前地图读（GET）

| 方法 | 路径 | 用途 | 返回 |
|---|---|---|---|
| GET | `/api/world/map/id` | 地图 ID + 名称 | `{"map_id":30,"id_name":"影子丛林1"}` |
| GET | `/api/world/map/exits` | 出口区域（**静态数据源** `maps/exits.json`，v0.6.5 起） | `{"exits":[{"x":24,"y":19,"targetMapId":30,"targetX":2,"targetY":10},...]}` |
| GET | `/api/world/map/units` | 全部场景单位 | `{"units":[...],"char_loc":[...]}` |
| GET | `/api/world/map/enemies` | 敌人/战斗单位（type==1） | `{"units":[...]}` |
| GET | `/api/world/map/interactives` | 可交互对象（type==2 且可交互） | `{"units":[...]}` |
| GET | `/api/world/map/drops` | 掉落物（暂为空） | `{"drops":[]}` |
| GET | `/api/world/map/distance?tx=&ty=` | 到目标点的距离计算（不移动） | `{"target","start","found","distance","nearest"}` |

> ⚠️ 复合端点 `/api/world/map` **已移除（v0.5.35）**，地图综合数据请分别查询 `id`/`exits`/`units` 等子端点。
> 过滤语义见 5.6：`enemies` 按 `type==1`，`interactives` 按 `type==2`（不再按 `status`）。

#### 移动操作（POST）

| 方法 | 路径 | 用途 | 请求 | 返回 |
|---|---|---|---|---|
| POST | `/api/world/movement/move_to` | 寻路移动到像素坐标 | `{"x":600,"y":400}` | `{"ok":true,"state":<Player>}` |
| POST | `/api/world/movement/walk_dir` | 方向键持续移动 | `{"direction":1}` (0-3) | `{"ok":true,"state":<Player>}` |
| POST | `/api/world/movement/stop_move` | 停止所有移动 | 无 body | `{"ok":true,"state":<Player>}` |
| POST | `/api/world/movement/interact_with` | 场景交互/攻击键（触发事件链） | 无 body | `{"ok":true,"state":<Player>}` |

**移动语义**：
- `move_to`/`walk_dir` 都是**正常走路**（后台线程逐帧，非瞬移），到达出口区域自动切图
- 目标不可达 → 走到最近可达点并面向目标
- 剧情/切图状态（screen=dialog_story 或切图中）时操作自动终止

#### 静态地图（GET）

| 方法 | 路径 | 用途 | 返回 |
|---|---|---|---|
| GET | `/api/world/maps/list` | 地图列表 | `{"maps":[{"map_id":3513,"name":"..."}]}` |
| GET | `/api/world/maps/{map_id}` | 指定地图静态信息 | `{"map_id","text_id","name","raw"}` |
| GET | `/api/world/maps/{map_id}/tiles` | 指定地图瓦片矩阵（双层数组） | `{"map_id","src":"static","size":64,"encoding":"array","tiles":[[...]]}` |

> ⚠️ **瓦片格式（v0.5.24 起）**：`encoding:"array"`，`tiles` 为 64×64 双层数组 `tiles[y][x]`，每格字节 **bit6=阻挡、bit7=出口**（可组合）。不再是 base64。缺失 → `{"error":"no tiles"}`。

> ⚠️ 两套地图编号：地图列表 `maps/list` 的 `map_id` 是**文本 ID**（3513-3928）；游戏内当前位置 `/api/world/map/id` 返回**记录下标**（0-415）。查询时对 0-415 按下标，其他值按文本 ID 兼容。

### 6.4 item（物品与背包）

#### 背包读（GET）

| 方法 | 路径 | 用途 | 返回 |
|---|---|---|---|
| GET | `/api/item/inventory` | 背包复合（6 袋×16 槽） | `<Inventory>` |
| GET | `/api/item/inventory/money` | 金币 | `{"money":72503}` |
| GET | `/api/item/inventory/items` | 全部物品展平 | `{"items":[...]}` |
| GET | `/api/item/inventory/bag/{bag}/info` | 袋信息 | `{"bag":0,"capacity":16,"slot_count":13}` |
| GET | `/api/item/inventory/bag/{bag}/{slot}` | 袋内指定槽 | `<Item>` 或 `null` |

> ⚠️ `bag/{bag}/{slot}` 按**实际格号**（物品的 `slot` 字段）匹配，不是数组下标。

#### 背包操作（POST）

| 方法 | 路径 | 用途 | 请求 | 返回 |
|---|---|---|---|---|
| POST | `/api/item/inventory/use_item` | 使用物品（4 路分派） | `{"bag":0,"slot":3}` | `{"ok":true,"state":<Inventory>}` |
| POST | `/api/item/inventory/accept_dice` | 接受掷骰结果 | 无 body | `{"ok":true,"state":<Player>}` |
| POST | `/api/item/inventory/reject_dice` | 拒绝掷骰结果 | 无 body | `{"ok":true}` |
| POST | `/api/item/inventory/discard_item` | 丢弃物品 | `{"bag":0,"slot":5}` | `{"ok":true,"state":<Inventory>}` |
| POST | `/api/item/inventory/sell_item` | 出售物品（价格=静态表） | `{"bag":0,"slot":5}` | `{"ok":true,"price":15,"state":<Inventory>}` |
| POST | `/api/item/inventory/move_item` | 移动/堆叠物品 | `{"bag":0,"slot":3,"count":1,"to_bag":0,"to_slot":4}` | `{"ok":true,"state":<Inventory>}` |
| POST | `/api/item/inventory/{role}/equip_item` | 穿装备（二选一） | `{"bag":0,"slot":3}` 或 `{"category":512}` | `{"ok":true,"state":<Party>}` |
| POST | `/api/item/inventory/{role}/unequip_item` | 脱装备 | `{"slot":2}` | `{"ok":true,"state":<Party>}` |
| POST | `/api/item/inventory/{role}/put_jewel` | 镶嵌宝石 | `{"bag":0,"slot":3,"equip_slot":3}` | `{"ok":true,"state":<Party>}` |
| POST | `/api/item/inventory/{role}/enchant` | 强化装备（消耗卷轴） | `{"bag":0,"slot":3,"equip_slot":5}` | `{"ok":true,"state":<Party>}` |

**常见错误**：
- use_item：非消耗品 → `item not usable`；冷却中 → `on cooldown`（不消耗）
- 骰子：掷出后返回 `base/pending/delta`（未应用），需 accept_dice/reject_dice 处理；无 pending → `no dice result pending`
- put_jewel：无孔 → `no socket`；非宝石 → `not jewel`；**镶嵌后自动消耗背包宝石**
- enchant：非强化卷轴 → `not enchant scroll`；不可强化 → `cannot enchant`；失败 → `enchant failed`；**成功才消耗卷轴 1 张**

#### 商店

| 方法 | 路径 | 用途 | 请求 | 返回 |
|---|---|---|---|---|
| GET | `/api/item/shop/items` | 当前商店商品 | — | `{"items":[{"slot":0,"category":5,"count":1,"price":15,"name":"恢复药水"}]}` |
| POST | `/api/item/shop/buy_item` | 购买商品 | `{"slot":0}` | `{"ok":true,"state":<Inventory>}` |

> ⚠️ 非商店界面时商品表为空（`items:[]`）——需先与商人对话进入商店（见 7.5）。
> 金币不足 → `not enough money`；无商品 → `item not found`。

### 6.5 quest（任务）

| 方法 | 路径 | 用途 | 返回 |
|---|---|---|---|
| GET | `/api/quest` | 任务复合 | `{"active":[...],"details":[...],"completed":[...]}` |
| GET | `/api/quest/active` | 已接任务（含进度，排除 hidden，注入静态字段） | `{"quests":[{"quest_id":381,"id_name":"拯救村子","group_id":196,"name":"弱化的原因","detail":"……","is_mainline":false,"is_side":true}]}` |
| GET | `/api/quest/details` | 已接任务全量静态详情（含交付对话/奖励/职业要求） | `{"quests":[{"slot":0,"quest_id":381,"group_id":196,"group_name":"弱化的原因","name":"……","detail":"……","accepted_dialog":"……","delivered_dialog":"……","class_req":"","reward_hint":"","rewards":[...],"is_mainline":false,"is_side":true,"hidden":false}]}` |
| GET | `/api/quest/{id}` | 单任务静态详情 | ⚠️ 暂不可用（恒 not found） |
| GET | `/api/quest/completed` | 已完成任务 | `{"quests":[...]}`（暂为空） |
| POST | `/api/quest/quit_quest` | 放弃任务 | `{"quest_id":381}` → `{"ok":true}` |

> `details` 关键字段：`group_id` 任务组（QUESTGROUPBASE 下标）、`group_name` 任务链标题（面板显示名）、`accepted_dialog`/`delivered_dialog` 接取/交付对话、`class_req` 职业要求、`reward_hint` 奖励提示、`rewards` 奖励数组、`is_mainline`/`is_side` 主线/支线标记、`hidden` 是否隐藏（hidden 任务不出现在 active）。

### 6.6 ui（界面与对话）

#### 界面状态读（GET）

| 方法 | 路径 | 用途 | 返回 |
|---|---|---|---|
| GET | `/api/ui/` | 界面状态复合 | `<GameState>` |
| GET | `/api/ui/screen` | 当前界面名 | `{"screen":"world"}` |
| GET | `/api/ui/panel` | 当前面板 | `{"panel":"inventory"}` 或 `{"panel":null}` |
| GET | `/api/ui/dialog` | **统一对话/弹窗检测** | `<DialogContent>`（见 5.9） |

#### 对话与弹窗操作（POST）

| 方法 | 路径 | 用途 | 请求 | 返回 |
|---|---|---|---|---|
| POST | `/api/ui/start_interact` | 开始与附近 NPC 交互 | 无 body | `{"ok":true}` 或 `{"ok":false,"error":"no npc nearby"}` |
| POST | `/api/ui/dialog/select` | 选择对话选项（唯一选择端点） | `{"action":"next"}` 或 `{"index":0}` | `{"ok":true}` |

**select 支持动作**（依当前对话 type）：
- story：`next` / `skip`
- npc：`next` 或 `index`
- npc_quest：`complete` / `close`
- wipeout：`revive` / `special_revive` / `game_over`
- popup：`ok` / `cancel`
- 面板态：`close`；save_slot 面板另接受 `save`

#### 界面操作（POST）

| 方法 | 路径 | 用途 | 请求 | 返回 |
|---|---|---|---|---|
| POST | `/api/ui/go_main_menu` | 回到主菜单 | 无 body | `{"ok":true,"state":<Player>}` |
| POST | `/api/ui/open_panel` | 打开面板 | `{"panel":"inventory"}` | `{"ok":true,"state":<GameState>}` |
| POST | `/api/ui/close_panel` | 关闭当前面板 | 无 body | `{"ok":true,"state":<GameState>}` |

**面板白名单**（真机验证可 API 打开）：`character_info` / `inventory` / `skills` / `mercenary` / `quests` / `settings`。
其他面板（shop/craft/npc/options/world_map 等）需游戏内上下文 → `panel requires in-game context`。

### 6.7 config（模块配置）

> 模块级配置的读取与修改（v0.5.22 新增）。**纯 Kotlin 层能力，不走 ControllerGuard**——native 未就绪时配置端点同样可用（如改端口解决冲突）。
> 配置持久化于外部文件 `/sdcard/Android/data/<游戏包>/files/config.json`（用户可见可编辑）；`POST /api/config/set` 修改立即持久化；删除该文件即恢复出厂默认。

| 方法 | 路径 | 用途 | 请求 | 返回 |
|---|---|---|---|---|
| GET | `/api/config/list` | 查看当前配置 | — | `{"listenAddress":"0.0.0.0","listenPort":8088,"stackLimitIncrease":false,"moveMergeEnabled":false,"opEnabled":false}` |
| POST | `/api/config/set` | 修改配置（**只更新请求中出现的字段**） | `{"opEnabled":true}` 等 | `{"ok":true,"restart":false}` 或 `{"ok":true,"restart":true}` |

**可配置项**：

| 配置项 | 类型 | 默认 | 说明 |
|---|---|---|---|
| `listenAddress` | string | `0.0.0.0` | HTTP 监听地址 |
| `listenPort` | int | 8088 | HTTP 监听端口（1-65535；越界报 `listenPort must be 1-65535`） |
| `stackLimitIncrease` | bool | `false` | 堆叠上限提升（变化即时通知 native，无需重启） |
| `moveMergeEnabled` | bool | `false` | 背包内拖拽同类可堆叠物品时自动合并（变化即时安装或还原 native hook，无需重启） |
| `opEnabled` | bool | `false` | **OP 越权操作全局开关（v0.5.47）**——开启后 `/api/op/*` 可用；关闭时全部 OP 端点返回 403 `op disabled` |

> ⚠️ 修改 `listenAddress`/`listenPort` 后返回 `restart:true` 并**自动重启 HTTP 服务**（延迟约 500ms 先让响应送达）；重启后旧地址/端口连接断开，需按新配置访问。**新端口被占用时自动回退默认端口 8088** 并同步修正配置。
> `opEnabled` 开启示例：`curl -X POST .../api/config/set -d '{"opEnabled":true}'`（见第 8 章）。

### 6.8 debug（调试）

| 方法 | 路径 | 用途 | 返回 |
|---|---|---|---|
| GET | `/api/debug/ui` | 调试用 UI 原始 JSON（不走 ControllerGuard） | `<原始 gamestate JSON>` |
| GET | `/api/debug/path?tx=&ty=` | 调试用 BFS 寻路（完整路线+阻挡信息） | `{"start","target","found","distance","path":[...],"blocked":[...]}` |

---

## 7. 玩法指南：用 API 完成游戏内目标

### 7.1 从主菜单进入游戏

```bash
# 启动后若出现无 API 跳过的弹窗（真机2专用前置），先点击确认：
ANDROID_SERIAL=192.168.3.54:5555 uv run python scripts/touch_automation.py --inject input click 420,280 1.0

# 1. 查存档
curl http://<手机IP>:8088/api/system/info

# 2. 创建新档（或 enter_slot 进已有档）
curl -X POST http://<手机IP>:8088/api/system/create_slot -d '{"slot":1,"class_idx":2}'

# 3. 跳过初始剧情（screen=dialog_story 时）
curl -X POST http://<手机IP>:8088/api/ui/dialog/select -d '{"action":"skip"}'

# 4. 确认进入世界
curl http://<手机IP>:8088/api/ui/screen
```

### 7.2 移动与探索

```bash
# 看地图信息与出口
curl http://<手机IP>:8088/api/world/map/id
curl http://<手机IP>:8088/api/world/map/exits

# 计算到出口的距离
curl "http://<手机IP>:8088/api/world/map/distance?tx=24&ty=19"

# 走过去（到达出口格子自动切图）
curl -X POST http://<手机IP>:8088/api/world/movement/move_to -d '{"x":384,"y":304}'

# 切图后确认新地图
curl http://<手机IP>:8088/api/world/map/id
```

### 7.3 战斗（打怪刷级）

```bash
# 1. 找敌人（/enemies 返回 type==1 的战斗单位）
curl http://<手机IP>:8088/api/world/map/enemies

# 2. 靠近敌人
curl -X POST http://<手机IP>:8088/api/world/movement/move_to -d '{"x":480,"y":400}'

# 3. 主控攻击目标
curl -X POST http://<手机IP>:8088/api/character/combat/0/attack_target -d '{"target_slot":12}'

# 4. 战斗中放技能
curl http://<手机IP>:8088/api/character/party/0/skills
curl -X POST http://<手机IP>:8088/api/character/combat/0/cast_skill -d '{"action_id":5}'

# 5. 停止战斗 / 切换主控
curl -X POST http://<手机IP>:8088/api/character/combat/0/stop_combat
curl -X POST http://<手机IP>:8088/api/character/combat/switch_player -d '{"slot":1}'

# 6. 回血
curl -X POST http://<手机IP>:8088/api/item/inventory/use_item -d '{"bag":0,"slot":3}'
```

**挂机战斗**：`set_auto_attack {on:true}` + `attack_target` 一个目标后角色持续作战（受击自动还击）。

### 7.4 装备与背包管理

```bash
# 看背包 / 穿装备 / 脱装备 / 卖垃圾 / 整理
curl http://<手机IP>:8088/api/item/inventory/items
curl -X POST http://<手机IP>:8088/api/item/inventory/0/equip_item -d '{"bag":0,"slot":3}'
curl -X POST http://<手机IP>:8088/api/item/inventory/0/unequip_item -d '{"slot":2}'
curl -X POST http://<手机IP>:8088/api/item/inventory/sell_item -d '{"bag":0,"slot":5}'
curl -X POST http://<手机IP>:8088/api/item/inventory/move_item -d '{"bag":0,"slot":3,"count":5,"to_bag":1,"to_slot":0}'

# 强化装备（消耗背包中的强化卷轴）
curl -X POST http://<手机IP>:8088/api/item/inventory/0/enchant -d '{"bag":0,"slot":3,"equip_slot":5}'
```

### 7.5 商店买卖

```bash
# 1. 找商人（interactives 里的 NPC）并走到旁边
curl http://<手机IP>:8088/api/world/map/interactives
curl -X POST http://<手机IP>:8088/api/world/movement/move_to -d '{"x":<商人x>,"y":<商人y>}'

# 2. 交互 → 查看对话 → 选商店选项
curl -X POST http://<手机IP>:8088/api/ui/start_interact
curl http://<手机IP>:8088/api/ui/dialog
curl -X POST http://<手机IP>:8088/api/ui/dialog/select -d '{"index":0}'

# 3. 确认进入商店 → 看商品 → 购买
curl http://<手机IP>:8088/api/ui/screen
curl http://<手机IP>:8088/api/item/shop/items
curl -X POST http://<手机IP>:8088/api/item/shop/buy_item -d '{"slot":0}'

# 4. 关面板离开
curl -X POST http://<手机IP>:8088/api/ui/close_panel
```

### 7.6 任务流程

任务接取/交付通过 NPC 对话完成（无直接接任务端点）：

```bash
# 与 NPC 对话 → 选任务选项 → 查看已接任务
curl -X POST http://<手机IP>:8088/api/ui/start_interact
curl http://<手机IP>:8088/api/ui/dialog
curl -X POST http://<手机IP>:8088/api/ui/dialog/select -d '{"index":0}'
curl http://<手机IP>:8088/api/quest/active

# 交付（npc_quest 面板）
curl -X POST http://<手机IP>:8088/api/ui/dialog/select -d '{"action":"complete"}'

# 放弃任务（可选）
curl -X POST http://<手机IP>:8088/api/quest/quit_quest -d '{"quest_id":381}'
```

### 7.7 角色养成

```bash
# 查看技能点/属性点
curl http://<手机IP>:8088/api/character/party/0/status
curl http://<手机IP>:8088/api/character/party/0/skills

# 批量分配属性点（strength/agility/vitality/intelligence/spirit）
curl -X POST http://<手机IP>:8088/api/character/grow/0/add_stat -d '{"attrs":{"strength":2,"agility":1}}'

# 学习技能（主角）
curl -X POST http://<手机IP>:8088/api/character/grow/add_skill -d '{"action_id":3,"level":1}'

# 重置（API 免费，游戏内需内购）——谨慎使用
curl -X POST http://<手机IP>:8088/api/character/grow/reset_stat
curl -X POST http://<手机IP>:8088/api/character/grow/reset_skill
```

### 7.8 队伍与佣兵

```bash
# 查看佣兵 → 入队 → 离队 → 取装备 → 遣散
curl http://<手机IP>:8088/api/character/mercenary
curl -X POST http://<手机IP>:8088/api/character/party/include -d '{"mercenary_slot":0}'
curl -X POST http://<手机IP>:8088/api/character/party/exclude -d '{"mercenary_slot":0}'
curl -X POST http://<手机IP>:8088/api/character/party/withdraw -d '{"mercenary_slot":0,"equip_slot":3}'
curl -X POST http://<手机IP>:8088/api/character/party/discharge -d '{"mercenary_slot":0}'   # 不可逆！
```

### 7.9 存档

```bash
# 关键操作前存档！
curl -X POST http://<手机IP>:8088/api/system/save

```

---

## 8. 越权操作（OP 端点）

> ⚠️ **OP = 游戏内做不到的越权操作**（改数据/强行操作）。这些端点不在正常游玩范围内，除非调试/测试，否则不建议使用。

### 8.0 全局开关（重要！）

**OP 端点受全局开关 `opEnabled` 门禁**（v0.5.47，默认 `false`）：

- **开关关闭（默认）**：所有 `/api/op/*` 端点返回 **403 + `{"ok":false,"error":"op disabled"}`**
- **开启方式**：`POST /api/config/set` 传 `{"opEnabled":true}`，或编辑外部配置 `config.json` 置 `opEnabled:true`
- 门禁在 OpApiService 统一入口（controller 无法绕过）；开启后无额外鉴权，仍仅限可信局域网

```bash
# 开启 OP（一次即可，持久化到 config.json）
curl -X POST http://<手机IP>:8088/api/config/set -d '{"opEnabled":true}'
# {"ok":true,"restart":false,"opEnabled":true,...}
```

### 8.1 可用（10 个）

| 方法 | 路径 | 用途 | 请求 | 返回 |
|---|---|---|---|---|
| POST | `/api/op/character/{role}/hp` | 直写血量 | `{"hp":8000}`（截断 0..max_hp） | `{"ok":true,"state":<Party>}` |
| POST | `/api/op/character/{role}/mp` | 直写魔力 | `{"mp":200}` | `{"ok":true,"state":<Party>}` |
| POST | `/api/op/character/{role}/experience` | 直写经验（不触发升级） | `{"exp":12000}` | `{"ok":true,"state":<Party>}` |
| POST | `/api/op/character/{role}/level` | 直写等级（完整升级结算） | `{"level":30}` 或 `{"level":200,"force":true}` | `{"ok":true,"state":<Party>}` |
| POST | `/api/op/character/{role}/set_attr` | 批量直写基础属性 | `{"stats":{"strength":10,"agility":7}}` 或 `{"stats":{"0":10}}` | `{"ok":true,"set":[...]}` |
| POST | `/api/op/inventory/add` | 直接生成物品进背包 | `{"category":1,"count":5}` | `{"ok":true,"state":<Inventory>}` |
| POST | `/api/op/inventory/money` | 直写金币（绝对值，非增量） | `{"money":99999}` | `{"ok":true,"state":<Inventory>}` |
| POST | `/api/op/character/{role}/status-point` | 直写可分配属性点 | `{"points":5}` | `{"ok":true,"state":<Party>}` |
| POST | `/api/op/party/swap` | 队伍槽位换位 | `{"a":0,"b":1}` | `{"ok":true,"state":<Party>}` |
| POST | `/api/op/movement/teleport` | 传送/切图 | `{"map_id":20,"x":15,"y":10}` | `{"ok":true,"state":<Map>}` |

**level 注意事项**：
- 默认限制 1-105（游戏上限）；超出 → `level 1-105`
- `force:true` 跳过限制——⚠️ 等级超过 127 会溢出显示为负数（实测 200 → -56），后果自负
- 降级 → `level down not allowed`

**set_attr 说明**：键用主属性英文名（strength/agility/vitality/intelligence/spirit）或索引 0-4，值 0-255。

**teleport 坐标语义**（真机验证）：`map_id>0` 时 `x`/`y` 为**瓦片索引**（走切图流程）；`map_id=0` 时 `x`/`y` 为**像素坐标**（原地传送）。`map_id`/`x`/`y` 均必填。

### 8.2 未开放（11 个）

> 结构已定，暂不可用（返回 403 `op disabled` 或 501 `not implemented`）：

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

## 9. 静态数据表查询

游戏全部数值表已内置在服务中，可随时查询。

### 9.1 常用表

| 表名 | 内容 | 记录数 |
|---|---|---|
| `ITEMDATABASE` | 物品主表 | 1,018 |
| `ITEMCLASSBASE` | 物品分类 | — |
| `ITEMOPTINFOBASE` | 词缀定义 | 37 |
| `MONDATABASE` | 怪物 | 553 |
| `QUESTINFOBASE` | 任务 | 507 |
| `QUESTREWARDBASE` | 任务奖励 | 394 |
| `MAPINFOBASE` | 地图 | 416 |
| `CHARCLASSBASE` | 职业 | 6 |
| `SKILLDESCBASE` | 技能 | 114 |

### 9.2 查询示例

```bash
# 表清单
curl http://<手机IP>:8088/api/system/tables

# 物品表搜索
curl "http://<手机IP>:8088/api/system/tables/ITEMDATABASE/search?q=治疗"
# {"items":[{"index":788,"name":"鑫迪的治疗药",...}]}

# 单表全量 / 剧情事件 / 中文字幕
curl http://<手机IP>:8088/api/system/tables/MONDATABASE
curl http://<手机IP>:8088/api/system/tables/story-events
curl "http://<手机IP>:8088/api/system/tables/text?lang=zh-Hans"
```

---

## 10. 事件流（感知游戏变化）

`GET /api/system/events` 提供**差异检测**事件（零 hook，轮询对比快照）：

```bash
curl http://<手机IP>:8088/api/system/events
```

```json
{ "events": [
  { "type": "money", "old": 100, "new": 150 },
  { "type": "hp", "role": 0, "old": 10598, "new": 8000 },
  { "type": "level_up", "role": 1, "old": 10, "new": 11 },
  { "type": "inventory", "old": 13, "new": 12 }
] }
```

**使用规则**：
- **500ms-1s 周期性轮询**
- 首次调用仅建立基线，返回空列表
- 事件类型：`money` / `hp` / `mp` / `exp` / `level_up` / `move` / `inventory`

**AI 建议**：轮询事件 + 定期快照（`/api/system/snapshot`），即可不读屏感知游戏状态。

---

## 11. 给 AI 代理的决策建议

> 核心思想：**状态驱动，不要次数驱动**。每一轮循环都重新感知游戏状态再决策；每一步都要防"卡死"（弹窗、死亡、剧情、寻路失败）。按 3.4 注意点执行。

### 11.1 感知 → 决策 → 行动循环

```
1. 感知（GET，并行）
   - /api/system/snapshot      # 全局状态（screen/血量/位置/队伍）
   - /api/ui/                   # 界面状态（完整 screen 枚举）
   - /api/world/map/units       # 周围单位（敌人/NPC）
   - /api/quest/active          # 当前任务
   - /api/system/events/        # 变化事件（扣血/升级/掉东西）

2. 决策（按优先级从上到下，命中即执行）
   P0 死亡      → screen=dialog_wipeout: 先保存可救→game_over→enter_slot 重进
   P0 弹窗      → screen=dialog_*（非 wipeout）: 处理对话（GET /api/ui/dialog → select）
   P0 剧情      → screen=dialog_story: select action=skip
   P0 教学暂停  → screen=tutorial_pause: 使用药水后恢复
   P1 血量过低  → hp < 30% max: use_item 药水
   P1 正在扣血  → 有敌人攻击: attack_target 反打（见 11.3）
   P1 寻路未完成→ 上一轮 move_to 还没走到: 等待/复查（见 11.3）
   P2 有任务    → 走到任务目标点 → start_interact → 对话推进
   P2 有敌人    → 打怪（升级+掉装备）→ 打完捡装备 → 换装
   P3 空闲      → 探索新地图/找任务点/回城整理背包

3. 行动（POST）
   - 移动：move_to / walk_dir+stop_move
   - 战斗：attack_target / cast_skill / stop_combat
   - 物品：use_item / equip_item / sell_item
   - 对话：start_interact → dialog/select
   - 存档：save（里程碑后必存）

4. 校验
   - 检查返回 ok 与 state；失败读 error 修正参数
   - 每个里程碑（升级/换装/交任务/捡好装备）后 POST /api/system/save
```

### 11.2 关键原则

1. **状态驱动，绝不次数驱动**：循环条件用"目标是否达成 / 状态是否正常"，不用"已执行 N 次"。固定次数循环遇到弹窗/死亡会卡死。
2. **每轮先查 screen 与 dialog**：任何行动前 `GET /api/ui/`，突发事件优先处理。
3. **生存优先**：血量低先喝药；被攻击先反打或逃跑；`set_auto_attack` 常开。
4. **里程碑必存档**：升级、换装、交任务、拾取好装备之后立即 `POST /api/system/save`——死亡只回退到上次存档点。
5. **操作后检查 state**：验证行动生效（如移动后坐标变化、用道具后数量变化）。
6. **目标定位用 units 的 slot**：攻击/交互都传场景单位的 `slot`（每帧会变，用前重新查询）。
7. **坐标换算**：瓦片坐标 ×16 = 像素坐标（move_to 用像素；地图接口会同时给出两种坐标）。
8. **寻路要轮询确认**：move_to 是异步走路，轮询坐标直到到达或超时；不可达就放弃。
9. **失败重试**：`{"error":"not ready"}` 等 1-2 秒重试；`not in game` 需先 enter_slot/create_slot。

### 11.3 典型 AI 决策伪代码（状态驱动）

```text
# 启动：进入游戏
info = GET /api/system/info
if info.save_slots 无 exists=true:  POST /api/system/create_slot {slot, class_idx}
else:                              POST /api/system/enter_slot {slot:0}

# 进入后确保能战斗：开启自动反击
POST /api/character/combat/0/set_auto_attack {on:true}

# 主循环：状态驱动，每轮最多 1 个行动，避免操作过载
loop:
    state = GET /api/ui/                    # ← 每次循环第一件事：看状态

    # P0 突发事件处理（任何情况下优先）
    if state.screen == "dialog_wipeout":    # 死亡
        POST ui/dialog/select {action:"game_over"}
        POST /api/system/enter_slot {slot:当前槽}
        POST /api/system/save
        continue
    if state.screen.startswith("dialog_"):  # 其他对话框占据（弹窗/剧情/NPC/任务等）
        dlg = GET /api/ui/dialog
        if dlg.type in (popup, save, sell, quest):  POST select {action:"ok"/"confirm"}
        elif dlg.type == "npc":                      POST select {index:0}
        elif dlg.type == "npc_quest":                POST select {action:"complete"}; POST save
        elif dlg.type == "story":                    POST select {action:"skip"}
        continue
    if state.screen == "tutorial_pause":    # 药水教学暂停
        POST item/inventory/use_item {bag, slot:药水}
        continue
    if state.screen.startswith("panel_"):   # 面板占据（背包/技能/设置等）
        POST ui/close_panel
        continue
    if state.screen != "world":             # 其他界面（加载/主菜单等）
        sleep 1s; continue

    snap = GET /api/system/snapshot

    # P1 生存：血量过低喝药
    if snap.party[0].hp < snap.party[0].max_hp * 0.3:
        找药水 → POST item/inventory/use_item {bag, slot}
        continue

    units = GET /api/world/map/enemies     # 战斗单位（type==1，已过滤装饰物）

    # P1 生存：正在被攻击（血量在降）→ 立即反打，不移动
    if 上轮 hp 值 > snap.party[0].hp:       # 刚扣血
        enemy = units 里最近的 type==1 且 distance>=0 的单位
        if enemy:  POST character/combat/0/attack_target {target_slot:enemy.slot}
        continue

    # P1 寻路：上轮在移动 → 检查是否到达
    if 有进行中的 move_to:
        if |snap.x - 目标x| < 16 且 |snap.y - 目标y| < 16:   # 已到达
            标记移动完成
        elif 已超时(10s):
            dist = GET /api/world/map/distance?tx=&ty=       # 复查可达性
            if not dist.found: 放弃该目标
            else: POST movement/move_to 重发一次
        continue

    # P2 任务优先
    quests = GET /api/quest/active
    if quests.quests 非空:
        找任务目标点（NPC 或区域）
        POST world/movement/move_to {x, y}
        if 到达 NPC 旁: POST ui/start_interact; continue

    # P2 打怪升级（任务不明确时）
    enemy = units 里最近的 type==1 且 distance>=0 的单位
    if enemy 且 等级差距不大:
        POST world/movement/move_to {x:enemy.x, y:enemy.y}
        if 距离近(像素差<48):  POST character/combat/0/attack_target {target_slot:enemy.slot}
        continue

    # P3 升级装备：打完怪后检查背包换装
    if 背包有新装备且属性更好:
        POST item/inventory/{role}/equip_item {bag, slot}
        POST /api/system/save
        continue

    # P3 空闲：探索
    找地图出口（/api/world/map/exits，x/y 瓦片坐标，像素 = ×16）→ POST world/movement/move_to {出口px, 出口py}

    sleep 1s
```

### 11.4 关键场景速查（遇到即处理）

| 场景 | 检测 | 处理 |
|---|---|---|
| 死亡 | `screen=dialog_wipeout` | `select game_over` → 存档 → `enter_slot` 重进 |
| 剧情打断 | `screen=dialog_story` | `select skip` |
| 弹窗阻塞 | `screen=dialog_popup` | 读 dialog → `select` 对应动作 |
| 教学暂停 | `screen=tutorial_pause` | 使用药水 |
| 被攻击扣血 | events 有 `hp` 下降 / snapshot hp 变小 | 立即 `attack_target` 反打最近敌人 |
| 血量过低 | hp < 30% max | `use_item` 药水 |
| 寻路卡住 | move_to 后坐标长时间不变 | 复查 `distance` 可达性，不可达则换目标 |
| 升级 | events 有 `level_up` | 分配属性点（add_stat）+ **立即存档** |
| 换装 | 背包有更高稀有度/攻击装备 | `equip_item` + 存档 |
| 交任务 | dialog `npc_quest` | `select complete` + 存档 |

---

## 12. 常见问题与注意事项

| 问题 | 处理 |
|---|---|
| 返回 `{"error":"not ready"}` | 游戏未就绪（刚启动），等 1-2 秒重试 |
| 返回 `{"error":"not in game"}` | 未进入存档，先 enter_slot / create_slot |
| 写操作无反应 | 先 `GET /api/ui/dialog` 检查弹窗，处理后再操作 |
| enter_slot 后崩溃 | 新版本会在原版进档前执行内部完整性检查；若仍异常，保留 tombstone 和存档字节快照 |
| 移动不生效 | 确认 `screen=world`；剧情/切图中操作自动终止 |
| 商店 items 为空 | 需先与商人交互进入商店界面 |
| attack 返回 `target not found` | target_slot 用 `map/units` 返回的 `slot`（每帧会变，重新查询） |
| cast 返回 `skill not learned` | action_id 必须是已学技能，先查 `party/{slot}/skills` |
| 等级溢出变负数 | 等级超过 127 会溢出显示为负；OP level 勿超 127 |
| 服务无法访问 | 确认游戏进程存活、同局域网、IP 正确 |

**安全与风险**：
- **无鉴权**：合法端点局域网内可访问，勿暴露公网
- **OP 门禁**：OP 端点默认关闭（`opEnabled=false` → 403）；开启后无额外鉴权，且破坏性大，用前先存档
- **OP 破坏性**：直改等级/属性/物品可能损坏存档，用前先存档
- **死亡处理**：全灭 → `screen=dialog_wipeout`；`revive` 在盗版版走网络链会失败，`game_over` 回主菜单可重进档
- **教学状态**：新档可能触发 `tutorial_pause`（药水教学），游戏暂停移动，需使用药水后恢复

---

## 13. 附录：参考表

### 13.1 主属性

| 索引 | 名称 | 效果（大修版） |
|---|---|---|
| 0 | 力量 | 物攻 + HP |
| 1 | 敏捷 | 物攻 + 命中 + 回避 |
| 2 | 体力 | HP + 防御 |
| 3 | 智力 | 魔攻 + MP |
| 4 | 精力 | 魔攻 + 命中 |

> API 参数（add_stat/set_attr）用英文名：`strength`/`agility`/`vitality`/`intelligence`/`spirit` 或索引 0-4。

### 13.2 stats 数组关键索引（战斗属性）

| 索引 | 属性 | 说明 |
|---|---|---|
| 0 | crit_rate | 暴击率 ×10（默认 30） |
| 3 | crit_damage | 暴击伤害 ×10（默认 1000） |
| 4 | attack | 攻击 |
| 8 | magic_attack | 魔攻 |
| 11 | magic_resist | 魔法抵抗 |
| 13 | dexterity | 敏捷（总） |
| 14 | hit_base | 命中率基数 |
| 15 | hit_rate | 命中率百分比 |
| 17 | defense | 防御 |
| 18 | phys_reduce | 物理减伤 |
| 19 | wdr | 武器伤害减免 ×10 |
| 20 | sub_weapon_attack | 副手武器攻击 |
| 28 | level_attr | 等级驱动属性 |
| 30 | max_hp | HP 上限 |
| 31 | max_mp | MP 上限 |

> ⚠️ 其余索引暂无定义，暂勿使用。

### 13.3 稀有度档位

| rarity | 档位 | 品质 |
|---|---|---|
| 0 | 白 | common |
| 1 | 绿 | uncommon |
| 2 | 蓝 | rare |
| 3 | 黄 | unique |
| 4 | 紫 | epic |

### 13.4 常用物品类别（category）

| category | 物品 |
|---|---|
| 1 | 治疗药水 |
| 5-8 | 恢复药水（小→大） |
| 15 | 超级药水 |
| 26/27 | 复活卷轴 |
| 51 | 技能书 |
| 62 | 佣兵封印卡 |
| 75/76 | 短剑/匕首 |
| 925/926 | 技能重置/属性重置 |
| 934-939 | 解封卷轴 |
| 1007-1009 | 开箱 |
| 16-25 | 强化卷轴（16-19 武器、20 混沌武器、21-24 防具、25 混沌防具） |
| 28-32 | 宝石（28 低级..32 混沌） |
| 52-56 | 骰子（陨子） |

> 完整物品名查询：`GET /api/system/tables/ITEMDATABASE/search?q=<名称>`

---

## 附：端点开放状态速查

| 端点 | 状态 |
|---|---|
| 全部 GET 读端点（角色/地图/背包/任务/界面/系统） | ✅ 可用（`quest/{id}` 恒 not found 除外） |
| POST 操作（移动/战斗/背包/队伍/成长/商店/对话/存档） | ✅ 可用 |
| `POST /api/character/combat/{role}/set_skill_usage` | ⏳ 未开放 |
| `GET /api/system/tables/{table}/download`、`/api/system/help`、`/api/system/download` | ⏳ 未开放 |
| `GET /api/quest/{id}` 单任务详情 | ⏳ 恒 not found |
| OP 越权端点 | 🔒 默认关闭（`opEnabled=false` → 403）；开启后 ✅ 10 个可用 / ⏳ 11 个未开放 |
