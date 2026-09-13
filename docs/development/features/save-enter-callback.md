# 存档生命周期回调（进入/退出）

> 状态：**进入回调已实现（2026-09-13）；退出回调已实现但未真机验证（NOT_ACCEPTED）** ｜ 日期：2026-09-13 ｜ 范围：进入存档（读档/新档）加载完成后触发一次；退出存档（world → 主菜单）触发一次。
> 相关：`docs/development/architecture.md` §2 / §2.1；`core/native/save_enter.{h,cpp}`、`core/native/save_exit.{h,cpp}`、`core/native/frame_task.{h,cpp}`、`core/native/frame_host.{h,cpp}`。

## 1. 需求

- **进入存档**：读档 / 新档加载完成、进入 world 后，触发一次回调。用于模块需要在「存档已就绪」时做一次性初始化/同步（例如按当前存档槽加载 sidecar、刷新状态）。
- **退出存档**：从 world 返回主菜单时触发一次回调（不含退档到选角、不含杀进程）。用于模块在存档关闭前做一次性收尾/同步。

## 2. 语义

- **触发时机**：读档 / 新档完成并进入 world（`GAMESTATE_nState == 0`，即 `game_in_world()`）后的首个渲染帧。
- **触发次数**：每次发起进入存档只触发一次；回调注册列表可被多个消费者使用。
- **不触发场景**：切图返回不经过两个发起点，故不标记 pending、不触发；回主菜单再正常进 world 也不触发（除非再次读档/新档）。
- **线程**：回调在游戏主线程执行；注册/注销/mark_pending 任意线程可调。

## 3. 发起点（call_patch 指令 patch，非 Native Hook）

| 场景 | 函数 | 调用点 | 原指令字 |
|---|---|---|---|
| 读档 | `SaveSlot_SlotButtonExe@0x14cd08` | +0x88 `bl GAME_StartResumeGame` | `0x97fecd56` |
| 新档 | `SelectCharacter_ButtonStartExe@0x14dee0` | +0x10 `bl SelectCharacter_StartGame` | `0x97ffffea` |

- 读档 wrapper：`save_enter_mark_pending()` → `fn_game_start_resume_game(slot)`。
- 新档 wrapper：`save_enter_mark_pending()` → `fn_select_character_start_game()`。
- 模块 API 路径同样标记：`data_op_enter_slot` 在 `fn_game_start_resume_game(slot)` 前、`data_op_create_slot` 在 `fn_select_character_start_game()` 前各调 `save_enter_mark_pending()`（这两条路径不经过上述按钮调用点）。
- 两个发起点**全成或全退**：第二个安装失败时用 `call_patch_revert_bl` 回滚第一个；未就绪/失败 fail-closed，不改写字节。

## 4. 检测策略

- 检测任务 `save_enter_tick` 由统一帧任务管理器注册在 `kFramePointRenderPre`（渲染开始前锚点，仅 world 态 DrawPlay 执行），因此天然只在 world 态运行。
- 判定纯函数 `save_enter_detail::should_fire(pending, in_world) = pending && in_world`（供 host 测试）。
- 消费：`pending` 以 `std::atomic<bool>` 保存，`should_fire` 为真时 `exchange(false)` 消费一次；回调列表持锁快照、**锁外**逐个调用（主线程）。
- 触发时打印一条 INFO 日志（domain=save，单一 tag `Inotia4Qol`，含回调数量）；`g_pending.exchange` 保证每次 pending 只触发一次，非逐帧，故保持 INFO，不每帧打印。

## 5. API（`core/native/save_enter.h`）

```cpp
using SaveEnterFn = void (*)(void* ctx);

bool save_enter_register(SaveEnterFn fn, void* ctx);   // 同一 fn+ctx 幂等；fn 空返回 false
void save_enter_unregister(SaveEnterFn fn, void* ctx); // 幂等
void save_enter_mark_pending();                        // 标记进入存档已发起（任意线程）
bool save_enter_init();                                // 注册检测任务（幂等）
bool save_enter_host_install_if_ready();               // 安装两个发起点（幂等，全成或全退）
```

## 6. 验证

- **Host**：`tests/test_infra_data.cpp` 的 `save_enter_detail::should_fire` 四组合（仅「已发起 且 在 world」为真）；`save_exit.h` 主机端可包含（编译性 `static_assert`）。
- **Android debug 构建**：`scripts/build-debug.sh`。
- **真机（进入，已办）**：读档进 world 触发一次且日志出现 `Inotia4Qol ... domain=save ... save enter fired callbacks=N`；新档同；切图返回不触发；无崩溃。
- **真机（退出，待办）**：见 §7。

## 7. 退出存档回调

### 7.1 语义

- **触发时机**：从 world 返回主菜单时触发一次。
- **覆盖范围**：游戏自身状态机统一路径，故模块 `go_main_menu`（`api/native/game_ui_operations.inc:6` 调 `fn_gamestate_set_state(4)`）与游戏原生回主菜单都覆盖。
- **不含**：退档到选角（走 `GAME_ExitSaveSlotSelectCharacter`，不同调用点）、杀进程。

### 7.2 发起点（call_patch 指令 patch，非 Native Hook）

| 场景 | 函数 | 调用点 | 原指令字 |
|---|---|---|---|
| world → 主菜单 | `GAMESTATE_SetState@0x151590` | state==4 分支落点 +0xb0 `bl GAME_Exit` | `0x97febb3d` |

- `GAME_Exit@0x100334` 在 `.text` 内仅此一个调用点（全量反汇编 grep 确认），故该锚点仅覆盖 world → 主菜单。
- wrapper：`save_exit_fire(); if (fn_game_exit) fn_game_exit();`——**先派发回调、再复刻原 `GAME_Exit`**（回调时 world 数据仍有效且保证执行）。
- 未就绪/失败 fail-closed，不改写字节；`std::atomic` exchange 幂等。

### 7.3 API（`core/native/save_exit.h`）

```cpp
using SaveExitFn = void (*)(void* ctx);

bool save_exit_register(SaveExitFn fn, void* ctx);     // 同一 fn+ctx 幂等；fn 空返回 false
void save_exit_unregister(SaveExitFn fn, void* ctx);   // 幂等
bool save_exit_host_install_if_ready();                // 安装发起点（幂等，fail-closed）
```

- 触发时打印一条 INFO 日志（domain=save，单一 tag `Inotia4Qol`，含回调数量），不重复打印。

### 7.4 验证状态

- **Host**：`save_exit.h` 主机端可包含（编译性断言），无纯逻辑可测。
- **Android debug 构建**：通过。
- **真机（待办，NOT_ACCEPTED）**：回主菜单触发一次且日志 `Inotia4Qol ... domain=save ... save exit fired callbacks=N`；进入存档 / 切图不触发；无崩溃。设备空闲后补验。
