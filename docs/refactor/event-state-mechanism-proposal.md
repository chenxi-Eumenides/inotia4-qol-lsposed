# 事件状态（Event State）机制方案

状态：已实现（见 §13 实现记录）。主路径已通过 host 测试、Debug 构建与真机进档验证；部分子场景仍为 NOT_ACCEPTED（见 `verification.md` VM-ES-01）。

## 1. 背景

### 1.1 现状

- 事件总线：`infra/events.{h,cpp}`。`subscribe` / `unsubscribe` / `publish`，publish 主线程调用，锁内复制订阅快照、锁外按订阅序同步派发。无 owner 维度；生产代码 0 处调用 `clear_all()`，订阅清理靠各插件 deactivate 自行 `unsubscribe`。
- 插件可见面：`contracts/host_sdk.h` 的 `QolEventsApi{subscribe, unsubscribe}`（`host/host_sdk.cpp:294-306`）。插件没有 publish 权限。
- 事件只有三个：`kEventSaveEnter=2`、`kEventSaveExit=3`、`kEventConfigChanged=1`（`contracts/events.h`）。
- 存档事件由宿主 `patch/save_enter.cpp` 的 `save_enter_tick`（RenderPre）发布，条件是三段与：
  1. `g_pending`：由 `save_load_wrapper` 在用户发起读档、原版加载函数返回非 0 后置位（`patch/save_enter.cpp:74`）；
  2. `game_in_world()`（`*g_state == 5`）；
  3. `save_enter_ready()`：经 `host/frame_source.cpp:346` 向硬编码的 `"core.save"` 派发 `kMaintenance` 探针，读取 `save_transition.cpp:477-480` 的 `ready_published`。
- 宿主有 4 处硬编码 `"core.save"` 的时机接缝（`host/frame_source.cpp:325/333/346/364`）；另有 2 处是私有端口 ACL（`host/plugin_registry.cpp:467/475`，与时机无关）。

### 1.2 结构性缺陷

1. **事件源绑在“发起点”**：站点 patch 装配失败时 wrapper 不存在，用户读档走原版路径，`g_pending` 永不置位 → 事件永不发布。
2. **catch-up 不置 pending**：core.save 观测到 world 已就绪但 session 未就绪时会补发起转换（`plugins/core.save/plugin.cpp:344-353`），该路径不经过发起点，也不置 pending。
3. **消费方各自轮询**：core.quest 自建 `world_transition_tick` 轮询 `world.facts.state()==5`（`plugins/core.quest/feature.cpp:59-76`）；extension_bag 另有一套 30 帧世界状态轮询（`plugins/extension_bag/core/lifecycle.cpp:507-539`）。
4. **迟到订阅者丢失状态**：事件是边沿，插件在已进档后才启用时收不到任何通知；延迟资源恢复（`host/plugin_registry.cpp:2883-2980`）和 core.quest 都只能靠额外状态变量兜底。

真实故障链（2026-11-07 日志）：装配末段 save 发起点 patch 安装失败 → 读档走原版 → 无 pending → 无 `kEventSaveEnter` → catch-up 产生 `transition ready` 但不发事件 → `custom_recipe` 的 `own:RECIPEBASE_MIXTUREBASE` 延迟恢复整局未执行 → 合成界面停留原版。

### 1.3 目标与非目标

目标：

- 事件从“边沿通知”升级为“带状态的事实”：所有者声明事件并更新状态，模块负责注册、状态保存、边沿计算与派发。
- 权威状态源唯一；迟到订阅者可获得当前状态；所有者停用对消费者可见。
- 宿主不再在时机路径上硬编码具体插件 id。
- 删除 `kEventSaveEnter`，原 4 个订阅者全部迁移到事件状态；不保留别名或旧代码。

非目标：

- 不新增第二套帧调度器（继续用 `infra::scheduler`）。
- 不给插件通用 publish 权限。
- 不替代 capability、snapshot、save_route native_call。

## 2. 模型

### 2.1 术语

| 术语 | 含义 |
|---|---|
| event id | 编译期常量，定义在 `contracts/`；一个 event id 只有一个 owner |
| owner | 声明该事件的插件；由 `QolOwnerToken` 派生，不由字符串信任 |
| availability | `kUnavailable` / `kAvailable`；表示 owner 是否存在且可用 |
| value | 领域事实（本方案为 ready 布尔 + slot） |
| revision | 每次被接受的状态更新单调递增 |
| change kind | `kSnapshot` / `kBecameReady` / `kCeasedReady` / `kUnavailable` |

### 2.2 边沿规则

| 迁移 | change kind |
|---|---|
| 尚未发布 | 不派发（订阅者收不到任何变更） |
| 首次发布 | `kSnapshot` |
| 值不变（availability 与 value 都相同） | 不产生变化 |
| `available,false → available,true` | `kBecameReady` |
| `available,true → available,false` | `kCeasedReady` |
| 任意可用 → unavailable | `kUnavailable` |
| 订阅时已有发布状态 | `kSnapshot`（入队，不在 subscribe 内联） |

发布者未发布时，订阅者不收到任何状态变更；允许先订阅、后发布。迟到订阅者仅在事件已有发布状态时收到一次 `kSnapshot`。

### 2.3 单 owner 约束

同一 event id 只允许一个 owner。第二个声明返回 `kQolErrConflict`。owner 停用后其声明被移除，之后的声明可以接管（用于宿主 fallback 交接，见 §5.4）。

## 3. 契约设计

### 3.1 `contracts/events.h`

新增（不修改现有 3 个事件 id）：

```cpp
// 存档会话就绪状态：owner = 存档域（当前为 core.save）。
constexpr uint32_t kEventStateSaveReady = 4;
constexpr uint16_t kEventStateSaveReadySchema = 1;

enum class EventStateAvailability : uint8_t { kUnavailable = 0, kAvailable = 1 };
enum class EventStateChangeKind : uint8_t {
    kSnapshot = 0, kBecameReady = 1, kCeasedReady = 2, kUnavailable = 3,
};

struct EventStateDescriptor {
    uint32_t size;              // = sizeof(EventStateDescriptor)
    uint32_t event_id;
    uint16_t schema_version;
    uint16_t reserved;
};

struct EventStateValue {
    uint32_t size;
    uint32_t event_id;
    uint16_t schema_version;
    uint8_t availability;       // EventStateAvailability
    uint8_t value;              // 0/1，语义由 schema_version 定义
    int32_t slot;               // 仅 kEventStateSaveReady：当前存档槽，-1 = 无效
    uint64_t revision;          // 由宿主写入
};

struct EventStateChange {
    uint32_t size;
    uint32_t event_id;
    uint8_t change_kind;        // EventStateChangeKind
    uint8_t availability;
    uint8_t value;
    uint8_t reserved;
    int32_t slot;
    uint64_t revision;
};
```

约束：POD、固定宽度、以 `size` 开头、无指针、无 STL；`event_id` 不接受运行时自由字符串。

### 3.2 `contracts/host_sdk.h`

不要在 `QolEventsApi` 里追加字段（它是 `QolHostSdk` 的内嵌成员，追加会移动后续成员偏移）。在 `QolHostSdk` **尾部**追加独立指针：

```cpp
// v1.32 tail: 事件状态注册/更新/查询/订阅；null 表示不可用。
const QolEventStateApi* event_state;
```

`kHostSdkMinor` 由 31 提升到 32，并追加注释说明。新增函数表（自带 `struct_size`）：

```cpp
struct QolEventStateApi {
    uint32_t struct_size;
    QolResult (*declare)(QolOwnerToken owner, const EventStateDescriptor* descriptor);
    QolResult (*set_state)(QolOwnerToken owner, const EventStateValue* value);
    QolResult (*read)(uint32_t event_id, EventStateValue* out_value);
    QolResult (*subscribe)(uint32_t event_id, QolEventStateHandler handler, void* ctx,
                           uint64_t* out_id);
    QolResult (*unsubscribe)(uint64_t id);
};
```

配套可用性 helper，沿用 `public_export_if_available` 的写法（`contracts/host_sdk.h:524-530`）：

```cpp
inline const QolEventStateApi* event_state_if_available(const QolHostSdk* sdk);
// 校验 abi.major、abi.minor >= 32、abi.size >= offsetof(QolHostSdk, event_state)+sizeof(...)、
// api->struct_size、以及每个用到的函数指针非空。
```

### 3.3 错误语义

只使用 `contracts/result.h` 现有错误码（没有 `PermissionDenied`）：

| 情况 | 错误码 |
|---|---|
| owner token 无效 / 非当前 generation / owner 与声明者不符 | `kQolErrBlocked` |
| 未知 event id / 声明不存在 | `kQolErrNotFound` |
| 重复声明（已有其他 owner） | `kQolErrConflict` |
| schema_version 或结构 size 不支持 | `kQolErrUnsupported` |
| 非主线程调用 `set_state` | `kQolErrBlocked` |
| 声明槽 / 订阅槽 / 待派发队列满 | `kQolErrBusy`（沿用 `sdk_events_subscribe` 的 capacity 先例） |
| 参数为空或 event id 为 0 | `kQolErrInvalidArgument` |

## 4. 宿主实现

### 4.1 文件

- 新增 `infra/event_state.h` / `infra/event_state.cpp`：声明表、状态槽、订阅表、待派发队列、`dispatch_pending()`。
- `contracts/events.h`、`contracts/host_sdk.h`：契约与 SDK 尾部扩展。
- `host/host_sdk.cpp`：导出 `QolEventStateApi`，实现 owner 校验与主线程判定。
- `host/plugin_registry.cpp`：在插件 install/enable/disable/deactivate 生命周期接入声明与清理。
- `host/assembly.cpp`：注册派发帧任务；启动期订阅点改为状态订阅或保留别名订阅。
- `host/frame_source.cpp`：删除 `save_enter_ready` 探针与 `owner_token_of("core.save")` 存在性检查；保留 `begin/cancel` 两个操作 thunk（它们是游戏层调用的操作入口，不是时机判定）。
- `patch/save_enter.cpp`：删除 `g_pending` 的“事件源”职责；保留 wrapper 的门禁与原版调用；`save_enter_tick` 只保留兼容别名所需的最小逻辑或整体删除（见 §6）。

### 4.2 数据结构与容量

| 表 | 建议上限 | 满时语义 |
|---|---|---|
| 声明槽 | 32 | `kQolErrBusy`，不覆盖他人声明 |
| 状态订阅 | 256 | `kQolErrBusy` |
| 待派发变化 | 256 | 保留每个事件最新状态并标记 pending，下一帧重建快照；不静默丢弃最终状态 |

每个待派发项携带 `owner_generation`；owner 失效后其生产者回调不得再运行，但消费者侧的 `kUnavailable` 仍须派发。

### 4.3 派发点与顺序

- 作为 `infra::scheduler` 的常驻 `kLogicPre` 任务注册（与 `drain_commands_task` 同一调度器，不新增调度层）。
- 注册序放在 `drain_commands_task` 之后、`save_route::step()` 之前，保证同帧的命令/生命周期先落态。
- 任务体：锁内取出待派发快照并清空队列 → 释放锁 → 按入队顺序逐项调用 handler。
- **禁止**在 `set_state`、`save_transition_confirm`、`save_route` native_call、插件生命周期回调中直接调用订阅者。`set_state` 只做校验、写状态、入队。
- 若状态提交发生在当前帧派发任务之后，变化自然延后一帧；这是允许代价，不得为降低延迟改为内联。
- 回调在锁外执行，允许在回调内 `subscribe` / `unsubscribe` / `read` / `set_state`。派发期间注销的订阅按现有总线语义：本轮快照仍可能被调用一次。

### 4.4 订阅时的快照

`subscribe` 成功后把当前状态作为 `kSnapshot` 入队，**不在 subscribe 调用栈内联回调**。下一次帧点派发。这样核心语义保持“所有回调都在帧点、都在锁外”。

### 4.5 宿主 fallback 源

`kEventStateSaveReady` 在 owner 缺席（未安装 / 未声明 / 已停用）时，由宿主内部 fallback 源合成状态：

- 数据源：`game_in_world()` 的 0→5 / 5→非 5 边沿。
- 权限：fallback 不是插件，不通过 SDK 暴露；只能在 registry 内部写入，且仅当该 event id **没有活跃声明**时生效。
- 交接：owner 声明成功后，fallback 立即停止并在同一帧提交一次状态迁移（不产生双发）。
- 目的：核心语义仍集中在宿主一处，消费者不各自探测 owner 是否存在。

## 5. 生命周期与权限

| 阶段 | 动作 |
|---|---|
| install（声明） | `declare(event_id, schema=1)`；状态初始化为 `unavailable,false`，revision=1 |
| on_enabled | `set_state(available,false)` |
| 领域事实成立（本方案：confirm 成功） | `set_state(available,true,slot)` |
| 领域事实撤销（begin 新转换 / cancel / save exit） | `set_state(available,false)` |
| on_disabled | `set_state(unavailable)`，随后清理订阅与声明；撤销通知在下一帧点派发 |
| deactivate / unload | 阻止新 `set_state`，提交 unavailable，按 owner generation 清理；消费者侧通知仍派发 |
| 重复声明（同 owner、同 schema） | 幂等成功，不重置 revision |
| 重复声明（不同 owner） | `kQolErrConflict` |

权限：`declare` / `set_state` 以 `QolOwnerToken` 为唯一凭据。实现位置复用 `host/owner_identity`（`owner_authorize_*` / `owner_verify_token`），由 token 派生 plugin id，**不信任插件传入的字符串**。因此 `EventStateDescriptor` 不含 owner 字段。

## 6. 删除 `kEventSaveEnter`

- `contracts/events.h` 删除 `kEventSaveEnter` 常量与 `SaveEnterEvent` 结构；id 2 退役且不得复用。
- `patch/save_enter.cpp` 删除 pending/tick/事件发布，只保留发起点 wrapper（转换准入门禁 + 原版调用）与两条声明式资源。
- `contracts/runtime_hooks.h` 删除 `save_enter_mark_pending` / `save_enter_ready` 两个过渡字段，版本升至 1.3。
- `host/frame_source.cpp` 删除 `host_save_enter_ready()` 与两个字段的初始化项。
- 4 个订阅者迁移：`core.quest`、`custom_recipe`、`autosell`、`host assembly` 改为订阅 `kEventStateSaveReady`，按 `availability==available && value==1` 判定就绪；`kEventSaveExit` 保持事件订阅。
- 语义变化：就绪通知现在覆盖用户按钮读档、API 读档、catch-up 补转换与"模块进档后启用"的快照；不再依赖发起点是否被 patch。
## 7. 与现有机制的边界

| 机制 | 关系 |
|---|---|
| `infra::scheduler` | 复用；负责“何时运行”，event state 负责“存什么、何时边沿、通知谁”。不新增任务注册表 |
| capability availability / probe | 共存不替代。capability 表示“接口能否调用”，event state 表示“领域事实是什么” |
| snapshot（`QolSnapshotHandle`） | 共存不替代。snapshot 是不透明数据快照，无 owner 校验与边沿语义 |
| `save_route` native_call | 保留。继续承载 begin/confirm/cancel 等操作；状态更新在事务结束后提交，不在 native_call 内派发 |
| `kEventConfigChanged` | 不迁移。它是纯边沿广播，没有“当前值”语义 |

## 8. 迁移路径（实施前计划；实际落地见 §12）

### 阶段 1：基础设施（无生产接线）

- 新增 `contracts/events.h` 状态契约、`infra/event_state.{h,cpp}`。
- 新增测试 target `infra_event_state_tests`（`tests/CMakeLists.txt` 独立 target，或加入 `INFRA_TEST_SOURCES`）；覆盖：state 查询、值不变不派发、ready 上升/下降、unavailable 一次、snapshot 入队、回调内订阅/注销、owner 伪造、generation 失效、容量上限、非主线程拒绝。
- `check_arch`：`infra/event_state` 不得依赖 `host/`、`game/`；主线程判定与 fallback 源通过注入接口。

### 阶段 2：SDK 与生命周期接线

- `QolHostSdk` 尾追加 `event_state`，`kHostSdkMinor` 31→32；`host/host_sdk.cpp` 导出函数表；`event_state_if_available` helper。
- `host/plugin_registry.cpp`：install 声明、enable/disable/deactivate 的状态与清理。
- 扩展 `runtime_hooks_contract_tests` 或新增 ABI 用例：旧前缀偏移不变、`abi.minor < 32` 时 helper 返回 null、`struct_size` 不足时拒绝。

### 阶段 3：core.save 成为状态源 + 兼容别名

- `plugins/core.save/plugin.cpp:install` 声明 `kEventStateSaveReady`；`on_enabled` 置 available,false；`on_disabled`/deactivate 置 unavailable。
- `plugins/core.save/save_transition.cpp`：begin 置 false，confirm 成功置 true + slot；cancel 置 false。
- 状态模块在 `kBecameReady` 时发布 `kEventSaveEnter` 别名。
- `host/frame_source.cpp` 删除 `save_enter_ready` 探针与 `owner_token_of("core.save")` 检查；`patch/save_enter.cpp` 删除 `g_pending` 事件源（wrapper 与门禁保留）。
- 宿主 fallback：owner 缺席时用 world 边沿合成同一状态。

### 阶段 4：消费方迁移

- `core.quest`：删除 `world_transition_tick` / `g_was_in_world` / `start_resync_task` 的轮询职责；改用 `kEventStateSaveReady` 订阅（保留 `kEventSaveEnter` 对存档重置的既有行为），或直接订阅状态事件。
- 保持 autosell / custom_recipe / assembly 零改动作为回归基线。
- `extension_bag` 的世界状态轮询作为独立后续阶段（见 §9），不与本阶段混做。

### 阶段 5（可选）：世界状态事件

若阶段 4 验证通过，再为世界状态定义 `kEventStateWorld`（value = `state==5`，owner = `core.world`），迁移 core.quest 与 extension_bag 的 `world.facts` 轮询。**不要在一次改动里同时引入两个事件。**

## 9. 使用者与受益者清单

### 9.1 必须迁移

| 使用方 | 位置 | 现状 | 迁移动作 |
|---|---|---|---|
| core.save（生产者） | `plugins/core.save/plugin.cpp`、`save_transition.cpp` | 维护 `ready_published`，被宿主探针轮询 | 声明事件并在 begin/confirm/cancel/停用提交状态 |
| 宿主时机接缝 | `host/frame_source.cpp:325/333/346/364` | 4 处硬编码 `"core.save"` | 删除 346/333（时机）；保留 325/364 作为操作 thunk |
| 宿主延迟恢复 | `host/plugin_registry.cpp:2883-2980` | 只认 `kEventSaveEnter`（生产唯一调用者 `host/assembly.cpp:540`） | 通过别名零改动；catch-up 后自然触发 |
| core.save 事件源 | `patch/save_enter.cpp:32/74/86/128-141` | `g_pending` + tick 判定 + 发布 | 删除事件源职责，保留 wrapper 门禁 |

### 9.2 零改动受益者

| 使用方 | 位置 | 收益 |
|---|---|---|
| custom_recipe | `plugins/custom_recipe/result/feature.cpp:640`、`plugins/custom_recipe/plugin.cpp:299` | 延迟 ownership 恢复在 catch-up 后也能触发；本次故障消失 |
| autosell | `plugins/autosell/scan.cpp:513/525` | catch-up 也会同步扫描任务；槽校验沿用 |
| core.quest（第一阶段） | `plugins/core.quest/plugin.cpp:506` | catch-up 也会 resync |
| 宿主 assembly | `host/assembly.cpp:540` | 延迟恢复获得 catch-up 触发路径 |

### 9.3 后续可迁移（第二阶段）

| 使用方 | 位置 | 现状 | 判断 |
|---|---|---|---|
| core.quest world 边沿 | `plugins/core.quest/feature.cpp:59-76` | 每帧轮询 `world.facts.state()==5`，本地 `g_was_in_world` | 高价值：世界事实至少 2 个独立消费者；迁移后删除自建边沿 |
| extension_bag 生命周期节拍 | `plugins/extension_bag/core/lifecycle.cpp:507-539` | 每 30 帧轮询世界状态，`lifecycle_last_state` + `main_menu_cleanup_dirty` 判定主菜单清理与 sidecar 装载 | 中价值：可与 core.quest 共用 `kEventStateWorld`；注意时序将从“≤30 帧内”变为“边沿当帧”，需单独验收 |
| extension_bag 渲染/投影查询 | `plugins/extension_bag/core/render.cpp:414`、`core/projection.cpp:766` | 同步查询 `game_state_gate` / 是否在世界 | 不迁移：需要同步取值，不是通知语义 |
| core.ui_state 原生快照 | `plugins/core.ui_state/plugin.cpp:237,1180` | 每帧刷新 snapshot epoch | 不迁移：它是快照生产者，不是边沿消费者 |
| `kEventConfigChanged` | `infra/config.cpp:68` | 纯广播 | 不迁移：无“当前值”语义 |

## 10. 风险与规避

| 风险 | 规避 |
|---|---|
| 回调重入 core.save confirm / 持锁 | `set_state` 只入队；订阅者只在帧点锁外调用；单测断言提交期间回调计数为 0 |
| 宿主 fallback 与 core.save 双写导致抖动 | fallback 仅在无活跃声明时生效；owner 声明后同帧一次性交接，带 generation |
| 停用与派发竞态 | 待派发项携带 owner generation；失效后不调用生产者，但消费者侧 unavailable 仍派发 |
| 迟到订阅者把 snapshot 当成刚进档 | change kind 显式区分；`kSnapshot` 不映射 `kEventSaveEnter` |
| ABI 前缀漂移 | 不扩内嵌 `QolEventsApi`；只尾追加 `event_state`；静态断言 + ABI 测试 |
| 队列满导致漏最终状态 | 按事件保留最新状态并标记 pending，下一帧重建；不承诺同一帧多个中间翻转 |
| 事件 id 被其他插件抢占 | 单 owner + 第二声明 `kQolErrConflict`；事件 id 为 contracts 常量 |
| 时序变化破坏 VM-FT-01 | 派发固定在 LogicPre 注册序；先跑 host 顺序测试，再真机核对 `save enter fired` 晚于 `transition ready` |

## 11. 测试与验收

### Host 单测

- 新增 `infra_event_state_tests`（机制级）。
- `plugin_registry_tests`：owner 声明/停用/接管、延迟恢复经别名触发。
- `runtime_hooks_contract_tests`：SDK 尾追加、minor/size 协商、旧前缀偏移。
- `save_transition_tests`：begin/confirm/cancel 的状态迁移与 revision。
- `quest_lifecycle_tests`、`autosell_plugin_tests`：消费方行为不回归。

### 静态检查

`uv run python scripts/maintenance/check_arch.py`（含 `check_layers`、`check_resources`）零新增。

### 真机验收卡（`docs/refactor/verification.md`）

- 更新 **VM-FT-01**：派发帧点、snapshot、ready 上升沿别名与 core.save confirm 顺序。
- 更新 **VM-SE-01**：core.save 缺席/停用时的 world fallback、恢复接管、停用 unavailable 通知。
- 新增 **VM-ES-01**：进档后才启用订阅者仍收到当前 ready 快照；未 ready 时订阅后收到后续上升沿；反复读档时 `kEventSaveEnter` 每次仅一次且各消费者行为不变；三条路径（正常按钮读档、装配 patch 失败走原版、catch-up 补转换）都必须让 custom_recipe ownership 最终恢复。
- 无真机证据时按 `verification.md` §1 标 `NOT_ACCEPTED`。

## 12. 实现记录（2026-11-07）

- 已实现：`contracts/events.h` 状态契约、`contracts/host_sdk.h` 尾追加 `event_state`（minor 31→32）、`infra/event_state.{h,cpp}`、`host/host_sdk.cpp` 导出、`host/assembly.cpp` fallback/dispatch/订阅、core.save 生产者、core.quest/custom_recipe/autosell 消费者、`kEventSaveEnter` 删除。
- 单测：新增 `infra_event_state_tests`；host 全量 159 项中 156 通过，3 项为改动前既存失败（`custom_recipe_tests`、`item_handle_contract_redline`、`core_api_lifecycle_tests`，涉及文件均未在本次修改）。
- 真机（Monster v29）：`transition ready` 后 custom_recipe 动态配方、autosell、quest 同步触发；`custom_recipe ownership restored`、表注入 `base=69 records=71 materials=191`；`/api/system/features` 无 degraded/error。
- 未验证子场景见 `verification.md` VM-ES-01。

## 13. 已裁决

1. 宿主 fallback 只允许写 `kEventStateSaveReady`，且仅在该事件没有活跃声明时生效。
2. `kEventSaveEnter` 直接删除，不保留别名；4 个订阅者全部迁移。
3. 事件 id 与状态结构固定在 `contracts/events.h`，命名 `kEventStateSaveReady`。
4. 世界状态事件不并入本次，留待后续独立阶段。
