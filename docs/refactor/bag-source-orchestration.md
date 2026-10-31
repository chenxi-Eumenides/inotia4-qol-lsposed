# 袋源编排层设计（core.item）

> 状态：设计稿
> 基础：`docs/refactor/extra-bag-orchestration.md`（地址、注册表、搜索、move/swap、单线程、无校验、无回滚）
> 范围：core.item 对原生袋与注册袋源的统一存储编排；不包含 UI、装备、出售、S2 位域、持久化策略。

## 1. 目标与非目标

### 目标

- 用 `(space_id, bag, slot)` 统一定位任意袋源中的普通物品槽。
- 支持读取、按 category 搜索/计数、空槽查找、整件移动/交换、指定槽写入、整件取出、按数量移除。
- 覆盖 28 个 inventory hook 的袋存储需求；hook 只通过编排层访问注册源，不再直接调用源接口。
- 袋源保持简单：只负责自己的存储，不做 item payload 校验、不做事务、不做回滚。
- 编排层保持简单：线性扫描、写入前预检、无索引、无缓存、无锁、无异步。

### 非目标

- 不处理 `item_backpack_at` / `set_item_backpack` / `clear_item_backpack`。
- 不做数量拆分、堆叠合并、部分移动；移动和交换以整件物品为单位。
- 不做持久化、UI、HTTP、使用、装备、出售、合成。
- 不建立全局物品索引或缓存。
- 不提供事务、快照、多级回滚、引用计数、重入保护。

## 2. 源契约

### 2.1 数据模型

```cpp
struct BagItem {
    int32_t category;                 // 游戏物品 category；也是搜索/堆叠/存档域键
    int32_t count;                    // 以该字段为权威；payload 不承载数量语义
    uint32_t payload_size;            // <= 255
    uint8_t payload[256] = {};
};

struct BagInfo {
    int32_t capacity;                 // 有效槽位范围 [0, capacity)
    int32_t occupied_slots;
};

struct BagRef {
    int32_t space_id;                 // 0/1 原生；>=2 注册源
    int32_t bag;
    int32_t slot;
};

// 仅作为不透明身份；与游戏 handle 的转换由适配层完成。
struct ItemHandle {
    uint32_t slot;
    uint32_t generation;
};
```

### 2.2 源接口

```cpp
struct BagSource {
    int32_t space_id;
    void* ctx;

    int32_t (*bag_count)(void* ctx);
    bool (*bag_info)(void* ctx, int32_t bag, BagInfo* out);
    bool (*item_at)(void* ctx, int32_t bag, int32_t slot, BagItem* out);

    // 写原语。put 覆盖目标槽；take 取走整件；remove 按数量移除。
    bool (*put)(void* ctx, int32_t bag, int32_t slot, const BagItem* item);
    bool (*take)(void* ctx, int32_t bag, int32_t slot, BagItem* out);
    bool (*remove)(void* ctx, int32_t bag, int32_t slot, int32_t count);

    // 可选：hook 面能力，编排层不调用。缺失时对应 hook 按现状 fail-soft/fail-closed。
    bool (*identify)(void* ctx, ItemHandle handle, BagRef* out);
    bool (*use)(void* ctx, BagRef ref);
};
```

### 2.3 契约规则

1. `space_id` 稳定且唯一；0/1 为原生空间，>=2 为注册源空间。注销后 `BagRef` 失效，不得复用同一 `space_id` 表示不同源。
2. `count` 以 `BagItem.count` 为权威；源若在 payload 中编码数量，必须在 `put`/`take`/`remove` 边界自行同步。编排层只读写 `count` 字段。
3. `put` 覆盖目标槽；目标是否为空由编排层预检，源不做校验。
4. `take` 取走整件；空槽返回 false。
5. `remove(bag, slot, count)` 从当前物品扣除 `count`；`count >= 当前数量` 时移除整件，否则更新 `count` 与 payload。成功返回 true，源内部拒绝返回 false。
6. `BagInfo.occupied_slots` 只统计普通物品槽，不包含物品背包装备位。
7. 源不做 item payload 校验、不做事务、不做回滚；源内部若有持久化/所有权/编码逻辑，由源自己维护，编排层不感知。
8. 源回调不得反向调用编排层；违反约定属于源缺陷，应修改源，编排层不做兜底。

### 2.4 为什么保留 `remove` / `identify` / `use`

- `remove`：hook 需要从堆叠中扣除任意数量（consume=1、RemoveItemData 补扣）。若编排层用 `item_at` + `put` 读改写，源内部的数量编码、所有权、持久化、UI 刷新等语义会被绕过，等于把源实现细节复制进编排层。保留一个 `remove` 原语，源仍然是“单次存储操作”，不是事务。
- `identify`：hook 拿到的是游戏对象/handle，只有物化该物品的源知道它对应哪个槽。放到 core.item 适配层会复制源的句柄/物化表，并产生悬垂风险。
- `use`：使用是源物品的游戏动作，源已经持有物化对象和所有权；core.item 重新实现会重复源的内部流程。
- 这三个方法都是可选或单次调用，不引入校验/回滚/事务；编排层不依赖 `identify`/`use`。

## 3. 编排层 API

```cpp
namespace qol::features::core_item::orchestration {

enum class Status {
    kOk,
    kInvalidRef,           // 源未注册 / bag 越界 / slot >= capacity
    kSourceEmpty,          // 源位置无物品
    kDestinationNotEmpty,  // move/put_item 目标已占用
    kDestinationEmpty,     // swap 一侧为空
    kNotEnough,            // remove_count 数量不足（不写入）
    kSourceRejected,       // 源 put/remove 返回 false
};

using ItemPredicate = bool (*)(const BagItem& item, void* ctx);

class BagOrchestrator {
public:
    // 注册表。主线程；不在编排操作进行中调用。
    bool register_source(const BagSource& source);
    void unregister_source(int32_t space_id);
    uint32_t source_count() const;
    bool has_source(int32_t space_id) const;

    // 读。
    std::optional<BagItem> item_at(BagRef ref) const;
    std::optional<BagInfo> bag_info(int32_t space_id, int32_t bag) const;

    // 搜索 / 计数。遍历顺序：注册序 -> bag 0..count-1 -> slot 0..capacity-1。
    std::optional<BagRef> find_first(int32_t category) const;
    std::vector<BagRef> find_all(int32_t category) const;
    std::vector<BagRef> find_if(ItemPredicate pred, void* ctx) const;
    int32_t count_item(int32_t category) const;       // 饱和到 INT32_MAX
    int64_t count_empty_slots() const;
    std::optional<BagRef> find_empty() const;
    std::optional<BagRef> find_stack(int32_t category, int32_t incoming,
                                     int32_t stack_cap) const;

    // 写。全部先预检，再按顺序调用源原语；不做回滚。
    Status move(BagRef from, BagRef to);
    Status swap(BagRef a, BagRef b);
    Status put_item(BagRef to, const BagItem& item);
    Status take_item(BagRef ref, BagItem* out);
    Status remove_count(BagRef ref, int32_t count);
    int32_t remove_item(int32_t category, int32_t count);  // 返回实际移除量
};

}  // namespace
```

### 3.1 操作语义

**`item_at` / `bag_info`**

- 未注册 `space_id`、`bag < 0`、`bag >= bag_count`、`slot < 0`、`slot >= capacity` → 空/`nullopt`。
- 每次实时读 `bag_info`，不缓存容量。

**`find_first` / `find_all` / `find_if` / `count_item`**

- 只扫描普通物品槽；空槽跳过；capacity 为 0 的袋跳过。
- `count_item` 累加 `BagItem.count`，饱和到 `INT32_MAX`。

**`count_empty_slots` / `find_empty`**

- 空槽数按 `capacity - occupied_slots` 计算，结果小于 0 时按 0。
- `find_empty` 按遍历顺序返回首个 `item_at` 为空的槽。

**`find_stack`**

- 匹配 `category`、`count > 0`、`count + incoming <= stack_cap` 的首个槽。
- `stack_cap` 由调用方传入（当前原生堆叠上限 126），编排层不内嵌策略。

**`move`**

```text
1. from == to → kOk
2. 地址无效 → kInvalidRef
3. item_at(from) 为空 → kSourceEmpty
4. item_at(to) 非空 → kDestinationNotEmpty
5. take(from)；失败 → kSourceRejected
6. put(to, item)；失败 → kSourceRejected（不回滚）
```

**`swap`**

```text
1. a == b → kOk
2. 地址无效 → kInvalidRef
3. item_at(a) 为空 → kSourceEmpty
4. item_at(b) 为空 → kDestinationEmpty
5. take(a)、take(b)（预检保证都存在）
6. put(a, itemB)、put(b, itemA)；任一失败 → kSourceRejected（不回滚）
```

**`put_item`**

```text
1. 地址无效 → kInvalidRef
2. item_at(to) 非空 → kDestinationNotEmpty
3. put(to, item)；失败 → kSourceRejected
```

**`take_item`**

```text
1. 地址无效 → kInvalidRef
2. take(ref, out)；失败 → kSourceEmpty
```

**`remove_count`**

```text
1. count <= 0 → kInvalidRef
2. 地址无效 → kInvalidRef
3. item_at(ref) 为空 → kSourceEmpty
4. item.count < count → kNotEnough（不写）
5. remove(ref, count)；失败 → kSourceRejected
```

**`remove_item`**

- `find_all(category)` 按遍历顺序逐堆 `remove_count(min(堆 count, 剩余))`。
- 返回实际移除量；部分失败时返回已移除量，不补偿。

## 4. Hook 覆盖矩阵

缩写：`iA`=item_at，`bI`=bag_info，`f1/fA/fF`=find_first/find_all/find_if，`cI`=count_item，`cE`=count_empty_slots，`fE`=find_empty，`fS`=find_stack，`mv`=move，`sw`=swap，`pI`=put_item，`tk`=take_item，`rC`=remove_count，`rI`=remove_item，`src`=源可选 identify/use。

| # | hook | 编排原语 | 不在编排层 |
|---|---|---|---|
| 1 | char_process_shortcut (opt) | f1 + src.use | 诊断日志；原生 backup |
| 2 | shortcut_get_state (opt) | — | 仅 trace |
| 3 | char_use_item_ex (opt) | — | 仅 trace |
| 4 | find_item | — | 透传 Game Host |
| 5 | have_item | cI | 原生 backup；原生+注册源相加 |
| 6 | get_item_count | cI | 同上 |
| 7 | get_cumulate_count | — | 原生 count ABI 解码 |
| 8 | consume_item | iA + rC(1) + src.identify | 原生 backup + S2 借位/回写 |
| 9 | remove_item | rC | 原生 backup；身份重入守卫 |
| 10 | equip_item_from_inven_to_slot | iA + rC + pI + src.identify | flat 解码、原生装备、多级补偿 |
| 11 | put_jewel | — | 透传；材料消耗经 #8 回流 |
| 12 | is_having_empty_slot | cE | 原生袋空槽走 port 读 |
| 13 | unequip_item_to_inven | fS + fE + pI + tk | 原生 backup、存档域门控、失败补偿 |
| 14 | button_equip_exe | 间接 #10/#17 | projection、detail_item_context、UI 门禁 |
| 15 | button_unequip_exe | 间接 #13 | projection、UI 门禁 |
| 16 | button_destroy_exe | iA + rC | 选中项解析、原生物理槽复核 |
| 17 | ok_confirm_use_item | iA + src.use | 原生 backup；源 use 内部消费 |
| 18 | ok_destroy_item | rC | UI 门禁、原生物理槽复核、退款 |
| 19 | button_use_mercenary_seal_exe | iA + src.use | projection、选中项解析 |
| 20 | save_item | — | backup、pending 清理、free_item |
| 21 | find_save_slot | fS + fE | 存档域门控、pending 记录、transfer 构造 |
| 22 | move_item | mv | 原生 S2 进位；注册源不再一律拒绝 |
| 23 | equip_control_event_proc | — | 纯透传/重入守卫 |
| 24 | refresh_item_area | — | UI 深度守卫 |
| 25 | save_item_direct | bI + iA + pI | pending 通道、flat 解码、原生 S2 合并 |
| 26 | item_system_divide | — | 原生 S2 拆分/借位 |
| 27 | make_item | — | 纯透传 |
| 28 | remove_item_data | cI + rI | 原生 S2 快照/重扫/计划修正 |

## 5. 边界

### 5.1 留在 hook / 原生适配层的能力

| 能力 | 归属 | 理由 |
|---|---|---|
| `identify` / `use` | 源可选能力，hook 调用 | 句柄/物化/所有权属于源 |
| 原生 S2 位域读写 | 原生层 + hook | 依赖 backup 前后采样顺序与对象指针 |
| 物理槽指针扫描 | 原生层 | 防 UAF；逻辑袋路径不适用 |
| pending save 通道 | hook thread_local | FindSaveSlot→SaveItemDirect 无参数通道 |
| flat 袋号编解码 | 适配层 | 游戏调用约定，不是存储地址 |
| 堆叠上限 126 | 调用方传参 | 规则参数，编排层不内嵌 |
| 失败补偿/多级回滚 | hook | 只有装备/出售等有原生副作用的流程需要 |
| free_item / 物化对象生命周期 | 适配层 | 原生资源管理 |
| UI/detail/projection/出售门禁 | hook | 非存储 |
| 存档域门控 | hook | category→domain 的游戏规则 |

### 5.2 原生袋接入

原生袋按同一 `BagSource` 契约注册为 space 0/1 的源：

- `bag_count` / `bag_info` / `item_at` 委托原版读面。
- `put` / `take` / `remove` 委托原版写入口；原生适配器负责序列化、S2、对象生命周期等内部语义，编排层不感知。
- 原生写路径若需要 S2 位域的前后采样（consume/move/save/divide/remove_data），继续走 hook 现有原生路径，不经过编排层；编排层的 native source 只用于统一寻址/搜索/跨源移动等不依赖位域时序的场景。
- 原生 source 的 `identify` / `use` 可选；缺失时对应 hook 走现有原生路径。
- 原生 `space_id` 0/1 与注册源 >=2 不重叠；原生 source 的 `bag_count` 可排除任务袋，任务袋由 hook 单独处理。

### 5.3 明确不做的抽象

- 不做事务/快照/回滚/补偿日志。
- 不做引用计数/锁/原子/异步/跨帧等待。
- 不做重入保护；源回调反向调用编排层属于源缺陷。
- 不做全局索引/缓存/策略层/优先级。
- 不做 item payload 校验、数量拆分、堆叠合并、部分移动。
- 不做背包装备位。

## 6. 线程、生命周期与注册

- 所有入口只在游戏主线程调用。
- 注册/注销只在插件 activate/deactivate，且不在编排操作进行中调用。
- 注册校验：`space_id` 唯一、`bag_count`/`bag_info`/`item_at`/`put`/`take`/`remove` 非空；`identify`/`use` 可为空。
- 注销后旧 `BagRef` 再访问：`item_at` 返回 `nullopt`，写返回 `kInvalidRef`；不会解引用已注销的源。
- `space_id` 不复用；避免旧 `BagRef` 指向新源。
- 无锁；源回调不得反向调用编排层。

## 7. 风险

1. **put/remove 失败无回滚**：`move`/`swap` 第二步失败会留下部分写入。按约束不补偿；调用方需在需要时自行处理（hook 层的装备/出售流程已有显式补偿）。
2. **源内部语义**：`remove` 由源实现；若源不维护 count/payload/所有权一致，编排层无法发现。注册前需源自测。
3. **原生 source 的 S2**：原生写入口若绕过 S2 前后采样，可能破坏数量位域。原生 source 不得用于 S2 敏感路径；这些路径继续走 hook 现有实现。
4. **悬垂 BagRef**：注销后旧 ref 失效；`space_id` 不复用，避免语义漂移。
5. **identify 过期**：源返回的 ref 可能因物品移动/移除失效；hook 使用前应 `item_at` 复验。
6. **重复注册**：同一 `space_id` 或同一 `ctx` 重复注册会双倍计数；注册表拒绝重复 `space_id`，并在调试构建检查重复 `ctx`。
7. **搜索成本**：`O(source × bag × capacity)`；当前规模可接受，不建索引。

## 8. 落地清单（设计阶段，不写实现）

- 新增 `plugins/core.item/orchestration/bag_types.h`：`BagItem`、`BagInfo`、`BagRef`、`ItemHandle`。
- 新增 `plugins/core.item/orchestration/bag_source.h`：`BagSource` 契约。
- 新增 `plugins/core.item/orchestration/bag_orchestrator.h/.cpp`：注册表、读、搜索、move/swap、put/take/remove。
- 新增 `plugins/core.item/native_source/native_bag_source.h/.cpp`：space 0/1 的原生适配器。
- hook 适配层新增 `resolve_hook_item`、`use_at`、`build_transfer`，收口源物品的句柄/物化/传输转换。
- 删除旧 `bags/bag_source_io.h`、旧 `orchestration/bag_orchestrator.h` 骨架、旧 `native_source/native_bag_source.h` 骨架；是否删除 `QolBagSource` 注册面由后续迁移决定。
- 先接读/搜索/计数，再接 move/swap/remove，最后接 save/equip 等有原生副作用的流程。
