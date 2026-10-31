# core.item 重构：item_data / inventory / item 三层

> 状态：设计稿
> 范围：完全重构 core.item，不保留旧契约/旧 capability/旧 HTTP 兼容。
> 结论：可行；建议并行新建插件，完成后整体切换，不做兼容 shim。

## 1. 现状与问题

- core.item 约 66 个源/头文件、1.1 万行；28 条 inventory hook claim、21 条 HTTP 路由、8 个 provides、6 个 ports。
- 背包/库存相关代码分散在 `bags/`、`usecases/read_inventory`、`hooks/inventory_hooks`（2799 行）、`http/query_handler`、`http/write_results`（1122 行）、`shop/`、`api/state` 等。
- item 动作（use/equip/sell）与库存存储互相调用：item_action/sell/item_slot 直接访问袋源；inventory hooks 又依赖 item facts/port；没有单一 ItemData 表示。
- 继续在 core.item 内加层会让 hook、HTTP、source、item 动作继续耦合。

## 2. 目标架构

三个模块，依赖单向：

```text
core.item_data        ItemData / Payload / ItemCodec / item facts
      ↑
core.inventory        payload-only 袋源 + ItemData 存储/查询/移动 + inventory hooks/HTTP
      ↑
core.item             物品动作/装备/使用/出售/UI hooks + item HTTP/shop（只依赖 item_data + inventory）
```

- `core.item_data` 不依赖存储，不知道袋源。
- `core.inventory` 只处理 ItemData 存储；不知道 use/equip/sell 的业务。
- `core.item` 只处理物品动作；通过 `inventory.store` 访问 ItemData。
- 没有任何循环依赖；codec 只在 `core.item_data` 一份。

## 3. 数据模型

### 3.1 Payload

Payload 是游戏原生序列化数据，只在边界出现：

```cpp
struct Payload {
    uint32_t size;
    uint8_t bytes[256];
};
```

出现位置：
- 袋源 `item_at` / `put` / `take`（原生或外部袋的存储边界）。
- 游戏原生对象 materialize/dematerialize。
- 存档/读档。

不出现在：
- `core.inventory` 的公开 API。
- `core.item` 的动作逻辑、hook 业务逻辑。
- UI/HTTP 的 ItemData 表示。

### 3.2 ItemData

```cpp
struct ItemData {
    int32_t item_id;
    int32_t count;
    // 解析后的属性字段由 item_data 定义；不再携带 raw payload。
};

struct BagInfo {
    int32_t capacity;
    int32_t occupied_slots;
};

struct ItemHandle {
    uint32_t slot;
    uint32_t generation;
};
```

- 插件内部流通的全部是 `ItemData`。
- 类型（实例型/数量型、可堆叠）由 item facts 按 item_id 判定，不在 inventory 或 source 中判断。
- ItemData 不持久化；持久化的是 Payload。

```cpp
struct ItemInfo {
    int32_t item_id;
    int32_t total_count;
    std::vector<ItemData> items;  // 每个实际堆；不含来源
};

// 不透明放置令牌；hook 适配层可用，item 动作不读取。
struct Placement {
    uint64_t token;
};

struct ItemPlacement {
    ItemData item;
    std::optional<Placement> target;  // 空 = 首个空位
};

enum class Status {
    kOk,
    kNotFound,
    kNotEnough,
    kNoSpace,
    kInvalid,
    kSourceRejected,
};
```

### 3.3 ItemCodec

`core.item_data` 提供唯一 codec：

```cpp
struct ItemCodec {
    bool decode(const Payload& payload, ItemData* out) const;
    bool encode(const ItemData& item, Payload* out) const;
    bool set_count(ItemData* item, int32_t count) const;
};
```

- `decode`：payload → ItemData；读档、source `item_at` 使用。
- `encode`：ItemData → payload；存档、source `put` 使用。
- `set_count`：只改 ItemData.count，并在 encode 时写入 payload 的数量位段。
- 任何层不得自行解析 payload 的 item_id/count/属性。

## 4. 袋源契约（payload-only）

源只存 payload，不提供 item_id/count，不解析、不校验、不回滚：

```cpp
struct BagSource {
    int32_t space_id;
    void* ctx;

    int32_t (*bag_count)(void* ctx);
    bool (*bag_info)(void* ctx, int32_t bag, BagInfo* out);
    bool (*item_at)(void* ctx, int32_t bag, int32_t slot, Payload* out);
    bool (*put)(void* ctx, int32_t bag, int32_t slot, const Payload& payload);
    // 仅整件取出；部分数量由 inventory 解码/改写/put 完成。
    bool (*take)(void* ctx, int32_t bag, int32_t slot, Payload* out);
};
```

- `bag_info` 只返回 capacity/occupied；occupied 按 payload 是否存在统计。
- 源不知道 `item_id`/`count`/类型/使用/装备。
- 源适配器只负责自己的存储介质；原生源负责 serialize/load，外部源只存 payload。
- 部分消耗：inventory `decode` → 改 count → `encode` → `put` 回同一槽；整件取出用 `take`。
- 读档/存档：源保存 raw payload；inventory 按需 decode/encode；不需要单独重建 count。

## 5. core.inventory

### 5.1 职责

- 袋源注册/注销/枚举（native + external）。
- 所有袋源的 payload ↔ ItemData 转换（调用 item_data codec）。
- ItemData 查询、取物、放物、移动/交换、部分消耗、合并、存档/读档边界。
- inventory 快照/刷新。
- 纯库存 hooks 和 inventory HTTP。
- item handle → ItemData/位置的材料化映射（源不再提供 identify）。

### 5.2 对外 API（草案）

```cpp
struct InventoryStore {
    // 查询：按 item_id 聚合所有源；返回 ItemData，不返回来源。
    std::optional<ItemInfo> query_item(int32_t item_id) const;

    // 取物：跨源聚合；总量不足整单拒绝。
    Status take_item(int32_t item_id, int32_t count,
                     std::vector<ItemData>* out,
                     const ItemData* match = nullptr);

    // 放物：按 ItemData 写回；批量预检，失败整单拒绝。
    Status put_items(const std::vector<ItemPlacement>& items,
                     std::vector<Placement>* out = nullptr);

    // 位置/空位/能力查询。
    bool has_empty_slot() const;
    bool can_put(const std::vector<ItemPlacement>& items) const;

    // hook 适配层：handle → ItemData/位置；不属于业务动作。
    std::optional<ItemData> resolve_handle(ItemHandle handle) const;
    std::optional<Placement> find_slot(const ItemData& item) const;
};
```

- 所有参数/返回值是 ItemData；没有 Payload、BagRef、space_id 暴露给上层。
- `match` 用于实例型 A 的特定实例，按解析后的 ItemData 属性匹配；nullptr = 任意。
- `Placement` 只给 hook 适配层使用；item 动作不读取。
- 部分消耗/合并由 inventory 内部 decode/encode；item 动作只表达“取/放”。

### 5.3 inventory hooks

纯库存/查询/保存类 hook 归 core.inventory，例如：

- INVEN_FindItem / HaveItem / GetItemCount
- ITEM_GetCumulateCount / IsHavingEmptySlot
- INVEN_ConsumeItem / RemoveItem / RemoveItemData
- INVEN_SaveItem / FindSaveSlot / SaveItemDirect
- INVEN_MoveItem（存储移动）
- UIEquip_RefreshItemArea（库存刷新）

同时把 native S2 计数写回、flat 袋号解码、item handle 材料化映射放在 inventory 内部。

## 6. core.item

### 6.1 职责

- 使用/装备/卸下/出售/镶嵌/拆分/制作/丢弃等动作。
- item 动作相关 hooks：装备、使用、销毁、佣兵徽章、镶嵌、拆分、制作、UI 门禁、快捷键诊断。
- item HTTP/商店/UI projection/merge control。
- 通过 `inventory.store` 取/放 ItemData；不直接访问袋源。

### 6.2 item 动作 API（草案）

```cpp
struct UseContext;
struct EquipTarget;

struct ItemActions {
    Status use(ItemData item, UseContext ctx);
    Status equip(ItemData item, EquipTarget target);
    Status unequip(ItemData item);
    Status sell(ItemData item, int32_t count);
    Status put_jewel(ItemData jewel, ItemData target);
    Status create(int32_t item_id, int32_t count, ItemData* out);
    Status place(ItemData item, Placement target);
    Status discard(ItemData item);
};
```

- `use/destroy/consume` 的业务判断在 item/use 层；库存取出由 inventory 完成。
- 装备/出售等需要回填物品的，调用 `inventory.put_items`；失败补偿由 item 动作显式处理。
- 不再有 `inventory.action` 大杂烩接口；item 动作和 inventory 存储分开。

## 7. Hook 归属

规则：一个 wrapper 只属于一个插件；如果同时需要另一侧能力，通过 capability 调用，不拆 wrapper。

| 归属 | hooks |
|---|---|
| core.inventory | INVEN_FindItem、HaveItem、GetItemCount、ITEM_GetCumulateCount、IsHavingEmptySlot、ConsumeItem、RemoveItem、RemoveItemData、SaveItem、FindSaveSlot、SaveItemDirect、MoveItem、RefreshItemArea |
| core.item | CHAR_EquipItemFromInvenToSlot、CHAR_UnequipItemToInven、UIEquip_ButtonEquipExe、ButtonUnequipExe、ButtonDestroyExe、OKConfirmUseItem、OKDestroyItem、ButtonUseMercenarySealExe、ITEMSYSTEM_PutJewel、ITEMSYSTEM_Divide、ITEMSYSTEM_MakeItem、EquipControlEventProc、3 个诊断 hook |
| 共享 | 无；需要跨侧时调用 capability |

- 28 条 claim 重新分配到两个插件；旧 `kInventoryHookClaims` 表删除/重建。
- native S2 写回、flat 袋号、item handle 映射归 inventory；item 动作通过 inventory 查询/取放。
- `INVEN_MoveItem` 的 S2 计数写回归 inventory；drag-merge/projection 仍由 core.item 提供。若 MoveItem wrapper 需要 merge 决策，inventory 通过 `item.merge` capability 调用，不拆 wrapper。
- 任何 wrapper 不得同时在两个插件注册。

## 8. HTTP 与能力

- 不保留旧路由/旧 capability ID；重新定义：
  - `core.item_data`：`item.data`（Payload/ItemData codec）、`item.facts`。
  - `core.inventory`：`inventory.store`（查询/取/放/移动/保存）、`inventory.bag_source`（外部源注册）、`inventory.snapshot`/`inventory.state`/`inventory.refresh`。
  - `core.item`：`item.actions`（use/equip/sell/...）、`item.projection`/`move_merge.control`。
- HTTP 路由按 owner 重新分组：
  - `/api/inventory/*`：查询、袋信息、移动、保存、消耗。
  - `/api/item/*`：使用、装备、卸下、出售、镶嵌、制作、商店。
- 写结果 ticket handler 按插件各自持有；不再共享 `ItemWritePayload`。
- 外部插件（extension_bag/extra_bag）按新契约重写适配器；旧 `QolBagSource`/`QolBagTransfer` 删除。

## 9. 迁移阶段

不做兼容 shim，但建议并行新建，避免一次性替换导致不可调试：

1. **阶段 0：契约与 codec**
   - 新建 `core.item_data`：Payload、ItemData、ItemCodec、item facts。
   - 单元测试覆盖 decode/encode/set_count、读档/存档、类型 A/B、异常 payload。
2. **阶段 1：inventory 核心**
   - 新建 `core.inventory`：BagSource 注册、native source、query/take/put/move、ItemData 快照。
   - 用 fake source + native source 做 host 测试；先不接 hook。
3. **阶段 2：inventory hooks/HTTP**
   - 迁移纯库存 hooks、inventory 查询/移动 HTTP、S2 写回、flat 袋号、handle 映射。
   - 旧 core.item 的对应 hook/路由停用；两插件不同时 claim。
4. **阶段 3：slim core.item**
   - 保留/重写 item 动作、UI hooks、item HTTP/shop；通过 inventory.store 取放。
   - 删除旧 `inventory.action` 大接口和旧 `bags/`、`orchestration/`、`inventory/` 残留。
5. **阶段 4：验收**
   - host tests 重建；check_arch/check_http_routes；构建；真机验收（背包读写、移动/交换、消耗、保存/读档、装备/使用/出售、RB 源）。
   - 旧 core.item target 移除，插件清单更新。

## 10. 风险

1. **Hook 归属**：28 条 claim 混有原生/扩展路径；一个 wrapper 只能一个宿主，迁移时要保持 fail-closed 与 claim timeout。
2. **native S2**：consume/move/save/divide/remove_data 依赖 backup 前后采样；S2 逻辑必须随 inventory hooks 一起迁移，不能留在 item 动作里。
3. **item handle 映射**：源不再提供 identify；inventory 需要维护 handle ↔ ItemData/位置的材料化映射，失效语义要明确。
4. **HTTP/ticket**：旧路由和写结果 handler 混在 `/api/item/inventory/*`；拆分后路由 owner、op 门禁、ticket handler 都要重新划分。
5. **测试**：至少 5 个测试 target 直接列 core.item 源文件；拆分后需重建 target/stub，不能只改 CMake。
6. **行为回归**：无兼容不等于无回归；必须有逐 hook/逐路由的验收矩阵，尤其是扩展袋/原生袋交叉移动、读档、出售回滚。
7. **插件生命周期**：inventory 必须在 item 之前 ready；hook 注册/注销顺序、disable/deactivate 要重新定义。

## 11. 非目标

- 不在第一阶段拆 shop/sell/use 的细粒度动作。
- 不保留旧 capability/旧 HTTP/旧 `QolBagSource` 兼容。
- 不做跨插件事务；失败补偿留在 item 动作/hook。
- 不把 item 定义表复制到 inventory；item facts 只在 item_data。
- 不在同一个 wrapper 内同时注册两个插件。

## 12. 结论

可行。真正的工作量不在“搬 bags 目录”，而在：
1. 先把 ItemData/codec 抽成 `core.item_data`；
2. 把 payload-only 袋源 + ItemData 存储做成 `core.inventory`；
3. 再把 slim core.item 的动作/hook/HTTP 接回 inventory。

按阶段并行新建、最后切换，比在旧 core.item 内继续加层更可控。
