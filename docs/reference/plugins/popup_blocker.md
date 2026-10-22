# `popup_blocker` 插件参考

## 作用

功能插件：尝试从教程状态队列入口阻止新手教程。当前实现只拦截 `Tutorialaddstate`，**不能屏蔽所有教程提示或暂停**；已知每日奖励 row 176 会通过空队列时 `Tutorialgetstate` 返回 state 2 绕过，且其它调用方会在 addstate 后直接写教程状态。插件不屏蔽一般游戏弹窗。

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

对 `Tutorialaddstate` 入口首条指令做 RET 替换，只能保证该函数不再向 5 槽队列写入状态；不能推出教程状态恒不出现：`Tutorialgetstate` 的空队列分支可返回 2（对应 row 176 每日奖励提示），而多个调用方会在 addstate 返回后直接写 `PTR_tutorialstate`。因此现有补丁不是完整的教程提示/教学暂停屏蔽，`TutorialDescription` 仍可能被调用。完整屏蔽尚待实现。

## 实现机制

| 项目 | 值 |
|---|---|
| claim | `P_TUTORIAL_ADDSTATE_SKIP`，owner=`popup_blocker` |
| site | `F_TUTORIAL_ADDSTATE_VMA + 0x0` |
| 改写 | `0xf100141f`（`cmp x0,#5`）→ `0xd65f03c0`（ARM64 `ret`） |
| 机制 | `kCallPatch` / `kWordReplace`，required、fail-closed、私有资源；仅阻止队列入队，不覆盖空队列状态 2 或调用方直接写入 |

资源的 `gate` 为空，由 Host 按 feature/plugin lifecycle 安装和物理还原。单 feature 默认启用，因此默认安装 RET；关闭 feature 后恢复原始 `cmp` 指令，重新允许教程状态入队。开启该 feature 不保证所有教程提示消失。

## 边界

- 安全性依据：空教学队列由 `Tutorialgetstate` 返回教学终态 99（或 2=「教学完成」），与模块既有 `tutorial_cancel()` 产生的终态一致；真机已验证终态 0 可正常游戏。
- `Tutorialpotioncheck` 只写教学 HUD 高亮标志，不门控治疗或战斗逻辑。
- 存量存档中的教学残留仍由 `game_state.cpp` 现有 `tutorial_cancel()` 兜底；本插件不改变 `tutorial_state` / `tutorial_cancel`。
- `P_HUB_LOGIN_SKIP` 是 Host 常驻 patch，不属于本插件，也不受本 feature 控制。
- 游戏符号/VMA 仅登记在 `game/symbols/game_symbols.h` 与 `symbol_registry.h`；插件不持有裸地址。
