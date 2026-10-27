# `popup_blocker` 插件参考

除特别说明，`plugins/...`、`contracts/...`、`game/...`、`host/...` 路径均相对
`module/app/src/main/cpp/`。

## 摘要

功能插件：抑制原生教程文案及其关联提示，并在暂停门闩写入前跳过 state 6（药水教学）的暂停设置。
插件保留原版教程状态队列和状态写入，不通过删除状态或“暂停后再解除”来隐藏效果。

不保证独立绘制的教学图形、高亮或焦点框全部消失；不屏蔽剧情对话、一般原生弹窗或 Java 层弹窗。

## 定位与注册

- 插件 id：`popup_blocker`；版本：`0.1.0`（`plugins/popup_blocker/plugin.cpp:15-16`）。
- descriptor：`plugin.cpp:96-113`；静态插件注册：`host/assembly.cpp` 中的 built-in plugin 清单。
- CMake 目标：`plugins/popup_blocker/CMakeLists.txt` 声明 `qol_plugin_popup_blocker`；`plugins/CMakeLists.txt` 自动发现插件目录并链接目标。
- 插件不声明 required/optional capability、provides、JNI、HTTP 路由、port、面板或命令。

## Feature 与设置

唯一 feature：

| feature id | 名称 | 类型 | 默认值 | 分类 | 回调 |
|---|---|---|---:|---|---|
| `popup_blocker.enabled` | 禁用弹窗 | toggle | 开 | `BasicOptimization`（基础优化设置） | 原生侧 no-op（资源由 Host feature/plugin lifecycle 管理）；Kotlin `PluginRuntimeEffects` 订阅同一 feature 的 desired 值，驱动 Android 层「更新通知」「Hive 内购选择」弹窗的放行/拦截 |

Host 根据 feature 值派生插件生命周期。开启时安装两个声明式资源；关闭时撤销资源，恢复教程文案绘制和暂停门闩写入。插件本身不在 hook handler 中查询 feature 状态。

Android 层由 `PluginRuntimeEffects` 以 plugin id `popup_blocker` 订阅 Host runtime effect：
`desiredEnabled=false` 时 `PopupBlockers` 的更新通知、内购选择与 `Connecting..` 进度框 hook 直接
`chain.proceed()`（恢复原版弹窗/进度框），开启时回到拦截。启动早期尚未收到首个 effect 时按默认开启处理。
同意页拦截/自动关闭不随该开关关闭——它承担读档流程的数据完整性保护（同意页 Activity 在进档期间启动
会打断读档），不是可选的弹窗体验。

## 游戏内行为

### 教程文案和关联提示

| 项目 | 值 |
|---|---|
| 目标函数 | `TutorialDescription`，VMA `0x16cff4` |
| 资源 id | `hook:TutorialDescription` |
| 机制 | `kEntryHook`；handler 返回 `int32_t(0)`，不调用原函数 |
| handler | `plugins/popup_blocker/plugin.cpp:22-28` |

原版 `TutorialDescription` 会按 `tutorialstate` 生成并绘制教程文案，返回非零表示有提示；`UIPlayPorting_Draw` 用该返回值控制关联箭头等提示绘制。只在原函数执行后把返回值改成 0 太晚，因为原函数已经产生绘制副作用。因此本插件在入口短路原函数并返回 0。

TEXTDATABASE row 176「点击左侧闪烁的收费商店按钮，可以获得每日奖励」对应 `tutorialstate=2`，也经过 `TutorialDescription`，因此受此 hook 抑制。教程状态仍可入队、轮转或由调用方直接写入；本插件不 patch `Tutorialaddstate`，也不清零 `tutorialstate`。

handler 通过 Host SDK 的 `InFlightGuard` 持有 `hook:TutorialDescription` 在途计数。它不使用原函数 backup，descriptor 因此不声明 `on_resource_ready`。正常资源撤销仍由 Host hook lifecycle 执行。

### state 6 教学暂停

| 项目 | 值 |
|---|---|
| 写入函数 | `UIPlayPorting_Draw`，VMA `0x0c87a0` |
| 资源 id | `P_TUTORIAL_PAUSE_GATE_SKIP` |
| 站点 | `F_UI_PLAY_PORTING_DRAW_VMA + 0xcf4` = `0x0c9494` |
| 原指令 | `0x39000260`：`strb w0, [x19]`，此处 `w0=1` |
| 替换 | `ARM64_NOP_WORD` = `0xd503201f` |

state 6 的状态值仍按原版路径写入；只跳过把教学暂停门闩设为 1 的那条 store。`GAMESTATE_ProcessPlay`（`0x9caf0-0x9cb10`）读取该门闩（GOT `0x2f6000+0xbb8`）并据此跳过游戏逻辑处理。NOP 位于暂停写入点之前的效果路径，因此不是先暂停再调用 `tutorial_cancel()`。

`game_state.cpp` 中既有的 `ui_screen()`、`tutorial_block_error()` 和 `tutorial_cancel()` 未被本插件改写；它们可能清理旧状态，但不是本插件阻止 state 6 暂停的机制。存量状态清理与暂停源头抑制是两个不同职责。

## 资源声明与生命周期

两个 claim 都是 `private / required / fail_closed / resumable=true`、`gate=nullptr`，并声明 `atomic=true`（`plugin.cpp:30-55`）。同一原子组按 descriptor 顺序先安装暂停门闩 NOP，再安装教程文案 hook；任一资源安装失败时 Host 回滚该组已安装资源，避免只启用一半的屏蔽行为。

- `P_TUTORIAL_PAUSE_GATE_SKIP`：`kCallPatch`，patch 站点由 `game/access/patches.cpp` 登记。
- `hook:TutorialDescription`：`kEntryHook`，handler 为 `tutorial_description_suppress`。
- `plugin_install` 校验 `PluginContext`、SDK 与 Game Host；`plugin_on_enabled` / `plugin_on_disabled` 只更新状态；`plugin_deactivate` 清理状态与 SDK 指针；`plugin_fill_status` 输出启用状态（`plugin.cpp:58-87`）。
- feature 默认启用；关闭后 Host 生命周期撤销 entry hook 并恢复暂停门闩原指令。

## 符号解析

entry hook 符号必须同时登记在以下两层：

1. `game/symbols/game_symbols.h`：`F_TUTORIAL_DESCRIPTION_VMA = 0x16cff4`。
2. `game/symbols/symbol_registry.h`：`SYM(F_TUTORIAL_DESCRIPTION_VMA, TutorialDescription)`。
3. `game/access/symbols.cpp` 的 `kHookEntrySymbols`：运行时 `resolve_hook_entry()` 的可 hook 入口白名单。

前三层缺一不可：只增加 VMA 常量和 symbol registry 不会自动让 entry hook 可解析。此前日志中的 `reason=not registered in game/access/symbols kHookEntrySymbols` 即由遗漏第三层导致；该层已在提交 `5bf3bc2d` 补齐。暂停门闩是 `kCallPatch`，其地址与原指令由 `game/access/patches.cpp` 的 patch-site 表解析。

monster 与 overhaul arm64 包静态核对得到相同的 `TutorialDescription`、`UIPlayPorting_Draw` 地址及暂停 store 指令；静态一致性不代替设备运行验收。

## 职责边界

- 本插件控制的是**原生教程文案绘制入口**和**state 6 暂停门闩写入**；不控制 Java/Xposed hooks，也不提供 native-to-Kotlin feature 配置通路。
- `P_HUB_LOGIN_SKIP`（主菜单 Com2uS Hub 登录询问框）是 Host 常驻 patch，不属于本插件。
- 更新通知、Hive 内购选择等 Android 层弹窗由 Kotlin `PopupBlockers` 管理，其中「更新通知」与「Hive 内购选择」
  以及 `Connecting..` 进度框都跟随同一 `popup_blocker.enabled` 开关；同意页拦截/自动关闭常驻，不随开关关闭。
- 独立教程图形、高亮或焦点框若不依赖 `TutorialDescription` 返回值，可能仍显示；不宣称屏蔽所有新手 UI。
- 一般 `UIPopupMsg`、剧情/NPC 对话及其他系统弹窗不受影响。

## 文件地图

| 文件 | 职责 |
|---|---|
| `plugins/popup_blocker/plugin.cpp` | descriptor、feature、两个资源 claim、短路 handler、lifecycle |
| `plugins/popup_blocker/plugin.h` | descriptor 对外声明 |
| `plugins/popup_blocker/CMakeLists.txt` | `qol_plugin_popup_blocker` 静态目标 |
| `game/symbols/game_symbols.h`、`symbol_registry.h` | VMA 与统一符号登记 |
| `game/access/symbols.cpp` | entry hook 按名解析白名单 |
| `game/access/patches.cpp` | `P_TUTORIAL_PAUSE_GATE_SKIP` 站点及原字登记 |
| `host/assembly.cpp` | built-in 插件注册 |
| `tests/test_popup_blocker_plugin.cpp` | descriptor、feature 和短路 handler 断言 |
| `tests/test_patch_layer.cpp` | 暂停门闩站点安装/恢复断言 |
| `tests/test_host_adapters.cpp` | `TutorialDescription` entry hook 符号解析回归 |

## 验证状态

- 2026-10-27 开关两态（C18L v588 真机，集成包 sha256 `83859ae3…`）：关闭 `popup_blocker.enabled` 重启后 logcat
  出现 `popup_blocker runtime effect: java popup blocking enabled=false`、
  `popup blocker disabled: allowing ActivityIndicatorOpen` 与
  `popup blocker disabled: allowing ActiveUser.showNoticePopup`；开启重启后出现
  `enabled=true`、`blocked ActivityIndicatorOpen (Connecting.. progress dialog)` 与
  `blocked ActiveUser.showNoticePopup (update notice dialog)`。
  更新通知的实际弹窗画面因本次服务端 `notice_show` 未触发而待目视补充。

- `host_adapter_tests`：通过（1/1）；覆盖 `F_TUTORIAL_DESCRIPTION_VMA` 可由 `resolve_hook_entry()` 解析。
- 插件测试 `popup_blocker_plugin_tests`、站点测试 `patch_layer_tests`、形状测试 `core_patches_shape_tests`：先前构建验证均通过（3/3）。
- `scripts/maintenance/check_symbols.py`：符号地址比对 446 项一致。
- `scripts/build-debug.sh`：成功；最近产物 `output/inotia4_qol_lsposed_debug_2610230311_60809512cb3f.apk`（符号注册修复后）。
- **真机行为尚未验收**：需验证 row 176 及其他 `TutorialDescription` 文案不绘制、state 6 门闩没有被置 1 且游戏逻辑未暂停、feature 关闭恢复原版行为。当前结论为 `NOT_ACCEPTED`，构建和 host 测试不能替代真机证据。
