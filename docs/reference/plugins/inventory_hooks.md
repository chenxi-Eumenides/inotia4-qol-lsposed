# `inventory_hooks`

库存行为域插件，声明式持有 24 条原生库存/装备入口 hook 资源，并对外提供库存刷新、出售与槽位访问能力。不注册 HTTP 路由、JNI 或 descriptor ports。

## 职责

- **owner**：24 条入口 hook 资源（`kEntryHook`）的声明式所有者；hook 的安装、冲突仲裁与还原由宿主执行。
- **行为职责**：在游戏原版库存查询、消耗、移动、拆分、装备与详情页出售入口上，桥接原版物理袋与 `extension_bag` 逻辑袋，并在数量编码扩展（S2）场景下修正数量读写。
- **不负责**：逻辑袋本体与 `for_each_logical_item`（`extension_bag`）；`item.facts`/`item.state` 与 `/api/item/*`（`core.item`）；HTTP 出售事务（`core/native/inventory_trade.cpp`）；物品句柄注册与失效（`game/access/item_handle_registry`）。

## 对外能力

| 能力 | 版本 | 内容 | 消费者 |
|---|---|---|---|
| `inventory_refresh` | 1.0 | `call_original()`：原版 `UIEquip_RefreshItemArea` trampoline | `extension_bag`（optional）、`move_merge`（optional） |
| `inventory.action` | 2.0 | `sell(bag, slot, expected_item, source)` | `autosell`（required） |
| `inventory.slot` | 1.0 | `resolve(bag, slot)`、`view(handle)`、`remove(bag, slot, expected, reason)` | 暂无生产消费者 |

能力定义：`plugin.cpp:272-291`（provides）、`contracts/inventory_action.h`、`contracts/inventory_refresh.h`、`contracts/inventory_slot.h`。

依赖：`world.facts 2.0`、`item.facts 3.5` 为硬依赖；`extension_bag`（≥3.1 sale-removal）为可选依赖（`plugin.cpp:43-55`）。`extension_bag` 反向 optional 依赖 `inventory_refresh`，两条软边成环，宿主按 required-only 拓扑降级软边破环。

## 资源清单（24）

全部为 `kPrivate / kRequired / kFailClosed / resumable=true`，声明于 `native_inventory_hook.cpp:1474-1527`。

| 族 | 资源 |
|---|---|
| 查询 | `hook:INVEN_FindItem`、`hook:INVEN_HaveItem`、`hook:INVEN_GetItemCount`、`hook:ITEM_GetCumulateCount`、`hook:INVEN_IsHavingEmptySlot` |
| 写 | `hook:INVEN_ConsumeItem`、`hook:INVEN_RemoveItem`、`hook:INVEN_SaveItem`、`hook:INVEN_SaveItemDirect`、`hook:INVEN_MoveItem`、`hook:ITEMSYSTEM_Divide`、`hook:ITEMSYSTEM_MakeItem`、`hook:INVEN_RemoveItemData` |
| 装备/UI | `hook:CHAR_EquipItemFromInvenToSlot`、`hook:CHAR_UnequipItemToInven`、`hook:ITEMSYSTEM_PutJewel`、`hook:UIEquip_ButtonEquipExe`、`hook:UIEquip_ButtonUnequipExe`、`hook:UIEquip_ButtonDestroyExe`、`hook:UIEquip_OKConfirmUseItem`、`hook:UIEquip_OKDestroyItem`、`hook:UIEquip_ButtonUseMercenarySealExe`、`hook:UIEquip_EquipControlEventProc`、`hook:UIEquip_RefreshItemArea` |

`INVEN_FindItem` 的资源 handler 是占位实现；运行时由 Game Host 注入的 `item_abi.find_item_wrapper` 替换，backup 经 `inventory_hooks_bind_backup()` 交还（`native_inventory_hook.cpp:1490-1520,1560-1600`）。

## 出售语义

三条入口共用 `item.facts.quote(handle, kDetailSell, …)` 计价（`contracts/item_facts.h`、`game/access/item_sell_quote.cpp`），事务分别位于：

| 入口 | 事务实现 | 价格 |
|---|---|---|
| 详情页「销毁/出售」按钮 | `ok_destroy_item_wrapper`（`native_inventory_hook.cpp:1281`）：加钱 → 复核槽内身份 → 删除 → 失败退款 → 刷新 | 变体倍率 |
| `inventory.action.sell` | `plugin.cpp:108` + `sell_transaction.h` | 变体倍率 |
| `POST /api/item/inventory/sell_item` | `core/native/inventory_trade.cpp`（owner `core.item`） | 变体倍率 |

倍率：original/unknown `0%`、overhaul/monster `70%`、c18 `85%`；商店手动出售 `100%`（`game/variants/game_variant.cpp` 的 `sell_multiplier`）。

UI 接管边界：仅当物品为 count-encoded 且实际使用 S2 编码（`canonical > 127`）时接管；`canonical ≤ 127`、任务袋 5、非 count-encoded 一律透传原版（`inventory_hook_stage4.cpp` 的 `vanilla_sell_route`）。任务袋在 `inventory.action`、`inventory.slot.remove` 与 API 路径一律拒绝。

## 读写与并发

- **查询**：原版优先；原生递归调用期间只走原版；非递归时按 `for_each_logical_item` 汇总逻辑库存。
- **写**：原版调用前采集物理槽快照，调用后按槽内句柄复核再回写数量；S2 编码的借位/进位在 `inventory_hook_stage4` 中计算。
- **并发**：wrapper 使用 `InFlightGuard` 登记在途；`g_hook_mutex` 只保护 Native API 表登记；扩展袋观察在短临界区采集，出锁后再调原版，避免重入。
- **空 backup**：所有 wrapper 调 backup 前检查空指针并记录 `hook=<id> backup missing`；空 backup 不产生半执行。

## 生命周期

- `on_resource_ready` 按资源 id 交还 backup；未知 id 或空 backup 返回非零，宿主按 fail-closed 回滚整插件资源（`plugin.cpp:359-366`）。
- `plugin_deactivate` 清空 SDK/Game Host/item.facts 缓存并解绑 slot provider（`plugin.cpp:334-341`）。
- **deactivate 约束**：`extension_bag`、`autosell` 等消费者仍直接依赖 24 条 wrapper 语义；当前约束只写在代码注释与流程中，代码不拦截（`plugin.cpp:25-30`）。
- FeatureDefinition：`inventory_hooks.inventory_support`（`kHidden` toggle，默认开启）是调试门禁；关闭时 23 条插件内 wrapper 透传原版（`FindItem` 由 Game Host handler 替换，不在开关范围）。合并进 `core.item` 后删除该开关。
- `fill_status` 保持既有全局 status 语义（ENG-04 统一处置）。

## 文件地图

| 文件 | 职责 |
|---|---|
| `plugin.cpp` | descriptor、能力表、`inventory.action.sell`、生命周期 |
| `native_inventory_hook.cpp` | 24 条 claim、全部 wrapper、backup 交还、FindItem 桥 |
| `inventory_hook_stage4.{h,cpp}` | 纯逻辑：查询/消耗/移动/拆分/出售分流、S2 数量规则 |
| `inventory_slot.{h,cpp}` | `inventory.slot` provider（resolve/view/remove） |
| `sell_transaction.h` | 出售事务状态机（credit → remove → refund） |
| `inventory_logging.{h,cpp}` | `kInventory` 域日志与节流 |
| `claims.h`、`inventory_find_item_poc.{h,cpp}` | claim 引用与 FindItem PoC |

关联契约：`contracts/item_facts.h`（3.5，`quote` 与 view 分词列表）、`contracts/inventory_action.h`（2.0）、`contracts/inventory_refresh.h`（1.0）、`contracts/inventory_slot.h`（1.0）、`contracts/sell_price.h`。
