# 袋源编排层设计（core.item）

> 状态：设计稿
> 基础：`docs/refactor/extra-bag-orchestration.md`（地址、注册表、搜索、move/swap、单线程、无校验、无回滚）
> 范围：core.item 对原生袋与注册袋源的统一存储编排；不包含 UI、装备、出售、S2 位域、持久化策略。

## 1. 目标与非目标

### 目标

- 插件内部统一使用 `item_id`，不再传播 `category`，避免依赖物品表变更。
- 使用层只看到“物品是什么、数量多少”，不知道物品来自哪个袋源/袋/槽。
- 编排层是唯一袋源网关：使用层向编排层查询/取物/放物，编排层再向袋源查询/写入。
- 支持一个请求跨多个袋源、多个位置完成：取物可拆成多堆，放物可填多个空位。
- 袋源保持简单：只负责自己的存储，不做 item payload 校验、不做事务、不做回滚。
- 编排层保持简单：线性扫描、写入前预检、无索引、无缓存、无锁、无异步。
- 使用/销毁/消耗由 core.item 的独立使用层负责；编排层只负责取出并交付。

### 非目标

- 不处理 `item_backpack_at` / `set_item_backpack` / `clear_item_backpack`。
- 不做数量拆分、堆叠合并、部分移动；移动和交换以整件物品为单位。
- 不做持久化、UI、HTTP、装备、出售、合成。
- 不做使用/销毁/消耗的业务判断；这些属于使用层。
- 不建立全局物品索引或缓存。
- 不提供事务、快照、多级回滚、引用计数、重入保护。

## 2. 数据模型与源契约

### 2.1 数据模型

```cpp
struct ItemStack {
    int32_t item_id;                  // 唯一的物品键；插件内部不再使用 category
    int32_t count;
    uint32_t payload_size;            // <= 255
    uint8_t payload[256] = {};
};

struct ItemInfo {
    int32_t item_id;
    int32_t total_count;              // 所有袋源中该 item_id 的总数量
    std::vector<ItemStack> stacks;    // 每个实际堆的 item_id/count/payload；不含来源
};

struct BagInfo {
    int32_t capacity;
    int32_t occupied_slots;
};

// 编排层内部地址；不暴露给业务使用层。
struct BagRef {
    int32_t space_id;                 // 0/1 原生；>=2 注册源
    int32_t bag;
    int32_t slot;
};

// 不透明放置令牌；hook 适配层可用，业务使用层不关心。
struct Placement {
    // 内部包含 BagRef；对外不展开。
};

// 仅作为不透明身份；与游戏 handle 的转换由 hook 适配层完成。
struct ItemHandle {
    uint32_t slot;
    uint32_t generation;
};
```

### 2.1.1 payload 两类

- **实例型（A）**：同一 `item_id`，payload 不同，每堆是不同实例；不可堆叠，`count` 恒为 1。典型：装备、宝石。
- **数量型（B）**：同一 `item_id`，payload 只随 `count` 变化；可堆叠时 `count > 1`，不可堆叠时 `count = 1` 且 payload 完全一致。典型：药水、材料。

编排层不判断堆叠/实例类型，只把 payload 当不透明字节；是否可堆叠由使用层/物品事实层按 `item_id` 判断。

```cpp
struct PayloadMatch {
    uint32_t payload_size;
    uint8_t payload[256] = {};
};
```

`ItemStack.count` 是插件 API 的数量字段；payload 是游戏原生序列化数据。两者在静止状态下必须一致：类型 B 的 payload 内嵌 count 时，payload count == `ItemStack.count`；类型 A 没有 count 位段，`count` 恒为 1。

`take_item` 默认按 `item_id + count` 取；需要精确到实例型 A 的某一堆时，传入 `PayloadMatch`。

### 2.1.2 ItemCodec 与 count 一致性

只有 `ItemCodec` 解析/写入 payload 内的 count：

```cpp
struct ItemCodec {
    // 从游戏原生 payload 解码出 ItemStack；count 从 payload 重建。
    bool decode(int32_t item_id, const uint8_t* payload, uint32_t payload_size,
                ItemStack* out) const;
    // 把 ItemStack 编码成游戏原生 payload；count 写入 payload。
    bool encode(const ItemStack& item, uint8_t* payload, uint32_t capacity,
                uint32_t* out_size) const;
    // 只改 count，并同步 payload 内的 count 位段。
    bool set_count(ItemStack* item, int32_t count) const;
};
```

- 源适配层：`item_at` 调 `decode`，`put` 调 `encode`，`remove` 调 `set_count`。
- 读档：`decode(payload)` → `ItemStack`，count 从 payload 重建。
- 存档：`encode(ItemStack)` → payload，count 写入 payload。
- 编排层和使用层不调用 codec、不解析 payload，只读写 `ItemStack.count`。
- codec 是唯一知道 count 位段的位置；不得在 hook/使用层/编排层重复实现。

### 2.1.3 读档 / 存档

- 读档：读取 `item_id` + 原生 payload，调用 `ItemCodec::decode` 重建 `ItemStack`；`count` 从 payload 解出，并校验与解码结果一致。
- 存档：调用 `ItemCodec::encode(ItemStack)` 生成原生 payload，把 `count` 写入 payload；保存 canonical payload。可另存一份 count 做完整性校验，但读取时以 codec 解码结果为准。
- 不允许“只存 count、不写 payload”或“只存 payload、不校验 count”的旁路。

### 2.2 源接口

源只有 5 个存储原语：

```cpp
struct BagSource {
    int32_t space_id;
    void* ctx;

    int32_t (*bag_count)(void* ctx);
    bool (*bag_info)(void* ctx, int32_t bag, BagInfo* out);
    bool (*item_at)(void* ctx, int32_t bag, int32_t slot, ItemStack* out);

    bool (*put)(void* ctx, int32_t bag, int32_t slot, const ItemStack* item);
    // count > 0。count >= 当前数量时移除整件；否则只移除 count。
    // out 可为 nullptr；非空时返回被移除的部分。
    bool (*remove)(void* ctx, int32_t bag, int32_t slot, int32_t count,
                   ItemStack* out);
};
```

可选的“身份面”，不属于存储原语，编排层不直接调用：

```cpp
struct BagSourceIdentity {
    bool (*identify)(void* ctx, ItemHandle handle, BagRef* out);
};
```

没有 `take`，没有 `use`，没有 `category`：
- `take` 是 `remove(all)` 的别名，统一由 `remove` 表达。
- `use` 由 core.item 使用层负责，不是袋源能力。
- 源只认 `item_id`，不再有 category 字段。

### 2.3 契约规则

1. `space_id` 稳定且唯一；0/1 为原生空间，>=2 为注册源空间。注销后 `BagRef` 失效，不得复用同一 `space_id` 表示不同源。
2. `ItemStack.count` 是插件 API 的数量字段；payload 是游戏原生序列化数据。两者在静止状态下必须一致，由唯一的 `ItemCodec` 维护。源适配层调用 codec；编排层不解析、不修改 payload。
3. `put` 覆盖目标槽；目标是否为空由编排层预检，源不做校验。
4. `remove(bag, slot, count, out)` 由源执行数量增减：`count >= 当前数量` 移除整件，否则扣除 `count`。源适配层用 `ItemCodec::set_count` 同步 payload；返回的 `ItemStack.count` 与 payload 必须一致。编排层不感知。
5. `BagInfo.occupied_slots` 只统计普通物品槽，不包含物品背包装备位。
6. 源不做 item payload 校验、不做事务、不做回滚；源内部若有持久化/所有权/编码逻辑，由源自己维护，编排层不感知。
7. 源不判断数量增减的业务合法性：编排层预检 ref、物品存在、`count > 0`、`count <= item.count`；源只执行存储操作。
8. 源回调不得反向调用编排层；违反约定属于源缺陷，应修改源，编排层不做兜底。

## 3. 编排层对外 API

业务使用层只使用以下入口；不接触 `BagRef`/`space_id`/`bag`/`slot`。

```cpp
namespace qol::features::core_item::orchestration {

enum class Status {
    kOk,
    kNotFound,        // 指定 item_id 不存在
    kNotEnough,       // 数量不足；整单拒绝，不写
    kNoSpace,         // 没有足够空位；整单拒绝，不写
    kInvalid,         // 参数/目标无效
    kSourceRejected,  // 源 put/remove 返回 false；不回滚
};

struct ItemPlacement {
    ItemStack item;
    std::optional<Placement> target;  // 空 = 首个空位
};

struct TakeRequest {
    int32_t item_id;
    int32_t count;
    const PayloadMatch* match = nullptr;  // 实例型 A 可选
};

class BagOrchestrator {
public:
    // 查询面：使用层向编排层查询，编排层再向所有袋源查询。
    std::optional<ItemInfo> query_item(int32_t item_id) const;
    bool has_empty_slot() const;
    bool can_put(const std::vector<ItemPlacement>& items) const;

    // 取物：按 item_id + count 跨源聚合；总量不足则整单拒绝，不写。
    // 成功时 out 返回实际取出的堆（可能来自多个源）。
    // 单个取出；等价于 take_items({{item_id, count, match}})。
    Status take_item(int32_t item_id, int32_t count,
                     std::vector<ItemStack>* out,
                     const PayloadMatch* match = nullptr);
    // 批量取出：先预检全部请求；任一不满足则整单拒绝，不写。
    Status take_items(const std::vector<TakeRequest>& requests,
                      std::vector<ItemStack>* out);

    // 放物：每个 ItemStack 可带目标 Placement；空目标 = 首个空位。
    // 先预检全部目标/空位，任一不足则整单拒绝，不写。
    Status put_items(const std::vector<ItemPlacement>& items,
                     std::vector<Placement>* out_placements = nullptr);

    // hook 适配层专用，不属于业务使用层。
    std::optional<ItemStack> resolve_handle(ItemHandle handle) const;
    // 首个可放置的空槽；不返回可合并的已有堆。
    std::optional<Placement> find_slot(const ItemStack& item) const;
};

}  // namespace
```

### 3.1 内部辅助

`query_item`、`take_item`、`can_put`、`put_items` 内部可复用线性扫描 helper：

- `find_all(item_id)`：按“源注册序 → bag → slot”收集所有匹配堆。
- `first_empty()`：首个空槽。
- `count_item(item_id)`：所有堆 count 之和。
- `find_stack(item_id, incoming, stack_cap)`：可合并的堆（仅 helper，不对外承诺自动合并）。

这些 helper 不扩大对外接口，也不建立索引/缓存。

## 4. 操作语义

### 4.1 查询

**`query_item(item_id)`**

- 扫描所有已注册源；空槽/容量 0 跳过。
- 返回 `total_count` 和每个实际堆的 `ItemStack`；不返回 `BagRef`。
- 实例型 A 的每个堆 payload 不同；数量型 B 的 payload 只随 count 变化。
- 找不到返回 `nullopt`；找到但总量为 0 也视为不存在。

**`has_empty_slot()`**

- 任一源、任一 bag 的 `occupied_slots < capacity` 即 true。
- 用于 `INVEN_IsHavingEmptySlot`。

**`can_put(items)`**

- 逐个检查：有 target 时目标必须为空且有效；无 target 时统计可用空位。
- 多个无 target 项共享同一批空位，按输入顺序模拟占用；不能把同一个空位重复计算。
- 只检查空位，不做自动堆叠；需要合并时由使用层先 `take` 两堆、合并后再 `put_items`。
- 不检查堆叠上限；合并前由使用层按 item facts 判断。
- 返回 false 时不做任何写入。

### 4.2 取物

**`take_item(item_id, count, match)`**

```text
1. count <= 0 → kInvalid
2. query_item(item_id) 不存在 → kNotFound
3. match 非空时只保留 payload 精确匹配的堆；无匹配 → kNotFound
4. 匹配堆 total_count < count → kNotEnough（不写）
5. 按“源注册序 → bag → slot”逐堆取：
   - 每堆取 min(剩余, 堆 count)
   - 源 remove 失败 → kSourceRejected（已取出的不回收）
6. out 返回取出的 ItemStack 列表
```

- 一个请求可以跨多个源、多个堆完成。
- 使用层只看到 item_id/count/payload，不知道来源。
- 部分堆的 `ItemStack` 由源适配层通过 codec 生成，count 与 payload 一致；编排层不合并、不重编码。
- 所有权：成功后 `out` 中的 `ItemStack` 归调用方；源已移除，编排层不保留引用。
- 实例型 A：`count` 表示取多少个不同实例；`match` 用于指定某一实例。不传 `match` 时按遍历序取首个/前几个。
- 数量型 B：可按 `item_id + count` 跨堆拆分；返回的各堆 count 之和等于请求量。`match` 只用于实例型 A；数量型 B 不按 payload 匹配。

**`take_items(requests)`**

```text
1. 每个 request 校验 count > 0；否则 kInvalid
2. 按请求顺序模拟扣减：对每个 request 查询匹配堆（含 match）
   - 不存在 → kNotFound
   - 匹配总量 < count → kNotEnough
   - 已被前序 request 预占的堆数量要扣除；不能各自单独通过
   - 任一不满足 → 整单拒绝，不写
3. 预检通过后，按 request 顺序逐项取；每项内部按“源注册序 → bag → slot”取
4. 源 remove 失败 → kSourceRejected（已取出的不回收）
5. out 汇总所有取出的 ItemStack
```

- 用于多个不同 item_id 一起取，或多帧选择后的提交。
- 全部预检通过才写；写入阶段失败不回滚，调用方负责把已取出的放回。
- 每个 request 的所有权独立：成功取出的 ItemStack 归调用方。

### 4.3 放物

**`put_items(items)`**

```text
1. 预检每个 ItemPlacement：
   - target 存在：目标有效且为空
   - target 为空：存在可用空位（can_put 同一套判断）
2. 任一不满足 → kNoSpace / kInvalid（整单拒绝，不写）
3. 按输入顺序放置：
   - 有 target：put 到 target
   - 无 target：按“源注册序 → bag → slot”找首个空位
   - 源 put 失败 → kSourceRejected（已放入的不回收）
4. out_placements 返回实际落位（可选）
```

- 编排层原样传递 `ItemStack`；源适配层用 codec 编码 payload。类型 A 每件占一个槽，类型 B 的合并由使用层先 `take` 两堆、合并 count 后再 `put_items`。
- 不做自动堆叠。
- 批量放入是“先预检、后写入”，不是事务；源写入阶段失败不回滚。
- 目标 Placement 由 hook 适配层在需要精确槽位时提供；业务使用层通常传空目标。
- 所有权：成功后物品归目标源；调用方不再持有。

### 4.4 move / swap / merge

不新增对外接口，由使用层组合：

- move：`take_item(item_id, count)` → `put_items({target})`
- swap：`take_item(a)`、`take_item(b)` → `put_items({b→a, a→b})`
- merge：仅数量型 B；`take_item(source)`、`take_item(target)` → `put_items({合并后的堆→target})`。类型 A 不合并。
- save：`can_put`/`find_slot` → `put_items({target})`
- equip：`take_item` 取新/旧装备 → `put_items` 回填 → 装备动作

### 4.5 多帧选择示例：宝石合成

1. **选择阶段**：`query_item(gem_id)` 返回所有宝石快照（实例型 A，每个 payload 不同）。UI/使用层保存选中的 `ItemStack` 快照；不调用 `take_item`，宝石仍在袋中。
2. **等待多帧**：继续持有快照。快照是只读副本，不代表所有权；期间宝石仍可能被移动/消耗。
3. **点击合成**：
   - 为每个选中宝石组装 `TakeRequest{gem_id, 1, PayloadMatch(payload)}`。
   - `take_items(requests)`：全部仍在才成功；任一缺失/变更则整单拒绝，UI 刷新选择。
   - 成功后，选中的宝石被取出，所有权归使用层。
   - 创建新宝石，`put_items({new_gem})` 放回袋中。
   - 若创建/放入失败：`put_items(已取出的宝石)` 放回；放回位置不一定与原槽相同。若要求原槽恢复，hook 适配层需要在选择时保存 `Placement` 快照。
4. **锁定**：当前设计不提供 reservation。选中期间宝石仍可被移动/消耗；提交时 `take_items` 会拒绝并让 UI 刷新。若游戏要求“选中后不可移动/消耗”，需要额外的 reservation 机制，当前设计不含。

## 5. 查询方向与跨源聚合

- 方向固定：**使用层 → 编排层 → 袋源**。
- 使用层不持有 `BagSource`、不调用 `item_at`/`bag_info`/`put`/`remove`。
- 编排层持有注册表，按注册顺序扫描所有源；查询结果对使用层是源无关的。
- 袋源只被动回答 `bag_count`/`bag_info`/`item_at`；不知道其他源，也不知道使用层。
- 一个请求跨多个源时，编排层负责拆分/合并；使用层只看到 `ItemStack` 列表或总数量。
- `Placement` 是 hook 适配层与编排层之间的不透明令牌；业务使用层不读取其内容。
- `resolve_handle` 只给 hook 适配层使用；业务使用层只用 `item_id`。

## 6. 使用层与 hook 边界

### 6.1 类型判断

- 物品类型（实例型 A / 数量型 B、是否可堆叠）由 core.item 的 item facts 层按 `item_id` 判定。
- 编排层不判断类型：它只按 `count`/payload 原样搬运，不解析、不比较 payload。
- 袋源不判断类型：源只返回 count/payload，并负责自己存储层内的数量编码；不做“这是装备还是药水”的业务判断。
- 类型判断统一以 core.item 的 item facts 为准；源与 item facts 不一致时，以 item facts 为准。

### 6.2 所有权与生命周期

- **静止时**：物品数据归袋源所有。
- **`take_item` 成功后**：源已移除该物品，返回的 `ItemStack` 归调用方（使用层）临时所有；编排层不再跟踪这个物品。
- **使用层**：只能在同一次同步操作内持有 `ItemStack`，并在返回前完成使用/销毁/消耗/放回。不得跨帧长期持有。
- **`put_items` 成功后**：所有权回到目标袋源；调用方不再持有。
- **禁止双持有**：同一物品不能同时存在于袋源和调用方。`take` 后不放回、不消费，物品就从袋源消失；重复放回会造成复制。
- **查询快照**：`query_item` 返回的是副本；使用层可以长期持有快照，但它不代表所有权，也不保证之后仍能取出。
- **跨帧持有**：当前不支持。若未来需要，物品应留在袋中，使用层只持 handle；或由编排层引入 detached buffer + handle，并明确清理责任。当前设计不引入 detached buffer。

### 6.3 分层职责

| 层 | 职责 |
|---|---|
| hook 适配层 | 接收游戏回调；把游戏 category/handle 转成 `item_id`/`ItemHandle`；处理 UI、S2、pending save、退款、失败补偿；必要时使用 `Placement` 回填游戏槽位 |
| 使用层 | 接收 `ItemStack`（item_id/count/payload）；决定使用、销毁、消耗或其他处理；不关心来源 |
| 编排层 | 查询、取物、放物、跨源拆分/聚合、批量预检；不关心使用/销毁/消耗 |
| 袋源 | 5 个存储原语；只认 item_id/count/payload；不做业务校验/回滚 |

- 使用层不接触 `BagRef`/`space_id`/`bag`/`slot`。
- hook 适配层若需要向游戏返回槽位，使用编排层的 `Placement`，不要把 `BagRef` 传播到使用层。
- 源不实现 `use`；使用层不实现存储。

## 7. Hook 覆盖矩阵

缩写：`q`=query_item，`t`=take_item，`c`=can_put，`e`=has_empty_slot，`p`=put_items，`r`=resolve_handle，`f`=find_slot，`u`=使用层，`h`=hook/原生层。

| # | hook | 编排原语 | 不在编排层 |
|---|---|---|---|
| 1 | char_process_shortcut (opt) | q + t(id,1) | 使用层；诊断；原生 backup |
| 2 | shortcut_get_state (opt) | — | 仅 trace |
| 3 | char_use_item_ex (opt) | — | 仅 trace |
| 4 | find_item | — | 透传 Game Host |
| 5 | have_item | q.total_count | 原生 backup；原生+注册源相加 |
| 6 | get_item_count | q.total_count | 同上 |
| 7 | get_cumulate_count | — | 原生 count ABI 解码 |
| 8 | consume_item | t(id,1) | 使用层；原生 backup + S2 借位/回写 |
| 9 | remove_item | t(id,count) | 使用层/丢弃；原生 backup；身份重入守卫 |
| 10 | equip_item_from_inven_to_slot | q + t + p + r | 装备动作、flat 解码、原生补偿 |
| 11 | put_jewel | — | 使用层/原生；材料消耗经 #8 回流 |
| 12 | is_having_empty_slot | e | 原生袋空槽走 port 读 |
| 13 | unequip_item_to_inven | p + f | 原生装备读取、存档域门控、失败补偿 |
| 14 | button_equip_exe | 间接 #10/#17 | projection、detail_item_context、UI 门禁 |
| 15 | button_unequip_exe | 间接 #13 | projection、UI 门禁 |
| 16 | button_destroy_exe | q + t | 使用层销毁；选中项解析、物理槽复核 |
| 17 | ok_confirm_use_item | t(id,1) | 使用层；原生 backup |
| 18 | ok_destroy_item | q + t | 使用层销毁；UI 门禁、物理槽复核、退款 |
| 19 | button_use_mercenary_seal_exe | t(id,1) | 使用层；projection、选中项解析 |
| 20 | save_item | — | backup、pending 清理、free_item |
| 21 | find_save_slot | c + f | 存档域门控、pending 记录、flat 编码 |
| 22 | move_item | q + t + p | 原生 S2 进位；注册源不再一律拒绝 |
| 23 | equip_control_event_proc | — | 纯透传/重入守卫 |
| 24 | refresh_item_area | — | UI 深度守卫 |
| 25 | save_item_direct | p（带 target） | pending 通道、flat 解码、原生 S2 合并 |
| 26 | item_system_divide | — | 原生 S2 拆分/借位 |
| 27 | make_item | — | 纯透传 |
| 28 | remove_item_data | q + t | 原生 S2 快照/重扫/计划修正 |

## 8. 原生袋接入

原生袋按同一 `BagSource` 契约注册为 space 0/1 的源：

- `bag_count` / `bag_info` / `item_at` 委托原版读面。
- `put` / `remove` 委托原版写入口；原生适配器负责序列化、S2、对象生命周期等内部语义，编排层不感知。
- 原生写路径若需要 S2 位域的前后采样（consume/move/save/divide/remove_data），继续走 hook 现有原生路径，不经过编排层；编排层的 native source 只用于统一寻址/搜索/跨源移动等不依赖位域时序的场景。
- 原生 source 的 `identify` 可选；缺失时对应 hook 走现有原生路径。原生 source 不提供 `use`。
- 原生 `space_id` 0/1 与注册源 >=2 不重叠；原生 source 的 `bag_count` 可排除任务袋，任务袋由 hook 单独处理。

## 9. 线程、生命周期与注册

- 所有入口只在游戏主线程调用。
- 注册/注销只在插件 activate/deactivate，且不在编排操作进行中调用。
- 注册校验：`space_id` 唯一、`bag_count`/`bag_info`/`item_at`/`put`/`remove` 非空；`identify` 可为空。
- 注销后旧 `BagRef`/`Placement` 失效；`item_at` 返回 `nullopt`，写返回 `kInvalid`。
- `space_id` 不复用；避免旧 `Placement` 指向新源。
- 无锁；源回调不得反向调用编排层。

## 10. 风险

1. **实例型 payload 的选择**：类型 A 同一 `item_id` 的不同堆 payload 不同。`take_item(item_id,count)` 默认按遍历序取；使用层若需要特定实例，必须先 `query_item` 拿到 payload，再传 `PayloadMatch`。类型 B 可按 `item_id + count` 跨堆拆分。
2. **codec 覆盖**：所有 item_id 的 count 编码必须由唯一 `ItemCodec` 覆盖；未知 payload 解码失败时不得写入。读档必须 `decode`、存档必须 `encode`，否则 count 与 payload 会漂移。
3. **批量写入失败无回滚**：`take_item`/`put_items` 预检通过后，源阶段仍可能失败；已执行的部分不回收。使用层/hook 需在需要时自行补偿。
4. **使用层取走物品后失败**：是否放回由使用层/hook 决定；编排层不自动回滚，可能丢件。使用层必须定义失败策略。
5. **长期持有/双持有**：本设计禁止使用层跨帧持有实际物品；`take` 后不放回、不消费会让物品脱离袋源统计，重复放回会复制。跨帧需求必须改为 handle + detached buffer 或让物品留在袋中。
6. **hook 需要精确槽位**：`find_slot`/`Placement` 只给 hook 适配层；业务使用层不要读取。若把 `BagRef` 泄漏到使用层，分层会被破坏。
7. **源内部语义**：`remove` 由源实现；若源不维护 count/存储状态一致，编排层无法发现。注册前需源自测。
8. **原生 source 的 S2**：原生写入口若绕过 S2 前后采样，可能破坏数量位域；S2 敏感路径继续走 hook 现有实现。
9. **search 成本**：`O(source × bag × capacity)`；当前规模可接受，不建索引。
10. **重复注册**：同一 `space_id` 或同一 `ctx` 重复注册会双倍计数；注册表拒绝重复 `space_id`，并在调试构建检查重复 `ctx`。

## 11. 落地清单（设计阶段，不写实现）

- 新增 `plugins/core.item/orchestration/bag_types.h`：`ItemStack`、`ItemInfo`、`BagInfo`、`BagRef`、`Placement`、`ItemHandle`。
- 新增 `plugins/core.item/orchestration/bag_source.h`：`BagSource`（5 原语）与可选 `BagSourceIdentity`。
- 新增 `plugins/core.item/item_codec/`：唯一的 count/payload 编解码模块；源适配层、读档、存档调用，编排层/使用层不调用。
- 新增 `plugins/core.item/orchestration/bag_orchestrator.h/.cpp`：注册表、`query_item`、`take_item`、`can_put`、`put_items`、`find_slot`、`resolve_handle`。
- 新增 `plugins/core.item/use_layer/`：使用/销毁/消耗的独立入口，输入为编排层返回的 `ItemStack`。
- 新增 `plugins/core.item/native_source/native_bag_source.h/.cpp`：space 0/1 的原生适配器。
- hook 适配层：把游戏 category/handle 转成 `item_id`/`ItemHandle`；收口 `Placement` 到 flat 袋号的转换；不再向使用层传播 `BagRef`。
- 删除旧 `bags/bag_source_io.h`、旧 `orchestration/bag_orchestrator.h` 骨架、旧 `native_source/native_bag_source.h` 骨架；是否删除 `QolBagSource` 注册面由后续迁移决定。
- 先接查询/取物/放物，再接 move/swap/merge 的使用层组合，最后接 save/equip 等有原生副作用的流程。
