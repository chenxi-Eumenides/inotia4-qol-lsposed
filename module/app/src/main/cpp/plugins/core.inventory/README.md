# core.inventory（骨架）

被动库存/袋源层。提供存储 hook、袋源注册面与库存 HTTP 读面（5 条 GET）。

## 边界

- 拥有：槽表、`SlotRef`/`ItemKey`、查询/计数/空槽、取放/移动/保存、S2/CountCodec、存储 hook、inventory HTTP 读侧（`/api/item/inventory*` 5 条 GET）。
- 只依赖：host `QolItemPort` 的 item category/count、host `QolItemCountApi` 的 S2 编解码。
- 不依赖：`core.item`、item_data、`extension_bag`。
- 不判断：item_type、stackable、禁售、报价。

## 当前状态

- 插件 target：`qol_plugin_core.inventory`
- 插件 id：`core.inventory`
- 已注册到 `host/assembly.cpp`，启动时输出安装/启停日志。
- 提供 `inventory.access` v1.0 与 `inventory.bag_source` v1.2。
- 已接管 12 条存储 hook：FindItem / HaveItem / GetItemCount / CumulateCount / IsHavingEmptySlot / ConsumeItem / RemoveItem / SaveItem / FindSaveSlot / SaveItemDirect / RemoveItemData / MoveItem。
- 存储核心：`SlotTable`、`MemorySlotTable`（host 测试）、`InventoryStore`、S2 写回、stage4 纯函数、pending save / flat bag / 注册源补扣。
- HTTP 读侧：`http/`（json_lite/response/query_handler/routes）+ `usecases/read_inventory` + `bags/snapshot_store`；descriptor 以 optional `http.registry`/`http.dispatch` 声明，注册 5 条 GET（route_owner=`core.inventory`）；快照由 scheduler 逻辑帧任务刷新，查询只读快照，未就绪回 503。
- 跨空间移动仍由 `core.item` 的 `move_item` + native/source transport 执行；inventory 只提供存储 hook 与袋源注册面。
- 尚未迁移：inventory HTTP 写侧（POST move/set-slot/money 等）、EB 5 条 UI hook/拖拽合并、新 `core.item` 整体替换。
