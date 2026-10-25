# core.item 物品操作方案

> 唯一操作入口是 `inventory.action` 3.0。扩展背包、自动出售、HTTP 都通过它执行物品副作用；任何插件不得直接修改扩展袋数据或原生槽表。

## 1. 术语

| 操作 | 定义 | 介质 |
|---|---|---|
| move | 同介质内换槽 | 原生↔原生、扩展↔扩展 |
| merge | move 的特例：目标已有同类别可堆叠堆 | 同上 |
| transfer | 跨介质搬运，payload 保真 | 原生↔扩展 |
| use | 使用物品并结算可能的消耗 | 单侧 |
| consume | 数量减少/移除（使用、丢弃、出售、镶嵌） | 单侧 |

## 2. 归属

| 层 | 职责 |
|---|---|
| `core.item` | 操作入口、介质判断、合并判定、事务/补偿/意图记录、快照刷新 |
| `extension_bag` | 扩展袋数据、`QolBagSource` 原语（export/import/remove/use/identify）、页签 UI、显示替换 |
| Host | 原生原语：序列化、装载、按句柄移动/装备/镶嵌、原生使用与消耗 |

## 3. 契约

`contracts/inventory_action.h` v3.0：

```
QolActionRef { space_id, bag, slot }

move(from, count, to)                        -> { status, moved_count, merged, target, target_item }
use(ref)                                     -> { status, consumed_count }
discard(ref, count)                          -> { status, consumed_count }
sell(ref, expected_item, count, source)      -> InventorySellResult
equip(role, ref, equip_slot)                 -> { status, consumed_count }
unequip(role, equip_slot)                    -> { status, consumed_count }
put_jewel(role, jewel_ref, equip_slot)       -> { status, consumed_count }
```

约束：

1. 全部函数只在游戏主线程调用。
2. `QolActionRef` 由调用方在操作发起时确定，不接受“当前页签”之类的延迟解析。
3. 执行前用 `item_at` 复核句柄；不一致返回 `kStale`。
4. 任务袋 `(0,5)` 一律 `kTaskBagExcluded`。

## 4. 身份解析

| 入口 | 源 | 目标 |
|---|---|---|
| 原生拖拽 wrapper | 源控件句柄 + 槽号 → `locate`/原生扫描 | 控件槽号 + `current_bag`（0..17） |
| 扩展 UI | `{2, tab, slot}` 显式传入 | `{space, bag, slot}` 显式传入 |
| HTTP | `space/bag/slot` | `to_space/to_bag/to_slot` |
| autosell | 扫描句柄 → `locate` | 无 |

`current_bag` 只表示原生袋号；扩展页签属于 EB 的 UI 状态，必须由 EB 显式传入。

## 5. 操作矩阵

| 源 \ 目标 | 原生（0/1） | 扩展（2） |
|---|---|---|
| **原生** | Host `move_item` / `move_handle`，游戏内部合并 | transfer：`serialize_item` → 源 `import` → `remove_item` |
| **扩展** | transfer：源 `export` → `load_item` + `move_handle` → 源 `remove` | 源 `import_item(payload, preferred_slot)` |

原生↔原生（含原版↔C18）不序列化；扩展↔扩展不经原生；只有跨介质使用 payload。

## 6. 流程

### move / merge

1. 校验 ref、count、任务袋、容量。
2. 源句柄复核。
3. 判定是否可合并：类别相同、目标 count < 上限、`can_apply_stuff` 不接管。
4. 同介质原生：Host move（`INVEN_MoveItem` 合并/找槽）。
5. 跨介质：transfer 协议。
6. 成功：刷新快照 epoch，返回实际落点与是否合并。

### use / discard

- 原生：Host `use_item` / `remove_item`。
- 扩展：源 `use` / `remove`。
- 失败不改变数量；部分移除必须由底层保证原子。

### sell

1. 报价（`item.facts.quote`）。
2. 移除物品。
3. 加钱。
4. 加钱失败：重新导入回原槽（回滚）；回滚失败返回 `kRollbackFailed` 并保留意图记录。

### equip / unequip / put_jewel

- 原生：Host 既有装备接口。
- 扩展：源 `export` → 源 `remove` → Host `equip_handle`/`put_jewel_handles`；失败回插。

## 7. 拖拽 wrapper

1. `event 0x02`：解析源句柄；源属于注册源 → 不接管。
2. `event 0x04`：解析目标槽与 `current_bag`，复核句柄。
3. 统一 dispatch：同原生走 merge/move/材料操作；目标为扩展时由 EB 自己的 UI 调用 action。
4. 成功刷新 UI 并清 touch cursor。

## 8. 扩展背包职责

保留：sidecar、catalog、会话锁、快照、页签/布局/绘制、`QolBagSource`、显示替换 hook。

禁止：

- 直接改 `INVEN_pItem` 或原生袋对象。
- 自行执行库存移动/合并/出售/装备结算。
- 提供 `owns_bag` 或原生物品识别接口。

UI 拖拽/使用/出售/丢弃一律调用 `inventory.action`。

## 9. 错误语义

| 场景 | 状态 |
|---|---|
| 句柄/槽已变 | `kStale` |
| 目标占用且不可合并 | `kConflict` |
| 容量不足 | `kFull` |
| 空槽 | `kEmpty` |
| 任务袋 | `kTaskBagExcluded` |
| 介质不支持 | `kUnsupported` |
| 补偿失败 | `kRollbackFailed` |

## 10. 验证

1. 四方向移动：原版↔C18、原生↔扩展。
2. 合并：部分堆叠、上限截断、材料操作不合并。
3. 使用/消耗：原生与扩展各一例。
4. 出售：成功、加钱失败回滚、回滚失败保留意图。
5. 装备/镶嵌：原生、扩展各一例。
6. 失败注入：目标占用、容量不足、句柄过期、payload 非法。
