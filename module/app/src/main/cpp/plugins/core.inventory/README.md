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
- 已注册到 `host/assembly.cpp`，启动时只输出安装/启停日志。
- 下一阶段：先落 `BagTableRegistry`/`InventoryReader`/`InventoryExecutor` 与 `BagAccess` 契约，再安装 12 条存储 hook。
