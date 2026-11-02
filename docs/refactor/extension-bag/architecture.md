# extension_bag 架构设计

状态：目标架构；P0–P4 已落地，P5 部分落地，P6 待查询编组（见 §9）。

## 1. 设计原则

1. **数据与规则分离**：物品属性来自游戏数据；背包定义来自 catalog；业务规则保持代码。
2. **依赖单向**：L0 model ← L1 组件 ← L2 适配 ← L3 用例 ← L4 接口；禁止反向包含。
3. **状态只在一个地方改**：业务状态只由用例层经组件修改；绘制、命中、查询只读快照。
4. **原生优先**：原版袋与 C18 追加袋以游戏为唯一数据源；扩展袋以 sidecar 为唯一数据源。
5. **失败关闭**：任何布局、来源、事务歧义都拒绝执行并留日志，不猜测。

## 2. 分层与目录

```
plugins/extension_bag/
├── model/            L0  纯数据与规则（无 UI、无 hook、无状态单例）
│   ├── virtual_bag_state.h   State、Item、容量派生
│   ├── bag_catalog.h         扩展袋上限/活动数量/槽数/类型容量
│   ├── bag_catalog.inc       由 ITEMSTATICBASE.json 生成
│   └── virtual_bag_*.inc     事务/拖拽/序列化规则
├── runtime/          L1  组件 + L2 适配
│   ├── bag_state_store.*     状态快照与 revision
│   ├── inventory_access.*    原生袋布局、物理槽读取
│   ├── item_access.*         物品属性/句柄/序列化
│   ├── ownership.*           原生对象所有权账本
│   ├── transaction_coordinator.* 事务 ID/pending/阶段回滚/隔离
│   ├── projection_controller.*   容量字、当前袋、C18 页计数、来源门禁
│   ├── native_tab_layout.*       C18 布局解析/reflow/命中/锚定
│   ├── entry_guard.h         重入守卫
│   ├── state.*               runtime_state() 容器与生命周期
│   └── lifecycle.cpp         事件路由（只调用例）
├── transaction/      事务方向实现（orig↔ext、ext↔ext、drop）
├── usecases/         L3  用例：移动/装备/使用/出售/进入视图/数量设置
├── api/              L4  扩展 API provider（参数校验 + 用例）
├── http/             L4  HTTP 路由（命令编组 + 用例）
├── public/           L4  bridge/status/equip/sale
├── ui/               L4  页面与控件适配（只把事件翻译成用例）
├── persistence/       sidecar JSON（v5）
└── extension_bag_geometry.*  UI 几何原语
```

## 3. 组件职责

| 组件 | 职责 | 不负责 |
|---|---|---|
| `BagCatalog` | 扩展袋上限、活动数量、槽数、类型容量、逻辑 ID 范围 | 状态存储、UI |
| `BagStateStore` | State 读写、脏标记、快照发布 | 事务语义、原生调用 |
| `InventoryAccess` | 原生袋布局探测、物理槽读写、当前袋 | 业务规则 |
| `ItemAccess` | 物品属性/句柄/序列化/物化 | 袋归属 |
| `OwnershipManager` | 原生对象所有权账本、临时对象回收 | 移动方向决策 |
| `TransactionCoordinator` | 事务 ID、pending、阶段回滚、失败隔离 | 原生调用细节 |
| `ProjectionController` | 容量字、当前袋原始/raw、C18 页计数、来源袋门禁 | 安装/恢复的用例编排 |
| `NativeTabLayout` | C18 布局解析、reflow、命中、锚定 | 页签点击业务 |
| `DragSession` | 拖拽会话生命周期、单次消费 | 事务提交 |
| `EntryGuard` | 同线程重入拒绝、主线程断言 | 跨线程互斥 |

## 4. 数据模型

### 4.1 两类袋

| 袋 | 数据源 | 容量来源 | 操作落点 |
|---|---|---|---|
| 原版 0–4 | 游戏 | 游戏容量字 | 原生接口 |
| 原版任务袋 5 | 游戏 | 游戏 | EB 禁止事务 |
| C18 追加袋 6..N | 游戏 | 游戏 | 原生接口 + EB 门禁/高亮 |
| 扩展袋 6..6+active−1 | sidecar | `capacity_for_bag_type` | EB 事务 + 物化/回收 |

“6..N” 是原生页签序号，与扩展逻辑 ID 是不同命名空间，只在投影层汇合。

### 4.2 扩展袋 catalog

```cpp
kMaxExtensionBags = 8;          // 数组上限
active_extension_bags();        // 1..8，来自 State.bag_count
capacity_for_bag_type(type);    // {0,4,8,12,16}，生成表
extension_logical_bag_last();   // 6 + active - 1
```

- `State.types/capacities/items` 固定为 `kMaxExtensionBags`，活动数量只影响可见/可用范围。
- sidecar v5：`bagCount`；v4 缺失按 5；越界拒绝加载。

### 4.3 投影

- 投影层只读：绘制与命中不写业务状态。
- 安装/恢复由用例层调用，副作用（容量字、控件同步、物品区刷新）集中在用例内。
- 扩展袋物品物化为原生对象时登记所有权；归还后释放。

## 5. 数据流

### 5.1 移动

```
UI/HTTP/API
  → usecase(move_*)
    → 校验（catalog、来源门禁、pending）
    → TransactionCoordinator.pending
    → 原生调用（原版/C18 目标）或 State 写入（扩展目标）
    → 复核（原生槽/指针一致性）
    → commit 或 rollback + isolate
    → 刷新（projection / shortcut）
  → 快照 revision++
```

### 5.2 查询

```
HTTP status → BagStateStore 快照（只读）→ JSON
```

### 5.3 绘制

```
draw hook → 读快照 + mode
  → NativeTabLayout 应用/恢复 reflow（不改业务状态）
  → 原生 DrawInvenBag（临时 current_bag 由投影控制器提供）
```

## 6. 线程与重入

- 目标：所有状态访问在主线程。
- 写命令：HTTP → `commands.post_typed` → `commands_drain()`（主线程）。
- 查询：读快照；不得在 worker 线程读 live State。
- 重入：入口 `EntryGuard`，同线程重复进入直接拒绝并计数；不使用 `recursive_mutex`。
- P0–P4 保留 `runtime_state().mtx` 作为过渡；P6 在查询快照化后删除。

## 7. C18 适配

- 能力位 `kQolTargetCapabilityC18NativeBagLayoutCandidate`（bit3），未知 MD5 运行期探测。
- 袋布局：`native_bag_layout()` 探测原生袋对象表，得到上界与物理槽表。
- 页签：模块列固定在原版列右侧；完整 72×84 框冲突时只平移 C18 竖向组；底行与其他控件不动。
- 命中：child index 仅作 UI 命中，不进入扩展逻辑 ID/事务域。
- 高亮：C18 module view 临时把 current_bag 写成页签总数（无对应页签→原生无高亮），绘制后恢复 raw。

## 8. 扩展点

| 需求 | 改动点 |
|---|---|
| 新增 C18 版本 | 跑变体表生成器，确认 capabilities=8 |
| 调整扩展袋数量 | 设置项 `bagCount`（1..8），无需改代码 |
| 新增袋类型/容量 | 改 ITEMSTATICBASE.json，重跑 `build_bag_catalog.py` |
| 新增移动方向 | 新增 usecase + transaction 方向实现，复用 coordinator |

## 9. P6 前置：查询编组与线程契约（审计结论）

只读审计确认删除 `runtime_state().mtx` 前必须处理：

1. `virtual_bag_inventory_bags_json()`（`api/api_impl.cpp:314-343`）仍遍历 live State，经
   `neutral_inventory_fragment` 暴露给只读 API；须改为快照或主线程投递。
2. `bag_capacity` / status 的 fallback 分支仍读 live State；删除锁后必须拒绝 fallback。
3. provider 只读回调（`owns_bag`/`item_ref_at`/`for_each_item`）的“仅主线程”只是注释约定，
   调用链 `game_state.cpp:295-312,327-363`、`game/access/inventory.cpp:129-176` 未逐条证明线程。
4. public/JNI 入口（`public/bridge.cpp`、`public/status.cpp`、`public/equip.cpp`）大量直接操作 live State，
   且未覆盖 `EntryGuard`。
5. `try_lock` 同时承担同线程递归保护（`public/bridge.cpp:262-266`、`runtime/tabs_ui.cpp:87-111`），
   EntryGuard 不阻塞、不串行化，不能直接替代。

最小改造顺序：inventory fragment 快照化 → fallback 拒绝 → provider 线程契约与断言 → public 入口覆盖 →
最后逐处替换 try_lock 的 busy 语义。完成前保留互斥量。

## 9.1 查询编组设计

删除 `runtime_state().mtx` 前必须满足：

1. 所有 HTTP 查询（`/api/debug/extension_bag/status` 等）经 `commands.post_typed` 编组到主线程，或在主线程发布“全量快照”（含 items 与 payload）。
2. 查询线程不得直接读 `runtime_state().objects`。
3. 原生回调重入由 `EntryGuard` 拒绝，不再依赖 `try_lock` 退让。
4. 迁移顺序：先做 status 查询快照 → 再删读锁 → 最后删写锁；每步跑 200 次连续命令压力。

在此之前保留互斥量是正确取舍：跨线程数据竞争比锁开销危险得多。

## 10. 阶段状态

| 阶段 | 状态 |
|---|---|
| P0 入口守卫/观测 | 已完成 |
| P1 BagStateStore + Access 拆分 | 已完成 |
| P2 TransactionCoordinator | 已完成 |
| P3 ProjectionController + NativeTabLayout | 已完成（P3a/P3b；P3c 与 Ownership 合并迁移） |
| P4 BagCatalog + 可配置数量 | 已完成：catalog 生成、sidecar v5、set_bag_count、1–5 可用 |
| P5 用例层 + 接口收口 | 部分：orchestrator 宏桥已删；view/move usecases 已建；status 查询走快照；api/ui 直连 runtime_state 与 internal 头待清理 |
| P6 删除互斥量 | **不可直接执行**：审计确认仍有跨线程 live State 读与递归 try_lock 语义，需先完成 §9 的最小改造集合 |
