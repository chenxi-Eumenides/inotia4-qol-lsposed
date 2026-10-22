# 教程系统（新手教学）状态机

## 数据对象（monster 包 arm64-v8a）

| 对象 | 地址/常量 | 语义 |
|---|---|---|
| `pretutorialstate` 队列 | GOT 槽 `[0x2f4000+0xce0]` | 5 槽 int64 历史队列（+0/+8/+0x10/+0x18/+0x20），99=空槽 |
| 教学状态对象 | GOT 槽 `G_TUTORIAL_OBJ_GOT_VMA = 0x2f5000+0x170`（game_symbols.h:179） | 对象头 u64 = 教学状态：0=无、2=教学完成、6=药水教学激活 |
| 当前状态变量 | `PTR_tutorialstate`（0x2f5170 槽） | `UIPlayPorting_Draw` 教程分支的判定值 |
| 教学标志 ×3 | `G_TUTORIAL_FLAG1/2/3_GOT_VMA`（game_symbols.h:180-182） | cancel 语义：FLAG1=0、FLAG2=1、FLAG3=0 |

## 函数语义

- `TutorialStart`（0x16ceb0，`F_TUTORIAL_START_VMA`）：新档初始化。`tutorialstate=0`、`tutorialdraw=1`、10 处教学标志清零（`t_inappmarketcheck=1`，其余 0）、队列 5 槽全 99。
- `Tutorialaddstate(long)`（0x16ddc8）：状态入队。仅接受 >5 且 !=99 且不与历史重复；写入槽 0 并右移其余。调用点：`UIPlayPorting_Draw` 0xc9478、0xe48e0、0xe72d0、0xfceb8、0x114ec4（教程链/事件触发）。
- `Tutorialgetstate`（0x16de40，`F_TUTORIAL_GETSTATE_VMA`）：轮转消费——队列左移 1 槽、槽 4 写 99，返回旧槽 0；队列空（99）且 `t_inappmarketinfo==0` 时返回 2（教学完成）。
- `TutorialDescription`（0x16cff4，`F_TUTORIAL_DESCRIPTION_VMA`）：按教程 id 从 .so 内指针表（非 TEXTDATABASE）取提示文案；调用点全部在 `UIPlayPorting_Draw`（0xc8bd0/0xc912c/0xc9540/0xc9748/0xc9838/0xc9970）。绘制后返回非零表示存在提示；调用方会用返回值控制关联箭头。
- `UIPlayPorting_Draw`（0xc87a0，`F_UI_PLAY_PORTING_DRAW_VMA`）：入口分支读取教程状态并调用描述绘制。state=6 路径在 `+0xcf4`（0xc9494）执行 `strb w0,[x19]`（原字 `0x39000260`，w0=1），设置 `GAMESTATE_ProcessPlay`（0x9caf0-0x9cb10）读取的暂停门闩，GOT 槽为 `0x2f6000+0xbb8`。

## 触发与绘制（UIPlayPorting_Draw 教程分支）

- 提示绘制条件：`tutorialdraw != 0` 且 `tutorialstate == N` 且 `TutorialDescription() != 0`，提示文案 + IMGSYS 组 0x16 闪烁箭头。`TutorialDescription` 的 switch table 映射 `tutorialstate=0..7` 到 TEXTDATABASE row 174..181（row 176 每日奖励提示对应 state 2；row 180 药水提示对应 state 6），state 10→row 182、state 13→row 183；其余状态需按分支单独追踪。
- 药水教学：HP 低触发 addstate(6)（frida 实测），伴随教学暂停（`tutorial_pause` screen 枚举）；`Tutorialpotioncheck` 只写教学 HUD 高亮标志（`t_tutorialhpotioncheck`/`t_tutorialpotion`），不门控治疗/战斗逻辑。
- 推进：玩家触摸高亮控件（控件 +0x68 字节置 1）→ `Tutorialgetstate` 轮转 → 下一态；教学完成链另见 `CHAR_ProcessShortcut` 0x9d3a4（[x19]==0xe 时调 getstate）。

## 模块处置

- 存量/旧状态清理：`game/state/game_state.cpp` 的 `tutorial_state()` / `tutorial_cancel()` 保持原样。`ui_screen()`（state==5 且教学态 6）与 `tutorial_block_error()`（world 操作前置）仍可能执行 cancel；这是遗留状态清理，不是本次暂停抑制路径。
- `popup_blocker` 保留 `Tutorialaddstate` 队列写入和调用方状态写入。`TutorialDescription` entry hook 直接返回 0，不进入原函数，因此不绘制该函数负责的教程文案，也阻断调用方基于非零返回值绘制的关联提示；state 2 对应的 row 176 每日奖励文案也经过此入口。此机制不保证所有独立教学图形、高亮都消失。
- state=6 暂停抑制位于暂停源头：`P_TUTORIAL_PAUSE_GATE_SKIP` 将 `UIPlayPorting_Draw+0xcf4` 的 pause 门闩 `strb`（原字 `0x39000260`）替换为 NOP。state=6 写入保留，但 pause gate 不会被置 1；不会采用“先 pause 再 tutorial_cancel”的路径。
- 静态证据：monster 与 overhaul arm64 包的 `TutorialDescription`、`UIPlayPorting_Draw` 符号地址及 state=6 pause store 指令一致。该证据支持站点与指令目标；插件实际绘制、暂停与开关行为仍需真机验收。
- 屏蔽范围不含 Hub 登录询问（见 `ui.md` §2.2 `P_HUB_LOGIN_SKIP`）与一般游戏弹窗（见 `uipopupmsg.md`）。
