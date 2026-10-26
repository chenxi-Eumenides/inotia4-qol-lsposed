# `core.item` 对外契约

## 定位与职责边界

`core.item` 是物品与库存领域 owner，提供物品事实、袋源注册、库存动作、库存状态查询，以及 inventory/shop HTTP 接口。它是 plugin authors、HTTP clients、扩展背包（EB）与 autosell 等消费者访问库存的统一边界。

| `core.item` 负责 | 不负责 |
|---|---|
| 原版/C18/注册袋源的库存读取、对外物品事实、动作编排与快照查询。 | 扩展袋 sidecar、袋数据、页签和 UI；EB 自己维护数据并注册袋源。 |
| 库存与 shop 的查询/写入接口、袋源注册，以及投影回调注册面。 | HTTP listener、传输与 OP 权限门禁；由 Host/API 层承担。 |
| 跨空间物品动作的统一入口。 | 原生游戏地址解释与内存适配；由 Host/game adapter 提供。 |

## 术语与身份

### 三个空间

| 空间 | `space_id` | 空间内 bag 索引 | 对外含义 |
|---|---:|---|---|
| 原版 | 0 | `0..5` | 原版六个袋；`(0,5)` 是只读任务袋。 |
| 改版袋 | 1 | `0..count-1` | 改版客户端自己的追加袋空间；袋索引只在本空间有效。C18 是当前唯一在该空间提供袋的改版；其他改版将来有追加袋时也使用该空间（同时只存在一个改版，不会冲突）。 |
| 注册袋源 | `>=2` | `0..count()-1` | 插件通过 `QolBagSource` 提供的独立空间。 |

袋身份由 `{space_id, bag}` 共同确定；相同 bag 数字不表示同一袋。注册源当前使用哪个 `space_id` 由注册方提供，注册顺序不是稳定身份。`QolBagInfo.capacity` 由袋源报告，`core.item` 不规定注册袋统一容量上限。

### 引用、句柄与扁平袋号

| 名称 | 结构/语义 |
|---|---|
| `QolActionRef` | `{space_id, bag, slot}`，显式指向调用时刻的物品位置；动作执行时会复核该位置对应的物品身份。 |
| `QolItemHandle` | `{slot, generation}` 组成的进程内身份，不含游戏地址；任一字段为 0 时无效。它不能替代空间、袋、槽定位，也不能跨进程持久化。 |
| 扁平袋号 | 为便利查询/写入，将原版 → C18 → 注册源连接起来的索引。可省略 `space` 时使用；注册源顺序可能变化，因此不得持久化或作为稳定身份/分页游标。 |

涉及袋位置的动作若省略 `space`，`bag` 按扁平袋号解析；显式提供 `space` 后，`bag` 是该空间内索引。移动目标同理使用 `to_space` / `to_bag`。

## 对外能力

| Capability | 版本 | 用途 |
|---|---:|---|
| `item.facts` | 3.6 | 物品类别、库存 view/count/编码、装备/宝石/禁售和报价等事实。 |
| `item.state` | 1.1 | raw/enriched 库存状态 JSON 与尺寸查询。 |
| `inventory.bag_source` | 1.1 | 注册/注销外部袋源，并提供跨空间转移所需的袋源原语。 |
| `inventory_refresh` | 1.2 | 原版库存区域刷新；`update_shortcut_category(category)` 按类别重算快捷栏数量（含注册袋源物品）。 |
| `inventory.action` | 3.0 | 移动、使用、丢弃、出售、装备、卸下和镶嵌。 |
| `inventory.slot` | 1.0 | resolve/view/remove 槽位查询面。 |
| `move_merge.control` | 1.0 | 报告拖拽合并是否可用。 |
| `inventory.projection` | 1.0 | 注册/注销投影回调；投影显示和数据仍归注册方。 |

## Feature toggle

| Toggle | 初始状态与可见行为 |
|---|---|
| `core.item.provider` | 默认开启的隐藏开关；该开关不单独改变物品领域服务行为，服务随 `core.item` 插件启停。 |
| `move_merge.drag_merge` | 显示名称「拖动合并」、描述「允许合并同种可堆叠物品」，默认开启。关闭时不执行拖动自动合并；其他库存动作不由此开关关闭。 |

## `QolBagSource` v1.1 公共契约

`inventory.bag_source` 由 `core.item` 提供。每个来源占用独立 `space_id>=2`；袋索引从 0 开始。来源提供自己的数据和写入原语，`core.item` 不接管其 sidecar 数据。

### 字段与注册表 API

| 字段 | 类型/要求 | 契约 |
|---|---|---|
| `version` | `QolVersion` | major 为 1，minor 至少为 1。 |
| `space_id` | `int32_t` | 必须 `>=2`，且不能与已注册来源冲突。 |
| `count` | `int32_t (*)()` | 必需；返回本空间当前袋数，不能为负。 |
| `info` | `QolResult (*)(bag, QolBagInfo*)` | 必需；返回袋容量 `capacity` 与占用槽数 `slot_count`。 |
| `item_at` | `QolResult (*)(bag, slot, QolInventoryItemRef*)` | 必需；读取指定槽，空槽返回 `kQolErrNotFound`。 |
| `for_each_item` | `QolResult (*)(QolInventoryBagSlotFn, void*)` | 必需；枚举占用物品。 |
| `export_item` | `QolResult (*)(handle, count, QolBagTransfer*)` | 必需；只读导出供转移的 payload。 |
| `import_item` | `QolResult (*)(transfer, preferred_bag, preferred_slot, result*)` | 必需；导入 payload 并返回实际位置、句柄和数量。 |
| `remove` | `QolResult (*)(expected_handle, count, result*)` | 必需；按预期句柄扣减或移除。 |
| `identify` | `QolResult (*)(handle, ref*, slot*)` | 可选；将来源句柄定位到袋与槽。 |
| `use` | `QolResult (*)(ref, slot)` | 可选；执行来源自己的使用结算。 |

| 注册约束 | 语义 |
|---|---|
| API | `QolBagSourceRegistryApi { struct_size, register_source, unregister_source }` 随 `inventory.bag_source` 1.1 提供。 |
| 时机与线程 | 注册/注销只在游戏主线程调用；来源激活时注册，停用时注销。所有来源回调也只在游戏主线程调用。 |
| 校验 | 七个必需回调均须存在；`identify`、`use` 可缺省。版本不匹配、空间号小于 2、重复空间或缺少必需回调均拒绝，不覆盖已注册来源。 |
| 生命周期 | 注册期间来源结构与函数表必须有效；注销后不再调用来源回调。 |
| 快照可见性 | 注册/注销改变来源集合；后续库存读取应反映变化。 |

### 回调语义与错误

| 回调 | 成功语义 | 失败语义 |
|---|---|---|
| `count()` | 返回非负袋数。 | 来源未就绪时返回 0，或由上层 readiness 报告不可用；不得伪造袋数。 |
| `info` | `capacity` 非负；`slot_count` 为占用槽数；容量 0 合法。 | 越界/无效参数为 `kQolErrInvalidArgument`；来源不可用为 `kQolErrNotReady`。 |
| `item_at` | 填入空间、袋、槽、有效句柄及物品事实。 | 空槽/不存在为 `kQolErrNotFound`；非法 ref 为 `kQolErrInvalidArgument`。 |
| `for_each_item` | 对占用物品调用回调；回调返回非 0 可停止枚举。 | 单项物化失败不得伪装成成功物品。 |
| `export_item` | 不修改来源；payload 由来源定义且保持不透明，`count` 是请求转移数量。 | 过期/非本源句柄为 `kQolErrNotFound`；数量非法或超过堆量为 `kQolErrInvalidArgument`；不可用为 `kQolErrNotReady`。 |
| `import_item` | 成功返回实际 `{ref, item, count}`；导入数量规范化为 transfer 数量。 | 非法参数/类别/数量/长度为 `kQolErrInvalidArgument`；指定槽占用为 `kQolErrConflict`；失败不得留下部分对象或持久化改动。 |
| `remove` | 部分移除保留有效句柄；整堆移除使句柄失效。 | 过期/非本源句柄为 `kQolErrNotFound`；数量非法为 `kQolErrInvalidArgument`；失败不得部分扣减。 |
| `identify` / `use` | 分别定位物品、执行来源结算。 | 可选回调缺失时对应操作为 `kQolErrUnsupported`。 |

来源数量、句柄及事务原子性由来源 callback 实现。不能跨线程或跨帧保存句柄以外的游戏裸指针。其他常见错误包括 `kQolErrBusy`（处理中）、`kQolErrBlocked`（业务拒绝/不可执行）与 `kQolErrInternal`（内部或回滚不一致）。

`import_item` 的目标选择参数：`preferred_bag=-1, preferred_slot=-1` 表示来源自行选择；只指定 bag 时在该袋选择槽；bag 与 slot 都非负时必须使用指定槽，占用返回 `kQolErrConflict`。ABI 不要求在明确槽请求时自动合并。

### `QolBagTransfer`

```cpp
struct QolBagTransfer {
    int32_t category;
    int32_t count;
    uint32_t payload_size;
    uint8_t payload[256];
};
```

payload 固定容量 256 字节，有效长度为 `1..255`，不得动态扩容。payload 是来源定义的序列化状态；`core.item` 只透传、不解释或改写。部分堆叠时 `count` 表示本次数量，目标须将 payload 中数量规范化为该值；接收端应在回调期间复制需保留的数据。

## `inventory.action` v3.0

所有物品副作用由该能力统一提供。全部方法只在游戏主线程调用；调用者传入显式 `QolActionRef`，不延迟解析当前页签。

| 方法 | 输入 | 结果 |
|---|---|---|
| `move(from,count,to)` | 源、目标位置与正数量 | `InventoryMoveOutcome {status, moved_count, merged, target, target_item}`。 |
| `use(ref)` | 物品位置 | `InventoryUseOutcome {status, consumed_count}`。 |
| `discard(ref,count)` | 物品位置与数量 | `InventoryUseOutcome {status, consumed_count}`。 |
| `sell(ref,expected_item,count,source)` | 预期句柄、数量、来源标签 | `InventorySellResult {status, price, category, count}`；出售还依赖世界状态事实。 |
| `equip(role,ref,equip_slot)` | 角色、物品位置、装备槽 | `InventoryUseOutcome`；当前仅支持原生物品。 |
| `unequip(role,equip_slot)` | 角色与装备槽 | `InventoryUseOutcome`。 |
| `put_jewel(role,jewel_ref,equip_slot)` | 宝石位置与装备槽 | `InventoryUseOutcome`；当前不支持注册袋宝石。 |

`InventoryActionStatus`：`kOk`、`kNotInWorld`、`kInvalidRef`、`kStale`、`kEmpty`、`kTaskBagExcluded`、`kConflict`、`kFull`、`kUnsupported`、`kInternal`、`kCreditFailed`、`kRollbackFailed`、`kExtensionSaleUnavailable`。

`InventorySellStatus`：`kOk`、`kNotInWorld`、`kTaskBagExcluded`、`kEmptySlot`、`kNoItemHandle`、`kStale`、`kSymbolsUnresolved`、`kNoSell`、`kPriceInvalid`、`kCreditFailed`、`kRemoveFailed`、`kRollbackFailed`、`kExtensionSaleUnavailable`。

动作执行前按位置重读并复核句柄；过期身份返回 `kStale`。失败使用 action/sell status 表达，不以 HTTP 状态码代替异步业务结果。

## 移动、合并与转移的外部行为

| 情形 | 可依赖的行为 |
|---|---|
| 同一空间移动 | `move` 按显式源/目标与数量执行；结果报告实际 `moved_count`、是否并入目标及目标身份。 |
| 跨空间移动 | 目标成功接收前不得丢弃来源物品；来源移除失败时尝试补偿目标。补偿失败不得报告成功。 |
| 合并 | 合并要求物品类别相同且目标堆不超过可用数量上限；动作结果通过 `merged` 与目标信息表达。 |
| 拖拽自动合并 | 仅在 `move_merge.drag_merge` 开启且 `move_merge.control` 报告可用时启用；源堆必须能整体放入目标剩余容量，拖拽 gate 不拆分源堆做部分合并。 |
| 任务袋 | `(space=0, bag=5)` 可读取，但不能作为 `inventory.action` 动作或跨空间转移的源/目标。 |
| 来源专属动作 | 来源缺少 `identify` 或 `use` 时对应能力返回 `kQolErrUnsupported`；装备限制见上方方法表。 |

use/discard 由库存动作结算；出售的加钱失败会尝试回滚物品。成功写入后会请求库存刷新。

## HTTP surface

常规路由共 17 条：15 条 inventory 与 2 条 shop。另有 4 条 OP-gated 路由。常规路由要求有效 lease，不要求 OP；OP 路由同时要求有效 lease 与 OP 权限。

### 常规路由：15 inventory + 2 shop

| 方法 | 路径 | 用途 |
|---|---|---|
| GET | `/api/item/inventory` | 袋元数据与分页物品列表。 |
| GET | `/api/item/inventory/money` | 当前金钱。 |
| GET | `/api/item/inventory/items` | 仅分页物品列表。 |
| GET | `/api/item/inventory/bag/{bag}/info` | 单袋容量与占用槽数。 |
| GET | `/api/item/inventory/bag/{bag}/{slot}` | 查询单槽；有效范围内空槽返回 `null`。 |
| POST | `/api/item/inventory/use_item` | 使用物品。 |
| POST | `/api/item/inventory/accept_dice` | 接受待结算骰子结果。 |
| POST | `/api/item/inventory/reject_dice` | 拒绝待结算骰子结果。 |
| POST | `/api/item/inventory/discard_item` | 丢弃物品或指定数量。 |
| POST | `/api/item/inventory/sell_item` | 出售物品。 |
| POST | `/api/item/inventory/move_item` | 移动物品到袋/槽。 |
| POST | `/api/item/inventory/{role}/put_jewel` | 将宝石放入角色装备槽。 |
| POST | `/api/item/inventory/{role}/enchant` | 角色装备附魔写命令。 |
| POST | `/api/item/inventory/{role}/equip_item` | 装备物品或按类别执行装备写命令。 |
| POST | `/api/item/inventory/{role}/unequip_item` | 从角色装备槽卸下物品。 |
| GET | `/api/item/shop/items` | 查询当前商店货架。 |
| POST | `/api/item/shop/buy_item` | 购买商店物品。 |

`/api/item/inventory/{role}/enchant` 是独立写命令，不属于 `inventory.action` 方法表。

### OP-gated 路由：4 条

| 方法 | 路径 | 用途/当前结果 |
|---|---|---|
| POST | `/api/op/inventory/money` | OP 设置金钱。 |
| POST | `/api/op/inventory/add` | OP 添加物品。 |
| POST | `/api/op/inventory/set-slot` | 当前未实现，返回 HTTP 501。 |
| POST | `/api/op/inventory/set-equip` | 当前未实现，返回 HTTP 501。 |

### 查询参数与分页

| 参数 | 语义 |
|---|---|
| `space` | 可选；存在时 `bag` 是空间内索引。只指定 `space` 时筛选该空间。 |
| `bag` | 可选；未指定 `space` 时按扁平袋号解析。无效袋返回 404。 |
| `to_space` / `to_bag` | 移动目标的空间与袋；省略 `to_space` 时 `to_bag` 按扁平袋号解析。 |
| `cursor` | 列表游标格式为 `space:bag:slot`，按三元组比较；空值等同未提供，格式错误返回 400。 |
| `limit` | 默认 100，允许 `1..500`；非法值返回 400。 |

列表按 `(space,bag,slot)` 排序，响应提供 `next_cursor` 与 `has_more`，不提供总数。分页期间库存或来源顺序变化可能造成后续页跳过或重复；游标和扁平袋号不是持久身份。袋槽查询按实际 `slot` 字段匹配，不跳过空格；越界槽为 404。

### HTTP 错误与写入结果

| 请求 | 错误/结果语义 |
|---|---|
| GET | 参数、游标或 limit 错误为 400；袋/槽不存在为 404；空槽 body 为 `null`；响应拼装失败为 500；快照不可读为 503。 |
| POST | 请求体或字段错误为 400；命令 port 或队列不可用为 503；成功接收后返回 ticket，最终业务结果由 ticket result 查询。 |
| OP POST | 权限由 OP gate 检查；当前未实现的两个 endpoint 返回 501。 |

## 线程模型与对外不变量

1. `inventory.action` 全部在游戏主线程调用；袋源注册、注销和全部来源回调也在游戏主线程执行。
2. HTTP GET 只读取已发布库存快照；不会在请求线程遍历原生库存对象。HTTP POST 在请求线程校验并投递写命令，不直接改物品。
3. `QolActionRef` 必须携带显式 `{space,bag,slot}`；动作执行前重新读取并校验物品句柄。旧句柄过期后不得复用。
4. `(0,5)` 不作为动作或转移源/目标；注册来源的 `space_id` 不得与已有来源重复。
5. 转移目标完整接收前不移除来源；来源移除失败须尝试目标补偿，补偿失败返回失败而非成功。
6. 缺少袋数、容量、来源或写入能力时 fail-closed；不得按相同 bag 数字猜测归属或伪造对象替代来源 payload。
7. 写成功后请求库存刷新；快照版本、句柄和袋位置各自承担不同语义，不能相互替代。

实现目录、资源声明、快照发布、恢复记录与已知实现风险见[内部实现参考](core.item-internals.md)。
