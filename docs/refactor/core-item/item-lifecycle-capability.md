# 物品生成与生命周期能力（inventory.action v3.3）

本文是 core.item 物品生成/落位/释放能力的实施方案。目标是把「生成 → 数据面配置 → 入库 → 失败释放」
收敛到 core.item 能力面，同时把内容定制保留在现有数据面端口。

## 1. 背景

现状三条路径各自为政：

- 生成/入库/释放：custom_recipe 直连宿主端口 `QolCustomRecipeWritePort`
  （`game/adapter_ports/craft_port.cpp`）：`create_perfect_item`/`create_item` → 数据面写入 →
  `save_item`（`INVEN_SaveItem`）→ 失败 `free_item`。
- 背包内消耗：`inventory.action` v3.2 `discard(ref, count)`（`plugins/core.item/usecases/item_action.cpp:417`），
  必须有显式袋引用。
- 底层已有原语：`QolItemInventoryStateWritePort` 2.2 的 `add_item`（创建+基础字段+找槽+合并）、
  `load_item`（detached 全保真）、`place_item`（精确/自动落槽）、`remove_item`（按句柄扣数量）、
  `QolItemPort.read.serialize_item`（3.8，SAVE_SaveItem 序列化）；`ops::free_loaded_item`
  （`game/ops/item_inventory_state_write.cpp:249`）可释放 detached 句柄但未暴露到端口。

缺口：

1. 生成/落位/释放没有 core.item 能力面，插件直连宿主端口，违背「物品副作用由 core.item 执行」的
   统一入口约束（`contracts/inventory_action.h:10-12`）。
2. detached 句柄（`load_item`/create 产物）没有主动释放口；现有端口只在 `place_item` 失败路径由
   Host 释放，「调用方主动放弃」无口可走。
3. `inventory.action.discard` 只接受有位置的 ref，无位置句柄无法释放或删除。

## 2. 设计决定

1. **不新增独立能力 id**；在现有契约尾部追加，版本递增：
   - `QolItemInventoryStateWritePort` 2.2 → 2.3：+ `create_item` / `create_perfect_item` / `free_item`。
   - `InventoryActionApi` 3.2 → 3.3：+ `create` / `place` / `discard_handle`。
2. **create 返回调用方拥有的 detached 句柄，不落位**。配置窗口内物品不进入背包；落位策略在 place
   时决定（auto 找槽+合并 / 精确槽 / 扩展袋导入）。
3. **数据面定制不进 API**：宝石字、原生词条、打孔、数量继续走现有端口
   （`write_jewel_word` / `write_native_option_value` / `QolItemCountApi`）；需要整物品保真时用
   `create` 的 payload 形态（`SAVE_LoadItem`）。
4. **place 失败语义按落位介质区分**：原生空间复用既有 `place_item` 的 Host 语义——失败时对象已由
   Host 释放、句柄失效；注册源（扩展袋）导入失败时句柄保持 detached，调用方可重试或
   `discard_handle` 放弃。两阶段 API 的「重试/放弃权」只对注册源路径成立，契约注释与文档必须
   写清这一点，调用方不得对原生失败后的句柄再做任何操作。
5. **所有权**：create 后 owner=调用方（`kQolLoadedItemHandleOwner`）；place 成功时 core.item 内部
   `rebind_item` 转 game-owned 后写入库存；`discard_handle` 对 detached 走 `free_loaded_item`，
   对背包物品走 `remove_item(handle, count)`。
6. **扩展袋落位**：detached 物品经 `QolItemPort.read.serialize_item` 序列化，组装 `QolBagTransfer`
   后调用 `QolBagSource::import_item`；导入成功后释放 detached 原对象。物理空间直接 `place_item`
   （原生空间限定 original/variant）。

## 3. 契约

### 3.1 QolItemInventoryStateWritePort 2.3

```cpp
// create_item / create_perfect_item 返回调用方拥有的 detached 句柄；失败不产生句柄。
// free_item 仅接受 detached 句柄：成功即 invalidate + itempool free；背包内物品拒绝（走 remove_item）。
using QolItemInventoryStateCreateItemFn = QolResult (*)(
    int32_t category, int32_t arg1, int32_t arg2, int32_t arg3, QolItemHandle* out_item);
using QolItemInventoryStateCreatePerfectItemFn = QolResult (*)(
    int32_t category, QolItemHandle* out_item);
using QolItemInventoryStateFreeItemFn = QolResult (*)(QolItemHandle item);

struct QolItemInventoryStateWritePort {
    ...
    QolItemInventoryStateCreateItemFn create_item;              // 尾部追加
    QolItemInventoryStateCreatePerfectItemFn create_perfect_item;
    QolItemInventoryStateFreeItemFn free_item;
};
```

版本常量 `contracts/game_ports/ports.h`：`kQolPortItemInventoryStateWriteVersion{2, 3}`。

### 3.2 InventoryActionApi 3.3

```cpp
struct QolItemCreateSpec {
    uint32_t struct_size;
    int32_t category;
    int32_t count;            // <=0 视为 1；>1 需可堆叠
    uint32_t flags;           // bit0 = perfect item
    const uint8_t* payload;   // 可选：SAVE_LoadItem 数据；非空时优先于 category 创建
    uint32_t payload_size;
};

struct QolItemPlaceTarget {
    uint32_t struct_size;
    QolBagRef ref;            // ref.bag < 0 → 原生空间自动找槽
    int32_t slot;             // -1 → 自动
    uint32_t flags;           // bit0 = 允许合并/adopt
};

struct QolItemCreateOutcome {
    uint32_t struct_size;
    InventoryActionStatus status;
    QolItemHandle item;
};

struct QolItemPlaceOutcome {
    uint32_t struct_size;
    InventoryActionStatus status;
    QolBagRef ref;            // 实际落位
    QolItemHandle item;       // 落位后句柄（扩展袋为导入后的新句柄）
    int32_t count;
    uint8_t merged;
};

struct InventoryActionApi {
    ...
    QolItemCreateOutcome (*create)(const QolItemCreateSpec* spec);
    QolItemPlaceOutcome (*place)(QolItemHandle item, const QolItemPlaceTarget* target);
    InventoryUseOutcome (*discard_handle)(QolItemHandle item, int32_t count);
};
```

语义：

| 调用 | 成功 | 失败 | 所有权 |
|---|---|---|---|
| `create(spec)` | detached 句柄，owner=调用方 | 状态码，无句柄 | 调用方 |
| `place(item, target)` | 写入库存 + 返回实际落位 | 原生空间：Host 释放对象、句柄失效；注册源：句柄保持 detached | 成功转 game-owned |
| `discard_handle(item, count)` | 背包物品扣数量 / detached 释放 | 状态码，无副作用 | 成功后句柄失效 |

- `create`：payload 非空 → `load_item`；否则 `create_perfect_item`（flags bit0）或 `create_item`
  （无 search_tbl 校验）。count>1 经 `QolItemCountApi` 写 canonical count；不可堆叠且 count>1 返回
  `kUnsupported`。
- `place`：原生空间 → `place_item`（slot=-1 自动找槽；合并由存档路径处理）；扩展袋
  （space_id >= `kQolBagSpaceFirstRegistered`）→ 序列化 + `import_item` + 释放 detached。
- `discard_handle`：detached → `free_loaded_item`；背包物品 → `remove_item(handle,count)`；
  无法解析 → `kInvalidRef`。

### 3.3 兼容性

- 两个结构均为尾部追加；旧调用方按自身 `struct_size` 访问，不受影响。
- `inventory_action_api_valid` 保持旧尺寸可接受，新函数仅在 `struct_size` 覆盖到对应字段时校验非空。
- 能力版本主/次：`InventoryActionApi` 3.3（minor 递增），`QolItemInventoryStateWritePort` 2.3。

## 4. 实现落点

| 层 | 文件 | 改动 |
|---|---|---|
| 契约 | `contracts/inventory_action.h` | 结构/函数指针/版本 3.3/valid |
| 契约 | `contracts/game_ports/item_inventory_state_write_port.h`、`ports.h` | 3 个函数 + 版本 2.3 |
| Host | `host/item_inventory_state_write_port.cpp` | 包装 ops create/free + kPort 初始化 |
| Ops | `game/ops/item_inventory_state_write.{h,cpp}` | detached create（loaded owner 注册）复用现有 free/load |
| core.item | `plugins/core.item/usecases/item_action.cpp` | create/place/discard_handle + API 实例追加 |
| 测试 | `tests/test_inventory_action_lifecycle.cpp`（新增） | create/place/discard_handle 三态 + 兼容性 |
| 文档 | `docs/reference/plugins/core.item.md` | 能力表与 v3.3 方法表 |

## 5. 测试与验收

- host ctest：新增 target 通过；全量不新增红（当前既有红 4 项：`custom_recipe_tests`、
  `item_handle_contract_redline`、`core_api_lifecycle_tests`、`core_settings_config_store_tests`）。
- `python3 scripts/maintenance/check_arch.py`、`check_http_routes.py` 通过。
- `scripts/build-debug.sh` 成功。
- 本能力无 UI/行为变化，真机验收随 custom_recipe 迁移卡。

## 6. 迁移次序

1. 本能力（契约 + host/ops + core.item + 测试）。
2. custom_recipe：create/configure/save 迁移到 `inventory.action.create/place` + 数据面端口；
   `QolCustomRecipeWritePort` 的 `create_perfect_item`/`create_item`/`save_item`/`free_item` 退役
   （stuff UI 写入保留）。
3. 验收：填第 N 个宝石扣第 N 个（已由 160ae780 完成）、堆耗尽回退、扩展袋物品、非堆叠不跨物品、
   产物落位与失败回滚。

## 7. 非目标

- 不做批量生成、事务队列、UI 改动。
- 不改 R-56（扩展袋类别补扣）、H-13（保存合并/adopt）既有语义。
- 不引入克隆源（`source_handle`）——v1 用 payload 形态覆盖整物品保真需求。
