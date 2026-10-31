# core.item.next（骨架）

物品领域层的新骨架。当前只建立可编译的插件入口，未注册到宿主，不与现有 `core.item` 竞争。

## 边界

- 拥有：item facts（item_type/stackable/category 映射/禁售/报价）、物品动作（use/equip/unequip/sell/put_jewel/enchant/create/place/discard_handle）、动作 hook、shop、item HTTP。
- 通过 `BagAccess` 调 `core.inventory` 完成存储；不直接读写槽表。
- 不依赖 `extension_bag`；不负责 UI 刷新。

## 当前状态

- 插件 target：`qol_plugin_core.item.next`
- 插件 id：`core.item.next`（迁移期 id，切换注册时改为 `core.item`）
- 未注册到 `host/assembly.cpp`；现有 `core.item` 继续提供运行行为。
- 下一阶段：先落 `BagAccess`/`ItemAction` 契约与动作骨架，再迁入 11 条动作 hook、shop 与 item HTTP。
