# extension_bag.next（骨架）

扩展袋 UI/投影层的新骨架。当前只建立可编译的插件入口，未注册到宿主，不与现有 `extension_bag` 竞争。

## 边界

- 拥有：UI 投影（ControlItem_SetItem/GetItem、INVEN_GetBagSize）、UI 刷新 hook、拖拽合并、扩展袋页签/详情/装备页展示。
- 通过 `core.inventory` 查询槽表，通过 `core.item` 触发动作与读取 item facts。
- 不拥有存储；不直接写原生袋/外部表。

## 当前状态

- 插件 target：`qol_plugin_extension_bag.next`
- 插件 id：`extension_bag.next`（迁移期 id，切换注册时改为 `extension_bag`）
- 未注册到 `host/assembly.cpp`；现有 `extension_bag` 继续提供运行行为。
- 下一阶段：先落投影上下文与 `BagAccess.locate` 消费，再迁入 5 条详情页/刷新 hook 与拖拽合并。
