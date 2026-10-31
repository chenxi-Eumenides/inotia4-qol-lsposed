# 背包系统设计（统一指针槽表 + 多袋源）

> 状态：参考设计（已被 `three-plugin-architecture.md` 取代；冲突以三插件方案为准）。
> 范围：一个统一的背包/库存系统，覆盖原版袋、C18 改版追加袋和插件袋源；通过函数级 entry hook 与原版系统交互。
> 前提：不与现有插件契约、能力、HTTP、`QolBagSource`、`core.item`/`extension_bag` 实现兼容；只保证游戏原生系统的行为、ABI 和副作用语义。
> 硬约束依据：[`hard-constraints.md`](hard-constraints.md)。
> 简化约束（用户明确要求）：**运行期不得有回滚、tentative、待入库、pending、journal 等中间状态；插件与游戏都在单线程执行。**

## 0. 对硬约束的修订提案（必须先裁决）

本设计以“槽表长期持有原生 item 指针”为基础，这与 `hard-constraints.md` §1.1 的字面表述“裸 `void* item` 指针不能跨调用/跨帧保存”冲突。不能静默改写基线，必须先裁决：

**提案**：把“袋槽表”定义为存储本身，并开一个受控例外：

- 槽表是唯一持有该指针的结构；指针不复制到其他插件结构、不进入 HTTP/快照、不作为跨调用身份。
- 每次动作前按 `ItemKey`（原生 UID）重读槽并复核；ID 不一致即 `Stale`。
- 任何可能释放该对象的游戏生命周期点（读档、换图、清池）必须 hook 并清表/重建；未覆盖前外部表不得跨该生命周期点使用。
- 通过阶段 0 阻断实验验证“item 池不会释放不在原生槽表中的对象”（§12 第 8 条）。

**如果基线不接受该例外**，降级为：

- 外部表只存 `{uid, payload}`，不存长期指针。
- 动作期临时物化原生对象，动作结束立即释放；动作期间不跨 hook、不跨帧。
- 代价是移动/使用/出售路径多一次物化/序列化；仍然没有 pending、回滚或 journal，但复杂度高于指针表。

在裁决前，本设计不宣称已满足 §1.1；所有涉及外部表指针的路径按“待批准”处理。

## 1. 设计原则

1. **一个物品在任一时刻只在一个槽里。** 不存在“已从源移除、尚未进入目标”的悬空状态。
2. **先预检，后单次写。** 所有可能失败的检查都在写入前完成；写入失败等于本次操作没有发生。
3. **不做运行期回滚。** 没有补偿事务、没有 tentative、没有 commit/rollback、没有恢复日志。
4. **所有袋都是原生 item 指针的槽表。** 原生袋和插件袋的区别只是“表在哪里、谁负责存档”，不是“物品是否存在原生对象”。
5. **单线程。** 不加锁、不做原子变量、不做跨线程交接；所有 hook、动作、投影都在游戏主线程。
6. **可排查。** 每次操作只由“预检 → 一次写入 → 清源/清目标”组成；失败点唯一，日志只记录输入、目标槽和结果。

## 2. 目标与非目标

### 2.1 目标

1. 统一地址空间：原版袋、C18 追加袋、插件袋都用同一套 `{来源, 袋, 槽}` 定位。
2. 物品移动是**指针换槽**，不是“序列化 → 删除 → 重建”。
3. 原生系统继续按自己的函数和 ABI 工作；hook 只做来源解析和槽表重定向。
4. 外部袋的物品也是原生 item 对象，因此使用、装备、出售、详情、图标等原生路径可以直接复用。
5. 插件袋的持久化只在存档点批量发生，不进入物品操作热路径。

### 2.2 非目标

- 不做运行期事务、回滚、补偿、pending、tentative、WAL。
- 不做多线程、异步、工作线程、跨帧等待。
- 不接管原生存档文件格式；原生袋仍由游戏保存。
- 不建立全局物品索引；槽表就是唯一索引。
- 不设计背包 UI 页面；只做原生 UI 的读取投影。
- 不承诺“进程在两次指针写之间崩溃”的跨存储原子性（§10.4 明确边界）。

## 3. 硬约束到设计规则的映射

| 硬约束 | 设计规则 |
|---|---|
| 原生调用参数必须落在合法原生索引域 | 只有原生槽表适配器持有原生 bag/slot；领域层只传 `{来源, 袋, 槽}`。外部来源的袋号永远不传给原生函数。 |
| 自定义路径直接返回，不调用 backup | hook 先解析槽属于哪张表；非原生表直接操作该表并返回，绝不带外部袋号调用 backup。 |
| 原生副作用只发生一次 | 原生目标走一次 `NativeActionPort` 调用；`NativeOperationScope` 防止嵌套 hook 再次路由。 |
| 主线程 | 所有操作在主线程同步完成；没有锁、没有异步、没有跨线程指针。 |
| 裸 `void* item` 不能跨调用/跨帧保存 | 默认规则不变；本设计提出“槽表是存储、不是缓存”的受控例外（§0），需先裁决。动作前仍按 `ItemKey` 重读复核；例外未批准时降级为 `{uid,payload}` + 动作期临时物化。 |
| 任务袋 `(0,5)` 只读 | 地址校验在领域入口统一执行。 |
| S2 数量编码 | 数量语义归 `CountCodec`；背包系统只传逻辑数量。 |
| 存档两段式调用 | 原生保存不重定向；插件袋在存档点批量序列化，不借用 `FindSaveSlot`/`SaveItemDirect` 的槽号。 |
| payload 固定 256 字节 | 只在外部袋**存档/读档**时使用；物品移动不经过 payload。 |
| 不复制/不丢件/不部分成功 | 预检通过后只写一次目标槽；目标写失败则源不变；目标写成功后清源是同一线程内的指针写，不再失败。 |
| 过期身份必须拒绝 | 动作前重读槽并比较 `ItemKey`；不一致返回 `Stale`。 |

## 4. 统一存储模型

### 4.1 槽表

```cpp
struct SlotTable {
    SourceId source;
    int32_t bag_count;
    int32_t (*capacity)(int32_t bag);       // 每袋真实容量
    void*   (*get)(int32_t bag, int32_t slot);
    bool    (*set)(int32_t bag, int32_t slot, void* item);  // 合法参数下不失败
};
```

- `get` 返回原生 item 指针或 `nullptr`。
- 槽表是**存储本身**，不是跨调用缓存；表内指针不复制到其他插件结构，动作前按 `ItemKey` 重读复核。
- 每个非空槽是该指针的唯一所有者；任何可能释放该对象的游戏生命周期点都必须同步清槽或重建表（§7.8）。
- `set` 只做槽位写入；调用前必须完成容量、空槽、任务袋、来源合法性检查。
- 原生表的 `set` 走 `NativeActionPort` 的写槽原语（精确空槽写入/清槽），一次调用完成；外部表的 `set` 是普通指针赋值。
- 槽表不持有“待写入”“已移除待提交”之类的状态。

### 4.2 来源

| 来源 | `SourceId` | 槽表位置 | 存档 |
|---|---:|---|---|
| 原版袋 | 0 | 游戏 `INVEN_pItem`，bag 0..5 | 游戏原生存档 |
| C18 追加袋 | 1 | 游戏改版槽表，bag 0..count-1（原生 6..17） | 游戏原生存档 |
| 插件袋 | >=2 | 插件自己的指针表 | 插件 sidecar，存档点批量写 |

- 原版袋和 C18 追加袋都是原生来源，只是槽表基址/索引映射不同。
- 插件袋的表里放的是**原生 item 对象指针**，不是序列化 payload；因此移动、使用、装备、详情都可以直接走原生路径。
- 插件袋不写 `INVEN_pItem`、不写原生容量字。
- 插件袋源必须以**原生 item 指针槽表**注册；纯 payload/数据库来源不进入本系统。需要外部数据时，先在来源内物化为原生对象，再放入表。
- 容量只由来源报告；缩小容量前必须清空超出新容量的槽，否则该次容量变更被拒绝（不搬迁、不回滚）。
- 外部表占用原生 item 池：每个非空槽一个原生对象。池耗尽时新建/落位失败，已有物品不受影响；不预分配、不缓存空闲对象。

### 4.3 外部表注册协议

外部来源只提供一张表，不提供“导入/导出/移除”业务回调：

```cpp
struct ExternalTableDesc {
    uint32_t struct_size;
    const char* persist_key;                 // sidecar 文件名/键；进程内唯一
    int32_t bag_count;
    int32_t (*capacity)(int32_t bag);
    void*   (*get)(int32_t bag, int32_t slot);
    bool    (*set)(int32_t bag, int32_t slot, void* item);  // 合法参数下只做指针写入
};
```

- `get`/`set`/`capacity` 只在主线程调用。
- `set` 不负责释放旧对象；目标槽必须已预检为空。
- 来源不实现移动、交换、使用、装备；这些由 `InventoryExecutor` 和 `ActionService` 统一处理。
- 来源不实现存档格式；`SaveCoordinator` 用原生序列化批量读写 `persist_key` 对应的 sidecar。
- 注册/注销只检查 `struct_size`、`persist_key` 非空且唯一、三个回调非空；不检查业务数据。

### 4.4 地址与身份

```cpp
struct BagRef  { SourceId source; int32_t bag; };
struct SlotRef { SourceId source; int32_t bag; int32_t slot; };

struct ItemKey {
    SourceId source;
    uint64_t id;       // 原生 item UID；插件袋同样使用对象 UID
};
```

- `SlotRef` 是位置；`ItemKey` 是身份。
- 动作入口接收 `SlotRef + expected ItemKey`；执行前 `get` 重读并比较 UID，不一致返回 `Stale`。
- 原生 UID 的跨读档稳定性是待确认事实；确认前 UID 只在同一进档会话内有效，读档后全部失效重建。
- 不需要镜像注册表：插件袋里本来就是原生对象。

### 4.5 任务袋

- `(kNativeOriginal, 5)` 可读，不可作为任何写动作、移动、装备、使用、出售的源或目标。
- 校验在领域入口统一执行。

### 4.6 ItemData 与 payload 边界

- 插件**不定义、不流转解析后的 ItemData**。原生 item 对象是物品内容的唯一真源。
- 层间只传 `SlotRef` + `ItemKey`；需要 category/count/type 时，通过只读 `ItemFacts` 端口当场读取，不缓存成 DTO。
- 唯一的只读 POD 是快照用的 `ItemView`，它不参与动作，也不回写：

```cpp
struct ItemView {
    ItemKey key;
    int32_t category;
    int32_t count;     // 逻辑数量
    uint32_t flags;
};
```

`ItemView` 只用于 HTTP/UI 展示，不回写槽表、不重建 payload、不作为动作输入。

- payload 只在两个边界出现：
  - 存档：`SAVE_SaveItem` 从当前原生对象序列化，插件只写不透明字节；
  - 读档：`SAVE_LoadItem` 从字节重建原生对象，插件不解析字段。
- 传给原版函数的是 item 指针，不是 payload；因此**不存在“传入原版函数前重建 payload”这一步**。
- 唯一例外是 §0 的降级模型：外部表存 `{uid,payload}`，动作前临时物化；这是降级路径，不是主设计。

## 5. 分层与职责

```text
原生系统（拾取/商店/合成/装备/使用/出售/UI/存档）
        │ 函数级 entry hook
┌───────▼──────────────────────────────────────────────┐
│ Hook 适配层                                           │
│ HookAdapters（按原生函数拆分）                         │
│ 只做 ABI 翻译、槽表解析、NativeOperationScope、结果映射 │
└───────┬──────────────────────────────────────────────┘
        │ 领域调用（只有 SlotRef/ItemKey）
┌───────▼──────────────────────────────────────────────┐
│ 库存领域层                                            │
│ BagTableRegistry / InventoryReader / InventoryExecutor│
│ ActionService / SaveCoordinator                       │
│ 地址、身份、预检、单次写入、动作路径、存档协调          │
└───────┬───────────────────────────────┬──────────────┘
        │ 槽表原语                       │ NativeActionPort（backup 范围）
┌───────▼──────────────┐  ┌─────────────▼──────────────┐
│ 槽表层                │  │ 原生适配层（Game Adapter）  │
│ NativeTable /         │  │ 槽表、容量、UID、序列化、   │
│ ExternalTable         │  │ 原生动作 backup、hook 安装  │
└───────────────────────┘  └────────────────────────────┘
```

| 组件 | 负责 | 不负责 |
|---|---|---|
| `BagTableRegistry` | 来源注册/注销、`SourceId` 分配、槽表查找、来源顺序 | 袋内数据、原生索引映射细节 |
| `NativeTable` | 原版/C18 槽表读写、容量、原生索引映射 | 插件袋数据、存档 |
| `ExternalTable` | 插件袋的指针表、容量、读写 | 伪造原生槽、调用游戏函数 |
| `InventoryReader` | 按地址/类别/谓词读取、计数、空槽统计、只读快照 | 写入、缓存原生内存 |
| `InventoryExecutor` | 移动、增加、移除、消耗的预检与单次写入 | 回滚、pending、补偿 |
| `ActionService` | 使用/装备/卸下/出售/合成产物的路径选择 | 直接读写槽表、UI 布局 |
| `HookAdapters` | 每个原生函数的入口适配、ABI 翻译、`NativeOperationScope` | 业务规则、存储决策 |
| `UiAdapter` | 控件 item/选择上下文 → `SlotRef + ItemKey`；原生 UI 读取投影与窗口容量 | 写槽表、写容量字、数据存储 |
| `SaveCoordinator` | 插件袋存档点的批量序列化/反序列化 | 物品移动、运行期事务 |
| `CountCodec`（宿主） | 逻辑数量 ↔ S2 位域 | 槽表、动作编排 |
| `NativeActionPort`（宿主） | 原生槽写入/清槽/移动/动作 backup、ABI 校验 | 业务规则、来源选择 |

依赖规则：

- 领域层只依赖槽表接口和 `NativeActionPort`，不 include `game/` 或原生符号。
- 槽表层不依赖 hook 层；槽表操作不得反向调用 hook。
- 只有一个 owner 安装库存 hook；插件袋源只提供槽表，不安装 hook。
- backup 的唯一调用者是 `NativeActionPort`。
- hook 适配器按原生系统分文件（inventory/equipment/use/display/save），不设第二个“动作 router”；动作路径统一进 `ActionService`。

组件收敛说明：

- `BagTableRegistry` 合并了原来的“来源注册”和“槽表集合”，不再单独维护 `SourceRegistry` + `SlotTables`。
- `HookAdapters` 只做入口适配；原 `ActionRouter` 的动作路径迁入 `ActionService`，避免两个 router。
- `UiAdapter` 合并输入解析与显示投影；内部仍按 input/projection 两段实现，但对外只有一个门面。
- `SourceKind` 不单独建 enum；原生表与外部表用不同实现区分，注册表按表对象分派。
- 保留 `ItemKey`/`SlotRef` 分离（身份 vs 位置）和 `SlotTable`/`NativeActionPort` 分离（存储 vs ABI）。

### 5.1 层间调用清单

| 调用方 | 被调用方 | 允许输入 | 禁止 |
|---|---|---|---|
| `HookAdapters` | `InventoryReader` / `InventoryExecutor` / `ActionService` / `UiAdapter` | 原生参数、`SlotRef`、`ItemKey` | 直接写槽表、直接调用 backup |
| `ActionService` | `InventoryReader` / `InventoryExecutor` / `NativeActionPort` | `SlotRef`、`ItemKey`、动作参数 | 直接读写原生内存、直接操作控件 |
| `InventoryExecutor` | `BagTableRegistry` → `SlotTable`、`NativeActionPort` | `SlotRef`、`ItemKey`、数量 | 调用 hook、操作 UI |
| `InventoryReader` | `BagTableRegistry` → `SlotTable`、`ItemFacts` | `SlotRef`、bag/category | 写槽表、缓存原生对象 |
| `UiAdapter` | `BagTableRegistry`、`ItemFacts`、投影 hook | 控件指针、`SlotRef` | 写槽表、写容量字 |
| `SaveCoordinator` | `BagTableRegistry`、`NativeActionPort` | 外部表非空槽 | 运行期移动、动作路由 |

所有跨层参数只有三类：原生 ABI 参数、`SlotRef`、`ItemKey`。没有第四种“解析后的物品对象”。

## 6. 操作语义

### 6.1 统一预检

所有写操作先完成：

1. 来源存在、袋/槽在容量内、不是任务袋。
2. 源槽非空，且 `ItemKey` 与调用方预期一致。
3. 目标槽为空（移动/交换）或目标容量足够（增加）。
4. 数量合法：`count > 0` 且不超过源堆数量/目标剩余堆量。

预检不通过直接返回错误，**不写任何槽**。

### 6.2 移动（同表/跨表）

**同一原生表**：直接调用原生移动函数一次（`INVEN_MoveItem` 或等价原语）；原生自己完成槽位交换/合并。插件不做两次指针写。

**同一外部表**：两个槽指针直接赋值；外部表赋值不会失败。

**跨表移动**：

```text
1. 预检（§6.1）
2. 写目标槽：
   - 目标原生表 → NativeActionPort.place_slot(bag, slot, item) 一次
   - 目标外部表 → set(bag, slot, item)
3. 目标写失败 → 返回失败；源槽保持不变（没有回滚动作）
4. 目标写成功 → 清源槽：
   - 源原生表 → NativeActionPort.detach_slot(bag, slot) 一次（只清指针，不释放对象）
   - 源外部表 → set(bag, slot, nullptr)
5. 复核源槽为空、目标槽为同一对象。
```

要点：

- 目标写是唯一可能失败的步骤；失败时源仍然持有物品，状态不变。
- 清源是同一线程内的指针写，不释放对象、不再调用可能失败的业务函数。
- **源是原生表且 detach 意外失败**：目标（外部表）里已有副本，原生源仍在。此时保留原生副本、立即清掉外部副本并报 `Internal`。这是前向清理，不是回滚；原生副本有存档背书，优先保留原生。
- 没有“待入库”“已出库未入库”状态；在步骤 2 与 4 之间，物品指针同时在源/目标槽中，但单线程下没有其他代码能观察到，且不写入任何持久化存储。
- 原生表清槽使用 `detach_slot`（只清指针），不使用会释放对象的 `RemoveItem`。`detach_slot` 的具体原生原语待确认（§12）。

### 6.3 交换

- **同一张外部表**：预检两个槽非空 → 读取 A、B 指针 → 两次普通赋值交换。外部表赋值不会失败，这是唯一提供的原子交换。
- **原生表**：交换由游戏自己的移动/交换路径处理；插件不提供跨表交换 API。
- **UI 拖拽跨表**：拆成两次独立移动（每次都是“预检 → 写目标 → 清源”）；任一次失败即停止并以真实槽表刷新，不做恢复、不补做第二次。
- 不引入临时槽，不引入 pending。

### 6.4 增加

```text
1. 创建原生 item 对象（可失败；失败即返回，无副作用）
2. 预检目标槽为空/容量足够
3. 写目标槽（原生 place_slot 或外部 set）
4. 写失败 → 释放刚创建的对象（资源清理，不是回滚；槽未变）
5. 写成功 → 返回新 ItemKey
```

- 创建失败和写入失败都只影响本次调用，不改变已有物品。
- “释放刚创建但未落位的对象”是清理未使用资源，不是回滚已发生的业务状态；代码路径只有一条 `free`，没有恢复分支。
- 外部表默认**不做自动堆叠合并**：新增只找空槽落位；原生表内的同类别合并仍由原生 `SaveItem`/移动完成。合并到外部表已有堆的功能不在此设计内，避免引入“先合并再落位失败”的多步路径。

### 6.5 移除/消耗

```text
1. 预检源槽/身份/数量
2. 清源槽（原生 detach_slot 或外部 set(nullptr)）
3. 释放对象或交给原生动作
```

- 清槽是不可失败的一步；释放对象在清槽之后，即使释放失败也不会留下“槽里还有指针”的状态。
- 部分数量：先按数量拆分原生对象（原生 `ITEMSYSTEM_Divide` 或来源自己的拆分），再走移动；不引入 pending。

### 6.6 使用/装备/卸下/出售

统一模式：**原生动作决定物品是否离开源槽；插件只在物品确实被消耗/转移时清槽。**

| 动作 | 顺序 |
|---|---|
| 使用 | 调原生 `CHAR_UseItemEx(ch,item,flag)`；插件**不在返回后盲目清槽**。消耗由嵌套的 `INVEN_ConsumeItem`/`INVEN_RemoveItem` hook 负责清槽；未消耗的使用保留槽。若某原生使用路径直接释放对象而不经过这两个 hook，该路径对外部物品 fail-closed。 |
| 装备 | 调原生装备原语（item 指针或原生 bag/slot）；成功且原生已把对象放入装备槽 → 清源槽；失败或原生未接管 → 源槽不动。 |
| 卸下 | 只有当存在“从装备槽摘出对象但不放入背包”的原生 detach 原语时才支持外部表目标；没有该原语时外部卸下 fail-closed。不能用“先卸到原生槽再移动”，因为需要外部兜底的场景正是原生满。 |
| 出售 | 只使用原生按 item 指针原子完成“定价 + 加钱 + 移除”的入口；没有该入口时外部物品出售 fail-closed。没有“加钱成功但移除失败”的补偿分支。 |
| 合成产物 | 原生创建对象后走 §6.4 增加；材料消耗走 §6.5。 |

- 不使用“先扣物品、失败再补回”的顺序。
- 如果某原生动作无法在外部物品上安全执行，直接 fail-closed，不做近似实现。
- 表里的“清源槽”都是单次指针写；不在清槽后再调用可能失败的业务函数。

### 6.7 失败语义

- 所有失败都是**操作前失败**或**目标写失败**；两种情况下源槽都不变。
- 唯一的“已成功一半”风险是原生动作本身不原子（例如出售的加钱与移除）；这类路径要求使用原生原子入口，否则不提供。
- 没有回滚函数、没有补偿函数、没有 pending 队列、没有恢复日志。

## 7. 原生系统接入（函数级 hook）

### 7.1 原则

- 只做函数入口 hook；不改指令。
- 每个 hook 有透明透传路径；插件关闭时与原版一致。
- hook 只做：解析槽属于哪张表、建立原生作用域、把结果映射回原 ABI。
- 原生表操作走一次 `NativeActionPort`；外部表操作直接 `set`，不调用 backup。

### 7.2 hook 分类

| 类别 | 语义 | 失败策略 |
|---|---|---|
| R 读聚合 | 原生结果 + 外部表聚合；无副作用 | 外部表不可用时返回原生结果并标记 degraded |
| N 原生专用 | 只接受原生索引/原生表；不虚拟化 | 透传；外部目标返回原生失败 |
| S 槽表路由写 | 解析槽表；原生走 backup 一次，外部直接 set | 预检失败不写 |
| D 显示投影 | 只替换 UI 读取到的物品/容量 | 投影未就绪时透传 |
| G 调用方门控 | 在“先检查再保存”的系统入口补齐外部容量 | 门控失败保持原版失败 |

### 7.3 读查询

| 原生函数 | 类别 | 行为 |
|---|---|---|
| `INVEN_FindItem(category)` | R | 先原生表；再按来源顺序扫外部表；返回对象指针。 |
| `INVEN_HaveItem(category)` | R | 原生表或任一外部表存在该类别。 |
| `INVEN_GetItemCount(category)` | R | 原生计数 + 各外部表计数。 |
| `ITEM_GetCumulateCount(item)` | N | 解码单个物品逻辑数量；外部表对象也是原生对象，直接透传。 |
| `INVEN_IsHavingEmptySlot` | R | 原生空槽或任一外部表有容量。 |
| `INVEN_FindItemSlot(item)` | N | 只查原生表；外部表对象返回未找到，不伪造槽号。 |
| `INVEN_GetBagSize(bag)` | D | 只在投影作用域内返回窗口容量；否则透传。 |

已知限制：原生路径若先用 `INVEN_FindItem` 拿到外部表对象，再调用 `INVEN_FindItemSlot` 或按原生槽删除，会找不到该对象。实现前必须审计这些调用链；不能审计的路径对外部物品 fail-closed，不用假槽号补救。

### 7.4 入库与保存

| 原生函数 | 类别 | 行为 |
|---|---|---|
| `INVEN_SaveItem(item)` | S | 原生表：backup 一次；成功返回。原生满但外部表有容量：直接把 `item` 指针写入外部空槽（一次 `set`），成功返回；外部无容量则返回原生失败。没有序列化、没有新建对象、没有释放分支。 |
| `INVEN_SaveItemOnEmpty(item,bag,slot)` | S | 原生目标：backup 一次；外部目标：直接 `set`。调用前校验真实容量。 |
| `INVEN_FindSaveSlot(item,out)` | G/N | 默认 backup；只有在调用方门控作用域内、原生失败且外部有容量时返回一次性 token（§7.4.1）。 |
| `INVEN_SaveItemDirect(item,bag,slot)` | S | 原生索引：backup 一次；token：消费并写入外部表；外部袋号不可表达，返回失败。 |
| `INVEN_MoveItem(item,count,bag,slot)` | S | 原生源+原生目标：backup 一次；任一侧外部：走 §6.2 移动。 |
| `INVEN_RemoveItemData(category,count)` | N | 只操作原生表；外部表物品不参与原生按类别批量删除。返回原生实际扣除数；不跨表补扣、不回滚。外部表物品的删除走 `RemoveItem`/`ConsumeItem` 或动作层逐槽处理。 |

#### 7.4.1 一次性保存 token

只有在确认某调用链“`FindSaveSlot` 的返回值只用于紧接着的保存调用”时才启用：

- 调用方入口（拾取/商店）建立 `SaveGateScope`，记录类别和外部目标表。
- 作用域内 `INVEN_FindSaveSlot` 先 backup；失败且外部有容量时返回成功并写入一次性 token。
- 紧随的 `INVEN_SaveItem`/`INVEN_SaveItemDirect` 识别 token，直接写入外部表；不调用 backup。
- token 只存在于当前调用作用域（普通全局/栈对象，单线程不需要 thread-local），不落存储、不进入任何持久化状态。
- 未审计的调用链不发放 token；这些链在原版满包时保持原版失败。
- **这是设计中唯一保留的非平凡机制。** 如果产品不要求“原生满时拾取/商店进入外部表”，直接删除本节：外部表只接收已经走到 `INVEN_SaveItem` 的物品，原版预检失败的链保持原版失败。

### 7.5 移、删、耗

| 原生函数 | 类别 | 行为 |
|---|---|---|
| `INVEN_RemoveItem(item)` | S | 原生对象：backup 一次；外部表对象：先通过原生关联清理入口清除快捷栏/详情等引用，再清槽 + 释放对象；没有安全关联清理入口时 fail-closed。 |
| `INVEN_ConsumeItem(item)` | S | 原生对象：backup 一次；外部表对象：数量减一或整堆清槽；同样需要处理原生关联引用；失败返回原版失败值。 |
| `ITEMSYSTEM_Divide(item,...)` | S | 原生对象：backup 一次；外部表对象：原生拆分后把新对象写入空槽；无空槽则释放拆分出的新对象并返回失败，源槽不变。 |
| `ITEMSYSTEM_MakeItem(...)` | N/G | 只创建对象；落位由后续 `SaveItem` 处理。 |
| `ITEMSYSTEM_PutJewel(...)` | S | 原生对象：backup 一次；外部表对象在来源不支持时 fail-closed。 |

### 7.6 装备、使用、出售、合成、商店、拾取

| 系统 | 入口 | 类别 | 行为 |
|---|---|---|---|
| 使用 | `CHAR_UseItemEx(ch,item,flag)` | S | 原生/外部表对象都调用一次 backup；不在返回后盲目清槽，消耗由嵌套 `ConsumeItem`/`RemoveItem` hook 清槽；未消耗的使用保留槽。 |
| 快捷栏 | `CHAR_ProcessShortcut`、`SHORTCUT_GetState` | G/R | 快捷栏指向投影窗口时解析为外部槽；否则透传。 |
| 装备 | `CHAR_EquipItemFromInvenToSlot` 或接受 item 指针的装备原语 | S | 原生来源 backup 一次；外部表对象用指针原语；成功且原生接管对象后清源槽。 |
| 装备 UI | `UIEquip_ButtonEquipExe` 等 | G | 从控件解析物品；外部表对象转 `SlotRef` 后走动作层；原生对象透传。 |
| 卸下 | `CHAR_UnequipItemToInven` | S | 只有存在“从装备槽摘出对象但不放入背包”的原生 detach 原语时才支持外部表目标；否则外部卸下 fail-closed。不能用“先卸到原生槽再移动”兜底，因为外部兜底场景正是原生满。 |
| 出售 | 原生出售入口 | S | 只使用原生按 item 指针原子完成“定价 + 加钱 + 移除”的入口；没有该入口时外部物品出售 fail-closed。 |
| 商店购买 | 商店入口 + `INVEN_SaveItem` | G/S | 空位检查计入外部表；入库走 `SaveItem` 外部写入；货架清除在入库成功后由原生执行。 |
| 合成 | `ITEMSYSTEM_MakeItem` + `INVEN_SaveItem` | N/S | 材料消耗按类别扣；产物走 `SaveItem`。 |
| 拾取 | `CHAR_PickItemAll` / 拾取事件入口 | G | 原生满时通过 `SaveGateScope` 让保存调用落到外部表；不虚拟化 `FindSaveSlot` 为真实槽号。 |
| 开箱/打包 | `ITEMSYSTEM_OpenItemBox`、`ProcessUnpack` | N/S | 产物走 `SaveItem`；具体入口待确认。 |

#### 7.6.1 背包装备（`INVEN_pBagSlot` 安装）

1. 原生空袋槽由 `INVEN_GetEmptyBagSlot` 取得；容量/类别校验用原生规则。
2. 原生物品：走原生装备路径，backup 一次。
3. 外部表物品：对象已经是原生 item；直接写入 `INVEN_pBagSlot[slot]`，成功后清源槽。
4. 写入失败：源槽不变；对象仍在插件袋。
5. 安装成功后该物品成为原生袋对象，进入原生存档流；插件袋槽已清空，不会双份。

### 7.7 显示

| 原生函数 | 类别 | 行为 |
|---|---|---|
| `ControlItem_SetItem(ctrl,item)` | D | 投影作用域内，把窗口槽对应的外部表指针直接传给控件；深度 > 0 时透传。 |
| `ControlItem_GetItem(ctrl)` | D | 投影作用域内返回外部表指针；否则透传。 |
| `INVEN_GetBagSize(bag)` | D | 只在投影作用域内返回窗口容量；否则透传。 |
| `UIEquip_RefreshItemArea` | D | 建立/提交/取消投影作用域；失败时抑制替换并恢复原生控件。 |

投影不变量：

1. 不写 `INVEN_pItem`、不写袋容量字。
2. 控件 item 只是显示副本；槽表才是真源。
3. 每个投影安装/恢复成对；失败不留下半投影。
4. root/control 指针只在当前帧有效。
5. **投影有效期内禁止写动作命中窗口袋**；写动作解析到窗口 bag 时重定向到真实表或 fail-closed，避免写入超出真实容量的槽。

### 7.8 存档

- 原版存档流不改写、不重定向；原生袋由游戏保存。
- 存档顺序冻结：先让 `SAVE_SaveInventory` 完成，再写插件 sidecar。sidecar 写在同一存档流内，不跨存档、不延迟到下一帧。
- 插件袋在存档点由 `SaveCoordinator` 批量序列化：遍历外部表非空槽 → 原生 `SAVE_SaveItem` 取 payload → 整份 sidecar 原子写（临时文件 + rename）。
- **sidecar 记录保存时刻的原生 UID**（不是读档后重新分配的 UID）。
- sidecar 按存档槽隔离；`persist_key` 必须绑定当前存档槽，读档时只加载当前槽的 sidecar，不读取其他槽。
- 读档时批量反序列化：逐条 `SAVE_LoadItem` 生成原生对象，先放入临时表；全部成功后整体替换外部表，任一条失败则该来源保持空表并上报。临时表是内存操作，不是 pending 状态。
- **读档 UID 去重**：若 sidecar 记录的 UID 已存在于原生袋，丢弃该 sidecar 副本（原生袋有存档背书）。这消除“复制”，只剩存档时序窗口内的“丢失”。
- 物品移动不触发任何 sidecar 写；sidecar 只反映存档点那一刻的外部表。
- 不把外部袋号编码进 `FindSaveSlot`/`SaveItemDirect` 的真实槽号；token 只在内存中存在。

外部表对象生命周期：

- 外部表里的对象是原生 item 池对象；它们不在 `INVEN_pItem`，游戏的原生槽表扫描不会主动管理它们。
- 任何可能清空/重建 item 池的游戏生命周期点（读档、换图、重置库存）都必须 hook 到，或者在该点把外部表清空并从 sidecar 重建。重建是批量反序列化，不是逐项 pending。
- 未确认生命周期点之前，外部表只在“进档后到下一次读档/换图”之间保证有效；生命周期点之后按 sidecar 重建。
- 不把外部表对象登记到原生槽表来“保活”；那会违反“不写 `INVEN_pItem`”的约束。

### 7.9 重入与副作用一次

- `NativeOperationScope` 是进程内单线程上下文（普通全局/栈对象，不用锁）：记录 `{operation, expected_slot, expected_key, native_called}`。
- 进入写 hook 创建作用域；调用 backup 前置位；嵌套 hook 只观察/透传。
- backup 返回后复核：目标槽是否按预期变化、`native_called` 是否恰好一次。
- 作用域退出清除；异常路径由 RAII 保证。

## 8. UI 投影

- 原生 UI 只认识原生袋号。外部表通过“窗口 bag”显示：借用原版事务袋 `0..4` 之一，窗口内槽位映射到外部 `{source,bag,slot}`。
- 投影上下文 `{source,bag,window_bag,root_controls}`；`ControlItem_SetItem/GetItem` 和 `INVEN_GetBagSize` 只在上下文有效且控件匹配时替换。
- 输入由 `UiAdapter` 解析：控件 item 指针 → 查外部槽表 → `SlotRef + ItemKey`；原生对象则走原生路径。
- 投影窗口的 `current_bag` 只能是原生合法索引；插件袋号不得写入。
- 退出投影先取消上下文，再恢复原生刷新。
- 投影期间写动作命中窗口 bag 时，按 §7.7 第 5 条拒绝或重定向。

## 9. 数量与 S2

- 背包系统只处理逻辑数量；`CountCodec` 负责逻辑数量与 `128a+b` 位域转换。
- 只有 `applied 999` 且 count-encoded 类别启用 S2 写回；写前采样 pre、写后确认 post；`post == pre` 不推断写发生。
- `delta % 128 == 0` 的合并必须由“源槽已不再指向被移动对象”证明；确认失败 fail-closed。
- 外部表对象同样是原生 item，数量读写走同一个 `CountCodec`。

## 10. 单线程、持久化边界与错误

### 10.1 单线程

- 所有 hook、槽表操作、投影、存档协调都在游戏主线程。
- 不加锁、不使用原子变量、不做跨线程命令队列。
- 没有“后台重试”“异步补偿”“延迟提交”。

### 10.2 错误模型

统一 `BagStatus`：`Ok / NotReady / InvalidRef / Stale / Empty / Conflict / Full / TaskBagReadOnly / Unsupported / Internal`。

- 失败即不写；调用方重新读取状态，不假设部分成功。
- 原生 ABI 只接受原生的 0/1/指针返回值；插件错误码不暴露给游戏。
- R 类 hook 在外部表不可用时返回原生结果并标记 degraded。

### 10.3 生命周期

- 系统停用：先拆除投影（控件不再持有外部表指针），再注销外部表并释放表内对象；hook 全部透传；快照清空。
- 单个来源停用只影响该来源；前置条件是投影已切走，然后清表并释放对象；原生袋和其他来源继续工作。
- 资源安装失败由 Loader 撤销已安装的 hook（机制层动作，不涉及物品状态）。

### 10.4 明确边界：跨存储崩溃一致性

- 运行期物品操作没有回滚、没有 pending、没有 journal。
- 运行期“写目标槽”和“清源槽”之间发生进程崩溃是**无害的**：两张表都是易失内存，重启后各自回到上次存档点，即移动前状态。
- 真正有害的只有**存档时序窗口**：游戏写原生袋与插件写 sidecar 不是同一瞬间；窗口内跨存储的移动可能只落在一侧。
- 本设计用两条规则收窄该窗口：
  1. 顺序冻结：先 `SAVE_SaveInventory`，再写 sidecar。
  2. 读档 UID 去重：sidecar 中与原生袋 UID 冲突的副本丢弃，保留原生。
- 结果：不会出现“复制”；仍可能出现“崩溃窗口内丢失”。要消除丢失必须引入持久化日志/两阶段提交，那会重新引入本设计刻意删除的复杂度和排查成本。
- 这是明确接受的取舍，不在实现中增加补偿或恢复分支。

## 11. 原生兼容性验收

1. **零外部来源**：所有 hook 透传；原生查/数/存/移/删/耗/装备/使用/出售/商店/合成/拾取/存档与原版一致。
2. **原生袋满 + 外部表有容量**：拾取/商店/合成产物把已创建的原生对象指针写入外部空槽；不弹“空间不足”；不产生序列化或新建副本。
3. **原生袋未满**：优先原生；外部表不被误用。
4. **外部物品动作**：在原生提供对应指针/原子原语时，使用、装备、卸下、出售、丢弃各成功一次，源槽清空一次，不复制不丢件；原生没有安全原语的动作，fail-closed 是合格结果，必须显式上报，不能近似实现。
5. **任务袋**：可读；任何写入/移动/装备/出售被拒绝。
6. **C18**：追加袋识别为原生来源；袋数/容量/槽位正确；索引 18 不是真实袋；`current_bag` 只写原生索引。
7. **存档/读档**：原生袋由游戏保存；插件袋批量恢复；读档后身份/数量/payload 一致；不存在半对象。
8. **失败路径**：预检失败、目标写失败、原生动作失败都保持源槽不变；没有回滚分支。
9. **停用/启用**：运行中停用外部来源不崩溃；重新启用后槽表正确。
10. **投影**：外部袋第一帧正确显示，退出无残留；`INVEN_pItem` 和容量字字节不变；投影期间无写动作落入窗口袋。
11. **背包装备**：外部袋物品装入原生袋槽后只存在一份；写入失败源槽不变。

## 12. 待确认事实

**阶段 0（阻断，未通过不得进入实现）**

1. item 池是否会在读档、换图、重置库存等生命周期点释放“不在原生槽表中的对象”。用哨兵对象跨生命周期验证；失败则本设计的指针表模型降级为 `{uid,payload}` + 动作期瞬态物化（§0）。
2. 原生 item UID 的稳定偏移、有效性、跨读档是否复用；sidecar 记录原 UID、读档按 UID 去重是否成立。

**实现前必须确认**

3. `INVEN_SaveItemOnEmpty`、`INVEN_RemoveItemData`、`INVEN_FindItemSlot` 的精确签名、返回值和副作用。
4. 原生“只清槽不释放对象”的 detach 原语是否存在；不存在时受控写槽 + 原生刷新是否安全，刷新路径不得解引用/释放刚移出的对象。
5. `INVEN_SaveItem` 失败后是否留下槽位/全局状态副作用；调用方是否释放对象、是否使用返回槽号。这决定外部空槽写入能否安全返回成功。
6. `CHAR_UseItemEx`、`INVEN_ConsumeItem` 的返回值语义：是否表示消耗、是否释放对象、是否经过 `ConsumeItem`/`RemoveItem` hook。不能判定时外部物品使用 fail-closed。
7. 是否存在接受 item 指针的装备/出售原语；是否存在“从装备槽摘出对象但不放入背包”的卸下原语。没有则对应外部动作 fail-closed。
8. `INVEN_RemoveItem` 对快捷栏、详情、选择状态等原生关联的清理入口；外部表对象删除前必须先清关联，否则 free 后游戏侧悬空。
9. `INVEN_pBagSlot` 安装/替换袋对象、`INVEN_GetEmptyBagSlot` 的失败语义与容量/角色状态重算。
10. C18 各版本的袋对象表、槽表、`current_bag` 上界和页签映射。
11. `SAVE_SaveInventory` 的完成点、sidecar 写入时机、sidecar 与存档槽的绑定方式。
12. `ITEMSYSTEM_Divide` 拆分出的新对象在落位失败时的所有权和释放路径。
13. 拾取/商店调用链是否在 `INVEN_SaveItem` 前用 `INVEN_FindSaveSlot` 预检；`SaveGateScope` token 是否会被中间步骤使用。

## 13. 建议落地顺序

0. **阶段 0 阻断实验**：哨兵对象跨读档/换图/清池，验证 item 池是否释放不在原生槽表中的对象；同时验证 UID 跨读档稳定性。未通过则按 §0 降级。
1. **原生槽表适配**：读/写/清槽/容量/UID，先无 hook 跑通只读。
2. **领域层**：`BagTableRegistry`、`NativeTable`、`ExternalTable`、`InventoryReader`、`InventoryExecutor`、`ActionService`，用 fake 表做 Host 测试。
3. **同表移动/增加/移除**：验证“预检 + 单次写 + 清源”没有回滚分支。
4. **读 hook**：查询、计数、空槽、`FindItem`。
5. **写 hook**：SaveItem/SaveItemOnEmpty/Move/Remove/Consume/RemoveItemData + `NativeOperationScope`。
6. **外部表**：注册、容量、直接写入、跨表移动。
7. **动作路由**：使用、装备、卸下、出售、商店、合成、拾取门控、背包装备。
8. **投影**：`ControlItem_SetItem/GetItem`、`INVEN_GetBagSize`、刷新作用域、写排斥。
9. **存档协调**：外部表批量序列化/反序列化。
10. **真机验收**：按 §11 逐项取证。

## 14. 一句话边界

**所有袋都是原生 item 指针的槽表；层间只传位置和身份，不流转 ItemData/payload；操作只有“预检 → 写目标 → 清源”，失败即无操作；没有回滚、没有 pending、没有 journal。**
