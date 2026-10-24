# extension_bag 实际架构（2026-10）

本文描述重构后的实际结构、线程与数据流，以及与目标架构的差距。目标设计见
`target-architecture.md`，迁移基线见 `migration-baseline.md`。

## 1. 分层与文件

```
L4 接口层
  api/            provider 表、句柄、neutral provider、api_impl（薄委托）
  public/         JNI/C ABI、状态/装备/出售/投影的对外函数
  interface/      bag_query（只读快照）、bag_command（命令分发）
  ui/             原生控件事件 → 用例；页面绘制

L3 用例层
  usecases/       view / item / unequip / move / set_bag_count

L2 组件层
  transaction/    transaction_coordinator、orig_ext、ext_orig、ext_ext、drop
  runtime/        bag_state_store、status_snapshot、projection_controller、
                  native_tab_layout、reentry_lock、entry_guard
  persistence/    slot_data 读写、sidecar JSON、v4→v5 迁移

L1 数据/适配层
  runtime/inventory_access、runtime/item_access   原生袋/物品只读
  model/                                         纯规则与状态结构（bag_catalog、virtual_bag_*）

L0 宿主契约
  contracts/      Game Host ABI、命令/票据、UI/物品/线程适配器
```

依赖只向下：`ui`/`api`/`public` → `usecases`/`interface` → 组件 → `model`/`contracts`。
`model/` 不包含任何 runtime/ui 头。

## 2. 线程模型

- 所有 EB 业务状态只在游戏主线程读写。
- HTTP 写命令：`commands.post_typed` 入队 → 主线程 `commands_drain()` 执行 →
  结果经 `ticket_result.read_result` 在任意线程回取拷贝。
- HTTP 查询：`bag_query` 读取已发布的不可变快照（status、容量、库存片段），
  不触碰 live state。
- 原生控件回调（页签 proc、延迟释放、详情动作）：`TryReentryLock` 判断是否
  由 EB 自身调用栈重入；重入时立即返回，不阻塞。
- 重入策略：`EntryGuard`（`kRejectWrite` / `kReadSnapshot`）+ 线程局部深度；
  `ReentryLock` 只记录嵌套深度，不再有跨线程互斥量。

## 3. 数据流

### 3.1 读

```
HTTP/UI → bag_query → status_snapshot / bag_state_store（不可变快照）
                    ↘ 原始容量字（projection_controller）
```

快照在写路径 `extension_bag_mark_dirty_locked()` 与绘制收尾
`publish_bag_snapshot_if_changed()` 发布；读侧无锁。

### 3.2 写

```
HTTP ──post_typed──▶ commands_drain（主线程）
                       └─ bag_command → usecases → 组件/事务
UI 控件 ─────────────▶ EntryGuard → usecases / lifecycle 事件路由
```

写路径内部流程：

```
校验（catalog/active_count/域）
  → 准备（临时对象、容量字、投影）
  → 原生调用（SaveItemOnEmpty / MoveItem / refresh）
  → 复核（来源/目标/所有权）
  → 提交或阶段回滚（transaction_coordinator）
  → mark_dirty + 快照发布
```

### 3.3 原生事件

`lifecycle` 只做事件翻译与门禁：

- 原版袋/任务袋：走原版语义；
- C18 追加袋：独立 UI 命中，child index 不进入扩展 ID/事务域；
- 扩展页签：`pending_extension_tab` 在 draw-end 提交；
- 拖动：`touch_capture` / `DropSession`，drop 单次消费。

## 4. 数据驱动边界

| 数据 | 来源 | 驱动方式 |
|---|---|---|
| 变体（series/version/能力位） | `game_variant_table.inc` | 构建期扫描 APK 生成 |
| 扩展袋上限/活动数量 | `model/bag_catalog.inc` + `State.bag_count` | 编译期上限 128 + 运行期设置（默认 5） |
| 袋类型容量 | `ITEMSTATICBASE.json` → `bag_catalog.inc` | 生成表 |
| 槽数 16 | 游戏槽上限 | 常量 |
| 物品属性（是否装备/宝石/可用/售价） | 宿主 `read_only.item.*` / `item_facts` | 游戏数据，EB 不复制 |
| 扩展袋状态 | sidecar JSON（v5，含 `bagCount`） | 数据 |
| UI 布局/交互/事务规则 | 代码 | 代码 |

不变量：

- 业务代码不写死扩展袋数量，统一 `virtual_bag::active_extension_bags()`；
- 扩展袋内容通过**代理袋**显示：`current_bag` 只写原版事务袋 0..4，扩展袋号不进入游戏数值域；
- 缩小 `bagCount` 仅当被移除袋为空，且有 pending/拖拽/未提交事务时拒绝；
- 投影只读：绘制/命中/查询不修改业务状态；
- C18 追加袋与原版袋始终以游戏数据为准，扩展袋以 sidecar 为准。

## 5. 关键组件

| 组件 | 职责 | 不负责 |
|---|---|---|
| `bag_state_store` | 轻量快照 + revision | 持久化、事务 |
| `status_snapshot` | 完整状态快照（含 items/effective_counts） | 写状态 |
| `projection_controller` | 容量字、当前袋原始/raw、C18 页计数、来源袋门禁 | 安装/恢复 |
| `native_tab_layout` | 布局解析、reflow、几何缓存、原生袋命中 | 页签交互 |
| `transaction_coordinator` | 事务 ID、pending、阶段回滚、失败隔离 | 方向实现 |
| `usecases/*` | 校验 → 事务 → 刷新 | 控件、JSON |
| `entry_guard` / `reentry_lock` | 主线程与重入策略 | 跨线程同步 |

## 6. 分层完成度

| 项 | 状态 |
|---|---|
| 命令编组、查询快照 | 完成 |
| 用例层承接全部命令 | 完成 |
| L4 访问收口（api/public/ui） | 完成：`interface/state_access.h`，直接 `runtime_state()` 为 0 |
| 聚合头与宏桥 | 完成：`extension_bag_internal.h` 已删除，包含数为 0；orchestrator/tabs_ui 宏桥已移除 |
| 业务互斥量 | 完成：删除 `mtx`，改为 `ReentryLock`/`TryReentryLock` |
| 投影状态 | `projection_controller.*`（L1） |
| 投影安装/恢复 | `runtime/projection.cpp`（L2 投影适配层），与状态端口分离 |
| 所有权 / 拖拽 / 页签 / 生命周期 | 独立文件：`runtime/ownership.*`、`runtime/drag_session.*`、`runtime/tabs.*`、`runtime/lifecycle.*` |
| 事务 | `transaction/transaction_coordinator.*` + 方向实现 |
| 持久化 | `persistence/*`，sidecar v5 |
| 数据驱动 | `model/bag_catalog.*` 生成表 + `State.bag_count` |

不存在“待迁移的安装/恢复”项：安装/恢复属于 L2 投影适配层，状态端口属于 L1，
依赖方向正确。10000 袋的存储/快照/持久化优化见 `optimizations.md`，属于性能演进，
不属于分层债。

## 7. 验证

- Host：CTest 160/160（含 catalog、sidecar 迁移、布局、重入、命令契约）。
- 静态：`check_arch.py` 全绿（层依赖、裸地址、JNI 导出）。
- Android：Debug 构建；v520 真机 smoke：变体识别、页签安装、reflow、
  模块/原生页签切换、bagCount 1..8 边界。

真机验收口径见 `docs/refactor/verification.md`；Host 测试不编译全部 TU，
行为改动必须同时跑 Android Debug 构建。
