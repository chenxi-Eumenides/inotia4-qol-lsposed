# `popup_blocker` 插件参考

## 作用

功能插件：屏蔽新手教程提示及由教程状态触发的教学暂停。插件只控制教程状态入队入口，不屏蔽一般游戏弹窗。

## 功能

- 插件 id `popup_blocker`，版本 `0.1.0`（`plugins/popup_blocker/plugin.cpp`）。
- 唯一 feature：`popup_blocker.enabled`，名称「弹窗屏蔽」，分类 `BasicOptimization`，类型 toggle，默认启用。
- feature 开启时安装 `P_TUTORIAL_ADDSTATE_SKIP`；关闭时由 Host lifecycle 撤销 claim 并恢复原指令。

## 依赖

- 不声明 required/optional capability，不提供 capability。
- 无 JNI、HTTP、面板、命令或端口；只声明一个 `kCallPatch` 资源。
- 注册于 `host/assembly.cpp`，资源目标和原字统一由 Game Adapter `game/access/patches.cpp` 登记。

## 范围

游戏的 `Tutorialaddstate(long)`（VMA `0x16ddc8`）将教学状态压入 5 槽轮转队列，仅接受大于 5 且不等于 99 的状态。`Tutorialgetstate` 消费队列状态；`UIPlayPorting_Draw` 按教学状态调用 `TutorialDescription`（VMA `0x16cff4`）绘制对应提示文案，并由教学态触发教学强制交互或暂停（状态 6 为药水教学暂停）。

对 `Tutorialaddstate` 入口首条指令做 RET 替换后，教学状态恒不入队，因此教程 HUD 提示、教学强制交互和教学暂停的源头均不触发，`TutorialDescription` 不会被调用。影响范围是教学提示及教学暂停，不涵盖普通弹窗、剧情对话或其它 UI。

## 实现机制

| 项目 | 值 |
|---|---|
| claim | `P_TUTORIAL_ADDSTATE_SKIP`，owner=`popup_blocker` |
| site | `F_TUTORIAL_ADDSTATE_VMA + 0x0` |
| 改写 | `0xf100141f`（`cmp x0,#5`）→ `0xd65f03c0`（ARM64 `ret`） |
| 机制 | `kCallPatch` / `kWordReplace`，required、fail-closed、私有资源 |

资源的 `gate` 为空，由 Host 按 feature/plugin lifecycle 安装和物理还原。单 feature 默认启用，因此默认安装 RET；关闭 feature 后恢复原始 `cmp` 指令，重新允许教程状态入队。

## 边界

- 安全性依据：空教学队列由 `Tutorialgetstate` 返回教学终态 99（或 2=「教学完成」），与模块既有 `tutorial_cancel()` 产生的终态一致；真机已验证终态 0 可正常游戏。
- `Tutorialpotioncheck` 只写教学 HUD 高亮标志，不门控治疗或战斗逻辑。
- 存量存档中的教学残留仍由 `game_state.cpp` 现有 `tutorial_cancel()` 兜底；本插件不改变 `tutorial_state` / `tutorial_cancel`。
- `P_HUB_LOGIN_SKIP` 是 Host 常驻 patch，不属于本插件，也不受本 feature 控制。
- 游戏符号/VMA 仅登记在 `game/symbols/game_symbols.h` 与 `symbol_registry.h`；插件不持有裸地址。
