# `popup_blocker` 插件参考

## 作用

功能插件：抑制 `TutorialDescription` 负责的教程文案与其非零返回值控制的关联提示，并阻止 state=6 设置教学暂停门闩。一般弹窗、剧情对话及独立教学图形/高亮不在保证范围内。

## 功能

- 插件 id `popup_blocker`，版本 `0.1.0`（`plugins/popup_blocker/plugin.cpp`）。
- 唯一 feature：`popup_blocker.enabled`，名称「弹窗屏蔽」，分类 `BasicOptimization`，类型 toggle，默认启用。
- 开启时，Host lifecycle 安装教程描述 entry hook 与 state=6 暂停门闩 patch；关闭时撤销资源，恢复原教程行为。

## 依赖

- 不声明 required/optional capability，不提供 capability。
- 无 JNI、HTTP、面板、命令或端口；声明一个 `kEntryHook` 和一个 `kCallPatch` 资源。
- 注册于 `host/assembly.cpp`，资源符号/站点由 `game/symbols/` 与 `game/access/patches.cpp` 登记。

## 范围

教学状态仍通过原版 `Tutorialaddstate(long)` 队列及 `UIPlayPorting_Draw` 状态写入流转，不拦截 addstate，也不清除 state=6。

`TutorialDescription`（VMA `0x16cff4`）entry hook 直接返回 0，不调用原函数；因此原函数不执行文案绘制，调用方基于其非零返回值控制的关联提示也关闭。state 2 对应的 TEXTDATABASE row 176 每日奖励提示同样经由此入口。插件不承诺独立绘制的图形或高亮全部消失。

state=6 暂停门闩由 `UIPlayPorting_Draw`（VMA `0x0c87a0`）`+0xcf4` 写入；暂停读取发生在 `GAMESTATE_ProcessPlay` `0x9caf0-0x9cb10`，对应 GOT `0x2f6000+0xbb8`。本插件 NOP 的是门闩写入，不是 state=6 写入，所以不会先进入 pause 再用 `tutorial_cancel()` 清除。

## 实现机制

| claim | 目标 | 改写/处理 | owner |
|---|---|---|---|
| `P_TUTORIAL_PAUSE_GATE_SKIP` | `F_UI_PLAY_PORTING_DRAW_VMA + 0xcf4` | `0x39000260` (`strb w0,[x19]`) → `ARM64_NOP_WORD` (`0xd503201f`) | `popup_blocker` |
| `hook:TutorialDescription` | `F_TUTORIAL_DESCRIPTION_VMA` | handler 直接返回 `int32_t(0)`；不调用原函数 | `popup_blocker` |

两项资源均为 private、required、fail-closed、resumable，并随 feature lifecycle 安装/还原。暂停门闩 patch 先安装；两项 claim 属同一原子组，安装失败时 Host 回滚已安装资源。EntryHook handler 不使用原函数 backup，因此 descriptor 不声明 `on_resource_ready`。

## 边界

- `game_state.cpp` 的 `ui_screen()` / `tutorial_block_error()` 与 `tutorial_cancel()` 不变。它们仍可清理旧状态，是 legacy cleanup，不是本次抑制 pause 的机制。
- `Tutorialaddstate` 的状态写入保留；暂停 gate store 被 NOP，避免设置暂停标志。
- `P_HUB_LOGIN_SKIP` 是 Host 常驻 patch，不属于本插件。
- monster 与 overhaul arm64 的静态符号地址及 pause store 指令一致；这仅验证静态目标。真机上教程文案、关联提示、state=6 暂停及 feature 往返仍待验收。
