# extension_bag 插件重构方案

状态：设计稿（待评审）
范围：`module/app/src/main/cpp/plugins/extension_bag`
前置结论：锁审阅三份报告、结构审阅报告（2026-10-24）；线程模型修正为**全部主线程执行**。

## 1. 目标

1. **底层组件先完善**：状态、所有权、事务、投影、原生布局各有唯一 owner，接口窄、可单测。
2. **上层只编排**：UI（装备页/商店/合成/详情）与 API/HTTP 只调用用例层，不直接读写运行时状态、不直接调原生。
3. **执行契约明确**：所有入口在主线程；重入有显式策略（读快照/写拒绝），不依赖互斥量。
4. **后续改动简单**：新增功能 = 新增一个用例 + 一个 UI/API 适配；修 bug 能定位到单一组件。
5. **行为不变**：现有 C18 布局、跨袋移动、投影、持久化、商店/合成语义保持不变。

## 2. 非目标

- 不改 Game Host ABI、UiAdapter 契约、ExtensionBagApi 版本语义（除新增内部组件所需的最小扩展）。
- 不重写 `model/` 纯模型，不改 sidecar 格式。
- 不做多实例/依赖注入框架，不引入 `recursive_mutex`。
- 不按行数机械拆文件。

## 3. 执行契约（先于一切重构）

### 3.1 线程与重入

- 所有 EB 入口断言主线程；HTTP 写命令由 Host 编组到主线程帧回调，查询读不可变快照。
- 原生回调可能在同一线程重入 EB（页签 proc、item proc、数量查询、快捷栏刷新、详情回调）。
- 重入策略：
  - **读**：返回最近一次已提交快照或回退值（数量查询回原版、状态查询回上一 revision），不阻塞。
  - **写**：拒绝并记日志（`reentry reject op=...`），不进入半事务。
- 事务期间的状态对外不可见：提交前不更新 `revision`，快照读不到中间态。

### 3.2 护栏 API（新增，先落地）

```cpp
// runtime/entry_guard.h
enum class ReentryPolicy { kReadSnapshot, kRejectWrite };
class EntryGuard {                 // 非递归；记录当前线程的活动入口
public:
    EntryGuard(const char* op, ReentryPolicy policy);
    ~EntryGuard();
    bool entered() const;          // false = 检测到重入，调用方按策略返回
};
void extension_bag_assert_main_thread();
```

规则：
- 所有 `ExtensionBagApi` 成员、HTTP handler、UI 回调入口第一行使用 `EntryGuard`。
- `EntryGuard` 取代现有 `try_lock` 重入规避（`tabs_ui.cpp:84,105,176`、`detail_actions.cpp:367,478`、`ownership.cpp:207`、`bridge.cpp:269`）。
- 删除 `std::mutex` 放在最后阶段；在此之前 `EntryGuard` 与现有锁共存，锁只作过渡。

### 3.3 快照

```cpp
struct BagSnapshot {           // 只读、值语义
    std::array<uint8_t, 5> types, capacities;
    std::array<std::array<ItemView, 16>, 5> items;
    Mode mode; int selected; uint64_t revision;
};
struct ProjectionSnapshot { bool installed; int bag; int host_bag; uint64_t generation; };
```

读路径统一走快照；不再有“读函数里懒加载/物化”的隐藏写。

## 4. 目标分层

```text
L4 接口层    ExtensionBagApi provider · HTTP command adapter · UI 回调注册
L3 用例层    move/equip/use/sell/mix/detail usecases（只编排）
L2 适配层    InventoryAccess · ProjectionAdapter · SlotDataAdapter · UiPageAdapter · ResourceHooks
L1 组件层    BagStateStore · OwnershipManager · TransactionCoordinator · ProjectionController
             NativeTabLayout · DragSession
L0 模型层    model/*（纯数据、规则、状态机；无原生、无 IO、无全局状态）
```

依赖规则：
| 层 | 允许依赖 |
|---|---|
| L4 | L3、L2（只读能力）、contracts |
| L3 | L1、L2、L0 |
| L2 | L1、L0、game/access 端口、contracts |
| L1 | L0、L2 的窄接口（不得反向依赖 L3/L4） |
| L0 | 无 |

禁止：
- `extension_bag_internal.h` 全量聚合头（新代码不得包含；存量按阶段清理）。
- `#define <name> <name>_internal/_bridge` 宏桥。
- L1/L2 直接包含 `ui/` 头；UI 只能通过用例层。

## 5. 组件规格

### 5.1 BagStateStore（L1）

- **拥有**：`virtual_bag::State`、`module_objects/module_object_*` 的槽位映射、`revision/dirty`。
- **接口**：
  - `BagSnapshot snapshot()`
  - `bool mutate(const Mutation&)`：进入写事务、校验、提交/回滚
  - `uint64_t revision()` / `mark_dirty()` / `clear_dirty()`
  - `bool load_from_sidecar(bytes)` / `serialize_sidecar()`
- **不变量**：`items` 与 `module_objects` 同槽一致；`capacities` 由 `types` 派生；`pending` 只能由 TransactionCoordinator 写。
- **映射**：现 `runtime/state.h` 字段、`orchestrator.cpp:ensure_state_loaded_locked/persist_state_locked`、`bridge.cpp` 的 JSON/物化入口。

### 5.2 OwnershipManager（L1）

- **拥有**：`ownership::Ledger`、item handle custody、use token、deferred free 队列。
- **接口**：`allocate/handover/release/retire`、`begin_use/finish_use/abort_use`、`defer_free/drain`、`is_referenced`。
- **不变量**：一个原生对象同一时刻只有一个 owner（module / game / provider）；handover 后模块不得再解引用。
- **映射**：`runtime/ownership.cpp` 全部；`model/ownership_ledger.h` 保持纯模型。

### 5.3 TransactionCoordinator（L1）

- **拥有**：pending 事务、恢复、隔离记录、阶段推进。
- **接口**：
  - `begin(direction, src, dst, payload) -> Txn`
  - `record_pending(txn)` / `verify_source(txn)` / `verify_destination(txn)`
  - `commit(txn)` / `abort(txn, stage, reason)` / `recover_pending()`
- **阶段**：Prepare → Validate → LogicalUpdated → NativeUpdated → Committed；每阶段失败走既有回滚/隔离语义。
- **不变量**：`pending` 只有这里写；提交前外部读不到变更；恢复只依赖 sidecar + 实态。
- **映射**：`transaction/ext_orig.cpp`、`ext_ext.cpp` 的公共部分；`orchestrator.cpp` 的 `txn_*`、`recover_pending_transaction_locked`。

### 5.4 ProjectionController（L1）

- **拥有**：`module_view_installed/index`、宿主袋与容量字、`projected_item_root`、投影安装/恢复。
- **接口**：`install(bag)`、`restore()`、`sync_slot(bag,slot)`、`sync_bag()`、`host_bag()`、`is_installed()`、`capacity_word(bag)`。
- **不变量**：宿主袋容量字在事务期临时还原后必须 RAII 写回；root/generation 变化时拒绝旧句柄操作。
- **映射**：`runtime/projection.cpp`、`public/projection.cpp` 的 apply/sync、`orchestrator.cpp:install_module_view_locked/restore_module_view_locked`。

### 5.5 NativeTabLayout（L1）

- **拥有**：C18 布局探测、reflow plan、页签命中、原生袋号映射。
- **接口**：`plan(root, generation)`、`apply(plan)`、`restore()`、`module_tab_hit(x,y,index)`、`native_bag_hit(x,y,&bag)`。
- **不变量**：child index = 原生袋号（与 `UIEquip_GetBagSlotIndex` 同义）；不推断 bag 语义；歧义 fail-closed。
- **映射**：`runtime/tabs_ui.cpp` 的 `prepare_c18_native_bag_layout*`、`extension_c18_native_bag_hit`、`runtime/equip_tab_layout.h`。

### 5.6 DragSession（L1）

- **拥有**：`extension_drag`、`extension_drag_session`、touch capture、pending tab。
- **接口**：`press/move/release`、`resolve_target`、`begin/finish_transaction`、`cancel(reason)`、`generation_current()`。
- **不变量**：source identity（handle+generation）在事务前复核；跨 generation 不重用。
- **映射**：`model/virtual_bag_state.h` 的 DragSession 纯模型 + `runtime/input.cpp`、`transaction/drop.cpp` 的会话部分。

### 5.7 适配层（L2）

| 适配器 | 职责 | 映射 |
|---|---|---|
| `InventoryAccess` | 袋布局探测、槽读取、`bag_size`、`save_item_on_empty`、`remove_item_direct`、`refresh_item_area`、`current_bag` | `game/access/inventory.*`、`public/projection.cpp` 的原生调用点 |
| `ProjectionAdapter` | 原版 UI 刷新、控件同步、draw gate | `public/projection.cpp`、`runtime/projection.cpp` 的 UI 部分 |
| `SlotDataAdapter` | sidecar 注册、load/capture/complete、dirty staging | `persistence/persistence.cpp` |
| `UiPageAdapter` | 装备页/商店/合成/详情的控件树与回调 | `ui/*.cpp` |
| `ResourceHooks` | claims、hook 备份、popup 能力 | `assembly/claims.cpp`、`plugin.cpp` |

### 5.8 用例层（L3）

- `move_original_to_extension`、`move_extension_to_original`、`move_extension_to_native`（C18）、`move_extension_to_extension`
- `equip_on_tab`、`unequip_to_extension`、`consume/use`、`sell/destroy`
- `enter_view/exit_view`、`open_store_view/open_mix_view`
- 每个用例：校验 → 调 L1 事务 → 成功后刷新（投影/快捷栏）→ 返回结果。**不直接碰控件。**

### 5.9 接口层（L4）

- `ExtensionBagApi` provider：只做参数校验 + 主线程断言 + 调用例。
- HTTP：写命令由 Host 编组到主线程；查询只读快照。
- UI 回调：只把控件事件翻译成用例调用，不读写状态。

## 6. 现状映射（文件去向）

| 现状 | 目标 |
|---|---|
| `runtime/state.h` + `orchestrator.cpp` 状态/缓存 | `bag_state_store.*` |
| `runtime/ownership.cpp` | `ownership_manager.*` |
| `transaction/ext_orig.cpp`、`ext_ext.cpp`、`orchestrator.cpp` 的 txn/pending/recovery | `transaction_coordinator.*` + 方向实现 `transaction/orig_ext.*`、`ext_orig.*`、`ext_ext.*` |
| `runtime/projection.cpp`、`public/projection.cpp` | `projection_controller.*` + `projection_adapter.*` |
| `runtime/tabs_ui.cpp`（1151 行） | `native_tab_layout.*`（布局/命中/锚定）+ `equip_tabs_ui.*`（交互/点击） |
| `runtime/lifecycle.cpp` | `lifecycle.*`（事件路由，只调用例） |
| `ui/*.cpp` | `ui_pages/*`（页面适配）+ 用例调用 |
| `public/status.cpp` | 拆成 `queries.*`（快照 JSON）与 `usecases/unequip.*` |
| `api/api_impl.cpp` | `commands.*`（参数校验 + 用例） |
| `extension_bag_internal.h` | 删除；按层拆窄头文件 |

## 7. 分阶段计划

每阶段独立提交、可回滚；行为不变，Host 测试全绿后才进下一阶段。

### P0 契约与观测（不改行为）

- 新增 `entry_guard.h`、主线程断言；在全部 API/HTTP/UI 入口接入（默认策略 `kRejectWrite` / 读 `kReadSnapshot`）。
- 保留现有锁；`EntryGuard` 与锁并存，重入时先由 guard 返回。
- 加观测：事务阶段耗时、快照版本、重入拒绝次数、原生调用次数。
- 验收：CTest 155/155；真机开包/切袋/跨袋移动无回归。

### P1 BagStateStore + InventoryAccess

- 状态读写收口到 `BagStateStore`；原生袋布局/槽读取收口到 `InventoryAccess`。
- 读路径改快照；移除读路径里的懒加载（加载只在进档/首次）。
- 验收：状态查询、数量查询、物理槽读取、跨袋移动（原版/C18/扩展）全部通过。

### P2 TransactionCoordinator

- 从 `orchestrator.cpp` 抽出 pending/恢复/隔离与阶段推进。
- 四个方向改为“准备 → 原生调用 → 复核 → 提交/回滚”。
- 验收：四方向移动、失败回滚、进程重启恢复、sidecar 往返。

### P3 ProjectionController + NativeTabLayout

- 投影安装/恢复/宿主容量字集中；`tabs_ui.cpp` 拆布局与交互。
- 验收：v513/v514 页签布局、reflow、页签点击、原生袋命中、商店/合成投影。

### P4 用例层 + 接口收口

- 新增 `usecases/`；`api/`、`ui/` 只调用例。
- 删除宏桥与 `extension_bag_internal.h` 的包含；按层拆头文件。
- 验收：所有 HTTP 命令、UI 面板、详情/出售/合成路径。

### P5 删除互斥量与线程字段

- 删除 `runtime_state().mtx`、`std::thread::id owner`、`try_lock` 退让代码。
- 保留主线程断言与 `EntryGuard`。
- 验收：并发命令编组到位后跑压力（连续 200 次移动/查询/开包），无重入拒绝死循环、无半状态。

## 8. 架构约束固化（CI）

在 `scripts/maintenance/check_arch.py` 增加 extension_bag 局部规则：
1. `model/` 不得包含 `runtime/`、`ui/`、`public/`。
2. `runtime/` 不得包含 `ui/`、`api/`。
3. 禁止新增 `#include "plugins/extension_bag/extension_bag_internal.h"`。
4. 禁止 `#define <ident> <ident>_internal/_bridge` 形态的宏桥。
5. 新增 `std::mutex` 在 extension_bag 内直接失败（P5 后）。

## 9. 测试策略

| 层级 | 测试 |
|---|---|
| L0 模型 | 现有纯逻辑单测保持 |
| L1 组件 | 新增：状态快照一致性、所有权状态机、事务阶段/回滚、投影安装恢复、布局计划 |
| L2 适配 | Host stub 契约测试（原生调用参数/失败注入） |
| L3 用例 | 方向矩阵 + 失败注入（满包、身份变化、generation 失效） |
| L4 接口 | HTTP 命令参数校验、快照查询、主线程断言 |

真机验收按 `docs/refactor/verification.md` 受影响卡执行；静态整理不得报告为行为验收通过。

## 10. 风险与回滚

| 风险 | 缓解 |
|---|---|
| 快照化后读到旧数据 | 快照带 revision；写用例提交后递增；UI 刷新按 revision |
| 事务阶段化改变回滚语义 | 每阶段保持现有 isolate/rollback 路径；逐方向迁移并对比日志 |
| 删除锁后遗漏 off-main 调用 | P0 断言先跑一轮真机；编组完成前不删锁 |
| 宏桥清理引入编译期隐式依赖 | 按文件逐个替换，保持薄 wrapper 直到所有调用点迁移 |

## 11. 完成定义（DoD）

1. L1 六个组件有独立头文件、窄接口与单测。
2. `api/`、`ui/` 不直接访问 `runtime_state()`；全部经用例层。
3. `extension_bag_internal.h` 与宏桥为零。
4. extension_bag 内无 `std::mutex`，入口统一 `EntryGuard` + 主线程断言。
5. Host 测试全绿；v513/v514 真机验收卡通过；无新增静态检查违规。
