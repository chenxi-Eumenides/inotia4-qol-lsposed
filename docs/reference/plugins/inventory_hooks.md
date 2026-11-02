# `inventory_hooks`（已按 hook 职责拆分并入三个插件）

本插件不存在独立实体后，其 hook 按职责拆分到三个 owner：

| hook 类别 | 当前 owner | 说明 |
|---|---|---|
| 存储类库存 hook（`INVEN_*`/`ITEM_GetCumulateCount` 等 12 条） | `core.inventory` | 存储读写入口与逻辑帧快照读面；架构见[三插件架构](../../refactor/core-item/three-plugin-architecture.md)。 |
| 动作类库存 hook（8 条 required fail-closed）+ 3 条快捷栏诊断（optional fail-soft） | [`core.item`](core.item.md) | 装备/卸下/镶嵌/销毁/拆分/生成等动作入口；诊断 hook 仅观察快捷栏使用。 |
| 装备页 UI hook（`UIEquip_ButtonEquipExe`/`ButtonUnequipExe`/`OKConfirmUseItem`/`RefreshItemArea`/`EquipControlEventProc`） | [`extension_bag`](extension_bag.md) | 原 `inventory_refresh` provider（`RefreshItemArea` trampoline）同归 `extension_bag`。 |

能力、资源与文件地图的当前权威事实见各 owner 文档。本文件仅保留历史入口，不再描述独立插件。
