# core.inventory（骨架）

被动库存/袋源层。当前只建立可编译、可注册的插件入口，不安装 hook、不提供能力、不接 HTTP。

## 边界

- 拥有：槽表、`SlotRef`/`ItemKey`、查询/计数/空槽、取放/移动/保存、S2/CountCodec、存储 hook、inventory HTTP。
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
- 跨空间移动仍由 `core.item` 的 `move_item` + native/source transport 执行；inventory 只提供存储 hook 与袋源注册面。
- 尚未迁移：inventory HTTP 8 条路由、EB 5 条 UI hook/拖拽合并、新 `core.item` 整体替换。
