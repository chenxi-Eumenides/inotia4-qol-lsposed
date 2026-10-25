# `core.item` 目标架构（v1）

状态：设计稿，已对齐编号模型与迁移边界；实现从 M1 开始。

当前实现见 [`docs/reference/plugins/core.item.md`](../../reference/plugins/core.item.md)；C18 事实见
[`docs/reference/game/c18-native-bag.md`](../../reference/game/c18-native-bag.md)。

## 1. 决策记录

| # | 决策 |
|---|---|
| 1 | 原版袋数固定 6（索引 0..5，含任务袋 5） |
| 2 | 改版（C18）袋数动态计算并缓存，索引从 0 起 |
| 3 | 扩展袋及其他注册袋由接口提供，索引从 0 起 |
| 4 | 每袋容量由接口给出，不设上限 |
| 5 | 袋源改为注册制后，`core.item` 不再依赖扩展背包；依赖方向为其他插件 → `core.item` |
| 6 | 读取改为脏标记驱动的按需重建 + 指纹兜底，取消 15 帧轮询 |
| 7 | 不保留任何旧接口，一次迁移，不做兼容层 |
| 8 | HTTP 列表输出分页；扁平袋号公开，但注册源顺序不保证 |
| 9 | 快照独立 epoch（库存 / 钱包 / 商店各自）
| 10 | 商店本次只拆为独立文件，仍留在 `core.item`；重构完成后另建 `core.shop` 迁移 |
| 11 | 迁移后 `extension_bag` 只负责扩展空间的数据与读写原语；原版/改版袋的读写全部由 `core.item` 承担 |
| 12 | 投影窗口对原版显示槽的写入保留为扩展背包的 UI 副作用例外，不参与物品所有权与存档 |

## 2. 目标与非目标

**目标**

1. 物品域唯一 owner：入库、消耗、使用、移动、出售、装备/镶嵌全部在 `core.item` 收口。
2. 三空间独立编号，`(space_id, bag)` 为稳定身份；扁平袋号仅为派生便利入口。
3. 分层单向依赖，入口 / 用例 / 数据源 / 模型职责不混。
4. 袋数、容量、物品枚举全部经接口获取，插件内没有 `6`、`16`、`11` 这类全局数字假设。
5. 读取成本与请求、变更次数成正比，不再有固定轮询。
6. 与 `extension_bag` 只经契约耦合；其源码本轮不动。

**非目标**

1. 本轮不改 `extension_bag` 源码、sidecar schema、页签与投影；迁移时它只保留扩展空间职责（见决策 11/12）。
2. 不在本任务内新建 `core.shop`（M9 独立任务）。
3. 不改 C18 内存布局探测逻辑（归 Host）。
4. 不引入分布式事务或跨插件锁。

## 3. 编号、计数与容量

### 3.1 空间

| 空间 | `space_id` | 空间内索引 | 袋数来源 | 原生袋号 |
|---|---|---|---|---|
| 原版 `kOriginal` | 0 | 0..5 | 常量 6 | 同索引 |
| 改版 `kC18` | 1 | 0..c−1 | 动态计算并缓存 | `6 + index` |
| 扩展 `kExtension` | 2 | 0..e−1 | 注册源 `count()` | 无 |
| 其他注册源 | ≥3 | 0..n−1 | 注册源 `count()` | 无 |

- `space_id` 在注册时分配，同一会话内稳定，对外不透明；不持久化。
- 任务袋固定为 `(kOriginal, 5)`：可作只读来源，不得作为移动源/目标。

### 3.2 改版袋数

1. 进世界时读袋对象表，取最高非空索引 + 1 得原生袋数，`c = clamp(native_count − 6, 0, 12)`。
2. 索引 18 是 getter 可接受值、不是真实袋，必须排除。
3. 缓存失效：进/出世界、切槽、读档；另由袋对象表指纹发现新袋对象时重算（§8.2）。

### 3.3 容量

1. `BagInfo { capacity, slot_count }` 由各源提供，`core.item` 不设上限、不写死 16。
2. 枚举优先用源提供的 `for_each_item`；无此能力时按 `0..capacity-1` 扫描。
3. 快照只保存已占用槽：内存与物品数成正比，与容量无关。
4. HTTP 输出的 `capacity` 直接取源值；单页输出只受 `limit` 约束。

### 3.4 扁平袋号

1. 按 `原版 → 改版 → 注册源` 顺序连接，索引从 0 起。
2. 原版 0..5、改版紧随其后是确定的；注册源之间的先后不做保证，跨会话可能变化。
3. 扁平号只作便利键：不得用于持久化、分页排序键或跨请求关联。
4. 排序与游标一律使用 `(space_id, bag, slot)`。

## 4. 依赖与注册

### 4.1 当前耦合（待反转）

| 耦合 | 位置 |
|---|---|
| `ExtensionBagApi` 能力调用（识别物品、使用、投影同步） | `plugins/core.item/inventory/inventory_slot.cpp:22,118`、`native_inventory_hook.cpp:740,912,1493` |
| `QolItemPortReadApi::inventory_extension_access` 返回 provider 指针 | `contracts/game_ports/item_port.h:148-149` |
| `inventory.projection`（core.item 提供注册表、EB 注册回调） | `plugins/core.item/merge/plugin.cpp` |
| Host 写端口带 `QolInventoryExtensionProvider*` 参数 | `host/item_inventory_state_write_port.cpp:74,155-184` |

### 4.2 目标依赖

```
extension_bag ──requires──> item.facts / inventory.bag_source ─┐
                                                               ├──> core.item
其他插件      ──requires──> item.facts / inventory.action ─────┘

core.item ──requires──> contracts + Host 原生端口（无插件依赖）
```

- `contracts/` 保持中立；注册协议定义在 contracts，实现由 `core.item` 持有。
- `core.item` 运行期不 include、不查询扩展背包能力。

### 4.3 注册协议

```
struct QolBagSource {
    QolVersion version;
    int32_t    space_id;
    int32_t  (*count)();
    QolResult (*info)(int32_t bag, QolBagInfo* out);
    QolResult (*item_at)(int32_t bag, int32_t slot, QolInventoryItemRef* out);
    QolResult (*for_each_item)(QolBagItemFn fn, void* ctx);
    QolResult (*insert)(const QolInsertRequest*, QolItemHandle* out);
    QolResult (*remove)(QolItemHandle expected, int32_t count, QolRemoveResult* out);
    QolResult (*identify)(QolItemHandle item, QolBagRef* out);
    QolResult (*use)(QolBagRef ref, int32_t slot, QolUseResult* out);
};
```

- 注册/注销只在主线程、插件 activate/deactivate 时进行。
- 注册表代次变化使库存快照失效。
- 重复注册同一 `space_id` 失败，不覆盖既有源。
- 注册源返回的句柄与原生句柄使用同一套 `slot + generation` 复核。

### 4.4 过渡适配器

`extension_bag` 本轮不动。`bags/extension_source_bridge.cpp` 把它现有的
`QolInventoryExtensionProvider` 包装成 `QolBagSource`（`space_id = 2`）。

约束：

1. 只有该文件可以引用扩展背包类型。
2. 迁移完成后整文件删除，其余层不改。
3. `inventory.projection` 已由 `core.item` 提供注册面、扩展背包注册投影回调；不再保留 `inventory.merge_projection` 提供面。

## 5. 分层

```
L4 装配   plugin.cpp / runtime/      描述符、端口绑定、注册装配、生命周期、命令注册、快照发布
L3 入口   http/ api/ hooks/          HTTP、能力面、入口 hook 与合并 wrapper
L2 用例   usecases/                  读、找、入库、消耗、使用、移动、出售、装备、合并
L1 数据   bags/                      BagCatalog、原生源、注册表、过渡适配器、快照、句柄复核
L0 模型   model/                     空间/袋引用、物品身份、数量编码、移动规则、变更集
契约      contracts/                 Host 端口、能力、注册协议
```

依赖规则（硬约束）：

1. 仅 `L4→L3→L2→L1→L0`，同层不反向。
2. `L3` 不直接触 `L1`；`hooks/` 只调用 `L2` 用例。
3. `L0` 只含 POD 与规则，不含端口与 I/O。
4. `bags/` 下各源互不 include，只实现同一接口。
5. 插件外仍只 include `contracts/`（`check_layers` 现有 `plugin_boundary` 规则）。

### 5.1 组件职责

| 组件 | 层 | 职责 | 明确不做 |
|---|---|---|---|
| `plugin.cpp` | L4 | 描述符、资源/能力声明 | 业务逻辑 |
| `runtime/wiring.cpp` | L4 | 端口/SDK 绑定、注册装配、feature、启停注销 | 读写语义 |
| `runtime/command_queue.cpp` | L4 | typed command 注册、主线程执行、结果回写 | 参数校验 |
| `runtime/snapshot_publisher.cpp` | L4 | 脏纪元、重建时机、发布 | 序列化 |
| `http/query_handler.cpp` | L3 | GET 解析、分页、错误码 | 袋号算术 |
| `http/write_handler.cpp` | L3 | POST 校验、构造命令、票据 | 直接调源 |
| `http/response.cpp` | L3 | JSON 拼装、名称注入、`space` 标签 | 决定袋数 |
| `api/facts.cpp` | L3 | `item.facts` 实现 | 绕过用例 |
| `api/state.cpp` | L3 | `item.state` 实现（完整 JSON + 尺寸查询） | 固定缓冲 |
| `api/action.cpp` | L3 | `inventory.action` 实现 | 价格表 |
| `api/slot.cpp` | L3 | `inventory.slot` 实现 | 袋号推导 |
| `api/refresh.cpp` | L3 | `inventory_refresh` 实现 | 重算策略 |
| `api/bag_source.cpp` | L3 | `inventory.bag_source` 注册面 | 具体源实现 |
| `hooks/inventory_hooks.cpp` | L3 | 入口 hook、backup 交还、S2 写侧 | 领域规则 |
| `hooks/merge_hook.cpp` | L3 | 拖拽 wrapper、投影回调分派 | 合并规则 |
| `usecases/read_inventory.cpp` | L2 | 三空间枚举、快照组装 | JSON 文本 |
| `usecases/find_item.cpp` | L2 | 按类别/空间查找 | 写 |
| `usecases/insert_item.cpp` | L2 | 入库目标选择与回滚 | 槽寻址 |
| `usecases/consume_item.cpp` | L2 | 身份复核 → 移除 → 复核 | 编码 |
| `usecases/use_item.cpp` | L2 | 使用事务、确认、骰子 | UI |
| `usecases/move_item.cpp` | L2 | 同空间/跨空间编排、拆分、合并判定 | 源实现 |
| `usecases/sell_item.cpp` | L2 | 报价 → 移除 → 退款回滚 | 价格表 |
| `usecases/equip_item.cpp` | L2 | 装备 / 卸下 / 镶嵌 | 角色状态 |
| `usecases/merge.cpp` | L2 | 合并门槛、同类别判定 | wrapper 安装 |
| `bags/bag_catalog.cpp` | L1 | 袋数、扁平映射、容量、合法性（唯一算术点） | I/O |
| `bags/native_bag_source.cpp` | L1 | 原版/改版读，写经 Host 端口 | 扩展知识 |
| `bags/bag_source_registry.cpp` | L1 | 注册/注销/按空间查找、代次 | 具体实现 |
| `bags/extension_source_bridge.cpp` | L1 | 过渡期把现有扩展背包能力包装成 `QolBagSource` | 扩展状态管理 |
| `bags/snapshot_store.cpp` | L1 | 不可变快照发布/读取 | 业务判断 |
| `bags/item_handle_guard.cpp` | L1 | `slot + generation` 复核 | 重试 |
| `model/*` | L0 | 纯类型与规则 | I/O |

## 6. 目录结构

```
plugins/core.item/
├── plugin.cpp
├── routes.inc
├── runtime/
│   ├── wiring.cpp
│   ├── command_queue.cpp
│   └── snapshot_publisher.cpp
├── http/
│   ├── query_handler.cpp
│   ├── write_handler.cpp
│   ├── response.cpp
│   └── routes.cpp
├── api/
│   ├── facts.cpp
│   ├── state.cpp
│   ├── action.cpp
│   ├── slot.cpp
│   ├── refresh.cpp
│   └── bag_source.cpp
├── hooks/
│   ├── inventory_hooks.cpp
│   ├── hook_backup.cpp
│   └── merge_hook.cpp
├── usecases/
│   ├── read_inventory.cpp
│   ├── find_item.cpp
│   ├── insert_item.cpp
│   ├── consume_item.cpp
│   ├── use_item.cpp
│   ├── move_item.cpp
│   ├── sell_item.cpp
│   ├── equip_item.cpp
│   └── merge.cpp
├── bags/
│   ├── bag_catalog.cpp
│   ├── native_bag_source.cpp
│   ├── bag_source_registry.cpp
│   ├── extension_source_bridge.cpp
│   ├── snapshot_store.cpp
│   └── item_handle_guard.cpp
├── model/
│   ├── bag_space.h
│   ├── bag_ref.h
│   ├── item_identity.h
│   ├── item_count.h
│   ├── move_rules.h
│   └── change_set.h
└── shop/                        # 过渡：M9 移交 core.shop 后删除
    ├── routes.inc
    ├── snapshot.cpp
    ├── query.cpp
    └── buy.cpp
```

约束：单文件 ≤500 行；`plugin.cpp` ≤300 行；现有 6888 行拆分后总量持平。

## 7. 契约

### 7.1 新增类型（`contracts/bag.h`）

```
enum class QolBagSpace : int32_t { kOriginal = 0, kC18 = 1, kExtension = 2 };
struct QolBagRef  { int32_t space_id; int32_t bag; };
struct QolBagInfo { int32_t capacity; int32_t slot_count; };
```

`QolInventoryItemRef` 尾部追加 `int32_t space_id`。

### 7.2 端口

| 端口 | 版本 | 变更 |
|---|---|---|
| `QOL_PORT_ITEM` | 4.0 | `bag_count(space_id)`、`bag_info(QolBagRef)`、`item_at(QolBagRef, slot)`；删除 `inventory_extension_access`、`inventory_json`、旧 `item_ref_at(bag, slot)` |
| `QOL_PORT_ITEM_INVENTORY_STATE_WRITE` | 2.0 | 删除 `QolInventoryExtensionProvider*` 参数；改用 `QolBagRef` + 中性 `QolBagSource` |
| `QOL_PORT_ITEM_MONEY_WRITE` | 1.1 | 无结构变更，仅版本随批次提升 |
| `QOL_PORT_ITEM_DICE_WRITE` | 1.2 | 同上 |
| `QOL_PORT_MOVE_MERGE` | 2.0 | 投影回调改为注册面 |
| `QOL_PORT_INVENTORY_EXTRAS` | 2.0 | `bag_size(QolBagRef)`、`is_empty(QolBagRef)`、按空间遍历 |

### 7.3 能力

| 能力 | 版本 | 说明 |
|---|---|---|
| `item.facts` | 4.0 | 空间化读接口 + `bag_count` / `bag_info` |
| `item.state` | 2.0 | 快照 epoch + 带 `space_id` 的库存 JSON；支持尺寸查询 |
| `inventory.action` | 3.0 | 出售（空间化） |
| `inventory.slot` | 2.0 | 解析 / 查看 / 移除（空间化） |
| `inventory_refresh` | 2.0 | 刷新入口 |
| `move_merge.control` | 2.0 | 合并门控 |
| `inventory.bag_source` | 1.1 | **新增**：袋源注册/注销 + 转移原语（`export_item`/`import_item`/`remove`） |

`inventory.wallet` 1.0 在 M9（`core.shop`）引入，本任务不定义。

### 7.4 删除清单

1. `kOriginalInventoryBagCount`、`kResultBagCount`、`kResultSlotsPerBag`、`slot < 16`、JSON 中的 `"capacity":16`。
2. `kSnapshotIntervalFrames`、`frame_tick`、15 帧定时任务。
3. `inventory_json`、`inventory_extension_access`、`owns_bag`、`inventory_json_fragment`。
4. `core.item` 内 `contracts/extension_bag.h` include 与 `ExtensionBagApi` 调用。
5. `bags/extension_source_bridge.cpp`（扩展背包迁移完成后）。
6. `game/access/inventory_legacy_read.*` 的 owner fallback 路径。
7. `kInventoryCapacity` / `kShopCapacity` 固定输出缓冲。

## 8. 读取模型

### 8.1 为什么原设计是 15 帧

1. HTTP 线程不能读游戏内存：原生 getter 与物品指针解析只能在主线程。
2. 一次完整物化要逐物品调用原生（类别/数量/词条/名称）。
3. 固定 15 帧（60fps 下约 4 次/秒）把成本与请求解耦，但无人读取时仍全量重建。
4. 快照存整段 JSON，容量被固定缓冲写死。

### 8.2 新模型

| 机制 | 触发 | 成本 |
|---|---|---|
| 脏纪元 `epoch++` | 我们的 hook、写命令、进/出世界、切槽 | O(1) |
| 变更后合并重建 | 帧末发现 `epoch != published_epoch`，每帧最多一次 | O(占用物品数) |
| 指纹兜底 | 每 60 帧比较袋对象指针、容量字、金钱、占用槽指针摘要 | O(袋数 × 容量) 纯指针读 |
| 按需刷新 | HTTP 发现快照过旧，投递 `refresh_snapshot`（单飞合并） | 一次主线程重建 |
| 商店快照 | 商店自身 epoch（进商店 / 购买 / 补货） | O(货架数) |

结果：空闲期 0 次全量重建；变更后当帧可见；越界写入最多 1 秒内由指纹发现；无定时器。

### 8.3 快照

```
InventorySnapshot { uint64_t epoch; uint32_t c18_bag_count; vector<BagSnapshot> bags; }
BagSnapshot       { int32_t space_id; int32_t bag; int32_t capacity; int32_t slot_count;
                    vector<SlotRecord> items; }
SlotRecord        { int32_t slot; QolItemInventoryView view; uint32_t count; }
WalletSnapshot    { uint64_t epoch; int64_t money; }
ShopSnapshot      { uint64_t epoch; vector<ShopItemRecord> items; }
```

- 全部为 POD / 值类型，不持原生指针。
- HTTP 线程只读快照并做 JSON 拼装与静态名称注入。

### 8.4 线程

| 通路 | 线程 | 机制 |
|---|---|---|
| 写命令 | 任意（提交）/ 主线程（执行） | `post_typed` → 主线程 → `ticket_result` |
| 只读查询 | 任意 | 不可变快照 |
| hook / UI 回调 | 主线程 | 直接调用用例，`EntryGuard` 防重入 |
| 袋源注册 | 主线程 | activate/deactivate |

## 9. HTTP 契约

### 9.1 分页

| 端点 | 参数 | 说明 |
|---|---|---|
| `GET /api/item/inventory` | `cursor`, `limit`, `space?`, `bag?` | 袋元数据 + 物品流 |
| `GET /api/item/inventory/items` | 同上 | 仅物品流 |
| `GET /api/item/inventory/bag/{bag}/info` | `space?` | 元数据，不分页 |
| `GET /api/item/inventory/bag/{bag}/{slot}` | `space?` | 单槽，不分页 |

规则：

1. `limit` 默认 100，上限 500；越界返回 400。
2. `cursor` 是 `(space_id, bag, slot)` 的不透明编码；非法游标返回 400。
3. 响应形如：

```
{
  "bags":  [{"space":0,"bag":0,"capacity":16,"slot_count":13}],
  "items": [{"space":0,"bag":0,"slot":3,"category":1,"count":1,"name":"治疗药水"}],
  "next_cursor": "...",
  "has_more": true
}
```

4. 不返回总数，避免全量扫描。
5. 排序固定 `(space_id, bag, slot)`；翻页期间袋数或物品变化只影响后续页，允许跳过/重复，文档写明不一致窗口。
6. 服务端只按 `limit` 构造当前页，不分配与容量相关的缓冲。
7. `space` 缺省时 `bag` 按扁平袋号解析；给出 `space` 时按空间内索引解析。

### 9.2 写端点

- 路径与方法不变；body 新增可选 `space` / `to_space`。
- 缺省语义与读端点一致：按扁平袋号解析。
- 结果 `slots` 与 `items` 一律带 `space_id`。
- 任务袋 `(kOriginal, 5)` 作为源/目标时返回既有 `task bag excluded` 语义。

### 9.3 `item.state`

- `kRawInventory` / `kEnrichedInventory` 保留“完整 JSON”输出供 Host 缓存使用。
- 支持尺寸查询：`out == nullptr` 时返回所需字节数，Host 按需分配，删除固定缓冲。
- 输出包含 `space` 字段，字段顺序与现有 golden 对齐后再冻结新 golden。

## 10. 数据流

**读**：HTTP → 快照可用则直接构造当前页；过旧则投递 `refresh_snapshot` → 主线程 `read_inventory` 遍历空间源 → 组装快照 → HTTP 拼 JSON。

**写**：HTTP 解析/校验 → typed command → 主线程用例 → 源原语 → 身份复核 → `ChangeSet` → 结果 JSON → 票据。

**hook**：主线程 → 入口翻译 → 同一用例 → `EntryGuard`；写后 `epoch++`。

**注册**：activate 时 `register_bag_source(space_id, ops)`；deactivate 注销；代次变化使快照失效。

## 11. 失败与安全规则

1. 移动/删除前必须复核 `slot + generation`；复核失败返回 `stale item handle`，不得重试写入。
2. 任务袋 5 不得作为移动源/目标；不得写入 `current_bag`。
3. 插件袋号不得写入游戏 `current_bag`；`current_bag` 只接受原生袋号。
4. 插件袋号不得持久化为扩展袋 ID；扩展袋持久化按自身内部索引。
5. 袋数、容量、注册源缺失时 fail-closed：读返回空段，写返回明确错误，不猜测。
6. 索引 18 不作为真实袋；`space_id` 不持久化。
7. 任何用例失败必须回滚已执行的源操作或进入隔离，不得返回成功。
8. 投影不再写原版显示槽：扩展背包更换方案后不再覆盖 `INVEN_pItem[bag][slot]`，读路径按槽表内容即可判定归属；若未来重新引入槽级投影，必须由投影注册面提供槽位信息，读路径不得逐物品猜测。
9. Host 仲裁未完成前，C18 原生袋号被扩展 provider 认领的写操作必须 fail-closed（HTTP 409），不得按数字猜测路由。

## 12. 迁移阶段

| 阶段 | 内容 | 行为变化 |
|---|---|---|
| M0 | 本设计文档 + API 草案 + 验收卡 | 无 |
| M1 | 纯拆分：按 §6 目录移动现有代码；`shop/` 独立成文件；不改逻辑 | 无 |
| M2 | 契约与 Host：`contracts/bag.h`、端口 4.0/2.0、原生空间计数/容量、C18 布局接入 | 无（旧字段同时保留至 M8） |
| M3 | 模型与骨架：`model/`、`bags/`、`runtime/` 接线；旧实现改为调用新组件 | 无 |
| M4 | 读路径：typed 快照、分页、`item.state` 尺寸查询 | 读接口形状变化 |
| M5 | 写路径：`ChangeSet`、空间化校验、结果带 `space_id` | 写结果形状变化 |
| M6 | hooks 迁入 `hooks/`，逐条对齐 | 无 |
| M7 | C18 空间启用：改版袋数缓存、指纹、任意容量 | 新行为 |
| M8 | 删除 §7.4 全部旧接口与 legacy fallback；基线冻结 | 删除 |
| M9 | 独立任务：新建 `core.shop`，迁移 `shop/` 并删除 | 路由 owner 变化 |

M1–M3、M6 必须“行为不变、测试逐字通过”；M4 起为显式契约迁移。

## 13. 验收

| 阶段 | Host | 静态 | 设备 |
|---|---|---|---|
| M1 | 全量 CTest 与拆分前一致 | `check_layers` / `check_arch` 基线不变 | 不需要 |
| M2 | `host_tests` + 新增袋数/容量/空间用例 | `check_arch`、`check_offsets` | 不需要 |
| M3 | `item_facts_plugin_tests` | `check_layers` | 不需要 |
| M4 | `item_facts_dispatch_tests` + 新 golden | `check_jni`、`check_http_routes` | 读面真机 |
| M5 | `inventory_action_sell_tests`、`inventory_trade_provider_sell_tests`、`stack_limit_plugin_tests` | `check_http_routes`、`check_jni` | 写面真机 |
| M6 | `host_tests`、`stage4_hook_tests` | `check_resources` | monster 冒烟 |
| M7 | 全部 + C18 静态金标 | 全部 | C18L v520 + 原版/monster 回归 |
| M8 | 全量 | 基线冻结 | 冒烟 |
| M9 | 见 `core.shop` 设计 | 全部 | 商店读写 |

缺少设备证据时结论为 `NOT_ACCEPTED`，Host 通过不替代真机。

## 14. 风险

1. 入口 hook 迁移是最高风险项：逐条对齐，单独提交并保留真机冒烟。
2. 库存 JSON 字节级 fixture 是硬门槛：字段顺序冻结前不得调整。
3. 无上限容量必须“按物品枚举”优先，否则大容量袋退化为全槽扫描。
4. 改版袋数缓存必须有指纹兜底，否则后期新增袋对象不可见。
5. 过渡适配器是唯一遗留耦合，必须物理隔离在单文件。
6. 分页游标依赖 `(space_id, bag, slot)`；注册源顺序变化不得影响既有游标语义。
7. Host 未提供序列化/装载/精确写槽原语前，原生↔扩展移动必须 fail-closed，不得用新建物品模拟。

## 15. 物品操作入口

所有物品副作用统一由 `inventory.action` 3.0（`contracts/inventory_action.h`）承载：`move`、`use`、`discard`、`sell`、`equip`、`unequip`、`put_jewel`。`move` 内部判定普通移动、合并与跨介质转移；扩展背包 UI 与自动出售只提供显式 `(space, bag, slot)`，不直接修改扩展袋数据或原生槽表。

详见 `docs/refactor/core-item/item-operations.md`。

provider 读仲裁与 `QolInventoryExtensionProvider` 是最后一项待删除的旧耦合；删除后 Host 只解析原生空间 0/1（含 C18），扩展空间由注册袋源在插件层提供。
