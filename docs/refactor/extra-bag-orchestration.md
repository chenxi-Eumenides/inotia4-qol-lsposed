# extra_bag 多袋源编排层设计

> 状态：设计稿
> 范围：多个 `extra_bag.storage` 袋源之间的读取、搜索、移动和交换。
> 前提：单线程调用；不做校验、不做回滚、不做持久化；不包含背包装备编排。

## 1. 目标与非目标

### 目标

- 用一个统一地址定位不同袋源中的物品。
- 读取指定位置对应的物品和袋信息。
- 按 `item_id` 或自定义谓词搜索物品及其位置。
- 在袋源内部、跨袋、跨袋源之间移动整件物品。
- 在两个已占用位置之间交换物品。
- 所有失败前置检查都在写入前完成，避免单线程下的部分写入。

### 非目标

- 不处理 `item_backpack_at` / `set_item_backpack` / `clear_item_backpack`。
- 不做数量拆分、堆叠合并、部分移动。
- 不校验 `payload`、`count`、`item_id` 是否合法。
- 不做持久化、UI、HTTP、使用、装备、出售、合成。
- 不建立全局物品索引或缓存。

## 2. 当前接口基线

编排层只依赖 `extra_bag.storage` 的普通物品接口：

| 接口 | 作用 |
|---|---|
| `bag_count()` | 袋源当前袋数 |
| `bag_info(bag, out)` | 容量、已占用槽数 |
| `item_at(bag, slot, out)` | 读取指定槽 |
| `put(bag, slot, item)` | 整体覆盖指定槽 |
| `take(bag, slot, out)` | 移除并返回整件物品 |

关键语义：

- `put` 无返回值，按契约视为成功写入；目标已有物品时直接覆盖。
- `take` / `item_at` 用 `bool` 表示是否存在。
- 越界 bag/slot 在 source 内部按空处理。
- `bag_info.capacity` 由该袋当前装备的物品背包决定；编排层只把它作为有效槽位范围读取，不修改装备。
- 当前接口没有部分数量取放，因此移动和交换都以整件物品为单位。

## 3. 地址模型

```cpp
struct SourceId {
    uint32_t value;
};

struct BagId {
    SourceId source;
    int32_t bag;
};

struct BagRef {
    SourceId source;
    int32_t bag;
    int32_t slot;
};
```

- `SourceId` 是稳定标识，建议使用插件注册 ID 或固定常量。
- `BagId` 定位一个袋。
- `BagRef` 定位普通物品槽；不用于物品背包装备位。
- 物品背包装备位不进入本编排层的地址空间。

## 4. 袋源注册表

```cpp
bool register_source(SourceId id, const QolExtraBagApi* api);
void unregister_source(SourceId id);
uint32_t source_count() const;
```

规则：

- 一个 `SourceId` 对应一个 `QolExtraBagApi`，重复注册拒绝。
- 按注册顺序保存，搜索和遍历顺序固定。
- `QolExtraBagApi` 指针在注册期间必须有效。
- 单线程，不加锁，不做引用计数。
- 注册和注销只能在没有进行中的编排操作时调用。
- 注销后不得再使用该 source 的 `BagRef`。

内部结构：

```cpp
struct SourceEntry {
    SourceId id;
    const QolExtraBagApi* api;
};

std::vector<SourceEntry> sources;  // 注册顺序
```

## 5. 读取与位置信息

```cpp
struct PositionInfo {
    BagRef ref;
    QolExtraBagItem item;
    QolExtraBagInfo bag;
};

std::optional<QolExtraBagItem> item_at(BagRef ref) const;
std::optional<QolExtraBagInfo> bag_info(BagId id) const;
std::optional<PositionInfo> position_info(BagRef ref) const;
```

行为：

- `item_at` 找不到 source、袋越界或槽为空时返回 `nullopt`。
- `bag_info` 找不到 source 或袋越界时返回 `nullopt`。
- `position_info` 同时读取物品、容量和占用槽数；物品不存在时返回 `nullopt`。
- 每次调用都直接访问 source，不缓存跨操作的物品数据。

## 6. 搜索

```cpp
using ItemPredicate = bool (*)(const QolExtraBagItem& item, void* ctx);

std::optional<BagRef> find_first(int32_t item_id) const;
std::vector<BagRef> find_all(int32_t item_id) const;
std::vector<BagRef> find_if(ItemPredicate predicate, void* ctx) const;
```

搜索顺序：

```text
source 注册顺序
  → bag 0..bag_count-1
    → slot 0..bag_info.capacity-1
      → item_at
        → 匹配 item_id 或 predicate
```

规则：

- 只搜索普通物品槽。
- 容量为 0 的袋直接跳过。
- 空槽跳过；`item_at` 返回 `false` 不视为错误。
- `find_first` 返回第一个匹配位置；`find_all` 返回全部匹配位置。
- `find_if` 用于按 item_id 之外的字段匹配；`ctx` 由调用方维护。
- 搜索不包含物品背包装备位。
- 不建立索引；数据量为 `source × bag × capacity`，直接线性扫描。

## 7. 跨袋移动

```cpp
enum class MoveStatus {
    kOk,
    kSourceEmpty,
    kDestinationNotEmpty,
    kInvalidRef,
};

MoveStatus move(BagRef from, BagRef to);
```

语义：把 `from` 的整件物品移动到空槽 `to`。

```text
1. from == to → kOk（无操作）
2. source 或 bag 不存在 → kInvalidRef
3. item_at(from) 为空 → kSourceEmpty
4. bag_info(to) 不存在或 to.slot >= capacity → kInvalidRef
5. item_at(to) 非空 → kDestinationNotEmpty
6. take(from)
7. put(to, item)
```

要点：

- 目标必须为空；目标已占用时返回 `kDestinationNotEmpty`，调用方可改用 `swap`。
- 所有可失败检查都在 `take` 前完成。
- 单线程下，预检后 source 状态不会变化，因此 `take` 成功即是最终成功。
- 不合并、不拆分、不覆盖已有物品。
- 不做回滚；`put` 为 void，按契约视为成功。
- 支持同袋内移动、同源跨袋移动、跨源移动。

## 8. 跨袋交换

```cpp
enum class SwapStatus {
    kOk,
    kSourceEmpty,
    kDestinationEmpty,
    kInvalidRef,
};

SwapStatus swap(BagRef a, BagRef b);
```

语义：交换两个已占用位置的整件物品。

```text
1. a == b → kOk（无操作）
2. source 或 bag 不存在 → kInvalidRef
3. item_at(a) 为空 → kSourceEmpty
4. item_at(b) 为空 → kDestinationEmpty
5. take(a) → itemA
6. take(b) → itemB
7. put(a, itemB)
8. put(b, itemA)
```

要点：

- 两边都必须有物品；任一边为空时，调用方应改用 `move`。
- 预检通过后，单线程下两次 `take` 都会成功。
- 支持同袋不同槽、同源不同袋、跨源交换。
- 不合并、不拆分，不做回滚。

## 9. 状态与失败语义

建议统一使用编排层自己的状态，不引入事务：

```cpp
enum class OrchestrationStatus {
    kOk,
    kNotFound,
    kSourceEmpty,
    kDestinationEmpty,
    kDestinationNotEmpty,
    kInvalidRef,
    kBusy,
};
```

- `kInvalidRef`：source 未注册、bag 越界、slot 超出容量。
- `kSourceEmpty` / `kDestinationEmpty`：对应位置没有物品。
- `kDestinationNotEmpty`：移动目标已有物品。
- `kBusy`：重入保护生效；单线程下只应在 source 回调反向调用编排层时出现。
- 不提供回滚状态；所有写入前检查通过后，操作按顺序执行。

## 10. 单线程与重入

- 所有入口只在游戏主线程调用。
- 不使用锁、原子变量、异步任务或跨帧等待。
- 一次操作期间不得注册/注销 source。
- 编排层维护一个 `bool in_operation`；重入时返回 `kBusy`。
- source 的 `take` / `put` 回调不得反向调用编排层。

## 11. 不包含背包装备

编排层明确不处理：

- `item_backpack_at`
- `set_item_backpack`
- `clear_item_backpack`

物品背包装备仍由调用方直接使用 `extra_bag.storage` 的装备接口管理。因此：

- 搜索、移动、交换都不涉及装备位；
- `bag_info.capacity` 只读取，不修改；
- 装备变化会使容量变化，编排层每次操作都实时读取 `bag_info`，不缓存旧容量；
- 装备位上的物品不会出现在 `find_first` / `find_all` / `find_if` 结果中。

## 12. 复杂度与不变量

- 搜索复杂度：`O(source_count × bag_count × capacity)`；当前单源最多 5 × 16，成本可接受。
- 移动/交换：每次固定若干次 source API 调用，不随物品数量增长。
- 不变量：
  - 一个 `BagRef` 只对应一个普通物品槽。
  - 移动只写到空槽；交换只针对两个已占用槽。
  - 写入前完成全部可失败检查。
  - 不缓存跨操作的物品数据。
  - 不修改装备位，不修改容量。
  - 搜索顺序和遍历顺序固定为 source 注册顺序 → bag → slot。

## 13. 对外接口草案

```cpp
class MultiBagOrchestrator {
public:
    bool register_source(SourceId id, const QolExtraBagApi* api);
    void unregister_source(SourceId id);
    uint32_t source_count() const;

    std::optional<QolExtraBagItem> item_at(BagRef ref) const;
    std::optional<QolExtraBagInfo> bag_info(BagId id) const;
    std::optional<PositionInfo> position_info(BagRef ref) const;

    std::optional<BagRef> find_first(int32_t item_id) const;
    std::vector<BagRef> find_all(int32_t item_id) const;
    std::vector<BagRef> find_if(ItemPredicate predicate, void* ctx) const;

    MoveStatus move(BagRef from, BagRef to);
    SwapStatus swap(BagRef a, BagRef b);
};
```

调用方负责：

- 在游戏主线程调用；
- 保证 source 注册期间 API 指针有效；
- 在操作期间不注销 source；
- 不通过编排层处理物品背包装备。
