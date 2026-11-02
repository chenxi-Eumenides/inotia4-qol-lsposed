# core.inventory（骨架）

被动库存/袋源层。提供存储 hook、袋源注册面、库存 HTTP 读面（5 条 GET）与写侧（move/money 2 条 POST）。

## 边界

- 拥有：槽表、`SlotRef`/`ItemKey`、查询/计数/空槽、取放/移动/保存、S2/CountCodec、存储 hook、inventory HTTP（5 条 GET + move/money 2 条 POST）、跨空间 transfer（`inventory.transfer`）。
- 只依赖：host `QolItemPort` 的 item category/count、host `QolItemCountApi` 的 S2 编解码、`QOL_PORT_ITEM_INVENTORY_STATE_WRITE` 与 `QOL_PORT_ITEM_MONEY_WRITE` 的原生写原语。
- 不依赖：`core.item`、item_data、`extension_bag`。
- 不判断：item_type、stackable、禁售、报价。

## 当前状态

- 插件 target：`qol_plugin_core.inventory`
- 插件 id：`core.inventory`
- 已注册到 `host/assembly.cpp`，启动时输出安装/启停日志。
- 提供 `inventory.access` v1.0、`inventory.bag_source` v1.2 与 `inventory.transfer` v1.0。
- 已接管 12 条存储 hook：FindItem / HaveItem / GetItemCount / CumulateCount / IsHavingEmptySlot / ConsumeItem / RemoveItem / SaveItem / FindSaveSlot / SaveItemDirect / RemoveItemData / MoveItem。
- 存储核心：`SlotTable`、`MemorySlotTable`（host 测试）、`InventoryStore`、S2 写回、stage4 纯函数、pending save / flat bag / 注册源补扣。
- HTTP 读侧：`http/`（json_lite/response/query_handler/routes）+ `usecases/read_inventory` + `bags/snapshot_store`；descriptor 以 optional `http.registry`/`http.dispatch` 声明，注册 5 条 GET（route_owner=`core.inventory`）；快照由 scheduler 逻辑帧任务刷新，查询只读快照，未就绪回 503。
- 提供 `inventory.transfer` v1.0（`transfer/`，从 `core.item` bags/move_item 暂存复制）：承接跨空间移动（`move_item`）、detached 句柄序列化（`serialize_item`）与按 space 分派的定位（`locate`）；原生同空间移动仍归宿主（返回 `kQolErrUnsupported`）。消费 `QOL_PORT_ITEM_MONEY_WRITE` 与 `QOL_PORT_ITEM_INVENTORY_STATE_WRITE`（与 `core.item` 双 owner）。
- HTTP 写侧：`http/write_handler` + `http/write_results`（handler id `kInventoryWriteHandlerId`）承接 `POST /api/item/inventory/move_item` 与 `POST /api/op/inventory/money`（owner `core.inventory`；`set-slot` 仍为 metadata 占位）。move 解析语义与原 core.item 逐字一致；原生同空间走 `QOL_PORT_ITEM_INVENTORY_STATE_WRITE` 的 `move_item`，其余走 `inventory.transfer`；结果体的 slots 顺序为 源槽 → 目标槽 → 其余变化槽（按空间/袋/槽排序）。money 四种结果体与原 core.item 逐字一致。结果处理器经 `kInventoryWriteHandlerId` 注册为 typed result handler；宿主不支持尾部票据 API 时不阻断安装，仅写命令投递失败。
- 尚未迁移：set-slot（metadata 占位，Kotlin 501）、EB 5 条 UI hook/拖拽合并、新 `core.item` 整体替换。
