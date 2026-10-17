# `inventory_hooks` 插件说明（审阅批 2026-10-18）

本文按插件审阅的六维说明整理，用于理解插件职责、边界与实现。事实参考以
`docs/reference/plugins/inventory_hooks.md` 为准；验收状态见
`verification-log.md`；开放问题见 `docs/refactor/issues.md` §13。

## 1. 作用

库存行为域插件，声明式持有 **24 条原生库存/装备入口 hook**，并对外提供库存刷新、出售与槽位访问能力。它不是库存数据的 owner，也不注册 HTTP/JNI。

## 2. 功能

- **24 条入口 hook**（`kPrivate / kRequired / kFailClosed`）：
  - 查询：`FindItem`、`HaveItem`、`GetItemCount`、`GetCumulateCount`、`IsHavingEmptySlot`
  - 写：`ConsumeItem`、`RemoveItem`、`SaveItem`、`SaveItemDirect`、`MoveItem`、`Divide`、`MakeItem`、`RemoveItemData`
  - 装备/UI：`EquipItemFromInvenToSlot`、`UnequipItemToInven`、`PutJewel`、6 个 `UIEquip_*`
- **能力**：
  - `inventory_refresh` 1.0：原版 `RefreshItemArea` trampoline
  - `inventory.action` 2.0：`sell(bag, slot, expected_item, source)`
  - `inventory.slot` 1.0：`resolve` / `view` / `remove`
- **游戏内效果**：逻辑袋接入原版查询、消耗、移动、合并、分类扣除、装备与卸下；原版详情页出售在 S2 数量下接管结算；原生出售的资格/计价/退款语义统一。

## 3. 使用的模块功能

| 面 | 内容 |
|---|---|
| 硬依赖 | `world.facts` 2.0、`item.facts` 3.4（`quote`） |
| 可选依赖 | `extension_bag` ≥3.1（`remove_for_sale`） |
| Host SDK | `log`、`in_flight`、`capabilities.require`、`hooks.intercept_blocks` |
| Game Host | item ABI（FindItem 替换、backup 绑定）、item handle 查询/登记/失效、item count facade、`inventory_write` |
| 不使用 | scheduler、commands、ticket_result；无 HTTP 路由、无 JNI、无 descriptor ports |

`extension_bag` 反向 optional 依赖 `inventory_refresh`，两条软边成环；宿主按 required-only 拓扑降级软边破环。

## 4. 范围

- **在内**：24 条入口 hook 的归属与行为；原生袋与逻辑袋的交织；S2 数量读写修正；原生详情页出售事务；调试门禁。
- **在外**：逻辑袋本体（`extension_bag`）；`item.facts`/`item.state` 与 `/api/item/*`（`core.item`）；HTTP 出售事务（`core/native/inventory_trade.cpp`）；物品句柄注册与失效（`game/access/item_handle_registry`）。

## 5. 对外提供与消费者

| 能力 | 消费者 | 方式 |
|---|---|---|
| `inventory_refresh` | `extension_bag`、`move_merge` | optional_requires |
| `inventory.action` | `autosell` | required |
| `inventory.slot` | 暂无生产消费者 | 1.0 已发布 |

24 条 wrapper 仍被 legacy 消费者直接依赖；因此 descriptor 设 `deactivate_allowed=false`，宿主拒绝 deactivate，直到合并进 `core.item`。

## 6. 实现机制

- **查询**：原版优先；原生递归调用期间只走原版；非递归时按 `for_each_logical_item` 汇总逻辑库存。
- **写**：原版调用前采集物理槽快照，调用后按槽内句柄复核再回写数量；S2 借位/进位在 `inventory_hook_stage4` 中计算。
- **并发**：wrapper 使用 `InFlightGuard`；`g_hook_mutex` 只保护 Native API 表登记；扩展袋观察在短临界区采集，出锁后再调原版。
- **空 backup**：所有 wrapper 调 backup 前检查并记录 `hook=<id> backup missing`。
- **生命周期**：`on_resource_ready` 交还 backup，空 backup/未知 id 返回非零触发整插件回滚；deactivate 被宿主拒绝。
- **调试门禁**：`inventory_hooks.inventory_support`（`kHidden`，默认开启）；关闭时 23 条插件内 wrapper 透传原版；`FindItem` 由 Game Host handler 替换，不在开关范围。合并进 `core.item` 后删除。

## 7. 三条出售路径

| 入口 | 事务实现 | 计价 | 说明 |
|---|---|---|---|
| API `POST /api/item/inventory/sell_item` | `core/native/inventory_trade.cpp` | `item.facts.quote` | 整槽出售；任务袋拒绝 |
| 详情页「销毁/出售」 | `native_inventory_hook.cpp` `ok_destroy_item_wrapper` | `item.facts.quote` | 预演只显示金额；确认才结算；S2 才接管 |
| `inventory.action.sell` | `plugin.cpp` + `sell_transaction.h` | `item.facts.quote` | autosell 使用；逻辑袋走 EB `remove_for_sale` |

倍率：original/unknown `0%`、overhaul/monster `70%`、c18 `85%`；商店手动出售 `100%`。`item.facts.quote` 是唯一计价入口，金额向下取整。

## 8. 本批裁决与改动

- 倍率表放 `game/variants`；API/autosell 跟随详情出售倍率，商店 100%。
- S2 接管边界：`canonical ≤ 127` 走原版，实际使用 S2 才接管；任务袋一律不由模块处理。
- `item.facts` 3.4 新增 `quote`；`item.facts` 3.4 / Item Port 3.4；新增 `inventory.slot` 1.0。
- 退款失败透传 `kRollbackFailed`；UI 日志区分 `refunded=1` 与 `refund_failed=1`。
- `deactivate_allowed=false`：宿主拒绝 deactivate。
- 逐变体符号矩阵：24/24 × 4 变体全部一致。

## 9. 开放项

- IH-06：`MoveItem`/`RemoveItemData`/`Divide` 的原版失败返回值语义未冻结，半写保护待逆向后补。
- G-2：替换后的 `FindItem` handler 未登记 in-flight，属宿主初始化顺序问题，需引擎级改动。
- UNVER-03：c18/overhaul 倍率仅静态金标；退款失败日志、调试门禁关闭分支未真机逐条触发；autosell 路径并入 autosell 审阅。
