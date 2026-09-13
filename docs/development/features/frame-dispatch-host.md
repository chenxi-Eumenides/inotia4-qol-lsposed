# 帧派发宿主（Frame Dispatch Host）hook 化可行性调查与设计

> 状态：**已实现（2026-09-13）** ｜ 日期：2026-09-13 ｜ 范围：对 native `FrameTaskManager` 是否可用 hook 重构、在游戏帧周期何处挂锚点、如何统一派发逐帧回调，给出可行性结论与设计。
> 本文不改变现有行为；实现须另立变更并按 `docs/guides/build-and-deploy.md` 真机验证。
> 相关：`docs/development/architecture.md` §1.4 / §2.1 / §2.2 / §2.2.1；`docs/development/features/auto-sell.md` §4.5；`docs/reference/game/`（帧周期逆向）。

## 0. 实现现状（2026-09-13）

**交付文件**：

- `core/native/frame_task.{h,cpp}`：统一帧任务管理器（多点位 + 句柄 API）。
- `core/native/frame_host.{h,cpp}`：锚点安装器（渲染开始前）。

**最终锚点**：`GAMESTATE_DrawPlay+0x20` 的 `bl MAP_DrawBase`（原指令字 `0x9401d18a`；常量 `F_MAP_DRAWBASE_VMA=0x111d14` / `F_GAMESTATE_DRAWPLAY_DRAWBASE_CALL_OFF=0x20`）。DrawPlay 开头 `byte==1` 分支经 `GRP_AddColorTone` 后 `b 0x9d6ec` 与主线汇聚于此，故每次 DrawPlay 必执行；`MAP_DrawBase` 自身重新加载指针、不吃入参（`void()`）。wrapper 先 `frame_task_dispatch(kFramePointRenderPre)`，再 `fn_map_drawbase()`。

**API**（与 `frame_task.h` 一致）：

- `frame_task_add(FramePointId point, FrameTaskFn fn, void* ctx, int interval, int count)`：返回句柄 `FrameTaskId`（0=失败）；`interval` 0=每帧、>0=每 interval 帧；`count` 0=一直、>0=最多 count 次；首个派发周期必触发。
- `frame_task_remove(FrameTaskId)`：幂等；未知 id 返回 false。
- `frame_task_query(FrameTaskId, FrameTaskStatus*)`：`FrameTaskStatus{bool active; int remaining_runs（无限=-1）; int frames_to_next}`。
- `frame_task_dispatch(FramePointId)`：锚点 wrapper 在游戏主线程调用；同一 (point, frame) 只派发一次；`thread_local` 重入门禁。
- 回调 `using FrameTaskFn = bool (*)(int64_t frame, void* ctx);`，返回 false 自动注销。
- 多点位：`enum : FramePointId { kFramePointRenderPre = 0, kFramePointCount };`，每点位独立按帧去重。

**已迁移消费者**：nav（`nav_task_tick`）、walk（`walk_task_tick`）、自动出售扫描（`autosell_tick`）。

**已删除**：`game_motion.{h,cpp}`（旧 FrameTaskManager）、`frame_tick.{h,cpp}`、`feature/autosell/autosell_host.{h,cpp}`。

**真机验证结论**：锚点安装成功；world 态 `move_to` / `walk_dir` / `stop_move` 逐帧驱动生效；无崩溃。

**锚点证伪**：§3 中 `GAMESTATE_Draw+0x3c`（`bl GAMESTATE_DrawPlay`）候选经真机探针证伪——world 态 `GAMESTATE_Draw` 的 DrawPlay 分支不执行（DrawPlay 由函数指针路径调用），故 call_patch 必须改挂 DrawPlay 内部。

## 1. 背景与问题

现有逐帧任务机制 `FrameTaskManager`（`module/app/src/main/cpp/core/native/game_motion.{h,cpp}`）：

- **后台线程轮询**：`task_thread_fn` 在独立 `std::thread` 每 1ms 轮询 `data_frame_count()`，帧号变化即执行回调（`game_motion.cpp:75-98`）。回调与游戏主循环**并发**读写游戏内存。
- **单任务语义**：`frame_task_register` 注册即 `g_tasks.clear()` 再插入（`game_motion.cpp:45`），并强制 `game_in_world() && *g_gamestate==0`（`:41-44`）。
- **消费者仅两处**：寻路 `nav_task_tick` 与方向键 `walk_task_tick`（`api/native/game_world_operations.inc:65,93`）；`stop_all_tasks()` 用于移动类端点的 UI/教学阻塞分支与 `walk_stop`（`:21,26,72,77,105`）。
- **已确认不可复用**：`auto-sell.md` §4.5 记录 FrameTaskManager「回调运行在后台线程、且注册会 `clear()` 顶掉移动/寻路任务」，故自动出售另走 draw 锚点。

同时项目已有两套相邻能力：

- **主线程逐帧派发** `core/native/frame_tick.{h,cpp}`：`frame_tick_register/unregister` + `frame_tick_dispatch()`，按 `data_frame_count()` 去重，回调在调用 dispatch 的线程（游戏主线程）执行。
- **锚点安装**：`feature/autosell/autosell_host.cpp` 用 `core/native/call_patch.h` 把 `GAMESTATE_DrawPlay` 内 `bl GRPX_Start` 改指 wrapper，wrapper 复刻原调用后执行 `frame_tick_dispatch()`。

因此本调查要回答：能否把 `FrameTaskManager` 换成「帧周期 hook 锚点 + 主线程回调 + 传入 frame 数」，以及在帧周期的哪个/哪些位置挂。

### 1.1 需求来源：调用方跨线程（关键）

`FrameTaskManager` 的**调用方不在游戏主线程**，这是本设计的核心驱动，不是「是否需要替换」的偏好问题：

- HTTP API 由 AndServer 处理，`BasicServer$1.run()` 对每个 accept 连接起线程执行路由（见 `ApiServer.kt:146-168` 反编译注释）；端点经 `NativeBridge` 调 JNI `op_move/op_walk/op_walk_stop`（`gamebridge_domain_operations.cpp:67-77`）→ 最终 `frame_task_register`（`game_world_operations.inc:65,93`）。**即注册动作发生在 AndServer 工作线程。**
- 启动/nativeInit 在 `HandlerThread("bridge-init")`（`HookMain.kt:92-93`）；LSPosed 原生 hook 回调运行在触发该 hook 的游戏线程；多处 JNI 用 `AttachCurrentThread`（`game_ui_settings_config.inc:7,35`、`autosell_store.cpp:38`、`extension_bag_runtime.inc:255`）证明 native 会从非主线程回 JVM。
- 现有 `FrameTaskManager` 用**自己的后台线程**跑回调，并未解决「回到游戏主线程」，反而在游戏主循环之外并发读写游戏内存。

**需求定义**：提供一个**同线程帧任务管理器**——任意线程（HTTP 工作线程、`bridge-init`、hook 回调线程、UI 线程）可安全提交回调，回调统一在**游戏主线程的帧周期**执行并收到当前 frame 数；注册/注销对任意线程线程安全。锚点（call_patch / Native Hook）只是「如何到达主线程帧周期」的实现手段，不是需求本身。

## 2. 事实：游戏帧周期逆向结论（overhaul libgame.so）

> 本章只记录当前静态逆向结论；动态行为（线程、调用次数）未真机确证的部分在 §8 标为开放项。

### 2.1 帧号来源

- `data_frame_count()`（`game_system.cpp:20-26`）读 GOT 槽 `G_FRAME_COUNT_VMA=0x2f5648` 解引用后的 `u64`。
- 计数递增点在逻辑帧函数 `MainProcess@0xd4984` 内 `0xd4a20`：`adrp 0x2f5000; ldr x0,[x0,#0x648]`（=0x2f5648）→ `add x1,x1,#1` → `str x1,[x0]`。即**一帧内回调读到的是本帧递增前的值**（递增在 `MainProcess` 尾部）。

### 2.2 逻辑帧链

```
内核定时器回调 → MainProcess@0xd4984
    UI_PopupProcess
    STATE_NextStartProcess
    [0x2f3000+0xcd8] 回调 / UIPopupMsg_Process
    POPUPSTATE_Exist → POPUPSTATE_Process
    [0x2f4000+0xa90] 回调
    NOTIFIER_Process(2)
    SOUNDSYSTEM_Process
    ★ 帧计数 ++（0xd4a20）
    b CS_knlSetTimer（重排约 10ms 定时器）
```

`MainProcess` 无直接 `bl` 调用者，由 CS 内核定时器以函数指针回调；入口参数 `x0/x1` 为定时器参数，返回 void（尾部 `b CS_knlSetTimer`）。

状态处理链（`STATE_ProcessGame@0x151540`）：

```
STATE_ProcessGame
    GAMESTATE_PressKey
    GAMESTATE_Process@0x151264   # 逻辑分发，经 [0x2f3000+0x938]
    GAMESTATE_Draw@0x1512b8      # 渲染分发，经 [0x2f4000+0x930]
```

### 2.3 渲染帧链（world 态）

```
GAMESTATE_Draw@0x1512b8
    GAMESTATE_DrawPlay@0x9d6cc
        MAP_DrawBase / EFFECTSYSTEM_DrawGround / MAP_DrawLayer / …
        UIPlay_Draw
        GRPX_Start@0x8f2fc          # DrawPlay 内 +0x74，原指令字 0x97ffc6ef
        UIPlayPorting_Draw
        GRPX_End@0x8f314
```

`GAMESTATE_Draw` 对非 world 态走其它 `GAMESTATE_DrawEvent/MapChange` 分支。

### 2.4 可解析的锚点符号

`MainProcess`、`GAMESTATE_Process`、`GAMESTATE_Draw`、`GAMESTATE_DrawPlay`、`GRPX_Start`、`GRPX_End`、`GAMESTATE_ProcessPlay`、`GAMESTATE_DrawEvent/MapChange` 均为 **`.dynsym` 导出 T 符号**，可经 `symbol_resolver` 按符号名解析（跨游戏版本稳健），无需写裸 VMA。

> `symbol_registry.h` 当前仅登记 `F_GRPX_START_VMA`/`F_GRPX_END_VMA`（:350-351）与 `F_GAMESTATE_DRAWPLAY_VMA`（:380）；`MainProcess`/`GAMESTATE_Process`/`GAMESTATE_Draw` 未登记，若采用须新增 `SYM` 条目并在 `game_symbols.h` 加常量。

### 2.5 既有锚点与派发设施

| 设施 | 位置 | 机制 | 现状 |
|---|---|---|---|
| FrameTaskManager | `game_motion.cpp` | 后台线程轮询 | 采用（待替代） |
| `frame_tick` | `core/native/frame_tick.cpp` | 主线程回调注册表 + 帧去重 | 自动出售使用 |
| draw 锚点 | `autosell_host.cpp:34-57` | `call_patch` BL 改指 `GAMESTATE_DrawPlay+0x74` | 真机验证可用（自动出售） |
| LSPosed Native Hook | `native_inventory_hook.{h,cpp}`、`game_ui_attr_range.cpp` | 函数入口 inline hook + backup | 扩展背包 / 属性范围使用 |

## 3. 候选锚点对比

| 锚点 | 可挂方式 | 所在链/线程（待确证） | 帧内位置 | 签名清晰度 | 评价 |
|---|---|---|---|---|---|
| `GAMESTATE_DrawPlay+0x74`（`bl GRPX_Start`） | `call_patch` BL（既有） | 渲染链 | draw 段 | 明确（复刻无参原调用） | **第一阶段首选**，零新机制，已验证 |
| `GAMESTATE_DrawPlay+0x20`（`bl MAP_DrawBase`） | `call_patch` BL | 渲染链 | 渲染首个调用 | 明确 void() | **最终采用**，每帧必执行（真机验证） |
| `GAMESTATE_Process` 入口 | LSPosed Native Hook | 逻辑分发（world 态每帧一次，待确证） | 计数器 ++ 之前 | 导出、签名清晰 | **需逻辑相位时首选 hook 目标** |
| `GAMESTATE_Draw` 入口 | LSPosed Native Hook | 渲染分发（全状态） | ++ 之前 | 导出 | 覆盖全状态 draw，可替代 DrawPlay 锚点。真机证伪：world 态 DrawPlay 分支不执行 |
| `GRPX_Start`/`GRPX_End` 入口 | LSPosed Native Hook | 渲染段 | draw 起/止 | 导出 | 覆盖面广但被多处调用，需去重 |
| `MainProcess` 入口 | LSPosed Native Hook | 逻辑帧根（全状态） | ++ 之前 | **不透明**（x0/x1 + 尾调用） | 覆盖最全但风险最高，**不推荐首选** |

## 4. 可行性裁决

**结论：需求成立（同线程帧任务管理器有其必要），实现以 `frame_tick` 注册表 + 锚点完成；是否引入 LSPosed Native Hook 取决于所需锚点是否被现有 call_patch draw 锚点覆盖。**

需求侧（成立）：

1. 调用方跨线程（§1.1），必须有「marshal 回调到游戏主线程」的能力；`FrameTaskManager` 的后台线程恰恰违背此点，并与主循环并发读写游戏内存。
2. 需要多回调（各功能独立注册）与线程安全的注册/注销；`FrameTaskManager` 的单任务 `clear()` 会互相顶替（`auto-sell.md` §4.5）。
3. 需要传递 frame 数，并区分「逻辑相位（可影响本帧行为，如移动）」与「渲染相位（UI/扫描）」。

实现侧（分层裁决）：

4. 「主线程逐帧 + 传 frame + 多回调」由 `frame_tick` 提供；缺的只是锚点与跨线程注册保证。
5. draw 锚点已由自动出售真机验证可用；world 态下 `frame_tick_dispatch()` 每帧都被调用，足以支撑现有消费者。
6. 依 `architecture.md §2.2.1`：机制优先级为 读写内存 → 调函数指针 → `PtrHook` → 指令 patch → **LSPosed Native Hook（最后手段）**。用 Native Hook 换锚点只增加 inline trampoline / ABI / 并发风险，不增加派发能力。
7. **锚点选择是独立决策**：仅当真机证明现有 draw 锚点覆盖不足（需 world 外/逻辑相位逐帧）时，才引入 Native Hook 挂 `GAMESTATE_Process` 入口，而非 `MainProcess`。

**已实现（2026-09-13）：**

- **阶段 1（无 Native Hook）：已完成。** 删除 FrameTaskManager；统一帧任务管理器（`frame_task` + `frame_host`，多点位 + 句柄 API）取代 `frame_tick`；`autosell_host` 的锚点逻辑上移为 core `frame_host`；nav/walk 迁入「移动槽」并移除 `stop_all_tasks()`。此阶段满足现有全部消费者。
- **阶段 2（条件触发）：未引入。** 阶段 1 的 draw 锚点（`GAMESTATE_DrawPlay+0x20`）经真机验证覆盖现有全部消费者，未引入 LSPosed Native Hook 挂 `GAMESTATE_Process`。
- **阶段 3（最后手段）：未评估/未引入。**

## 5. 设计：统一帧派发宿主

### 5.1 分层（两层，不新增第四套机制）

```
core/native/frame_tick.{h,cpp}    唯一主线程帧注册表
        ▲ 注册回调（phase, fn(frame,ctx)->bool continue, ctx）
        │
core/native/frame_host.{h,cpp}    锚点安装器（新，从 autosell_host 上移并泛化）
        │  调用 frame_tick_dispatch(phase)
        ├─ draw 锚点：call_patch GAMESTATE_DrawPlay+0x74（复用既有）
        └─ logic 锚点：可选，LSPosed Native Hook GAMESTATE_Process（阶段 2）
```

- `FrameTaskManager`（`game_motion.cpp`）**删除**；其头文件语义由 `frame_tick` 承接。
- `autosell_host.cpp` 不再自持锚点，改为注册 `frame_tick` 回调；锚点逻辑归 `frame_host`。

### 5.2 相位模型

不建议四相位（YAGNI）。定义两个相位：

| 相位 | 锚点 | 语义 | 主要消费者 |
|---|---|---|---|
| `FRAME_PHASE_LOGIC` | `GAMESTATE_Process`（或等价逻辑子相位） | 本帧逻辑处理，可影响本帧游戏行为 | nav/walk（CHAR_Move） |
| `FRAME_PHASE_DRAW` | `GAMESTATE_DrawPlay+0x74` | 本帧渲染段，UI/背包/拾取扫描 | autosell |

**帧内位置与去重陷阱（关键）**：帧计数 `++` 位于 `MainProcess` 尾部，故同一 `MainProcess` 内两个子相位读到的是**同一未递增帧号**；若两相位共用一份 `g_last_dispatched_frame`，第二个锚点会被去重吞掉。因此 `frame_tick` 必须**每相位独立去重状态**（或按 `(phase, frame)` 去重），且回调的 frame 由**参数传入**而非回调内二次读 `data_frame_count()`。

### 5.3 API（已实现）

```cpp
using FramePointId = int;
enum : FramePointId { kFramePointRenderPre = 0, kFramePointCount };

using FrameTaskId = uint64_t;  // 0=无效

// 返回 false 由派发器自动注销；首个派发周期必触发。
using FrameTaskFn = bool (*)(int64_t frame, void* ctx);

struct FrameTaskStatus {
    bool active = false;
    int remaining_runs = 0;  // 无限=-1
    int frames_to_next = 0;
};

FrameTaskId frame_task_add(FramePointId point, FrameTaskFn fn, void* ctx, int interval, int count);
bool frame_task_remove(FrameTaskId id);
bool frame_task_query(FrameTaskId id, FrameTaskStatus* out);
void frame_task_dispatch(FramePointId point);
```

注册/注销与派发共用同一把锁：`frame_tick_register/unregister` 可被**任意线程**调用（AndServer 工作线程、`bridge-init`、hook 回调线程、UI 线程），派发只在游戏主线程；持锁快照、锁外执行，回调内注册/注销在下一帧生效（快照语义）。跨线程「只跑一次」的提交用「注册 + 回调返回 false 自动注销」，不另设任务队列，避免双缓冲/内存序复杂度。

移动互斥单独收口，不塞进通用注册表：

```cpp
// 移动槽：nav/walk 共用，注册即替换并取消上一个；仅此槽可被 stop 清空。
bool frame_motion_slot_register(FrameTickFn fn, void* ctx);
void frame_motion_slot_clear();   // 对应 stop_all_tasks() 的移动语义
```

`stop_all_tasks()`（`data_op_walk_stop` 等）映射为 **`frame_motion_slot_clear()`**，绝不能清空全局注册表（否则连同 autosell 一起注销，正是 `auto-sell.md` §4.5 拒绝复用 FrameTaskManager 的根因）。

### 5.4 锚点安装（`frame_host`）

- draw 锚点：复用 `call_patch_install_bl`（fail-closed 原指令字校验 + 近址 thunk + `mprotect` + `clear_cache`）；锚点为 `GAMESTATE_DrawPlay+0x20` 的 `bl MAP_DrawBase`（原字 `0x9401d18a`，常量 `F_MAP_DRAWBASE_VMA` / `F_GAMESTATE_DRAWPLAY_DRAWBASE_CALL_OFF`）；wrapper 先 `frame_task_dispatch(kFramePointRenderPre)` 再 `fn_map_drawbase()`。
- logic 锚点（阶段 2，可选）：`native_hook_func()(GAMESTATE_Process, &logic_wrapper, &backup)`；`logic_wrapper` 先调 `backup()`（原逻辑），后调 `frame_tick_dispatch(kLogic)`——**回调在 ++ 之前，读到的是本帧起始帧号**（与既有自动出售一致，不额外 +1）。
- 安装条件：`bridge_ready() && native_hook_func()!=nullptr`；用 CAS/exchange 幂等；多 hook **全成或全退**（照 `native_inventory_hook.cpp:1156-1173` 的 rollback 模式，勿复制属性范围 `game_ui_attr_range.cpp` 失败不回滚的写法）。
- 卸载：锚点**永久安装**，运行期用回调内使能开关控制行为，不做运行期 unhook（目标可能正在执行，inline 卸载风险最高）。

## 6. 线程 / 重入 / ABI 约束

- **回调必须在游戏主线程**：不得 detached 线程；不得阻塞（join/HTTP/文件/DB）；不得每帧分配或打日志；不得调用会触发存档/背包/UI 状态机的 API（会与既有 hook 链递归）；不得持游戏函数需要的锁。
- **重入门禁**：帧号去重只防同帧嵌套，不防「定时器重排后帧号已变」的同步再入。派发器加 `thread_local dispatch_depth`，`>0` 直接返回。
- **`MainProcess` 特殊风险（若挂它）**：入口 ABI 不透明（x0/x1 及 x2–x7/向量寄存器使用未知），C++ wrapper 序言可能在转发 backup 前破坏寄存器；且原函数尾 `b CS_knlSetTimer` 使 backup 之后的 post-code 运行在「定时器已重排」之后甚至不执行。故**只做无参检 pre-callback，不写参数检查/转发**；这也是不首选 `MainProcess` 的强理由。
- **回调执行时长**：单次回调须远小于帧预算（名义 ~59ms，按微秒级约束）。

## 7. 迁移方案

| 步骤 | 内容 | 影响面 |
|---|---|---|
| 1 | 新建 `core/native/frame_task.{h,cpp}`：多点位 + `frame` 参数 + `bool continue` + 每点位去重 + 重入门禁；同步更新 host 测试 `tests/test_infra_data.cpp`（`should_dispatch` / `normalize_interval` / `is_due`）——已完成（2026-09-13） | core |
| 2 | 新建 `core/native/frame_host.{h,cpp}`，锚点逻辑从 `autosell_host` 上移并泛化；删除 `autosell_host.{h,cpp}`——已完成（2026-09-13） | core + feature/autosell |
| 3 | nav/walk 迁入移动互斥槽 `g_motion_task` + `motion_task_start/stop`；删除 `stop_all_tasks()`——已完成（2026-09-13） | api/native |
| 4 | 删除 `game_motion.{h,cpp}`（旧 FrameTaskManager）与 `frame_tick.{h,cpp}`——已完成（2026-09-13） | core |
| 5 | 文档同步：`architecture.md` §1.4 / §2.1 / §2.2、`auto-sell.md` §3.5 / §9 / §11.2、本文件状态更新——已完成（2026-09-13） | docs |
| 6 | （阶段 2 条件触发）Native Hook `GAMESTATE_Process` 逻辑锚点——未触发（阶段 2 条件未满足） | core + registry |

迁移须保持 nav/walk 行为不变：逐帧 `CHAR_Move`、「撞墙/切图/路径空」终止、60 帧 walk、`walk_stop` 打断。

## 8. 实施前必须真机验证的开放项

以下影响设计成立与否，静态无法确证：

1. `MainProcess` 实际执行线程 tid 是否为主/渲染线程；若为独立定时器线程，则「主线程派发」前提对 `MainProcess` 不成立。
2. 每个渲染帧内 `MainProcess` 调用次数，及与 `data_frame_count()` 1:1 关系在 world/menu/story/map-change 全状态是否成立。
3. `STATE_ProcessGame` 是否在 `MainProcess` 内、位于 `++` 之前、且每帧每态各一次（决定逻辑锚点归属）。
4. `MainProcess` 完整 ABI 与 prologue 可 hook 性（指令长度、PC 相对指令）；`hook_func` 返回 0 且 backup 非空。
5. backup 调用后 wrapper 的 post-code 是否执行（尾调用返回路径）。
6. `CS_knlSetTimer` 是否同步再入 `MainProcess`（决定是否必须 depth 门禁）。
7. `GAMESTATE_Process`/`GAMESTATE_Draw`/`MainProcess` 在目标版本 `.dynsym` 导出且能被 `symbol_resolver` 命中。
8. 安装 inline hook 时定时器是否正在运行（安装期崩溃风险）。
9. draw 锚点是否覆盖移动任务所需全部场景；否则逻辑锚点成为硬需求。
10. 回调读 `data_frame_count()` 与传入 frame 在所有相位/状态下是否一致。

> 说明（2026-09-13）：阶段 1 采用 call_patch 指令 patch（非 Native Hook），开放项 1–8 涉及 Native Hook/`MainProcess`，本次未触发；第 9 项已由真机验证关闭（`GAMESTATE_DrawPlay+0x20` 每帧必执行、覆盖移动任务）；第 10 项不适用（frame 由锚点参数直传，回调内不再二次读 `data_frame_count()`）。

## 9. 验证计划

- **Host 测试**：`frame_tick` 相位独立去重、`bool continue` 自动注销、快照语义（回调内注销下一帧生效）、重入门禁纯函数；扩展 `tests/test_infra_data.cpp`。
- **Android debug 构建**：`scripts/build-debug.sh`。
- **真机回归**：安装 + force-stop + monkey 重启，按 `build-and-deploy.md` 步骤②③③a 轮询 `/api/health` 就绪；VM-B：移动/寻路逐帧行为、walk_stop 打断、自动出售扫描/处置、无崩溃与卡死。
- 交付定义：**静态审查通过 ≠ 可交付**；触及行为面须附真机日志证据，否则报 `NOT_ACCEPTED`。
> 现状（2026-09-13）：阶段 1 的 host 测试与 debug 构建已通过；真机回归（锚点安装 + move_to/walk_dir/stop_move）见 §0 与 `architecture.md` §2.1。

## 10. 决策记录（思考，非事实）

- **需求成立**：调用方跨线程（§1.1）使「同线程帧任务管理器」成为硬需求；`FrameTaskManager` 的后台线程模型恰好违背该需求，故必须重构而非保留。
- **为什么不直接上 Native Hook**：能力需求由 `frame_tick`（唯一注册表，含跨线程注册 + 主线程派发）满足，Native Hook 只解决「锚点够不够得着」，不增加派发能力；`§2.2.1` 要求「仅当前四种机制无法覆盖时」才用 Native Hook。故阶段 1 锚点复用已验证的 call_patch draw，不引入 Native Hook。
- **为什么逻辑锚点选 `GAMESTATE_Process` 而非 `MainProcess`**：`GAMESTATE_Process` 导出、签名清晰、有确定返回点，backup 后可安全做 post；`MainProcess` 入口 ABI 不透明且尾调用 `b CS_knlSetTimer`，post 与参数转发均不可靠。
- **为什么保留移动槽**：FrameTaskManager 的「单任务语义」实质是移动所有权互斥（nav/walk 不能同时驱动 `CHAR_Move`）；把它上提为显式槽位，既保留原语义，又让通用注册表支持多回调而不互相顶替。
- **为什么必须每相位独立去重**：帧计数 `++` 在逻辑帧尾部，多锚点共用一份去重状态会导致漏派发或每帧双派发。
