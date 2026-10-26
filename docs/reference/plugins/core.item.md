# `core.item` 插件

`core.item` 是物品与库存领域 owner：实现物品事实、库存状态、袋源注册、动作接口、库存 HTTP 查询/写入，以及原生库存入口与拖拽合并 wrapper；descriptor、能力和端口由 `plugins/core.item/plugin.cpp` 声明，领域代码位于 `module/app/src/main/cpp/plugins/core.item/`。

## 职责边界

| `core.item` 拥有 | 不拥有 |
|---|---|
| 物品事实与状态投影、原生空间读取、袋源注册表、库存动作编排、快照、库存入口 hook、拖拽合并 wrapper、当前内置的 shop 路由/快照。见 `plugin.cpp`、`usecases/`、`hooks/`、`shop/`。 | 扩展袋 sidecar 数据、页签和 UI；`extension_bag` 实现 `QolBagSource` 并注册投影回调。见 `plugins/extension_bag/api/bag_source.cpp`、`plugins/extension_bag/assembly/projection_registration.cpp`。 |
| `inventory.action` 作为跨插件库存动作契约；autosell 经该能力请求出售。见 `contracts/inventory_action.h`、`plugins/autosell/scan.cpp`。 | 原生游戏内存原语及适配：由 Game Host / Game Adapter 端口提供；角色与世界事实由对应领域 owner 提供。见 `contracts/game_ports/`、`game/ops/item_inventory_state_write.cpp`。 |
| inventory/shop handler 与插件路由描述。 | HTTP 监听、OP 权限门禁和传输由 Host/Kotlin 承担；`core.shop` 尚未拆出，shop 当前仍在 `plugins/core.item/shop/`。 |

## 术语与身份

| 概念 | 当前契约 |
|---|---|
| 原版空间 `space=0` | 空间内袋索引 0..5；任务袋是 `(0,5)`，可读但不可作为动作源/目标。见 `contracts/bag.h`、`usecases/item_action.cpp`。 |
| C18 空间 `space=1` | 袋索引从 0 开始；袋数由 ITEM port 查询，读取和写入走原生端口。见 `usecases/read_inventory.cpp`、`contracts/game_ports/item_port.h`。 |
| 注册袋源空间 `space>=2` | 每个 `QolBagSource` 有自己的袋索引；当前 `extension_bag` 使用空间 2。注册需要 `count/info/item_at/for_each_item/export_item/import_item/remove`，`identify/use` 可选。见 `contracts/bag.h`、`api/bag_source.cpp`。 |
| `QolActionRef` | 显式 `{space_id, bag, slot}`；调用方传入时确定，不延迟按当前页签解析。操作前重新读取并核对物品。见 `contracts/inventory_action.h`、`usecases/item_action.cpp`。 |
| `QolItemHandle` | `{slot, generation}`；任一字段为 0 都无效，不包含游戏地址。它用于身份复核，不替代 `{space, bag, slot}` 位置。见 `contracts/item_handle.h`、`bags/item_handle_guard.cpp`。 |
| 扁平袋号 | 为兼容便利入口而由空间/袋映射出的编号；注册源排序不构成稳定身份，不得持久化或用作跨请求关联。见 `model/bag_catalog.cpp`、`http/write_handler.cpp`。 |

## 分层与目录

| 目录/组件 | 职责 | 代码位置 |
|---|---|---|
| 插件装配 | descriptor、capability/port/resource/feature 声明及生命周期入口 | `plugins/core.item/plugin.cpp`、`plugins/core.item/runtime/lifecycle.cpp`、`runtime/context.h` |
| `runtime/` | Host SDK/端口上下文、安装/启停、帧任务、启动恢复 | `runtime/context.cpp`、`runtime/lifecycle.cpp` |
| `api/` | `item.facts`、`item.state`、袋源注册、投影回调注册面 | `api/facts.cpp`、`api/state.cpp`、`api/bag_source.cpp`、`api/projection.cpp` |
| `bags/` | 袋源注册/查找、原生与注册源传输、快照存取、句柄校验 | `bags/bag_source_registry.cpp`、`bags/native_transport.cpp`、`bags/source_transport.cpp`、`bags/snapshot_store.cpp`、`bags/item_handle_guard.cpp` |
| `model/` | 袋空间/映射、物品身份、变更集、移动规则 | `model/bag_catalog.cpp`、`model/item_identity.h`、`model/change_set.cpp`、`model/move_rules.cpp` |
| `usecases/` | 快照读取、动作、移动/跨空间转移、出售与 journal | `usecases/read_inventory.cpp`、`usecases/item_action.cpp`、`usecases/move_item.cpp`、`usecases/sell_item.cpp`、`usecases/transfer_journal.cpp` |
| `hooks/` | 原生库存入口 wrapper、backup 绑定、`MOVE_MERGE_GOT` wrapper | `hooks/inventory_hooks.cpp`、`hooks/merge_hook.cpp` |
| `http/` | 查询分页、请求解析、命令投递、响应拼装与路由装配 | `http/query_handler.cpp`、`http/write_handler.cpp`、`http/routes.cpp` |
| `shop/` | 目前仍随 `core.item` 提供商店查询、购买和快照 | `shop/routes.inc`、`shop/query.cpp`、`shop/buy.cpp`、`shop/snapshot.cpp` |
| `inventory/` | FindItem 查找流程的 POC，不是主要库存读写入口 | `inventory/inventory_find_item_poc.cpp` |

插件外只通过 `contracts/` 消费公开 ABI；原生访问由端口实现，不能把 Host 原语当作插件内业务规则。深层设计见[目标架构](../../refactor/core-item/architecture.md)、[袋源契约](../../refactor/core-item/bag-source-contract.md)与[物品操作契约](../../refactor/core-item/item-operations.md)。

## 插件声明与公开契约

插件 id/version 为 `core.item` / `0.1.0`，`deactivate_allowed=false`；声明来自 `runtime/context.h`、`plugin.cpp`。当前插件声明不设 required capability，以下 optional requires 共 4 项。

### Provides：8 项

| 能力 | 版本 | 责任 |
|---|---:|---|
| `item.facts` | 3.6 | 物品类别、库存视图、计数、编码、装备/宝石/禁售与报价等事实。`contracts/item_facts.h`、`api/facts.cpp`。 |
| `item.state` | 1.1 | raw/enriched 库存状态 JSON 与尺寸查询。`contracts/item_state.h`、`api/state.cpp`。 |
| `inventory.bag_source` | 1.1 | 注册/注销外部袋源；袋源是读面和转移原语的实现者。`contracts/bag.h`、`api/bag_source.cpp`。 |
| `inventory_refresh` | 1.1 | 调用原版库存区域刷新与快捷栏数量更新。契约 id 在 `contracts/inventory_refresh.h` 中使用下划线。 |
| `inventory.action` | 3.0 | 统一库存动作契约：移动、使用、丢弃、出售、装备、卸下、镶嵌。`contracts/inventory_action.h`、`usecases/item_action.cpp`。 |
| `inventory.slot` | 1.0 | 槽位 resolve/view/remove 查询面。`contracts/inventory_slot.h`、`usecases/item_slot.cpp`。 |
| `move_merge.control` | 1.0 | 报告 feature 与 wrapper 是否均允许拖拽合并。`contracts/move_merge.h`、`hooks/merge_hook.cpp`。 |
| `inventory.projection` | 1.0 | 投影回调注册/注销；扩展袋投影由注册方实现。`contracts/inventory_projection.h`、`api/projection.cpp`。 |

### Ports：6 项

| Port | 版本 | 用途 |
|---|---:|---|
| `QOL_PORT_ITEM` | 3.8 | 原生物品事实与库存读面。 |
| `QOL_PORT_ITEM_MONEY_WRITE` | 1.0 | 金钱写入。 |
| `QOL_PORT_ITEM_DICE_WRITE` | 1.1 | 骰子接受/拒绝写入。 |
| `QOL_PORT_ITEM_INVENTORY_STATE_WRITE` | 2.2 | 原生库存动作/物品/装备/商店写入。 |
| `QOL_PORT_MOVE_MERGE` | 1.2 | 原生移动、当前袋与句柄校验。 |
| `QOL_PORT_CHARACTER_EXTRAS` | 1.2 | 合并时的材料应用判定。 |

Port id/顺序在 `plugins/core.item/plugin.cpp`；版本常量在 `contracts/game_ports/ports.h`、`item_money_write_port.h`、`item_dice_write_port.h`、`move_merge_port.h`。

### Optional requires：4 项

| 能力 | 版本 | 使用面 |
|---|---:|---|
| `character.state` | 1.0 | 可选角色状态消费者。 |
| `http.registry` | 1.1 | HTTP 注册能力。 |
| `http.dispatch` | 1.1 | HTTP 分发能力。 |
| `world.facts` | 2.0 | 出售时检查世界状态；缺失时出售以 `kSymbolsUnresolved` 失败，不能继续扣款/移除。 |

声明见 `plugins/core.item/plugin.cpp`；可选能力不是安装所需项，具体调用见 `usecases/sell_item.cpp` 与 `http/routes.cpp`。

### Features 与资源

| 项目 | 当前声明 |
|---|---|
| `core.item.provider` | 默认开启；Hidden toggle，不执行额外 toggle 回调。 |
| `move_merge.drag_merge` | 默认开启；Basic optimization toggle，只切换 wrapper 内 gate。 |
| 库存资源 | `kInventoryHookClaims` 共 27 项：24 个必需 fail-closed 入口 hook，另有 3 个快捷栏诊断用 optional fail-soft hook。`hooks/inventory_hooks.cpp`。 |
| 合并资源 | 1 个 optional fail-closed `ptrslot:MOVE_MERGE_GOT`；未就绪时 `move_merge.control.enabled()` 为 false。`hooks/merge_hook.cpp`。 |
| 合计 | 28 项资源；`runtime/lifecycle.cpp` 合并并发布两组 claim。 |

## 读取模型与状态投影

`read_inventory()` 在游戏线程读取 ITEM port 的空间 0/1，再按注册源逐个读取空间 2 及更高空间；每袋取 `info`，优先用 `for_each_space_slot`/源的 `for_each_item`，不支持时按容量调用 `item_at`，只把有效占用槽物化为快照记录。完成后写入金钱并通过 `SnapshotStore::publish()` 原子发布不可变快照；HTTP GET 从 `SnapshotStore::read()` 取共享快照，不直接遍历游戏对象。见 `usecases/read_inventory.cpp`、`bags/snapshot_store.cpp`、`http/query_handler.cpp`。

当前周期兜底由 `runtime/lifecycle.cpp` 每 60 帧调度 `frame_tick()`；`api/state.cpp` 的 `frame_tick()` 调用 `read_inventory()`、更新 item state 和 shop snapshot。这是定时读取/重建，不是库存指纹比较。`request_inventory_refresh()` 先 bump epoch，再立即调用 `read_inventory()`；成功重建也 bump epoch 后发布。写操作完成后通过该入口刷新，见 `usecases/read_inventory.cpp`、`usecases/item_action.cpp`、`http/write_results.cpp`。

`item.state` 保持 legacy raw inventory 外形：原版袋 0..5；C18 以扁平展示号 6 起追加。注册袋段由 `QolBagSource` 的 `count/info/item_at` 结构化生成，包含空间和袋索引；不把其拼为来源方 JSON 字符片段。enriched 输出由 item owner 在 raw 数据上构造。见 `contracts/item_state.h`、`api/state.cpp`；raw 与旧结果对照见 `tests/test_item_facts_dispatch.cpp`。

## 写模型与 `inventory.action` v3.0

库存副作用必须使用明确的 `{space,bag,slot}` 和预期句柄，并在主线程执行。`move` 同空间原生路径使用 ITEM 写 port；同一注册源通过 export/import/remove 与失败补偿；跨空间则由 `usecases/move_item.cpp` 编排传输。`use`/`discard` 按原生 ITEM 写 port 或源回调执行；`sell` 由出售事务执行并核对报价/禁售/世界状态；当前 equip/unequip/put_jewel Host 写操作仅接受原生物品。见 `usecases/item_action.cpp`、`usecases/move_item.cpp`、`usecases/sell_item.cpp`。

| API | 输入/结果 | 状态 |
|---|---|---|
| `move(from,count,to)` | `{status,moved_count,merged,target,target_item}` | `InventoryActionStatus` |
| `use(ref)` | `{status,consumed_count}` | `InventoryActionStatus` |
| `discard(ref,count)` | `{status,consumed_count}` | `InventoryActionStatus` |
| `sell(ref,expected_item,count,source)` | `{status,price,category,count}` | `InventorySellStatus`；依赖运行时 `world.facts`。 |
| `equip(role,ref,equip_slot)` | `{status,consumed_count}` | `InventoryActionStatus`；注册袋物品当前 `kUnsupported`。 |
| `unequip(role,equip_slot)` | `{status,consumed_count}` | `InventoryActionStatus` |
| `put_jewel(role,jewel_ref,equip_slot)` | `{status,consumed_count}` | `InventoryActionStatus`；注册袋宝石当前 `kUnsupported`。 |

`InventoryActionStatus`：`kOk`、`kNotInWorld`、`kInvalidRef`、`kStale`、`kEmpty`、`kTaskBagExcluded`、`kConflict`、`kFull`、`kUnsupported`、`kInternal`、`kCreditFailed`、`kRollbackFailed`、`kExtensionSaleUnavailable`。`InventorySellStatus`：`kOk`、`kNotInWorld`、`kTaskBagExcluded`、`kEmptySlot`、`kNoItemHandle`、`kStale`、`kSymbolsUnresolved`、`kNoSell`、`kPriceInvalid`、`kCreditFailed`、`kRemoveFailed`、`kRollbackFailed`、`kExtensionSaleUnavailable`。完整枚举见 `contracts/inventory_action.h`。

HTTP POST 是另一条当前可见的接线：`http/write_handler.cpp` 做 body 校验并 `post_typed`，返回 ticket；结果 handler 在游戏写 port 执行并形成结果。该 HTTP 命令通道与跨插件消费者直接调用 `inventory.action` 的接口形状不同。`/api/item/inventory/{role}/enchant` 也仍是独立写命令，不属于 v3.0 action 表。见 `http/write_handler.cpp`、`http/write_results.cpp`、`runtime/lifecycle.cpp`。

## 跨空间转移与恢复

跨空间转移顺序固定为“先完整写目标，再移除来源”，不允许先删来源：

```text
记录 intent (item.transfer.journal v1)
    → source export (native serialize / QolBagSource::export_item)
    → target import (native load+place / QolBagSource::import_item)
    → 持久化 target_written
    → source remove
    → 成功：清除 journal、刷新库存
```

export/import 失败时清记录并返回；source remove 失败时移除目标作补偿；补偿失败保留记录并返回 internal error。实现见 `usecases/move_item.cpp`、`bags/native_transport.cpp`、`bags/source_transport.cpp`。

`item.transfer.journal` 是 owner store 的 section v1，当前为二进制 header/record，最多 8 条；记录 intent 与 target_written 两阶段，附 payload hash。启动 install/enabled 时调用 `reconcile()`：源/目标都存在则删除目标保留来源；仅一侧存在则清记录；两侧均无则告警后清记录；删除目标失败保留记录并返回 false。无法取得当前存档槽时只保留内存记录并告警。见 `usecases/transfer_journal.cpp`、`runtime/lifecycle.cpp`。

**保存阻断边界**：当前生命周期忽略 `reconcile()` 返回值，且 `transfer_journal::count()` 未接入 core.item 的保存拦截。因此，不能把“未恢复 journal 必然阻止存档”描述为当前代码不变量；当前明确行为是 journal 无法建立或已满时跨空间 move 返回 blocked。见 `usecases/transfer_journal.cpp`、`usecases/move_item.cpp`、`runtime/lifecycle.cpp`。

## 投影与显示桥接

`inventory.projection` 是 core.item 持有的回调注册表；`extension_bag` 注册其投影/UI 回调。`core.item` 自己拥有 `ptrslot:MOVE_MERGE_GOT` wrapper：事件 `0x02` 可交由 projection drop 回调处理；事件 `0x04` 保护注册源/非原生物品并检查原生输入门禁，符合条件时走原生同类堆叠合并。成功后同步显示槽、调用原版库存刷新并清 touch cursor。feature 关闭或 wrapper 未就绪时不启用合并路径。见 `api/projection.cpp`、`hooks/merge_hook.cpp`、`plugins/extension_bag/assembly/projection_registration.cpp`。

该桥只负责显示/UI 回调和 wrapper 协作，不转移袋数据所有权；扩展袋数据及 UI 仍属于 `extension_bag`。更完整投影行为见 [`extension-bag/display-hook.md`](../../refactor/extension-bag/display-hook.md)。

## HTTP surface

以下 15 条库存路由和 2 条 shop 路由由插件 route table 声明；`routes.inc`、`shop/routes.inc` 是方法/路径/门控事实来源，query/write handler 实现当前目的。所有普通 item/shop 路由 `lease_required=1`，非 OP 路由 `op_gated=0`。

| 方法 | 路径 | 目的 |
|---|---|---|
| GET | `/api/item/inventory` | 袋元数据与分页物品列表。 |
| GET | `/api/item/inventory/money` | 读取当前金钱。 |
| GET | `/api/item/inventory/items` | 仅分页物品列表。 |
| GET | `/api/item/inventory/bag/{bag}/info` | 单袋容量与占用槽数。 |
| GET | `/api/item/inventory/bag/{bag}/{slot}` | 读取单槽；空槽返回 `null`。 |
| POST | `/api/item/inventory/use_item` | 使用指定物品。 |
| POST | `/api/item/inventory/accept_dice` | 接受待结算骰子结果。 |
| POST | `/api/item/inventory/reject_dice` | 拒绝待结算骰子结果。 |
| POST | `/api/item/inventory/discard_item` | 丢弃指定物品/数量。 |
| POST | `/api/item/inventory/sell_item` | 出售指定物品。 |
| POST | `/api/item/inventory/move_item` | 移动物品到目标袋/槽。 |
| POST | `/api/item/inventory/{role}/put_jewel` | 将宝石放入角色装备槽。 |
| POST | `/api/item/inventory/{role}/enchant` | 对角色装备执行附魔写命令。 |
| POST | `/api/item/inventory/{role}/equip_item` | 装备物品或按类别执行装备写命令。 |
| POST | `/api/item/inventory/{role}/unequip_item` | 从角色装备槽卸下物品。 |
| GET | `/api/item/shop/items` | 查询当前商店货架。 |
| POST | `/api/item/shop/buy_item` | 购买商店物品。 |

另有 4 条 OP-gated inventory route（`op_gated=1`、`lease_required=1`）：

| 方法 | 路径 | 目的/当前实现 |
|---|---|---|
| POST | `/api/op/inventory/money` | OP 设置金钱；Native typed command。 |
| POST | `/api/op/inventory/add` | OP 添加物品；Native typed command。 |
| POST | `/api/op/inventory/set-slot` | 仅 owner metadata；Kotlin 返回 501 `not implemented`。 |
| POST | `/api/op/inventory/set-equip` | 仅 owner metadata；Kotlin 返回 501 `not implemented`。 |

HTTP 路由完整响应/API 参数以[HTTP API 参考](../api-reference.md)为准。

### 分页与查询错误

`inventory` 与 `items` 接受 `cursor`、`limit`、可选 `space`/`bag`；游标是 `space:bag:slot`，按该三元组递增。`limit` 默认 100、上限 500；参数做百分号解码，空 cursor 等同未提供。给出 `space` 时 bag 是空间内索引，省略时按扁平袋号解析。见 `http/query_handler.cpp`。

查询常见 HTTP 结果：无可读快照 503；非法参数/游标/limit 400；袋或越界槽不存在 404；空槽 body 为 `null`；快照富化或内部响应失败 500。列表响应含 `next_cursor` 和 `has_more`，不提供总量。

## 错误语义

| 层 | 错误映射 |
|---|---|
| Action API | 以 `InventoryActionStatus` / `InventorySellStatus` 表达业务失败；过期句柄为 `kStale`，任务袋为 `kTaskBagExcluded`，不支持的注册袋动作返回 `kUnsupported`。 |
| Bag source / transfer | 参数非法、找不到/过期句柄、源未就绪、目标冲突、容量/任务袋/journal 门禁、原语不支持、内部/补偿失败分别使用 `kQolErrInvalidArgument`、`kQolErrNotFound`、`kQolErrNotReady`、`kQolErrConflict`、`kQolErrBlocked`、`kQolErrUnsupported`、`kQolErrInternal`。见 `contracts/bag.h`、`usecases/move_item.cpp`。 |
| HTTP GET | 按上节的 400/404/500/503 返回；响应缓冲不足也属于内部错误。见 `http/query_handler.cpp`。 |
| HTTP POST | 参数错误 400；命令端口/队列不可用 503；接收成功先返回 ticket，最终业务结果由 ticket result handler 提供。见 `http/write_handler.cpp`、`http/write_results.cpp`。 |

## 线程与顺序约束

- `inventory.action` 仅允许在游戏主线程调用；袋源注册/注销及袋源回调也限定主线程，注册期间来源函数表必须有效。见 `contracts/inventory_action.h`、`contracts/bag.h`。
- HTTP GET 只读不可变 `InventorySnapshot`；HTTP POST 在请求线程做解析/校验后投递 typed command，不在请求线程直接改物品。见 `http/query_handler.cpp`、`http/write_handler.cpp`。
- 原生 hook/UI 回调位于游戏入口；动作先按显式位置解析当前物品并复核 `{slot,generation}`，不能缓存原生地址作为跨线程身份。见 `usecases/item_action.cpp`、`contracts/item_handle.h`。
- 成功写入后触发库存 refresh；跨空间 transfer 必须按 export → import → source remove 运行，失败先补偿，不得颠倒次序。见 `usecases/read_inventory.cpp`、`usecases/move_item.cpp`。

## 不变量

1. `(0,5)` 是只读任务袋，任何动作都不得把它作为源或目标。
2. `space` 独立编号；`bag` 仅在所属 space 内有意义，不能仅凭相同数字跨空间推断同一袋。
3. 扁平袋号只是便利映射，不持久化、不作为跨请求稳定键；稳定定位使用 `{space_id, bag, slot}`。
4. 物品身份是 `{slot, generation}`；任何写前身份复核失败均不得重试旧句柄。
5. 所有物品/袋源变更只在游戏主线程发生；查询使用已发布快照。
6. 成功写入请求库存刷新并推进 snapshot epoch；注册/注销袋源会改变来源集合，必须使后续快照反映变化。
7. 跨空间转移目标写入成功前不得移除源；源移除失败必须补偿目标，补偿失败保留 journal。
8. journal 不可建立或达到 8 条上限时不执行新的跨空间转移；当前代码没有证明 journal 未恢复会阻止存档（见“跨空间转移与恢复”）。
9. `core.item` 声明 `deactivate_allowed=false`；插件停用不得释放其持有的库存 wrapper 资源。

## 测试与验证映射

| 范围 | 代码测试/检查 |
|---|---|
| descriptor、能力、端口、feature 与路由元数据 | `tests/test_item_facts_plugin.cpp`、`tests/test_item_facts_dispatch.cpp` |
| 查询分页、raw inventory 对照、HTTP 写校验与结果 | `tests/test_item_facts_dispatch.cpp` |
| 动作出售、扣款/回滚 | `tests/test_inventory_action_sell.cpp`、`tests/test_inventory_trade_sell.cpp` |
| HTTP 移动/注册源传输路径 | `tests/test_item_facts_dispatch.cpp`（请求、目标导入与错误结果）；当前没有独立的 transfer journal 测试文件。 |
| 袋映射、句柄身份 | `tests/test_bag_catalog.cpp`、`tests/test_item_handle_registry.cpp`、`tests/test_item_handle_custody.cpp`、`tests/check_item_handle_contracts.cmake` |
| hook 与拖拽合并 wrapper | `tests/test_inventory_hook_stage4.cpp`、`tests/test_move_merge_plugin.cpp` |
| 静态资源/路由契约 | `scripts/maintenance/check_arch.py`（含资源检查）、`scripts/maintenance/check_http_routes.py` |
| 游戏内行为 | 按 `docs/refactor/verification.md` 对应验收卡执行；Host/静态测试不替代要求的真机证据。 |

测试文件均位于 `module/app/src/main/cpp/tests/`；实际执行范围和设备验收结论以验证卡及其证据为准。

## 已删除的旧结构

- `ExtensionBagApi`：已从当前 core.item 实现移除，袋源能力改为注册式契约。
- `QolInventoryExtensionProvider`：不再作为 core.item 的扩展袋读写类型。
- provider-first arbitration：当前读取直接区分 ITEM port 的空间 0/1 与注册 `QolBagSource` 空间。
- `extension_source_bridge`：当前目录树不再包含该过渡适配器。
- `inventory.merge_projection`：旧投影面由 `inventory.projection` 回调注册表替代。
- 旧 fragment state：库存扩展段不再依赖来源方 JSON 字符片段，当前由袋源结构化读取生成。
