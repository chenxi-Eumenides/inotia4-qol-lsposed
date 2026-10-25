# `inventory.bag_source` v1.1：转移模型与失败恢复契约

状态：定稿（`contracts/bag.h` v1.1）。替代 `bag-source-v1.1.md` 的 ABI 部分。

适用：`core.item` 袋源注册/读写/移动编排；`extension_bag` 的 `QolBagSource` 实现。

相关：[`architecture.md`](architecture.md)、[`c18-native-bag.md`](../../reference/game/c18-native-bag.md)。

## 1. 职责

| 能力 | owner |
|---|---|
| 原版袋 0..5、改版袋 0..c−1 读写 | `core.item`（经 Host 端口） |
| 扩展袋数据与 `export/import/remove/use` 原语 | `extension_bag` |
| 跨空间移动顺序、补偿、意图记录、恢复 | `core.item` |
| 扩展袋 UI、页签、投影 | `extension_bag`（投影窗口写入属 UI 副作用例外） |

## 2. 编号

| 空间 | `space_id` | 索引 | 袋数 |
|---|---|---|---|
| 原版 | 0 | 0..5 | 常量 6 |
| 改版（C18） | 1 | 0..c−1 | Host 动态探测 + 缓存 |
| 扩展 | 2 | 0..e−1 | `QolBagSource::count()` |

注册源索引 0 基；扁平袋号 = 原版 → 改版 → 注册源顺序连接，仅便利键，不持久化。

## 3. 转移记录

```
constexpr uint32_t kQolBagTransferPayloadCapacity = 256;   // 固定缓冲
constexpr uint32_t kQolBagTransferPayloadMaxSize = 255;    // 有效长度上限

struct QolBagTransfer {
    int32_t category;
    int32_t count;
    uint32_t payload_size;
    uint8_t payload[kQolBagTransferPayloadCapacity];
};
```

- `payload` 是来源方定义的整堆序列化状态；`core.item` 不解析、不修改。
- `count` 是本次转移数量；`import_item` 必须把 payload 内数量规范化为 `count`。
- 不可堆叠物品 `count == 1`。
- payload 只在本次调用内有效，接收方必须立即复制。

## 4. 注册

```
能力 id : "inventory.bag_source"
版本    : 1.1
struct QolBagSourceRegistryApi {
    uint32_t struct_size;
    QolResult (*register_source)(const QolBagSource*);
    QolResult (*unregister_source)(int32_t space_id);
};
```

规则：

1. 主线程注册/注销；重复 `space_id` 拒绝；`space_id < 2` 拒绝。
2. `qol_bag_source_valid()` 要求 1.1、`space_id >= 2`、`count/info/item_at/for_each_item/export_item/import_item/remove` 全部非空。
3. `identify/use` 可选；缺失时对应功能返回 `kQolErrUnsupported`。
4. 注册/注销递增注册表代次，使库存快照失效。
5. 回调期间 `QolBagSource` 指针必须有效；注销后不得再回调。

## 5. 回调语义

### 5.1 读

`count()`：活动扩展袋数 `e`。
`info(bag, out)`：`capacity` 任意非负值；`slot_count` 已占用槽数。
`item_at(bag, slot, out)`：空槽 `kQolErrNotFound`；成功必须填 `space_id=2`、`bag=空间内索引`、`slot`、有效句柄。
`for_each_item(fn, ctx)`：遍历全部扩展物品；引用只在回调内有效；物化失败跳过并记录日志。

### 5.2 `export_item`

```
QolResult (*export_item)(QolItemHandle item, int32_t count, QolBagTransfer* out);
```

- 只读：不得改变物品或持久化状态。
- 句柄不属于本源/过期：`kQolErrNotFound`。
- `count <= 0` 或超过当前数量：`kQolErrInvalidArgument`。
- 成功：填 `category/count/payload_size/payload`，`payload_size ∈ [1,255]`。
- 源不可用：`kQolErrNotReady`。

### 5.3 `import_item`

```
QolResult (*import_item)(const QolBagTransfer* transfer, int32_t preferred_bag,
                         int32_t preferred_slot, QolBagMutationResult* out);
```

- 参数空、`category <= 0`、`count <= 0`、`payload_size > 255`：`kQolErrInvalidArgument`。
- `preferred_bag == -1 && preferred_slot == -1`：自行选袋选槽。
- `preferred_bag >= 0 && preferred_slot == -1`：该袋内选槽。
- 两者都 `>= 0`：必须使用该槽，占用则 `kQolErrConflict`。
- 校验 payload 结构与 category 一致性；失败不得部分写入。
- 成功后填 `out->ref`、`out->item`、`out->count`。
- 失败不变式：不改变任何物品、句柄、持久化状态；临时对象释放。

### 5.4 `remove`

```
QolResult (*remove)(QolItemHandle expected, int32_t count, QolBagMutationResult* out);
```

- 句柄不属于本源/过期：`kQolErrNotFound`（message 含 `stale item handle`）。
- `count <= 0` 或超过当前数量：`kQolErrInvalidArgument`。
- 部分移除：数量扣减，句柄保持有效。
- 整堆移除：对象释放，句柄失效。
- 失败不变式：不得部分扣减。

### 5.5 `identify` / `use`

`identify(item, out_ref, out_slot)`：成功填 `space_id=2`、空间内袋号、槽号；不属于本源 `kQolErrNotFound`。
`use(ref, slot)`：使用结算；错误码见 §8。

## 6. 移动编排（`core.item`）

### 6.1 顺序

```
1. 记录意图（journal: intent）
2. 源 export：注册源 export_item / 原生 Host serialize
3. 目标 import：注册源 import_item / 原生 Host place
4. 目标成功后记录 journal: target_written
5. 源 remove：注册源 remove / 原生 Host remove
6. 成功：清除 journal
```

不使用“先删源再写目标”。

### 6.2 失败矩阵

| 失败点 | 处理 | 结果 |
|---|---|---|
| export 失败 | 清 journal，返回错误 | 两边不变 |
| import 失败 | 清 journal，返回目标错误 | 两边不变 |
| remove 失败 | 用 `target.item` 执行目标 `remove` 补偿 | 两边不变 |
| 补偿成功 | 清 journal，返回源错误 | 两边不变 |
| 补偿失败 | 保留 journal（state=leftover_target），返回 `kQolErrInternal` | 可能重复，待修复 |

### 6.3 意图记录与恢复

持久化位置：`core.item` 自己的 owner store section `item.transfer.journal` v1（JSON 数组，最多 8 条）。

记录字段：

```
{"id","state":"intent|target_written",
 "source":{"space","bag","slot"},"target":{"space","bag","slot"},
 "category","count","payload_sha256","time"}
```

- 写盘时机：步骤 1（intent）与步骤 4（target_written）。
- 清除时机：步骤 6 成功或补偿成功。
- 启动恢复（`plugin_install` / `on_enabled`）：
  1. 两边都存在 → 默认删除目标、保留源；
  2. 只有目标存在 → 视为已提交，清除记录；
  3. 只有源存在 → 视为未开始，清除记录；
  4. 两边都不存在 → 清除记录并记录 WARN；
  5. 删除目标失败 → 保留记录，阻止存档并记录 ERROR。
- 无法取得当前存档槽（`save.facts` 不可用）时：仅内存记录 + WARN，不阻塞功能。

> 说明：意图记录是崩溃恢复的“推荐”能力；`core.item` 自行实现，来源/目标不需要感知。

## 7. 责任分级（不强制第三方）

| 项 | 归属 | 性质 | 不做的后果 |
|---|---|---|---|
| `export/import/remove` 原子性、句柄/数量校验 | 来源/目标各自 | 契约要求 | 无法保证任何移动语义；`core.item` 返回错误，不重试 |
| 移动顺序、补偿 | `core.item` | 契约要求 | 可能重复/丢失 |
| 意图记录落盘、启动恢复 | `core.item` | 推荐 | 崩溃窗口内残留，需人工/日志排查 |
| `identify/use` | 来源可选 | 推荐 | 对应功能返回 `kQolErrUnsupported` |
| 投影槽不被读作原生物品 | `core.item` 读路径 + EB 投影注册 | 契约要求 | 原版袋重复计数 |

`core.item` 不因第三方未实现推荐项而拒绝注册或拦截功能；只在缺失必需原语时返回明确错误。

## 8. 错误码

| 场景 | 码 |
|---|---|
| 参数空/越界/数量非法 | `kQolErrInvalidArgument` |
| 句柄过期、槽空、物品不存在 | `kQolErrNotFound` |
| 源不可用/未进世界 | `kQolErrNotReady` |
| 目标槽占用、payload 不匹配 | `kQolErrConflict` |
| 使用中/冷却 | `kQolErrBusy` |
| 存档忙、业务拒绝 | `kQolErrBlocked` |
| 原语缺失、物品不可使用 | `kQolErrUnsupported` |
| 回滚失败、内部不一致 | `kQolErrInternal` |

## 9. 对外 HTTP（不变）

分页与 `space` 参数见 `docs/reference/api-reference.md` §4.1/§4.2。
扩展袋在响应中为 `space=2`、`bag=0..e−1`。

## 10. 投影例外

`extension_bag` 写原版显示槽 0..4 仅用于显示：不存档、不参与物品事务、退出投影时恢复。
`core.item` 读原版空间时按投影注册信息跳过这些槽，不得把扩展对象当原生物品。

## 11. 迁移步骤（EB 侧）

1. 实现 `QolBagSource` 1.1：0 基索引 + `export/import/remove/identify/use`。
2. activate 注册、deactivate 注销；`bagCount` 变化后重注册。
3. 删除 `owns_bag`、全库存 `move_item`、`inventory_json_fragment`、原生袋读写分支。
4. 保留 sidecar、页签、投影与 UI。

## 12. 已知缺口

| # | 缺口 | 归属 |
|---|---|---|
| 1 | 已实现：Host `serialize_item/load_item/place_item/remove_item`（`1bdcdff6`），原生↔扩展移动可执行（`50993c29`） | Host + `core.item` |
| 2 | 已实现：写端口去 provider 参数（2.1，`54a98ab5`）；provider 读仲裁清理进行中 | Host |
| 3 | 已实现：`item.state` 扩展段由注册源结构化生成（`1ac6e3ec`） | `core.item` |
| 4 | 已撤销：扩展背包不再写原生槽，读路径无需投影过滤（`f2315c79`） | `core.item` |
| 5 | `save.facts` 缺失时意图记录仅内存 | 已知降级 |
| 6 | 已实现：hooks 物理扫描上界随原生袋数动态化 | `core.item`（`56c9f45a`） |
| 7 | 已实现：跨空间移动四方向（原版↔C18、原生↔注册袋）；同空间原生仍走 Host 快路径 | `core.item`（`50993c29`） |
| 8 | 已实现：`ExtensionBagApi` 全部删除；出售/使用/装备经注册源或 `inventory.action` 3.0（`54a98ab5`、`1ecb486b`） | `core.item` + EB |
| 9 | 已实现：`inventory.action` 3.0 唯一操作入口（`2221312d`） | `core.item` |