# `extension_bag` 目标架构（v2）

状态：设计稿。当前实现见 `docs/reference/plugins/extension_bag.md`；本文件描述目标形态与迁移路径。

## 1. 目标

1. **单线程状态**：业务状态只在主线程读写；任何线程的调用都经命令队列或不可变快照。
2. **接口层收口**：ui/http/api 只调用 `BagQuery` / `BagCommand`，不直接访问数据层。
3. **数据层显式化**：原版袋、C18 改版袋、扩展袋各有独立数据源与适配器。
4. **投影独立**：投影是 UI 副作用适配器，不冒充数据层。
5. **删除互斥量**：状态单线程后，锁只剩重入守卫（EntryGuard），不再承担跨线程互斥。

## 2. 分层

```
L4  调用方        ui/  http/  api/  public/
                        │  参数校验、事件翻译、票据
L3  接口层        BagQuery（只读）      BagCommand（写/移动）
                        │  快照读取            │ post_typed + ticket_result
L2  用例层        view / move / equip / use / sell usecases
                        │  校验、方向矩阵、事务编排
L1  数据层        NativeInventory   ExtensionStore   Persistence   ProjectionPort
                        │  原生指针/槽       State+快照        sidecar v5     原生 UI 安装
L0  模型          catalog / state 类型 / 事务规则 / 拖拽规则
                        │
底层              游戏 hook / native 调用 / UI adapter
```

依赖只向下。L4 不 include `runtime/state.h`；L2 不直接调 native。

## 3. 线程模型

### 3.1 三条通路

| 通路 | 线程 | 机制 |
|---|---|---|
| 写命令（移动/装备/使用/出售/切袋/数量设置） | 提交线程任意；执行主线程 | `QolHostSdk.commands.post_typed` → `commands_drain()` 主线程执行 → `ticket_result` 回取 |
| 只读查询（status/capacity/inventory fragment） | 任意线程 | 读主线程发布的不可变快照 |
| 游戏 hook / UI 回调 | 主线程 | 直接调用用例，EntryGuard 防重入 |

### 3.2 命令协议

- 每个命令一个 `handler_id`，payload 为 POD（≤128B），结果 JSON 由 `QolTypedResultCommandHandler` 写入。
- 票据状态：`pending / done / failed / cancelled / unknown / expired`；HTTP 侧统一映射为票据。
- 需要跨帧等待的操作（如存档写入）用 `defer_current_result()`；受宿主延迟上限约束。
- 命令执行失败必须返回 `failed` 与原错误体，不得静默成功。

### 3.3 为什么不再需要 `runtime_state().mtx`

- 状态写入只发生在主线程命令处理器与主线程 hook 中，天然串行。
- 跨线程读取只允许快照：`shared_ptr<const Snapshot>` 原子发布，读线程不触碰 live State。
- 原生指针/句柄的 provider API 保持同步签名，但契约限定主线程调用（游戏 inventory hook 在主线程）。
- 保留 `EntryGuard` 仅用于同线程重入（native 回调 → 业务 → 再次进入）。

**前置**：所有 L4 入口完成编组/快照化；provider 线程契约逐条断言；`try_lock` 的 busy 语义用显式重入标志替代。

## 4. 接口层

### 4.1 BagQuery（只读）

| 查询 | 返回 | 数据源 |
|---|---|---|
| `status` | 完整状态 JSON（含 bagCount/maxBagCount） | StatusSnapshot |
| `bag_capacity(logical_bag)` | 容量 | BagSnapshot |
| `inventory_fragment` | 扩展袋 inventory JSON | StatusSnapshot |
| `item_at(logical_bag, slot)` | 物品描述 JSON | StatusSnapshot |
| `module_view` | 是否安装投影 | BagSnapshot |

- 实现：读快照，缺失返回未就绪，不回退 live State。
- 不产生副作用；不改变 revision。

### 4.2 BagCommand（写）

| 命令 | payload | 说明 |
|---|---|---|
| `enter_view` / `exit_view` / `select_bag` | bag | 视图切换 |
| `move` | from bag/slot, to bag/slot | 四方向由用例判定 |
| `equip_bag` / `unequip_bag` | index/bag | 装备/卸下 |
| `use_item` / `sell_item` / `destroy_item` | bag/slot/count | 物品操作 |
| `set_bag_count` | count | 1..kMaxExtensionBags |

- 所有命令：参数校验 → EntryGuard → 用例 → 事务 → 刷新 → 写结果。
- 结果 JSON 与现有 HTTP 响应一致，便于迁移期兼容。

### 4.3 兼容层

- 现有 `ExtensionBagApi` 保留签名；内部改为 `post_typed` + 等待/票据，或对只读成员读快照。
- 现有 HTTP 路由保留路径与票据语义；Kotlin 控制器不再直接映射到 native query handler，而是统一走命令/票据。
- 中立 inventory provider 保持同步签名（返回指针），契约限定主线程。

## 5. 数据层

### 5.1 NativeInventory

- 数据源：游戏原版袋 0–4、任务袋 5、C18 追加袋 6..N。
- 能力：`native_bag_layout()`、`physical_item_at()`、`item_ref_at()`、当前袋读写、页计数。
- 只提供**活对象**；返回指针/句柄。仅主线程。
- 不持有扩展袋状态。

### 5.2 ExtensionStore

- 数据源：插件 State（types/capacities/items/bag_count/mode/selection/pending）。
- 能力：读快照、写命令、revision、脏标记、快照发布。
- 不返回原生指针；物品以 category/count/payload 描述。

### 5.3 Persistence

- sidecar `extensionbags.items` v5；v4 迁移。
- load / capture / complete；pending 恢复；失败隔离。
- 只在主线程存档点调用。

### 5.4 ProjectionPort

- install / restore / sync / reflow / 高亮抑制。
- 输入：ExtensionStore 快照 + NativeInventory 容器句柄；输出：原生控件副作用。
- 不拥有数据；失败回退不改变原版选择。

## 6. 用例层

每个用例：校验 → 选择数据源 → 事务 → 复核 → 提交/回滚 → 刷新。

- `move`：四方向矩阵；原生目标走 NativeInventory，扩展目标走 ExtensionStore；跨源由所有权账本协调。
- `view`：enter/exit/select；安装/恢复投影。
- `equip` / `use` / `sell`：扩展袋物品与原生对象互转。
- `set_bag_count`：范围、pending、拖拽、非空校验。
- 用例不直接碰控件；投影通过 ProjectionPort。

## 7. 数据流

### 7.1 写命令

```
ui/http/api
  → BagCommand.post_typed(handler_id, payload) → ticket
  → 主线程 commands_drain()
    → EntryGuard → usecase
      → NativeInventory / ExtensionStore / Transaction
      → ProjectionPort.refresh
    → 结果 JSON → ticket_result
```

### 7.2 只读查询

```
ui/http/api → BagQuery → load_snapshot()
  → 命中：直接序列化
  → 未命中：未就绪响应（不回退 live State）
```

### 7.3 绘制

```
draw hook（主线程）→ 读快照 + mode
  → ProjectionPort.apply_reflow / 高亮
  → 原生 DrawInvenBag
```

### 7.4 存档

```
slot_data capture（主线程）→ ExtensionStore + pending 序列化 → sidecar v5
```

## 8. 迁移映射

| 当前 | 目标 |
|---|---|
| `api/api_impl.cpp` 直接实现 + 锁 | `interface/bag_query.cpp`、`interface/bag_command.cpp` |
| `http/routes.cpp` 参数校验 + `post_typed` | 保留路由，handler 转调 command |
| `public/bridge.cpp` 锁 + live 读 | 读快照；写转 command |
| `runtime/state.h` 全局 `runtime_state()` | `data/extension_store.*` 私有 State |
| `runtime/inventory_access.*` | `data/native_inventory.*` |
| `persistence/persistence.cpp` | `data/persistence.*` |
| `runtime/projection.cpp`、`projection_controller.*` | `data/projection_port.*`（适配器） |
| `usecases/view`、`usecases/move` | 保留并扩为 `usecases/*` |
| `transaction/*` | 保留为 L2 内部事务实现 |
| `runtime/status_snapshot.*`、`bag_state_store.*` | `data/snapshot.*` |

## 9. 不变量

1. 扩展逻辑 ID `6..6+active−1`；原版 0–5 与 C18 页序号不混用。
2. 状态只在主线程写；查询只读快照。
3. 返回原生指针的 API 只在主线程调用。
4. 投影是副作用，失败不得改变原版选择。
5. 事务失败必须回滚或隔离；所有权只在账本内转移。
6. 布局/来源/事务歧义 fail-closed。
7. catalog 是数量/容量/槽数的唯一来源。

## 10. 迁移阶段

| 阶段 | 内容 | 验收 |
|---|---|---|
| A 接口层 | 建立 BagQuery/BagCommand；HTTP/api 改写命令；只读改快照 | 现有 HTTP 行为不变；无线程违规日志 |
| B 数据层 | 拆 NativeInventory / ExtensionStore / Persistence / ProjectionPort | 用例不直接 include `runtime/state.h`；Host 全绿 |
| C 删锁 | 移除 `runtime_state().mtx`；`try_lock` 改显式重入标志 | 200 次混合命令压力；无重入拒绝死循环 |
| D 契约固化 | provider 主线程断言；CI 禁止 L4 include 数据层 | `check_arch` 新规则通过 |

当前进度：A 部分完成（view/move 用例、status/inventory 快照、set_bag_count 命令）；B/C/D 未开始。

## 11. 与当前架构的差异

| 维度 | 当前 | 目标 |
|---|---|---|
| 线程 | 写命令主线程，查询部分读 live State + 锁 | 写命令主线程，查询只读快照，无锁 |
| 接口 | api/http/public 直接调 runtime/usecase | 统一 BagQuery/BagCommand |
| 数据 | 全局 `runtime_state()` + access + persistence 混用 | 四个显式数据组件 |
| 投影 | runtime/projection + public/projection 被多处调用 | ProjectionPort 单一入口 |
| 锁 | 116 个 `mtx` 获取点 + 7 个 try_lock | 0 个状态互斥量；仅 EntryGuard |
| 三袋来源 | ad-hoc 判定 | NativeInventory / ExtensionStore 显式分流 |
