# core.item payload 与 core.inventory replace 接口方案

> 状态：代码已实现；真机验收 NOT_ACCEPTED（待 `docs/refactor/verification.md` 对应卡取证）。
> 范围：替换“游戏侧原地改写扩展物品后不写回权威存储”的临时 commit 方案。
> 相关：`docs/refactor/core-item/bag-system-design.md`、`docs/refactor/core-item/hard-constraints.md`、
> `docs/refactor/extension-bag/rulebook.md`、`docs/refactor/verification.md`。

## 0. 结论

物品内容变更拆成两段：

1. **`core.item` 负责 payload 与对象字段**：从物品句柄或已有 payload 出发，应用类型化字段修改，
   生成**新 payload**；有 `source` 时把同一组 mutation 应用到源对象，保留对象身份与句柄。
   只保证结构合法，不保证内容合法；不写槽、不改 provider 存储。
2. **`core.inventory` 负责 replace**：确认 `expected` 仍是当前物品，把 payload 原地写回
   既有对象（小改动，保留 handle）；provider 再分派 `QolBagSource::replace_item` 提交
   descriptor/hash/dirty/刷新。结构变化需要整体替换时由 replace 后续分支处理。
   内容不校验；不换对象、不释放旧对象。

调用方（如 `custom_recipe`）负责读数据、计算新值、串联两段。

**为什么保留对象身份**：craft UI 的 stuff 控件与 `UseStuff` 在 `api_make_item` 返回后仍持有
输入物品的原生指针。因此升阶这类小改动的对象更新放在 `replace_item` 内部完成：按 payload
原地写回既有对象，保留对象地址和 handle；`update` 仍只生成 payload。provider 随后提交权威副本。

## 1. 职责边界

| 层 | 拥有 | 不拥有 |
|---|---|---|
| `custom_recipe` 等业务插件 | 业务计算、字段/值选择、调用顺序、材料 | 序列化、槽位写入、对象指针 |
| `core.item` | payload 更新、字段写入源对象、结构校验、UID 策略 | 槽位写入、provider 分派、UI 刷新 |
| `core.inventory` | 定位、replace 分派、provider 元数据提交 | payload 内容语义、对象生命周期 |
| `extension_bag` | provider descriptor/hash/category 更新、投影刷新 | payload 内容语义、对象指针替换 |
| `game/` | load/serialize/free、字段偏移写入 | 业务决策 |

## 2. 调用链

```text
custom_recipe
  1. 取得目标 handle（craft UI 已有）
  2. 读取 item.facts / attribute.quality
  3. 计算新值
  4. item.payload.update(source=handle, mutations) -> payload（纯 payload，不改对象）
  5. 写材料 need（现有 stuff need）
  6. inventory.transfer.replace_item(expected=handle, replacement) -> 同一 handle
     （replace 内部把 payload 原地写回对象；provider 再提交 descriptor/hash/dirty/refresh）
```

顺序约束：

- 第 4 步先装载临时对象、应用 mutation、重新序列化并通过结构校验，全部成功后才对源对象
  应用 mutation；失败时源对象不变。
- 第 6 步是唯一提交 provider 权威副本的点；失败时 descriptor 保持旧值（对象可能已是新值，
  与重构前的 commit 失败语义一致，日志记录）。
- need 在第 5 步写，保持“replace 失败时材料未被 UseStuff 消耗”的现有语义。

## 3. 契约

### 3.1 `item.payload` 1.0（owner `core.item`）

```cpp
// contracts/item_payload.h
namespace qol::contracts {

constexpr const char* kItemPayloadCapabilityId = "item.payload";
constexpr uint16_t kItemPayloadVersionMajor = 1;
constexpr uint16_t kItemPayloadVersionMinor = 0;

enum class QolItemPayloadMutationKind : int32_t {
    kJewelWord = 1,           // 仅宝石；完整 u32 word
    kNativeOptionValue = 2,   // 装备原生词条；index=第 N 条，value=int16
    kCount = 3,               // canonical 数量；仅可堆叠类别
    kSocketLevel = 4,         // 0..15
    kSocketFilled = 5,        // 0..15
    kEnchantLevel = 6,        // 0..31（I_ENCHANT bits6-10）
    kEnchantRemaining = 7,    // 0..15
    kMagicRate = 8,           // 0..255
};

struct QolItemPayloadMutation { int32_t kind; uint32_t index; int64_t value; };

struct QolItemPayloadUpdateSpec {
    uint32_t struct_size;
    QolItemHandle source;          // 与 base_payload 二选一
    const uint8_t* base_payload;
    uint32_t base_payload_size;
    const QolItemPayloadMutation* mutations;
    uint32_t mutation_count;
    uint32_t flags;
};

constexpr uint32_t kItemPayloadUpdateValidateStructure = 1u << 0;
constexpr uint32_t kItemPayloadUpdatePreserveUid = 1u << 1;

struct QolItemPayload {
    uint32_t struct_size;
    uint32_t category;
    uint32_t count;
    uint32_t payload_size;
    uint8_t payload[kQolBagTransferPayloadCapacity];
};

struct QolItemPayloadValidation {
    uint32_t struct_size;
    uint32_t consumed;
    uint32_t option_count;
};

struct ItemPayloadApi {
    uint32_t struct_size;
    uint16_t version_major;
    uint16_t version_minor;
    QolResult (*update)(const QolItemPayloadUpdateSpec* spec, QolItemPayload* out);
    QolResult (*validate)(const uint8_t* payload, uint32_t payload_size,
                          QolItemPayloadValidation* out);
};

}  // namespace qol::contracts
```

`update` 内部：

1. `source` 非空 → `QolItemPort.read.serialize_item` 得到 base payload；否则使用 `base_payload`。
2. game loader 把 base payload 装载成临时对象。
3. 按 mutations 调 game 层 typed 写入临时对象。
4. 重新序列化临时对象并释放。
5. 按 flags 做结构校验。
6. **有 `source` 时**把同一组 mutation 应用到源对象（保留对象身份）。
7. 返回 payload + category + count。

`update` 的错误语义：

- `kQolErrInvalidArgument`：source/base 同时提供、mutation 越界、kind 未知；
- `kQolErrNotFound`：source 句柄失效；
- `kQolErrUnsupported`：序列化/装载/字段写入能力缺失；
- `kQolErrInternal`：装载/序列化/结构校验失败。

`validate` 只保证结构：

- 长度在 `[header, kQolBagTransferPayloadMaxSize]`；
- 长度前缀匹配；
- game loader 接受且 `consumed == payload_size`；
- 重新序列化后结构等价（精确或变体允许的规范化）。

不保证：词条 ID 存在、数值合理、品质/类别规则、UID 唯一、堆叠上限。

### 3.2 `inventory.transfer` 1.1（owner `core.inventory`）

```cpp
// contracts/inventory_transfer.h 尾部追加
QolResult (*replace_item)(QolItemHandle expected,
                          const QolBagTransfer* replacement,
                          QolBagMutationResult* out);
```

- `expected` 必须仍是当前槽/源物品；否则 `kQolErrConflict`/`kQolErrNotFound`。
- native：只确认 `expected` 仍在原生袋内并返回同一 handle；对象身份不变，详情/拖拽控件指针仍有效，不额外拒绝。
  对象字段已由 `item.payload.update` 写好，不 load/swap/free。
- provider：按空间分派到 `QolBagSource::replace_item`。
- 成功返回同一 handle；不失效旧 handle（对象身份保留）。
- 内容不校验；结构由 `item.payload.update` 的 validate 保证。

### 3.3 `QolBagSource` 1.3（owner `core.inventory` 的注册面）

```cpp
// contracts/bag.h 尾部追加
QolResult (*replace_item)(QolItemHandle expected,
                          const QolBagTransfer* replacement,
                          QolBagMutationResult* out);
```

`QolBagSource` 版本 1.2 → 1.3；`qol_bag_source_valid` 要求 `replace_item != nullptr`。
`extension_bag` 实现只更新 descriptor/category/count/hash/dirty/refresh，不释放/重建物化对象。

## 4. 字段与适用性

| kind | 适用类别 | 语义 | 结构约束 |
|---|---|---|---|
| `kJewelWord` | 宝石 | 写 `I_COUNT` 的完整 jewel word | 值位域在 u32 内 |
| `kNativeOptionValue` | 真实装备 | 写第 N 条 type 0 词条的 int16 | index 必须存在、值为 int16 |
| `kCount` | 可堆叠类别 | canonical 数量 | 1..count limit |
| `kSocketLevel` | 装备 | 插槽等级（`I_SOCKET` bits4-7） | 0..15 |
| `kSocketFilled` | 装备 | 已镶数量（`I_SOCKET` bits0-3） | 0..15 |
| `kEnchantLevel` | 装备 | 附魔等级（`I_ENCHANT` bits6-10） | 0..31 |
| `kEnchantRemaining` | 装备 | 剩余强化次数（`I_ENCHANT` bits2-5；产物配置用 15） | 0..15 |
| `kMagicRate` | 装备 | 魔法倍率（`I_MAGIC_RATE`） | 0..255 |

不开放：类别、UID、原始 payload、任意偏移。结构性变更走 `create`/`place`/未来的 replace 物品。

## 5. native replace 流程

1. `inventory.transfer.replace_item` 按 space 0/1 定位 `expected`；找不到再尝试注册源。
2. 任务袋拒绝。
3. `apply_payload_to_item` 把 replacement payload 原地写回既有对象：标量字段 + 现有 option
   节点的值；option 结构必须一致。
4. 返回 `expected` 作为结果 handle，`count = replacement->count`，请求库存刷新。对象地址和
   handle 不变，详情/拖拽引用不需要拒绝或重指。

## 6. provider replace 流程（extension_bag）

1. core.inventory 先按 payload 原地更新既有对象（见 §5）。
2. 用 handle 定位槽，确认 `expected` 与 `module_objects[bag][slot]` 一致。
3. 校验 replacement 的长度前缀、category/count、堆叠上限和 `valid_payload`。
4. 写新 descriptor（category/count/payload）与 `payload_hash`、category 缓存。
5. `extension_bag_mark_dirty_locked()`，`persist_state_locked()`；失败恢复旧 descriptor/hash/category，
   core.inventory 再用旧 payload 回滚对象。
6. 若当前显示该袋：`refresh_module_item_area_locked(bag)`。
7. 返回同一 handle（对象地址不变）。

## 7. handle 与 UID

- 有槽 replace：返回同一 handle，保持对象身份；craft UI/UseStuff 的指针继续有效。
- 三格产物：对象来源由 `ThreeSlotRecipe::product_origin` 逐条配方声明，不由 `ProductMode` 推断。
  - `kNewItem`：初始产物纯 `item.payload.update` 生成 payload → `create(payload)` → 丢弃旧 detached 产物；
  - `kUpdateFirstItem`：第 1 格原物品即产物，纯 `item.payload.update(source=items[0])` +
    `inventory.transfer.replace_item`（replace 原地写回），该格不扣料、不 place。
  - 当前第 7 条（宝石 + 混沌卷轴）为 `kUpdateFirstItem`，其余 7 条与动态配方为 `kNewItem`；
    堆叠宝石走 `kNewItem` 回退，避免一次升级整堆。
- UID：
  - `source` 路径自动保留原 UID；
  - `base_payload` 路径默认由调用方保证；`kPreserveUid` 仅作意图声明；
  - source/base 同时提供 → `kQolErrInvalidArgument`。

## 8. 结构校验与改版词条

- 使用当前变体的游戏 `SAVE_LoadItem` / serializer，不写模块侧 schema。
- 多词条：option 数量由 `(payload_size - header) / 4` 自然推导，由 loader 重建链表。
- 改版自定义词条：由该变体的 loader/serializer 接受；不维护白名单。
- 不把某个变体的保留位掩码写死到 `core.item`；需要语义比对时由 Game Adapter 提供规则。

## 9. 版本与迁移（已实现）

1. 新增 `contracts/item_payload.h`；`core.item` provide `item.payload` 1.0。
2. `QolItemInventoryStateWritePort` 2.4 → 2.5，尾部追加 `apply_payload_mutation`；
   `game/ops` 与 Host 适配层实现，复用原 craft 写字段逻辑。
3. `inventory.transfer` 1.0 → 1.1，尾部追加 `replace_item`；`core.inventory` 实现 native
   确认与 provider 分派。
4. `QolBagSource` 1.2 → 1.3，尾部追加 `replace_item`；`extension_bag` 实现 descriptor 提交。
5. `custom_recipe` 升阶/产物配置改为 `item.payload.update` + `inventory.transfer.replace_item`。
6. 删除上一轮的 `contracts/inventory_item_writeback.h`、`extension_bag/api/item_writeback.*`、
   `core.item` 的 writeback 同步入口（`apply_stuff` 使用的 `item_descriptor_sync` 保留）。
7. `QolCustomRecipeWritePort` 1.4 → 1.5，删除 `write_jewel_word`、`set_max_socket_enchant`、
   `write_native_option_value`；只保留合成界面/材料写面与 `read_jewel_word`。
8. `custom_recipe` required 依赖新增 `item.payload`、`inventory.transfer`；optional 删除
   `inventory.item_writeback`。

## 10. 测试与验收

Host（已执行）：

- `extension_bag_bag_source_mutation_tests`：replace 写 descriptor/hash/category、保留对象身份、
  persist 失败恢复、显示刷新。`pass=133 fail=0`。
- `custom_recipe_handle_chain_tests`：宝石/装备升阶改走新接口后材料顺序、产物配置、失败路径。
- `host_adapter_tests`：`apply_payload_mutation` 对 Game/Provider/detached 对象写 jewel/socket/
  enchant，stale 句柄返回 NotFound（该 target 的 `kGameHostMinor` 静态断言为既有失败）。
- `custom_recipe_owner_fixture_tests`：required/optional 依赖更新后装配通过。
- `inventory_action_lifecycle_tests`、`inventory_placement_tests`、`core_inventory_store_tests`
  通过。
- `uv run python scripts/maintenance/check_arch.py`：PASS。

真机（NOT_ACCEPTED 直到取证）：

- 扩展袋宝石升阶 → 同袋移动 → 跨袋移动 → 关闭/切换背包 → 存档重进，数值保持；
- 原版袋宝石升阶回归；
- 装备升阶（多词条/改版自定义词条）替换后仍可正常显示、移动、存档；
- 材料只扣一次；craft UI 输入物品在升阶后不出现悬空指针/崩溃。

Debug APK：`output/inotia4_qol_lsposed_debug_2611091818_8b16b0cf32df.apk`，
SHA-256 `8b16b0cf32df284d3492464fa2c8c7a7b44205d4129543ba3c7b72acfef3b092`。

## 11. 已确认决策（实现口径）

- replace 内容结构由 `item.payload.update` 的 validate 保证；replace 再把 payload 原地写回对象。
- 对象身份保留：小改动由 replace 原地应用 payload，保留对象地址/handle；provider 再提交 descriptor。
- native/provider replace 都返回同一 handle；结构变化需要整体替换时由 replace 后续分支返回新 handle。
- `kNewItem` 产物：纯 update → `create(payload)` → 丢弃旧 detached 产物；`kUpdateFirstItem` 走 replace。
- source 与 base_payload 同时提供时拒绝。
- `update` 返回 `QolResult`，结果结构不重复放 status。

## 12. 非目标

- 不在本方案中开放任意字节/任意偏移写入。
- 不在 v1 中设计词条 add/remove（结构性变更）。
- 不改变 `inventory.action` 的 create/place/discard 语义。
- 不引入回滚事务跨 update + replace 两步；失败由调用方按顺序处理。
