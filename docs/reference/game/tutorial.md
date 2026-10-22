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
- `TutorialDescription`（0x16cff4）：按教程 id 从 .so 内指针表（非 TEXTDATABASE）取提示文案；调用点全部在 `UIPlayPorting_Draw`（0xc8bd0/0xc912c/0xc9540/0xc9748/0xc9838/0xc9970）。

## 触发与绘制（UIPlayPorting_Draw 教程分支）

- 提示绘制条件：`tutorialdraw != 0` 且 `tutorialstate == N` 且 `TutorialDescription() != 0`，提示文案 + IMGSYS 组 0x16 闪烁箭头；状态与提示族对应（TEXTDATABASE 同文案行）：1=移动（row 174）、2=保存（row 175）、3/7/10=动作键/快捷键/菜单（rows 176-187）、6=药水（row 180）。
- 药水教学：HP 低触发 addstate(6)（frida 实测），伴随教学暂停（`tutorial_pause` screen 枚举）；`Tutorialpotioncheck` 只写教学 HUD 高亮标志（`t_tutorialhpotioncheck`/`t_tutorialpotion`），不门控治疗/战斗逻辑。
- 推进：玩家触摸高亮控件（控件 +0x68 字节置 1）→ `Tutorialgetstate` 轮转 → 下一态；教学完成链另见 `CHAR_ProcessShortcut` 0x9d3a4（[x19]==0xe 时调 getstate）。

## 模块处置

- 教学暂停阻断（兜底，存量存档残留）：`game/state/game_state.cpp` `tutorial_state()`/`tutorial_cancel()`——cancel 复现 getstate 侧效果（对象写 0 + 三标志），v0.4.50 起直接写 0 不依赖轮转返回；触发点 `ui_screen()`（state==5 且教学态 6 自动 cancel）与 `tutorial_block_error()`（world_port.cpp 世界操作前置）。
- 源头阻断（`popup_blocker` 插件，`P_TUTORIAL_ADDSTATE_SKIP`）：`Tutorialaddstate` 入口 RET，状态恒不入队——提示与暂停均不触发，`TutorialDescription` 不被调用。安全性：空队列返回 99/2（教学完成），与 cancel 终态一致；存量存档由上述 cancel 兜底。
- 屏蔽范围不含 Hub 登录询问（见 `ui.md` §2.2 `P_HUB_LOGIN_SKIP`）与一般游戏弹窗（见 `uipopupmsg.md`）。
