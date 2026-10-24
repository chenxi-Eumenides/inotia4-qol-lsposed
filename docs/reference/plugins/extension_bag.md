# `extension_bag` 插件设计

## 摘要

功能插件：在装备页提供可配置数量的扩展逻辑背包（默认 5，范围 1–8）、原生风格页签、投影式拖放与 sidecar 持久化；同时兼容 C18L 的原生追加袋布局。

- 插件 id `extension_bag`，版本 `0.1.0`（`plugin.cpp`）。
- 对外能力：`extension_bag` v3.1（API 3.0 前缀 + 尾追加 `remove_for_sale`）、`inventory.merge_projection`。
- HTTP：9 条（8 条调试/操作 + `POST /api/extension_bag/set_bag_count`）。
- 持久化：`extensionbags.items` **v5**（v4 自动迁移，默认 5 袋）。
- 线程模型：写命令主线程执行；状态查询读已发布快照；过渡期保留 `runtime_state().mtx`。

除特别说明，路径相对 `module/app/src/main/cpp/plugins/extension_bag/`。

## 1. 分层

| 层 | 目录 | 职责 |
|---|---|---|
| L0 模型 | `model/` | 纯数据与规则：袋/物品/事务/拖拽/序列化；无 UI、无 hook |
| L1 组件 | `runtime/`、`transaction/` | 状态存储、所有权、事务、投影、布局、重入守卫 |
| L2 适配 | `runtime/*_access.*`、`extension_bag_geometry.*` | 原生袋布局、物理槽、物品属性、UI 原语 |
| L3 用例 | `usecases/` | `view`（enter/exit/select）、`move`（四方向） |
| L4 接口 | `api/`、`http/`、`public/`、`ui/` | 参数校验、命令编组、事件翻译、页面适配 |

依赖只向下：`model ← runtime/transaction ← usecases ← api/http/ui/public`。

### 核心组件

| 组件 | 文件 | 职责 |
|---|---|---|
| BagCatalog | `model/bag_catalog.h`、`bag_catalog.inc` | 扩展袋上限 8、活动数量、槽 16、类型容量 |
| BagStateStore | `runtime/bag_state_store.*` | 轻量只读快照（types/capacities/mode/…）+ revision |
| StatusSnapshot | `runtime/status_snapshot.*` | 完整状态快照（含 items），原子发布供查询线程读 |
| InventoryAccess | `runtime/inventory_access.*` | 原生袋布局探测、物理槽读写、当前袋 |
| ItemAccess | `runtime/item_access.*` | 物品属性、句柄、序列化、物化 |
| Ownership | `runtime/ownership.*`、`model/ownership_ledger.h` | 原生对象所有权账本与回收 |
| TransactionCoordinator | `transaction/transaction_coordinator.*` | 事务 ID、pending、阶段回滚、失败隔离 |
| ProjectionController | `runtime/projection_controller.*` | 容量字、当前袋原始/raw、C18 页计数、来源门禁 |
| NativeTabLayout | `runtime/native_tab_layout.*` | C18 布局解析、reflow、命中、锚定 |
| EntryGuard | `runtime/entry_guard.*` | 同线程重入拒绝、主线程断言 |
| Usecases | `usecases/view.*`、`usecases/move.*` | 视图与移动的用例入口 |

## 2. 数据模型

### 2.1 编号域

- 扩展内部索引 `0..active-1`；逻辑 ID `6..6+active-1`。
- 原版事务袋 `0..4`；任务袋 `5` 禁止扩展事务。
- C18 追加袋页签序号（运行时可达 18）是 **UI 命名空间**，只在投影层与扩展逻辑 ID 汇合。

### 2.2 袋定义

```cpp
kMaxExtensionBags = 8;        // 数组上限
kSlotCount = 16;              // 游戏槽上限，固定
active_extension_bags();      // 1..8，来自 State.bag_count
capacity_for_bag_type(type);  // {0,4,8,12,16}，生成表
extension_logical_bag_last(); // 6 + active - 1
```

`State.types/capacities/items` 固定按上限分配；`bag_count` 只控制可见/可用范围，不改变内存布局。

### 2.3 State

- `types[8]`、`capacities[8]`、`items[8][16]`、`bag_count`、`mode`、`original_selected`、`selected`、`inspected`、`info_bag`、`pending`、隔离记录。
- `Item`：category、canonical count、payload 长度与 256B 固定缓冲。
- `mode`：Original / Module / ExitingModule；`ExitingModule` 过渡态用于原版回调收尾。
- 所有权账本：None/ModuleOwned/BorrowedForView/InventoryOwned/Released/ReservedForRelease（容量 128）。
- 句柄保管：Empty/Active/Releasing（容量 1024）。

### 2.4 两类袋的数据来源

| 袋 | 数据源 | 容量来源 | 操作落点 |
|---|---|---|---|
| 原版 0–4 | 游戏 | 游戏容量字 | 原生接口 |
| 任务袋 5 | 游戏 | 游戏 | 禁止 EB 事务 |
| C18 追加袋 6..N | 游戏 | 游戏 | 原生回调 + EB 门禁 |
| 扩展袋 6..6+active−1 | sidecar | catalog | EB 事务 + 物化/回收 |

**物品属性不由插件复制**：是否装备/宝石/强化卷/可用、售价、装备槽均读宿主 `read_only.item.*` / `item_facts.*`。

## 3. 数据驱动

- `bagCount` 是设置项：随存档写入 sidecar 状态 JSON 顶层 `bagCount`。
- 增加袋：新袋 `types=0`、`capacities=0`、items 空；减少袋：被移除袋必须为空，否则拒绝。
- 变更前置条件：无 pending、无活动拖拽、无未提交事务。
- 容量表由 `scripts/maintenance/build_bag_catalog.py` 从 `apk/static-data/json/tables/ITEMSTATICBASE.json` 生成；业务代码不得再写死 5/16/容量数组。
- 原版/C18 袋仍由游戏数据驱动；扩展袋由 sidecar 驱动，二者只在投影层汇合。

## 4. 持久化

- section `extensionbags.items`，版本 **v5**；v4 读取时按默认 5 袋迁移并记录日志。
- `bagCount` 越界、非整数或与 pending 冲突 → 拒绝加载，不静默截断。
- load：缺失清空并放行；版本不符、不可读、payload 损坏阻断会话。
- capture：锁内校验 + 序列化 + 容量检查，记录 transaction/epoch/revision。
- complete：仅身份与 revision 匹配时清 pending/dirty。
- dirty staging：每 revision 至多一次 `request_write`；失败清回 staged，安全点重试。
- 启动发布：load 完成后发布状态快照，供查询线程读取。

## 5. 投影与绘制

- 安装扩展视图：选可用原版宿主袋 → 写容量字 → 同步物品控件 → 记录投影宿主；失败回退不改变原版选择。
- 恢复：走 `restore_module_view_locked()`（含物品区刷新），否则“扩展袋→改版袋”不刷新。
- 绘制只读：绘制 wrapper 读快照/mode，不改业务状态；临时 `current_bag` 的写入/恢复由投影控制器提供。
- 原版与 C18 原生袋物品不复制，仅投影显示。

## 6. C18L 兼容

- 能力位 bit3 `kQolTargetCapabilityC18NativeBagLayoutCandidate`：已知 C18L v507/v508/v512/v513/v514/v520 置位；未知 MD5 仅在运行期特征匹配时置位。
- 袋布局：`native_bag_layout()` 探测原生袋对象表（原版 6 袋 / C18 19 袋），`physical_item_at`、`item_ref_at`、`set_current_bag` 上界同源。
- 页签 reflow：模块列固定在原版列右侧；用完整 72×84 框判断冲突；冲突时只平移 C18 竖向组（保持 5px 间距），底部横排与其他控件不动；歧义 fail-closed。
- 几何按每个 equip root/generation 重新扫描（≤32 子控件，排除模块锚点），不跨 generation 缓存原生指针。
- 命中：C18 原生页签 child index 只作 UI 命中，不进入扩展逻辑 ID/事务域。
- 高亮：C18 module view 临时把 `current_bag` 写成页签总数（无对应页签→原生无高亮），绘制后恢复 raw；不使用 18/19 作为持久哨兵。

## 7. UI 与输入

- 页签宿主：背包装备页、商店、合成器；挂载在游戏袋容器上，所有权归游戏。
- 锚定：每帧重新解析活容器并证明身份（proc/type/user_type/data 快照）；失效只注销记录并重建；禁用/退出时 detach 后删除。
- 页签控件：user type 2、data 16 字节；点击经 proc 投递，主线程消费；同一帧多击做合并。
- 当前页签渲染上限 `kMaxRenderedExtensionBagTabs = 5`；`bagCount > 5` 时只渲染前 5 个并记录 `tab layout pending multi-column`，多列布局待实现。
- 输入：`0x02` 松开确认、`0x04` drop 到袋；moving 保留六条件；吞 release 后锁外 reset moving。
- 详情/出售/合成：详情按钮槽、商店宿主、合成器宿主各自注册资源；扩展物品与原生表现一致。

## 8. 用例与接口

### 8.1 用例

- `usecases/view.*`：`enter_view`、`exit_view`、`select_bag`；校验顺序、锁边界、错误字符串与原 API 一致。
- `usecases/move.*`：四方向转发到事务层；API/页签 UI 不直接调用 transaction。
- `transaction/drop.cpp`：drop 路由属于事务层，不经用例层。

### 8.2 扩展 API（v3.1）

- 状态/索引、对象反查、投影、收编/消耗、交互接管、观测/拖放、移动、容量、jewel、`remove_for_sale`。
- 线程约定：成员仅游戏主线程；`for_each_logical_item` 回调持锁，回调内禁调本表加锁成员；撤销后不得缓存接口/裸指针。
- 中立 provider：`inventory_extension_provider_table()`，消费者 `core.item`、`core.save`。

### 8.3 HTTP（9 条，owner `extension_bag`）

| 方法/路径 | 请求 | 语义 |
|---|---|---|
| `GET /api/debug/extension_bag/status` | 无 | 读快照返回状态（含 `bagCount`/`maxBagCount`） |
| `POST .../enter_view` | `{bag:6..}` | 进入扩展视图 |
| `POST .../exit_view` | 可省略 | 退回原版视图 |
| `POST .../select_bag` | `{bag}` | 扩展视图内切袋 |
| `POST .../unequip` | `{bag}` | 卸下扩展袋 |
| `POST .../click_item` | `{bag,slot}` | 选中/读取物品 |
| `POST .../equip` | `{index,bagType}` | 测试注入装备类型 |
| `POST .../item` | `{index,slot,category,count}` | 测试注入物品 |
| `POST /api/extension_bag/set_bag_count` | `{count:1..8}` | 调整扩展袋数量 |

- POST 只做参数校验并投递 typed command，主线程执行后回写票据；成功返回 `{"ok":true,"ticket":N}`。
- 路由聚合由 `scripts/maintenance/sync_http_routes.py` 生成 `plugins/core.api/routes.inc`；Kotlin 控制器在 `DebugController.kt` 显式映射。

## 9. 线程、锁与重入

- 写命令：HTTP → `commands.post_typed` → `commands_drain()`（主线程）。
- 查询：`/status` 读 `StatusSnapshot`（`shared_ptr` 原子发布）；快照未发布时回退加锁路径并记录一次日志。
- 重入：`EntryGuard` 使用 `thread_local` 深度检测，同线程重入拒绝并计数；不提供跨线程互斥。
- 过渡期保留 `runtime_state().mtx`：部分 provider 只读路径与 public/JNI 入口仍读 live State。
- `try_lock` 同时承担递归/竞争保护（逻辑扫描、页签 proc 等），不能直接用 EntryGuard 替代。

### 删除锁的前置条件（P6）

1. `virtual_bag_inventory_bags_json()` 快照化或主线程投递（当前遍历 live State 与物化对象）。
2. `bag_capacity`/status fallback 分支改为拒绝或快照。
3. provider 只读回调的“仅主线程”从注释约定变为逐链证明/断言。
4. public/JNI 入口覆盖 EntryGuard 与主线程投递。
5. 保留 `for_each_logical_item`、tab proc 的 busy/重入拒绝语义。

完成前保留互斥量是正确取舍。

## 10. 失败与恢复

- 每个方向：准备 → 原生调用 → 复核 → 提交/回滚。
- pending 记录在 State 与 sidecar；启动恢复按方向/源目标一致性判定 complete/rollback/隔离。
- 失败隔离保留原始 payload 与事务上下文，记录原因，不猜测内容。
- 原生调用失败必须回收临时对象或登记所有权，避免复制/泄漏。
- 布局/来源/事务歧义一律 fail-closed，不移动控件、不写选择。

## 11. 已知限制

| 限制 | 影响 | 后续 |
|---|---|---|
| `bagCount > 5` 只渲染前 5 个页签 | 6–8 袋需 API 访问 | 多列页签布局 + reflow 区域加宽 |
| provider/JNI 仍有 live State 读 | 不能删除 `mtx` | 完成 §9 前置条件 |
| `recover_pending_transaction_locked` 仍在 orchestrator | 与投影/对象加载耦合 | 与 OwnershipManager 一起迁移 |
| C18 高亮抑制依赖“页签总数” | 依赖原生无该索引 | 新版本需复核页签计数 |
| 详情/合成残留（双高亮、闪原版袋） | 视觉瑕疵 | 按真机反馈收敛 |

## 12. 关键不变量

1. 扩展逻辑 ID `6..6+active−1`；原版事务袋固定 `0..4`、任务袋 `5`，命名空间不混用。
2. 绘制、命中、查询不写业务状态；状态只由用例/事务层修改。
3. 原生对象所有权只在账本内转移；事务失败必须 isolate 或回收。
4. 布局、来源、事务歧义 fail-closed。
5. 主线程写 + EntryGuard；跨线程读只允许快照。
6. 容量/上限/槽数只从 catalog 取，业务代码不写死。

## 13. 测试与验收

- Host：`module/app/src/main/cpp/tests`，CTest 159 项；覆盖模型、catalog、快照、事务、布局、HTTP 参数校验。
- 静态：`scripts/maintenance/check_arch.py`、`check_http_routes.py`。
- 构建：`scripts/build-debug.sh`（唯一 Debug 入口）。
- 真机：v513/v514/v520；页签挂载、reflow（`moved=6 delta=75`）、点击切袋、原生竖列退出、`set_bag_count` 增删。
- 详细验收口径见 `docs/refactor/verification.md`；重构状态见 `docs/refactor/extension-bag/`。
