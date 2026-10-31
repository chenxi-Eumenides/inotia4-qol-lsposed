# core.item 三插件架构（方案 A）

> 状态：设计稿
> 决策：采用方案 A —— **item 动作调用 inventory 存储**；inventory 不反向调用 item 动作。
> 前提：不保留旧 capability/旧 HTTP/旧 `QolBagSource`/旧 `inventory.action` 兼容。
> 存储细节：沿用 `bag-system-design.md` 的“原生 item 指针槽表”模型；其 §0 指针表例外仍以阶段 0 实验为准。若阶段 0 失败，只替换 inventory 内部存储（UID/payload + 瞬态物化），三插件边界与调用链不变。
> 硬约束：见 [`hard-constraints.md`](hard-constraints.md)。

## 1. 插件与共享模块

| 名称 | 类型 | 定位 |
|---|---|---|
| `core.inventory` | 插件 | 被动库存/袋源层：槽表、地址、查询、取放、移动、保存、S2、存储 hooks、inventory HTTP |
| `core.item` | 插件 | 物品领域层：item facts、物品动作、动作 hooks、shop、item HTTP |
| `extension_bag` | 插件 | 扩展袋 UI/投影层：UI 投影、投影重绘、拖拽合并、扩展袋页签/展示 |

不再拆 `core.item_data`。item_type/stackable/category 映射/禁售/报价等物品事实全部留在 `core.item`；`core.inventory` 不依赖 item 或 item_data，只通过 host 的 `QolItemPort` 读取原生 item 的 category/count，通过 host `QolItemCountApi` 做 S2 编解码。

## 2. 依赖关系

```text
core.item   →   core.inventory
      ↑               ↑
      └── extension_bag ──┘
```

- `core.item` → `core.inventory`：动作需要 query/take/place/move。
- `extension_bag` → `core.inventory`：槽表/查询/投影读取。
- `extension_bag` → `core.item`：UI 触发 use/equip/sell/动作，以及读取 item facts。
- `core.inventory` 不依赖 `core.item`；只依赖 host 的 item/count 端口。
- `core.item` 不依赖 `extension_bag`；写后刷新由 inventory 负责（原生 refresh + 可选投影重绘回调），item 只调用 inventory 的刷新入口。
- `extension_bag` 不拥有原生刷新 hook；它向 inventory 注册 `ProjectionRefreshPort`，只负责投影重绘。

依赖规则：

1. `core.inventory` 只认识槽表、`SlotRef`、`ItemKey`、host item/count 字段，不认识 item_type/stackable/use/equip/sell 业务。
2. `core.item` 只认识物品事实/动作，不直接读写槽表；所有存储通过 `BagAccess`。
3. `extension_bag` 只做 UI/投影/拖拽；存储走 inventory，动作走 item。
4. 不允许 item ↔ extension_bag 双向 hard require；原生刷新归 inventory，EB 只注册投影重绘。
5. 一个 hook 只能有一个 owner；跨插件能力通过 interface，不通过直接符号。

## 3. 权责矩阵

| 能力 | core.inventory | core.item | extension_bag |
|---|---|---|---|
| 槽表/袋源注册 | 拥有 | 不接触 | 只提供扩展表/persist_key |
| 地址/身份 `SlotRef`/`ItemKey` | 拥有 | 使用 | 使用 |
| 原生 item 指针/UID/句柄映射 | 拥有 | 使用 | 使用 |
| 查询/计数/空槽 | 拥有 | 通过 BagAccess | 通过 inventory |
| 移动/交换/消耗/移除/保存 | 拥有 | 请求 | 请求 |
| S2/CountCodec | 拥有（host `QolItemCountApi`） | 通过 inventory | 通过 inventory |
| item_type/stackable/禁售/报价 | 不判断；只读 host category/count | 拥有/实现 | 读取 core.item |
| use/equip/unequip/sell/put_jewel/enchant | 执行存储部分 | 拥有动作规则 | UI 触发 |
| create/place/discard_handle | 落位/清槽/释放 | 创建规格/detached 所有权 | — |
| 动作 hooks | 不安装 | 拥有 | 不安装 |
| 存储 hooks | 拥有 | 不安装 | 不安装 |
| UI 投影/刷新/拖拽合并 | 拥有原生 refresh hook + `request_refresh`；调用投影重绘回调 | 不拥有 | 拥有投影显示/投影重绘/拖拽合并 |
| shop/价格/购买 | 提供落位/扣除 | 拥有 | 不拥有 |
| inventory HTTP | 拥有 | — | — |
| item/shop HTTP | — | 拥有 | — |
| 存档 sidecar | 拥有 | 不接触 | 提供扩展表/persist_key |
| `item.state`/库存 JSON | 拥有 | 可薄代理或废弃 | — |

## 4. Hook 归属（28 个 inventory hook + 扩展 hooks）

### 4.1 core.inventory（13）

- INVEN_FindItem
- INVEN_HaveItem
- INVEN_GetItemCount
- ITEM_GetCumulateCount
- INVEN_IsHavingEmptySlot
- INVEN_ConsumeItem
- INVEN_RemoveItem
- INVEN_RemoveItemData
- INVEN_SaveItem
- INVEN_FindSaveSlot
- INVEN_SaveItemDirect
- INVEN_MoveItem
- UIEquip_RefreshItemArea

### 4.2 core.item（14）

- CHAR_EquipItemFromInvenToSlot
- CHAR_UnequipItemToInven
- UIEquip_ButtonEquipExe
- UIEquip_ButtonUnequipExe
- UIEquip_ButtonDestroyExe
- UIEquip_OKConfirmUseItem
- UIEquip_OKDestroyItem
- UIEquip_ButtonUseMercenarySealExe
- ITEMSYSTEM_PutJewel
- ITEMSYSTEM_Divide
- ITEMSYSTEM_MakeItem
- CHAR_ProcessShortcut（诊断）
- SHORTCUT_GetState（诊断）
- CHAR_UseItemEx（诊断）

### 4.3 extension_bag（1 + 扩展显示/合并）

- UIEquip_EquipControlEventProc

以及扩展袋自己的显示/合并入口（不属于 core.item 的 28 条）：

- ControlItem_SetItem / ControlItem_GetItem
- INVEN_GetBagSize（投影作用域）
- MOVE_MERGE_GOT（拖拽合并）
- 扩展袋页签/详情/装备页投影

规则：

- 一个 wrapper 只归一个插件；需要另一侧能力时通过 interface 调用。
- 存储 hook 不调用 item 动作；动作 hook 调用 inventory `BagAccess`。
- extension_bag 的 UI hook 可以调用 item 动作和 inventory 查询；item 不调用 extension_bag。

## 5. 调用链

### 5.1 原生库存查询（无 item 参与）

```text
game
  → core.inventory hook (FindItem/HaveItem/GetItemCount/IsHavingEmptySlot/Cumulate)
    → InventoryReader
      → BagTableRegistry → NativeTable/ExternalTable
        → host QolItemPort / QolItemCountApi（category/count/S2）
  → 返回原 ABI 结果
```

- inventory 不判断 item_type/stackable；这些属于 core.item。
- 查询只需原生 item 的 category/count；S2 只需 host count API。

### 5.2 原生库存写/移动/保存（无 item 参与）

```text
game
  → core.inventory hook (Consume/Remove/RemoveItemData/SaveItem/FindSaveSlot/SaveItemDirect/MoveItem)
    → InventoryExecutor
      → 预检（来源/容量/任务袋/ItemKey）
      → 一次写目标槽 / 原生 NativeActionPort
      → 清源槽（同一线程、指针写）
    → 返回原 ABI 结果
```

- 跨表移动：先预检 → 写目标 → 清源；目标写失败源不变。
- 没有 pending/rollback/journal；保存 token 只在产品要求“原生满时进外部表”时启用。

### 5.3 物品动作（方案 A 的核心）

```text
game / extension_bag UI
  → core.item action hook
    → ItemAction
      → BagAccess.locate(ptr) / query/take/place/move（core.inventory）
        → InventoryReader/Executor
          → 槽表 / NativeActionPort
      → 原生动作（use/equip/sell/...）
      → BagAccess 清理/落位（如需要）
      → 成功
  → 可选：inventory.request_refresh()
      → 原生 refresh（backup + shortcut）
      → 已注册的 ProjectionRefreshPort（extension_bag）→ 投影重绘
  → 返回原 ABI / HTTP 结果
```

关键：

- item 动作是调用方；inventory 是被调用的存储层。
- inventory 不反向调用 item 的 use/equip/sell。
- 物品动作失败时，源槽不变；成功后才清槽/落位。
- 部分数量、S2 写回由 inventory 的 CountCodec/Executor 完成；item 只表达“取多少/放哪”。

#### 5.3.1 控件指针 → SlotRef

item 拥有的 5 条 UI 动作 hook（ButtonEquip/ButtonUnequip/OKConfirmUse/ButtonUseMercenarySeal/ButtonDestroy）拿到的是控件/物品指针，不是 `SlotRef`。解析由 inventory 提供：

```cpp
// BagAccess 增加
std::optional<LocatedItem> locate(void* item_ptr) const;
// LocatedItem { SlotRef ref; ItemKey key; }

Status take_by_pointer(void* item_ptr, int32_t count, ...);
Status place_by_pointer(...);   // 如动作需要
Status remove_by_pointer(...);
```

- 控件指针 → 槽位/身份是 inventory 的槽表查找，不是 item 的业务逻辑。
- item 先 `locate`，再按 `SlotRef + ItemKey` 做动作；找不到时按原版路径/fail-closed。
- 指针表模型下外部物品也是原生对象，因此同一套 `locate` 覆盖原生/外部表；不再需要 EB 的 projection 尾部回调。
- `locate` 失败（投影副本/已释放/未知对象）不得猜测槽位；直接拒绝或透传原版。

### 5.4 UI 投影/刷新

```text
game UI
  → extension_bag display hook (ControlItem_SetItem/GetItem、INVEN_GetBagSize)
    → ProjectionContext
      → core.inventory query/slot table（读取外部表）
      → core.item item facts（类型/图标/详情基础）
  → 返回控件 item / 容量
```

- 投影只替换 UI 读取，不写 `INVEN_pItem`、不写容量字。
- 投影期间写动作命中窗口 bag 时，extension_bag/inventory 必须拒绝或重定向到真实表。
- `UIEquip_RefreshItemArea` 归 inventory：inventory 调 backup 原生刷新，再调用已注册的 `ProjectionRefreshPort`（extension_bag）做投影重绘。
- item 只调用 inventory 的 `request_refresh()`；不直接调用 extension_bag。
- 快捷键刷新是数据就绪 workaround：先归 inventory，等找到“首次刷新前 bag 已 ready”的版本后删除。

### 5.5 拖拽合并

```text
game
  → extension_bag MOVE_MERGE_GOT hook
    → 解析 source/target 控件与 SlotRef
    → 合并规则（extension_bag）
    → core.inventory move/merge（存储）
      → NativeActionPort / 外部表 set
    → 刷新投影
```

- 拖拽 gate、整堆合并、投影显示归 extension_bag。
- 存储写入、预检、源/目标槽更新归 inventory。
- 不拆成部分堆；不引入 pending。

### 5.6 存档/读档

```text
game SAVE
  → core.inventory save hooks
    → 原生袋：原生 SAVE
    → 外部表（extension_bag 提供的表）：
      → SaveCoordinator 遍历非空槽
      → 原生 SAVE_SaveItem 取 payload
      → 写 persist_key 对应 sidecar（原子写）
  → 返回

game LOAD
  → core.inventory save hooks/coordinator
    → 原生袋：原生 LOAD
    → 外部表：sidecar → SAVE_LoadItem → 临时表 → 整体替换
    → UID 去重（与原生袋冲突时保留原生）
```

- item/extension_bag 不实现存档格式；extension_bag 只提供表/persist_key 和 UI。
- 原生袋由游戏保存；外部表由 inventory 批量 sidecar。
- 读档后外部表对象重建；旧指针/UID 失效。

### 5.7 HTTP

```text
inventory HTTP:
  request thread → core.inventory handler → ticket → main thread InventoryReader/Executor
  → ticket result

item HTTP:
  request thread → core.item handler → ticket → main thread ItemAction
    → BagAccess → core.inventory
  → ticket result

shop:
  request thread → core.item shop handler → ticket → ItemAction/Shop
    → BagAccess → core.inventory
  → ticket result
```

- GET 只读已发布快照；POST 在主线程执行。
- inventory 写与 item 写分别持有 ticket handler，不共享旧的 `ItemWritePayload`。

## 6. 数据流

- 层间只传：原生 ABI 参数、`SlotRef`、`ItemKey`、`ItemView`（只读展示）、item facts。
- 不定义/不流转 ItemData；原生 item 对象是物品内容真源。
- 原生 item 指针只存在于槽表；不复制到 HTTP/快照/跨帧身份。
- payload 只在存档/读档边界出现；物品移动不序列化。
- `ItemView` 只给 UI/HTTP 展示，不参与动作/回写。
- S2 数量只在 inventory 的 CountCodec 内转换；item 只表达逻辑数量。

## 7. 能力/HTTP 归属

### 7.1 能力

| 能力 | owner |
|---|---|
| `item.facts` | core.item |
| `inventory.store` / `inventory.query` | core.inventory |
| `inventory.bag_source` | core.inventory |
| `inventory.slot` | core.inventory |
| `inventory.refresh` | core.inventory（原生 refresh；extension_bag 注册投影重绘回调） |
| `inventory.projection` | extension_bag |
| `move_merge.control` | extension_bag |
| `item.actions` | core.item |
| `item.state` / inventory JSON | core.inventory |
| `shop` | core.item |

### 7.2 HTTP

- core.inventory：库存查询、袋信息、单槽、money、items、move_item、set-slot/OP 存储类。
- core.item：use_item、discard_item、sell_item、equip/unequip、put_jewel、enchant、shop、OP add/set-equip。
- 具体路径可重新命名，不保留旧前缀兼容；每个插件持有自己的 ticket handler。

## 8. 失败与生命周期

- 所有写操作：预检 → 单次写 → 清源；失败即不写，源槽不变。
- 无运行期回滚、pending、tentative、journal。
- 源/目标跨表移动在目标写成功后才清源；清源是指针写，不调用可能失败的业务函数。
- 动作失败/身份过期：`Stale`/`InvalidRef`/`Empty`/`Conflict`/`Full`/`TaskBagReadOnly`/`Unsupported`/`Internal`。
- 原生 ABI 不暴露插件错误码；HTTP ticket 用业务状态。
- 停用：先拆投影/刷新，再注销外部表；hook 透传。
- 读档/换图/清池：外部表清空并从 sidecar 重建；旧指针/UID 失效。

## 9. 硬约束与风险

硬约束见 `hard-constraints.md`，本设计额外风险：

1. **指针槽表例外**：`bag-system-design.md` §0 的长期指针例外仍待阶段 0 批准；失败则 inventory 内部降级为 UID/payload + 瞬态物化，三插件边界不变。
2. **Hook 归属**：28 条 claim 已按 13/14/1 分配；一个 wrapper 不能跨插件，跨侧调用走 interface。
3. **item facts 依赖**：inventory 不依赖 core.item；它只读 host item 字段和 count API。只有未来做“外部表自动堆叠合并”时才需要额外的 StackRules 端口，不能把 item_type/stackable 塞回 inventory。
4. **刷新**：item 不直接调用 extension_bag；inventory 持有原生 refresh（backup+shortcut），extension_bag 只注册 `ProjectionRefreshPort` 做投影重绘。
5. **扩展表/存档**：extension_bag 提供表/persist_key，inventory 写 sidecar；不要把 sidecar 写回 item 或 UI。
6. **HTTP ticket**：inventory/item 各自 ticket；不要共享旧 `ItemWritePayload`。
7. **任务袋/原生索引**：inventory 统一校验；item/extension_bag 不直接传自定义袋号给原生。
8. **行为回归**：无兼容不等于无回归；逐 hook/逐路由/逐动作取证。

## 10. 落地顺序

1. **inventory 存储核心**：BagTableRegistry/Native/External/Reader/Executor/NativeActionPort；fake 表测试。
2. **inventory hooks/HTTP/存档**：13 条存储/refresh hook、查询/移动 HTTP、SaveCoordinator、S2。
3. **item 动作**：ItemAction + BagAccess；14 条动作 hook；shop/item HTTP；item facts 留在 core.item；删除旧 `inventory.action`。
4. **extension_bag UI**：投影/投影重绘/拖拽合并/页签；通过 inventory 查询和 item 动作；注册 ProjectionRefreshPort。
5. **集成验收**：check_arch/check_http_routes、host tests、构建、真机逐项验收。

## 11. 非目标

- 不保留旧 capability/旧 HTTP/旧 `QolBagSource`/旧 `inventory.action` 兼容。
- 不做运行期事务/回滚/pending/journal。
- 不把 UI 投影/投影重绘/拖拽合并放回 item；原生 refresh 归 inventory。
- 不让 inventory 反向调用 item 动作。
- 不把 item facts 复制到 inventory；inventory 只读 host item 字段，item facts 留在 core.item。
- 不把原生 item 指针写进 HTTP/快照/跨帧身份。

## 12. 一句话

**inventory 管“物品在哪、怎么存取、原生 refresh”，只依赖 host item/count 端口；item 管“物品是什么、动作怎么做”，item facts 留在 core.item；extension_bag 管“怎么显示、投影重绘、怎么拖拽”。item 调 inventory，inventory 不调 item 动作、不依赖 core.item；刷新由 inventory 负责，存档/槽表/原生 ABI 全在 inventory。**
