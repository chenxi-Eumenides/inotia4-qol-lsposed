# 进入存档回调（save-enter-callback）

> 状态：**已实现（2026-09-13）** ｜ 日期：2026-09-13 ｜ 范围：进入存档（读档/新档）加载完成后触发一次回调；切图返回不触发。
> 相关：`docs/development/architecture.md` §2 / §2.1；`core/native/save_enter.{h,cpp}`、`core/native/frame_task.{h,cpp}`、`core/native/frame_host.{h,cpp}`。

## 1. 需求

进入存档（读档 / 新档）加载完成、进入 world 后，触发一次回调。用于模块需要在「存档已就绪」时做一次性初始化/同步（例如按当前存档槽加载 sidecar、刷新状态）。

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
- 触发时打印一条 INFO 日志（tag `Inotia4SaveEnter`，含回调数量），不每帧打印。

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

- **Host**：`tests/test_infra_data.cpp` 的 `save_enter_detail::should_fire` 四组合（仅「已发起 且 在 world」为真）。
- **Android debug 构建**：`scripts/build-debug.sh`。
- **真机（待办）**：读档进 world 触发一次且日志出现 `Inotia4SaveEnter save enter fired callbacks=N`；新档同；切图返回不触发；无崩溃。
