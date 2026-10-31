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
- 提供 `inventory.access` v1.0：`item_at` / `locate` / `find_first` / `find_empty` / `count_item` / `move` / `take` / `place` / `remove_count`。
- 存储核心已落地：`SlotTable` 接口、`MemorySlotTable`（host 测试用）、`InventoryStore`（槽表注册、查询、整件移动、取放、部分移除、任务袋只读）。
- 尚未接入原生表/外部表、hook、HTTP；`inventory.access` 当前只看到测试内存表。
- 下一阶段：先接原生表适配器与外部表注册，再接 12 条存储 hook 与 inventory HTTP。
