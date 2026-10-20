# `core.item` 插件

## 1. 作用与架构定位

`core.item` 是物品/库存领域的官方 owner：提供物品事实、库存状态投影、查询与写命令，并持有库存/装备入口 hook（24 条）与装备栏拖拽合并 wrapper。它通过 typed capability、命令、Game Host ports 与声明式资源实现，不依赖 Loader/Game Adapter 私有源码。

`inventory_hooks` 与 `move_merge` 已并入本插件；不再有独立插件目录或 descriptor。历史审计中的 D4 口径（库存行为域并入 `core.item`）已落地。

## 2. 功能面

### HTTP 路由

以下路由仍由 `core.item` owner 与 handler 提供，合并未增删路由：

| 方法 | 路径 |
|---|---|
| GET | `/api/item/inventory` |
| GET | `/api/item/inventory/money` |
| GET | `/api/item/inventory/items` |
| GET | `/api/item/inventory/bag/{bag}/info` |
| GET | `/api/item/inventory/bag/{bag}/{slot}` |
| GET | `/api/item/shop/items` |
| POST | `/api/item/inventory/use_item` |
| POST | `/api/item/inventory/accept_dice` |
| POST | `/api/item/inventory/reject_dice` |
| POST | `/api/item/inventory/discard_item` |
| POST | `/api/item/inventory/sell_item` |
| POST | `/api/item/inventory/move_item` |
| POST | `/api/item/inventory/{role}/put_jewel` |
| POST | `/api/item/inventory/{role}/enchant` |
| POST | `/api/item/inventory/{role}/equip_item` |
| POST | `/api/item/inventory/{role}/unequip_item` |
| POST | `/api/item/shop/buy_item` |
| POST | `/api/op/inventory/money` |
| POST | `/api/op/inventory/add` |

`/api/op/inventory/set-slot` 与 `/api/op/inventory/set-equip` 仅 owner metadata，当前为 Kotlin 501 占位。正式 API 定义见 [API 参考 §3、§7.9–§7.10](../api-reference.md)。

### Capabilities

| 能力 | 版本 | 内容 |
|---|---|---|
| `item.facts` | 3.6 | 分类、装备/宝石/禁售判断、价格、堆叠计数/上限/编码、inventory view、详情上下文、金钱等事实 |
| `item.state` | 1.1 | raw / enriched inventory JSON 投影 |
| `inventory_refresh` | 1.0 | 原版 `UIEquip_RefreshItemArea` trampoline；仅主线程 |
| `inventory.action` | 2.0 | `sell(bag, slot, expected_item, source)` 出售事务 |
| `inventory.slot` | 1.0 | `resolve` / `view` / `remove` 槽位访问 |
| `move_merge.control` | 1.0 | `enabled()` = `move_merge.drag_merge` feature 开启且 merge wrapper 已就绪 |

`item.facts` v3.6 的 inventory view 按来源提供 `native_options[]`/`jewel_options[]`/`enchant_options[]`；inventory JSON 的 `options`/`option_ids`/`option_types` 按原生、宝石顺序拼接，附魔列表在属性未解码前恒为空。

### 注册 feature

| feature id | 默认 | 分类 | 类型 |
|---|---:|---|---|
| `core.item.provider` | 1 | Hidden | toggle |
| `move_merge.drag_merge` | 1 | Basic optimization | toggle |

`core.item.provider` 表达常驻 provider；`move_merge.drag_merge` 是玩家可见的拖拽合并开关，`on_toggle` 只切换 merge 内部 gate，不卸载 wrapper。原 `inventory_hooks.inventory_support` 调试 feature 已删除；库存 wrapper 常驻启用。

### 游戏内行为

- **库存/装备入口**：`plugins/core.item/inventory/native_inventory_hook.cpp` 声明 24 条 `kEntryHook` 资源，覆盖查询、写、装备与 UI 出售入口。wrapper 负责原版物理袋与 `extension_bag` 逻辑袋桥接、S2 数量进位/借位修正、详情页出售事务与出售门禁。关闭/缺失 backup 时按原版透传。
- **拖拽合并**：`plugins/core.item/merge/plugin.cpp` 持有 `ptrslot:MOVE_MERGE_GOT`（`UIEquip_InvenItemControlEventProc`）。`event 0x02` 先交给 `inventory.merge_projection.projection_drop`；`event 0x04` 做扩展/非 native 来源保护、原版输入门禁与原生同类合并。合并成功后同步投影槽、调用 `inventory_refresh` 原函数并清理 touch cursor。
- `move_merge.drag_merge` 关闭时 wrapper 整体透传原版；`ptrslot` 安装失败时 wrapper 不安装，`move_merge.control.enabled()` 返回 false，EB 合并门禁随之关闭。

## 3. 依赖前置与资源

- **required capabilities**：无。
- **optional capabilities**：`character.state`、`http.registry` 1.1、`http.dispatch` 1.1、`world.facts` 2.0、`extension_bag` 3.1、`inventory.merge_projection` 1.0。全部按调用时 `capabilities.require`；缺失只降级对应行为。
  - `world.facts` 缺失时 `inventory.action.sell` 返回 `kSymbolsUnresolved`，在任何扣款/删除前返回。
  - `extension_bag`/`inventory.merge_projection` 缺失时扩展袋出售与投影路径失败/透传，不阻断 `item.facts`。
- **ports**（6）：`QOL_PORT_ITEM` 3.6、`QOL_PORT_ITEM_MONEY_WRITE` 1.0、`QOL_PORT_ITEM_DICE_WRITE` 1.1、`QOL_PORT_ITEM_INVENTORY_STATE_WRITE` 1.1、`QOL_PORT_MOVE_MERGE` 1.2、`QOL_PORT_CHARACTER_EXTRAS`。
- **resources**（25）：24 条库存入口 hook，全部 `kPrivate / kRequired / kFailClosed / resumable / step_budget=120`，owner `core.item`；`ptrslot:MOVE_MERGE_GOT` 为 `kPrivate / kOptional / kFailClosed`，安装失败只 degraded，`move_merge.control.enabled()` 因 `wrapper_ready=false` 返回 false。
- **deactivate_allowed=false**：legacy 消费者仍直接依赖库存 wrapper；宿主拒绝 deactivate，避免在消费者未迁移前物理释放 24 条 hook。

## 4. 职责边界

- **在内**：物品事实/状态、原版库存读取与序列化、查询投影、写命令及票据、24 条库存/装备入口 hook、S2 写侧修正、详情页出售、拖拽合并 wrapper 与 `move_merge.control`。
- **在外**：扩展背包逻辑袋/投影事务（`extension_bag`）；角色事实（`core.character`）；世界状态（`core.world`）；底层游戏内存访问（Game Host / Game Adapter）；HTTP 监听与传输（Kotlin host / `core.api`）。
- `core.item` 不把扩展背包当作自身功能；逻辑袋归属与投影数据仍由 `extension_bag` 提供。

## 5. 核心实现流程

- **读/快照**：每 15 帧在游戏线程检查 world 与静态数据准备状态，序列化原版库存；provider 可用时追加扩展 fragment；完整后发布 `Snapshot<ItemSnapshot>`。HTTP 线程从不可变快照取数据并富化。
- **写/命令/票据**：POST handler 解析输入并提交 typed command，返回 ticket；主线程结果 handler 调用物品写 port；扩展动作经中立 provider 转发，写入由实际 owner 执行。
- **库存 hook**：宿主按 descriptor 资源表安装 24 条入口 hook；`on_resource_ready` 按资源 id 交还 backup。FindItem 的 handler 由 Game Host `item_abi.find_item_wrapper` 提供，backup 经 `bind_find_item_backup` 交还。
- **合并 wrapper**：`on_resource_ready` 识别 `ptrslot:MOVE_MERGE_GOT` 后置 `wrapper_ready=true`；`plugin_install`/`on_enabled` 绑定 SDK、move port、character extras port 与本地 `item.facts` 表；`plugin_deactivate` 逆序 detach merge 与 inventory。
- **配置迁移**：`move_merge.drag_merge` 从旧 `move_merge` namespace（`features.move_merge.drag_merge`、顶层 `enable`、根 `moveMergeEnabled`）迁入 `core.item`；旧有效态按 `enable && feature` 计算，仅在写入成功后删除已知旧键，保留未知字段。

## 6. 文件地图

| 子职责 | 文件 | 职责 |
|---|---|---|
| 主 owner | `plugins/core.item/plugin.cpp` | descriptor、combined resources、6 capabilities、快照、查询/写命令、生命周期 |
| 库存 hook | `plugins/core.item/inventory/native_inventory_hook.cpp` | 24 条 claim、wrapper、backup 交还、S2/出售行为 |
| 库存纯逻辑 | `plugins/core.item/inventory/inventory_hook_stage4.*` | 查询/消耗/移动/拆分/出售分流与 S2 数量规则 |
| 库存 slot | `plugins/core.item/inventory/inventory_slot.*` | `inventory.slot` 实现 |
| 库存出售 | `plugins/core.item/inventory/plugin.cpp`、`sell_transaction.h` | `inventory.action.sell` 与退款/删除事务 |
| 拖拽合并 | `plugins/core.item/merge/plugin.cpp` | item-proc wrapper、合并门限、`move_merge.control`、feature gate |
| JSON/响应 | `plugins/core.item/json_lite.*`、`response.*` | 有界 JSON 解析、库存/商店投影 |
| 声明/快照 | `plugins/core.item/plugin.h`、`snapshot_sdk.h` | 模块入口、`item.facts` 表、Host 快照封装 |
| 公共契约 | `contracts/item_facts.h`、`item_state.h`、`inventory_*.h`、`move_merge.h`、`game_ports/*` | capability、资源、Game Host 端口类型 |
| Kotlin 请求入口 | `module/app/src/main/java/com/inotia4/qol/api/controller/InventoryController.kt`、`InventoryActionController.kt`、`ShopController.kt` | HTTP 参数/请求转交，不实现 native 领域语义 |

接口规格：[HTTP API 参考](../api-reference.md)。
