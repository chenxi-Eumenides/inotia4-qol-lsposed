# `combat_move` 内部实现

> 对外行为契约见 [`combat_move.md`](combat_move.md)。本文件只记录资源、判定逻辑、地址与限制。

## 1. 装配与资源

插件目录 `module/app/src/main/cpp/plugins/combat_move/`，静态库 `qol_plugin_combat_move`。
全部资源跟随 feature 生命周期，`atomic`、`resumable`、`Required`、`FailClosed`：

| 资源 id | 符号 | 机制 | 语义 |
|---|---|---|---|
| `patch:GAMESTATE_PressKeyPlay+0x1d4` | `P_GAMESTATE_PRESSKEYPLAY_KEEP_TARGET` | call patch（BL 重定向） | 攻击移动态下忽略 `CHAR_SetTarget(ch, 0)` |
| `patch:CHAR_Move+0x40` | `P_CHAR_MOVE_IS_NO_MOVE` | call patch（BL 重定向） | 攻击移动态下让 `CHAR_IsNoMove` 结果为可移动 |
| `hook:CHAR_SetAction` | `F_CHAR_SET_ACTION_VMA` | entry hook | 攻击移动态下丢弃走路动作切换 |
| `hook:CHAR_StartActionEx` | `F_CHAR_START_ACTION_EX_VMA` | entry hook | 普攻出手前的距离门（超距取消本次出手，保留目标） |

私有端口 `QOL_PORT_COMBAT_MOVE`（v1.0，仅 `combat_move` 可声明）。端口只提供只读判定和一个受控写原语
（`set_target`，用于非锁定态透传清目标）；游戏调用全部在 `game/adapter_ports/combat_move_port.cpp` 内完成。

## 2. 攻击移动态判定

无状态、无帧轮询，每次 hook 调用即时计算：

```
enabled
&& 当前出战主角（ch+0x2FA == 0 且 ch+0x09 type == 0）
&& ch+0x278 目标非空
&& ch+0x280 当前动作非空且 CHAR_IsDefaultAttack(ch, 当前动作)
```

`CHAR_IsDefaultAttack`（0xda18c）同时接受“默认攻击动作指针”和 ACTDATABASE 默认攻击位，避免武器/职业差异导致
动作 id 比较失配。判定结果只用于三处：保留目标、放行移动、抑制走路动作。

## 3. 范围门

`CHAR_StartActionEx` 入口：

```
target_locked && 本次动作为默认攻击
  且 距离 > effective_range(动作攻击距离) → 返回 0（取消本次出手，不清目标、不停战）
否则 → 原函数
```

- `target_locked` = 开关开启 && 当前出战主角（`ch+0x2FA == 0` 且 `ch+0x09 type == 0`）&& `ch+0x278` 目标非空。
  保留目标不依赖当前动作，因此取消出手后进入空闲仍不会在移动时被清锁。
- `attacking` = `target_locked` && `ch+0x280` 当前动作为默认攻击；只有它触发走路动作抑制与移动放行。
- 余量：`margin% = clamp(10 + (13 - range) * 10, 10, 100)`，`effective_range = range + range * margin% / 100`。
  取值来自 ACTDATABASE 基础攻击动作距离 `+0x05`（现为 4/6/12/13）：4→+100%、6→+80%、8→+60%、12→+20%、13→+10%。
- 距离用 `CHAR_GetDistance`，范围用 `CHAR_GetActDistance`（动作表距离 + attr75），与
  `CHAR_ProcessNormalAIOnCombat` 构造目标列表时同口径。
- 超距只取消本次动作启动，不调用 `CHAR_SetTarget`/`CHAR_StopCombat`；回到范围内后由原版动作链再次尝试。

## 4. 关键常量（C18 arm64）

| 常量 | 值 | 说明 |
|---|---:|---|
| `F_GAMESTATE_PRESSKEYPLAY_VMA` | `0x9cfc0` | 方向键接收器 |
| `F_GAMESTATE_PRESSKEYPLAY_KEEP_TARGET_CALL_OFF` | `0x1d4` | `bl CHAR_SetTarget`，原字 `0x9400fd70` |
| `F_CHAR_MOVE_VMA` | `0xe9808` | 方向键移动 |
| `CHAR_MOVE_IS_NO_MOVE_CALL_OFF` | `0x40` | `bl CHAR_IsNoMove`，原字 `0x97ffde5b` |
| `F_CHAR_SET_ACTION_VMA` | `0xe7630` | 动作切换汇合点（`StartActionID`/`SetActionID` 均至此） |
| `F_CHAR_START_ACTION_EX_VMA` | `0xe8950` | 动作启动入口 |
| `F_CHAR_IS_DEFAULT_ATTACK_VMA` | `0xda18c` | 默认攻击动作判定 |
| `F_CHAR_IS_NO_MOVE_VMA` | `0xe11b4` | 原移动闸门（状态掩码 269） |
| `F_CHAR_GET_ACT_DISTANCE_VMA` | `0xe0fa4` | 动作攻击距离 |
| `C_ACTIVE_PLAYER_FLAG` | `0x2FA` | 0=当前出战主角 |
| `C_ACTIVE_SKILL` | `0x280` | 当前动作节点（`CHAR_SetAction` 写入） |

## 5. 与其它插件的关系

- `better_move` 占用 `hook:GAMESTATE_PressKeyPlay` 与 `patch:GAMESTATE_PressKeyPlay+0x21c`；本插件只用同一函数内的
  `+0x1d4` 调用点，机制不冲突。两者同时启用时需要真机确认朝向/对齐与移动 wrapper 的叠加顺序。
- `push_all` 在 `CHAR_Move` 内有两处调用点 patch（`+0x180`、推怪 BL），与本插件 `+0x40` 不重叠。
- `better_move` 在 `CHAR_SetAction+0x26c` 有字替换；本插件是入口 hook，二者可共存。

## 6. 限制与未验证项

1. 移动中普攻是否自动连段取决于原版动作链；若动作链要求站立，需要另加“动作结束事件补触发”，当前未实现。
2. 方向变化时原版 1–2 帧对齐停顿未消除；如需零停顿，需要额外处理 `PressKeyPlay` 的两处对齐早退。
3. 未改变 `CHAR_IsNoMove` 的全局语义，因此普攻期间的击退免疫保持原版。
4. 全部结论基于 C18 arm64 静态反汇编；monster/原版变体的站点原字与行为未验证，资源按 fail-closed 处理。
5. 真机验收未执行，见 `VM-CM-01`。
