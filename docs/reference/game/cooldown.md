# 冷却机制（技能 / 药水 / 道具）

> 日期：2026-11-08（2026-11-10 补充复活卷轴与 monster v29 变体） ｜ 适用版本：`c18-v520`、`c18-v603`（冷却符号同址） ｜ 样本：`apk/decoded/c18-v520/lib/arm64-v8a/libgame.so`
> 来源：`readelf` 动态符号 + `aarch64-linux-gnu-objdump` 反汇编 + `apk/static-data/json/tables/ACTDATABASE.json`

## 1. 结论摘要

- 冷却全部在游戏 native 层实现；`apk/decompiled/` 的 Java/smali 中没有玩法冷却逻辑。
- 时间基准是**逻辑帧 tick**，不是毫秒：`g_nGlobalCoolTimeCount` 每帧 `+1`。
- 技能冷却：`D = 1.5 × ACTDATABASE[actionId][+0x0F] × CHAR_GetAttr(char, 95) / 1000` tick。
- 药水冷却：全局 `CHECKTIME_nPotionTick = now + 165` tick（共享，不按物品独立）。
- 道具/Buff 冷却：全局 `CHECKTIME_nBuffItemTick = now + 15` tick（共享）。
- 没有 sleep/定时器轮询；每帧比较时间戳。缩短冷却等价于提前改写时间戳。

## 2. 时间基准

| 项 | 位置 | 说明 |
|---|---|---|
| `g_nGlobalCoolTimeCount` | `0x3099b0`（`.data`，8 字节） | 全局逻辑帧计数，同时作为冷却的“当前时间” |
| 初始化 | `GAME_Initialize`（`0x100084`） | `*g_nGlobalCoolTimeCount = 0` |
| 递增 | `GAMESTATE_ProcessPlay`（`0x9cba0`） | 每次处理 `*g_nGlobalCoolTimeCount += 1` |

反汇编：

```asm
; GAMESTATE_ProcessPlay
9cb94: ldr  x19, [x19, #2896]   ; x19 = &g_nGlobalCoolTimeCount（经 GOT）
9cb9c: ldr  x0,  [x19]
9cba0: add  x0,  x0, #1
9cba4: str  x0,  [x19]
```

因此所有冷却常量单位均为 tick。按 60 FPS 估算：`165 tick ≈ 2.75 s`，`15 tick = 0.25 s`；技能按公式换算。实际秒数依赖游戏逻辑帧率，非固定 60 时需要真机校准。

## 3. 技能冷却

### 3.1 运行时结构

每个角色 `char+672`（0x2A0）是一条动作冷却链表，由 `CHAR_InitCoolTime`（`0xdbdb8`）清零：

| 节点偏移 | 类型 | 含义 |
|---|---|---|
| `+0x00` | u16 | actionId（`ACTDATABASE` 记录下标） |
| `+0x10` | u64 | 冷却结束 tick（`nextTime`） |
| `+0x18` | pointer | next 节点 |

`CHAR_InitCoolTime` 只遍历链表并把 `+0x10` 置 0。

### 3.2 时长计算

`CHAR_GetActionCoolTime(char, node)`（`0xe30c8`）：

```text
raw = MEM_ReadUint16(ACTDATABASE_pData + actionId * ACTDATABASE_nRecordSize + 0x0F)
D   = (raw * 15 / 10) * CHAR_GetAttr(char, 95) / 1000
```

- 数据表：GOT `0x2f49e0` → `ACTDATABASE_pData`（`0x301990`）；GOT `0x2f6150` → `ACTDATABASE_nRecordSize`（`0x301998`，值为 32）。
- `ACTDATABASE`：192 条、每条 32 字节；偏移 `+0x0F` 是 u16 冷却基数。
- `raw` 实际分布（`apk/static-data/json/tables/ACTDATABASE.json`）：`0, 40, 50, 60, 80, 90, 100, 120, 170, 180, 300, 900` 等；`0` 表示无冷却。
- 除法魔数：`0x66666667`（÷10）、`0x20C49BA5E353F7CF`（÷1000），均为整数运算并按顺序截断。
- `CHAR_GetAttr(char, 95)` 是 per-mille 倍率（1000 = 100%）。属性名尚未从现有文档/符号确认。

### 3.3 设置与判定

设置（`CHAR_SetActionNextTime`，`0xe31f4`）：

```text
D = CHAR_GetActionCoolTime(char, node);
node->nextTime = g_nGlobalCoolTimeCount + D;
```

判定（`CHAR_GetActionState(char, node, flag)`，`0xe3228`）：

```text
if (node->nextTime > g_nGlobalCoolTimeCount) return 2;  // 冷却中
```

返回值：`0` 可用、`1` 魔法不足、`2` 冷却中、`3` 条件不满足、`7` 空节点。魔法检查使用 `ACT_GetMana` 与角色 `+0x1F4` MP。

技能施放入口 `CHAR_SetAction`（`0xe7630`）在扣蓝后调用 `CHAR_SetActionNextTime`；`CHAR_SetActionID`（`0xe79ec`）是动作 ID 入口。

### 3.4 UI 剩余时间

`SHORTCUT_DrawCoolTime`（`0x121e8c`）对技能条目读取 `node+0x10` 与当前 tick，调用：

```text
SHORTCUTPorting_Draw_CoolPie(x, y, nextTime, now, D)
```

即用 `nextTime - now` 与总时长 `D` 画冷却饼。

### 3.5 批量设置（导出但无内部调用）

`CHAR_SetActionCoolTimeAll(char, n)`（`0xe3160`）：

```text
for node in char->cooldownList:
    node->nextTime = min(node->nextTime, g_nGlobalCoolTimeCount + 10 * n)
```

`n` 以 10 tick 为单位。反汇编中未发现库内 `bl` 调用点，属于导出给外部/调试用途的函数。

## 4. 药水与道具冷却

### 4.1 全局时间戳

| 全局变量 | 地址 | 含义 | 写入值 |
|---|---|---|---|
| `CHECKTIME_nPotionTick` | `0x307568` | 恢复类药水冷却结束 tick | `now + 165`（`0xA5`） |
| `CHECKTIME_nBuffItemTick` | `0x307560` | Buff 类道具冷却结束 tick | `now + 15`（`0x0F`） |

两者都是**全局共享**：同类物品共用一个冷却，不是每件物品独立计时。

### 4.2 `CHAR_UseItemEx` 流程（`0xeb670`）

物品分类字段来自 `ITEMDATABASE` 记录：

| 偏移 | 判断 | 分支 |
|---|---|---|
| `+0x02` | bit4 | 特殊使用路径（先于普通冷却检查） |
| `+0x06` | bit5 | 恢复类（`ITEMSYSTEM_FindInRecover`）→ 使用 `CHECKTIME_nPotionTick` |
| `+0x07` | bit0 | Buff 类（`ITEMSYSTEM_FindInBuff`）→ 使用 `CHECKTIME_nBuffItemTick` |

恢复类冷却检查与写入（简化）：

```text
// 使用前
if (CHECKTIME_nPotionTick >= g_nGlobalCoolTimeCount) return 0;   // 拒绝，不消耗
// 使用成功后
CHECKTIME_nPotionTick = g_nGlobalCoolTimeCount + 165;
```

Buff 类同理，写入 `+15`。

额外修正：`MERCENARYGROUPSKILLSYSTEM_CheckCondition(0x8D)` 成立时，写入后再减去 `0x2D`（45 tick），即实际为 `now + 120` / `now - 30`。

`CHAR_UseItemEx` 返回 `0` 表示未使用（冷却中或其他失败），非 `0` 表示使用成功。

### 4.3 复活卷轴

`ITEMDATABASE` 记录下标 26/27 分别是「复活卷轴」和「优质的复活卷轴」（`apk/static-data/json/tables/ITEMDATABASE.json`）。这两类物品直接走复活卷轴处理路径（`CHAR_ProcessReviveScroll`，`0x0de3d8`），反汇编中不引用 `CHECKTIME_nPotionTick` 或 `CHECKTIME_nBuffItemTick`，因此**没有全局药水/Buff 冷却**，连续使用只受物品数量和复活目标状态限制。

## 5. 符号与地址（c18-v520 / c18-v603 arm64）

| 符号 | VMA | 作用 |
|---|---|---|
| `CHAR_InitCoolTime` | `0x0dbdb8` | 清零角色冷却链表 |
| `CHAR_GetActionCoolTime` | `0x0e30c8` | 计算技能冷却时长 |
| `CHAR_SetActionNextTime` | `0x0e31f4` | 写入单个动作冷却结束时间 |
| `CHAR_SetActionCoolTimeAll` | `0x0e3160` | 批量设置动作冷却 |
| `CHAR_GetActionState` | `0x0e3228` | 冷却/魔法/条件判定 |
| `CHAR_SetAction` | `0x0e7630` | 技能行动入口 |
| `CHAR_UseItemEx` | `0x0eb670` | 物品使用与药水冷却 |
| `CHAR_ProcessReviveScroll` | `0x0de3d8` | 复活卷轴使用处理；不读取 `CHECKTIME_*` 冷却 |
| `SHORTCUT_DrawCoolTime` | `0x121e8c` | 快捷栏冷却饼 |
| `g_nGlobalCoolTimeCount` | `0x3099b0` | 全局 tick |
| `CHECKTIME_nPotionTick` | `0x307568` | 药水冷却结束 tick |
| `CHECKTIME_nBuffItemTick` | `0x307560` | Buff 道具冷却结束 tick |
| `ACTDATABASE_pData` | `0x301990` | 动作表数据指针 |
| `ACTDATABASE_nRecordSize` | `0x301998` | 动作表记录大小（32） |

以上均为动态导出符号（`.dynsym`），可按名称解析；VMA 仅供无符号场景回退。

## 6. 字段的通用含义

- 缩短技能冷却：改写对应节点 `+0x10`（单技能独立）。
- 缩短药水冷却：改写 `CHECKTIME_nPotionTick`（会影响所有恢复类药水，因为共享）。
- 取消冷却：把结束 tick 写成当前 tick 或更小。
- 只读展示剩余时间：`nextTime - g_nGlobalCoolTimeCount`；物品用 `CHECKTIME_*` 同类计算。

## 7. 使用范围（调用者）

### 7.1 技能/动作冷却

判定入口 `CHAR_GetActionState` 的调用者：

| 调用者 | 调用点 | 用途 |
|---|---|---|
| `CHAR_ProcessNormalAIOnCombat` | `0xe4a78` | 非玩家 AI（队友/佣兵等）选技能前检查 |
| `MONSTERAI_RunAIProc` | `0xacd3c` | 怪物 AI 选技能前检查（`CHAR_FindAction` 后） |
| `SHORTCUT_GetState` | `0x121c08` | 快捷栏 UI 状态 |

写入链路：`CHAR_SetActionNextTime` 的唯一调用者是 `CHAR_SetAction`（`0xe7954`）；`CHAR_SetAction` 的调用者包括 `CHAR_BeginAction`、`CHAR_StartActionEx`、`CHAR_ActionAI`、`CHAR_SetActionForEvent`、`CHARSYSTEM_UpdateTownAll`。

- 玩家路径：`GAMESTATE_PressKeyPlay` → `CHAR_ProcessShortcut` → `SHORTCUT_GetState` → `CHAR_StartActionEx` → `CHAR_SetAction`。
- 非玩家路径：`CHAR_Process`（角色 ≠ 当前操作角色）→ `CHAR_ProcessAI`（`0xf21bc`）→ `CHAR_ProcessAIOnCombat` → `CHAR_ProcessNormalAIOnCombat`（普通 AI）或 `MONSTERAI_RunAIProc`（怪物 AI）→ `CHAR_GetActionState` → `CHAR_StartActionEx`。
- 队友/佣兵是 AI 控制的队友角色，同样走该路径，因此受技能冷却约束。
- 事件脚本：`EVTSYSTEM_Process` → `CHAR_SetActionForEvent` → `CHAR_SetAction`，同样写入冷却。

清空全部冷却：`CHAR_InitCoolTime` 的调用者为 `CHARSYSTEM_Revive`（复活）、`CHAR_SuccessOnPhysicalAttack`（属性 70 概率触发）、`CHAR_Act_DD`（属性 54 概率触发）。

### 7.2 药水/道具冷却

`CHAR_UseItemEx` 的调用者：

| 调用者 | 调用点 | 用途 |
|---|---|---|
| `UIEquip_ButtonUseExe` | `0xb8100` | 装备/物品详情“使用”按钮 |
| `UIEquip_OKConfrimUseItem` | `0xb84ec` | 确认后使用 |
| `CHAR_ProcessShortcut` | `0xec1f8` | 快捷栏按键使用 |
| `CHAR_PickManaGem` | `0xec4ac` | 拾取法力宝石时使用 |
| `CHAR_PickItemAll` | `0xec744` | 拾取地图物品时使用 |

UI 判定与绘制：`SHORTCUT_GetState`（`0x121c68`、`0x121d14`）、`SHORTCUT_DrawCoolTime`。

**没有怪物 AI 或佣兵 AI 使用药水/道具的路径**：所有 `CHAR_UseItemEx` 调用都来自玩家 UI、快捷栏或玩家拾取流程。

### 7.3 其他 CHECKTIME 计数

`CHECKTIME_nLifeTick`、`CHECKTIME_nUnitTick`、`CHECKTIME_nSiegeItemTick`、`CHECKTIME_nFreeWillTick` 等是独立系统的计时器，不参与物品/技能冷却；`CHECKTIME_Initialize` 只负责统一初始化。

## 8. 变体差异（monster v29）

样本：`apk/decoded/monster-v29/lib/arm64-v8a/libgame.so`（旧 `apk/decoded/monster` 是 v23）。c18-v520 / c18-v603 / 大修 / 原版的 `CHAR_GetActionCoolTime` 与 `CHAR_SetActionNextTime` 是正常函数；monster v29 把两者改成入口 `b` 跳板：

- `CHAR_GetActionCoolTime`：入口 `b 0x747400`，真实逻辑在额外可执行段 `0x740000`。该段分派链存在固定返回 `150 / 300 / 60 / 100 / 200` tick 的分支，说明部分动作的冷却不经过 `ACTDATABASE +0x0F × attr95` 公式。
- `CHAR_SetActionNextTime`：入口 `b 0x14e600`；跳板目标先执行正常写入，再按角色字段 `char[10]` 与全局难度条件覆盖 `nextTime`（观察样例为 `now + 18000`）。因此最终冷却可能远大于 `CHAR_GetActionCoolTime` 的计算值。
- 只观察 `CHAR_GetActionCoolTime` 的返回值不能代表 monster v29 的最终冷却；实际结束时间以 `CHAR_SetActionNextTime` 返回后的 `action+0x10` 为准。

## 9. 未确认项

- `CHAR_GetAttr(char, 95)` 的属性名称与来源表尚未确认，只确认它是 per-mille 倍率。
- tick 与真实时间的换算按“每帧 +1”推断；游戏若使用固定步长或变速，需要真机计时校准。
- `ACTDATABASE` 同时是技能动作参数表：`+0x0F` u16 为冷却基数，已由 `CHAR_GetActionCoolTime` 直接寻址确认；它在 `game-systems.md` 中仍归入事件/演出相关表，两重用途并存。
- `ITEMDATABASE +0x02 bit4`、`+0x06 bit5`、`+0x07 bit0` 与物品类别的完整对应关系未逐项枚举；已确认记录下标 26/27 为两类复活卷轴且不参与药水/Buff 冷却。

## 10. 复现方法

```bash
SO=apk/decoded/c18-v520/lib/arm64-v8a/libgame.so
readelf -sW "$SO" | grep -iE 'CoolTime|Revive|PotionTick|BuffItemTick'
readelf -rW "$SO" | grep -E '2f49e0|2f6150|2f4b50|2f4ba0|2f4740'
aarch64-linux-gnu-objdump -d --start-address=0xe30c8 --stop-address=0xe31f4 "$SO"
aarch64-linux-gnu-objdump -d --start-address=0xeb670 --stop-address=0xec028 "$SO"

# monster v29：两个入口都是 b 跳板
SO=apk/decoded/monster-v29/lib/arm64-v8a/libgame.so
aarch64-linux-gnu-objdump -d --start-address=0x747400 --stop-address=0x747480 "$SO"
aarch64-linux-gnu-objdump -d --start-address=0x14e600 --stop-address=0x14e680 "$SO"
```

GOT 槽位与目标全局的映射以 `readelf -rW` 的 `R_AARCH64_RELATIVE` 加数为准（例：`0x2f49e0 -> 0x301990 = ACTDATABASE_pData`）。

`ACTDATABASE` 实测偏移提取：

```python
import json
d = json.load(open("apk/static-data/json/tables/ACTDATABASE.json", encoding="utf-8"))
vals = [int.from_bytes(bytes.fromhex(r["hex"])[15:17], "little") for r in d["records"]]
```
