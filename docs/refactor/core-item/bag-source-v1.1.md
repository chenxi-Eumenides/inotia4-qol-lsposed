# `inventory.bag_source` v1.1：`extension_bag` 侧实施设计

状态：EB 侧接口边界已实施；UI/原生清理待续。ABI 与转移模型以
[`bag-source-contract.md`](bag-source-contract.md) 和 `contracts/bag.h` v1.1 为准，
本文只描述扩展背包侧如何实现与删除。
适用：`plugins/extension_bag`。

> 实施进度（当前分支）：
> - 已新增 `api/bag_source.{h,cpp}`，注册 `QolBagSource` 1.1（space=2）。
> - `plugin.cpp` 已改为 activate 注册 / deactivate+disable 注销；旧 provider、
>   `ExtensionBagApi`、`inventory.merge_projection` 提供面与 EB HTTP 路由已从构建移除。
> - `autosell` 已删除 `extension_bag` 依赖与两个门禁。当前 autosell 只扫描原版袋；
>   若后续仍需自动出售扩展袋物品，应改走 `core.item` 的库存 API，而不是恢复插件能力。
> - 旧 `public/sale.cpp`、`public/equip.cpp`、`transaction/*`、`usecases/move.cpp` 仍作为
>   UI 内部实现保留，待 `core.item` 操作能力就绪后删除；它们不再对外暴露。
> - 旧 provider / API 表 / HTTP 源文件已从 `CMakeLists.txt` 移除，但尚未物理删除。
> - 投影显示已改为 `ControlItem_SetItem/GetItem` 读取入口虚拟化：refresh 前建立
>   pending 上下文，第一帧就写扩展物品；`INVEN_pItem` 不写。详见
>   [`../extension-bag/display-hook.md`](../extension-bag/display-hook.md)。
> - 投影回调反转已完成（`inventory.projection` 注册面）；旧 sidecar pending 清理、UI 操作面接线仍待完成。

## 1. 边界

扩展背包最终只保留两件事：

1. 向 `core.item` 注册 `QolBagSource`（space = 2），提供扩展袋数据的读、导出、导入、移除、定位、使用原语。
2. 扩展袋 UI 适配：页签、布局/reflow、投影、绘制、输入、商店/合成/详情页面。

不再保留：

- `QolInventoryExtensionProvider` 与 `contracts/inventory_extension.h` 提供面。
- `ExtensionBagApi` / `extension_bag` 能力。
- 全部 EB HTTP 路由与 typed command。
- 全库存 `move_item`、`ext_orig` / `ext_ext` 事务、pending/journal 恢复。
- `inventory_json_fragment`、`remove_for_sale`、旧 `use_item` 包装。
- 原版/改版袋直接读写，以及 `adopt_native_item`、`merge_native_item`、
  `consume_native_item`、`equip_projected_item`、`put_jewel_native`、
  `handle_original_bag_unequip` 等物品域行为。
- `inventory.merge_projection` 提供面已删除；EB 改为向 `core.item` 的 `inventory.projection` 注册表注册回调。

保留：

- `model/`、sidecar v5、catalog、session、快照。
- 页签、布局/reflow、投影安装/恢复/同步、绘制、输入/拖拽 UI、商店/合成/详情页面。
- 所有权账本、句柄保管、扩展物品物化。
- `bagCount` 设置页。
- `QolBagSource` 的 `identify` 与 `use`（契约允许可选，本插件提供）。

## 2. `QolBagSource` 回调映射

索引一律 0 基；所有回调主线程。

| 回调 | 实现 |
|---|---|
| `count()` | `active_extension_bags()`；disabled / 未进世界 / 会话未准入返回 0 |
| `info(bag, out)` | `capacities[bag]`；`slot_count` 扫描非空槽；容量 0 合法 |
| `item_at(bag, slot, out)` | 空槽 `kQolErrNotFound`；物化对象、注册句柄；填 `space_id=2`、`bag`、`slot`、category、effective count |
| `for_each_item(fn, ctx)` | 遍历活动袋容量内槽；逐项物化句柄；物化失败跳过并记录日志；回调非 0 停止 |
| `export_item(item, count, out)` | 句柄复核 → 数量校验 → 填 `QolBagTransfer`；只读，不持久化 |
| `import_item(transfer, preferred_bag, preferred_slot, out)` | 校验 payload → 规范化数量 → 选袋/槽 → `SAVE_LoadItem` 物化 → 写 descriptor → 持久化 |
| `remove(expected, count, out)` | 句柄复核 → 部分扣减或整堆移除 → 持久化 |
| `identify(item, out_ref, out_slot)` | 句柄 → `{space=2, bag}` + 槽；非本源 `kQolErrNotFound` |
| `use(ref, slot)` | 复用现有扩展物品使用业务体，返回 `QolResult` |

### 2.1 `export_item`

- `out` 非空、`count > 0`、`count <= descriptor.count`。
- `out->category = descriptor.category`。
- `out->count = count`（本次转移数量；payload 内是整堆状态）。
- `out->payload_size = descriptor.payload_size`；拷贝到 `out->payload[256]`。
- payload 必须通过 `validate_serialized_payload_buffer`；失败 `kQolErrConflict`。
- 只读：不改 dirty、不持久化、不刷新投影。
- 源不可用：`kQolErrNotReady`。

### 2.2 `import_item`

- `transfer/out` 非空；`category > 0`、`count > 0`、`payload_size ∈ [1,255]`。
- 校验 payload 结构、长度前缀、尾部；category 必须与 `transfer->category` 一致。
- 用 `patch_payload_count` 把 payload 内数量改写为 `transfer->count`；
  不可堆叠物品只允许 `count == 1`。
- 选袋/槽：
  - 两者 `-1`：自行选择有容量的袋和空槽；可先尝试合并同类同 payload 的堆，
    再选空槽；
  - `bag >= 0`、`slot == -1`：该袋内选空槽；
  - 两者 `>= 0`：必须使用该槽，占用 `kQolErrConflict`，不做合并。
- 用 `SAVE_LoadItem` 建一份**扩展背包自己的**对象；校验 round-trip
  （与 sidecar 读档同一套语义校验）。
- 写 descriptor（category/count/payload/payload_size）、物化、注册句柄。
- 成功后持久化、标记快照变更、若投影已安装则刷新。
- 失败不变式：descriptor、对象、句柄、持久化全部不变；临时对象释放。
- `payload_size == 0` 不在本实现支持：`core.item` 应先在原生侧创建物品并导出 payload；
  空 payload 返回 `kQolErrInvalidArgument`。

### 2.3 `remove`

- 句柄不属于本源或过期：`kQolErrNotFound`（message 含 `stale item handle`）。
- `count <= 0` 或超过当前数量：`kQolErrInvalidArgument`。
- 部分移除：扣减 descriptor 与 payload 数量，句柄保持有效。
- 整堆移除：释放对象，返回后句柄失效。
- 原子性：失败不得部分扣减；成功后持久化、标记快照变更、刷新投影。

### 2.4 `use`

- `ref.space_id == 2`；槽越界 `kQolErrInvalidArgument`；空槽 `kQolErrNotFound`。
- 复用 `extension_bag_api_use_item_impl` 的业务体，外层改为 `QolResult`：
  - 不可用/未进世界：`kQolErrNotReady`
  - 使用中/冷却：`kQolErrBusy`
  - 存档忙/佣兵槽满：`kQolErrBlocked`
  - 物品不可使用：`kQolErrUnsupported`
  - 符号未解析/内部不一致：`kQolErrInternal`
- 成功负责持久化与投影刷新；`core.item` 负责快照 epoch。

## 3. 注册与生命周期

- descriptor `requires` 增加 `inventory.bag_source` 1.1。
- `plugin_install` 后 require 注册表；`register_source(&kSource)`。
- 注册失败进入 degraded，不阻塞 UI；原版功能不受影响。
- `plugin_on_enabled` 注册，`plugin_on_disabled` / `plugin_deactivate` 注销。
- `set_bag_count` 成功后注销 + 重新注册，使注册表代次变化、`core.item` 快照失效。
- 注册期间 `QolBagSource` 与函数指针保持有效；注销后不再回调。
- 恢复顺序：先 `core.item`，后 `extension_bag`；停用反向。

## 4. 数据写入约束

- 扩展背包不得在回调之外直接改物品数据。
- 所有写都经 `core.item` 调用 `import_item` / `remove` / `use`。
- 这样快照 epoch、转移 journal、失败补偿都由 `core.item` 统一负责。
- `bagCount` / 袋类型变更属于结构变更：改完后重注册 source。

## 5. UI 适配需要 `core.item` 提供的操作面

扩展背包 UI 不再直接改数据。拖放、使用、出售、装备、镶嵌需要调用 `core.item` 的操作能力
（能力名由 `core.item` 决定，建议 `inventory.operation`）：

```
move(from ref/slot, to ref/slot, count)
use(ref, slot)
sell(ref, slot, expected, count)
equip(ref, slot, equip_slot)
put_jewel(jewel ref/slot, equip ref/slot)
unequip_bag(ref)
```

EB 只做控件事件 → 上述操作的翻译；不在 UI 层做物品校验/事务。

## 6. 投影

- EB 不再写 `INVEN_pItem[bag][slot]`；显示读取由 `ControlItem_SetItem/GetItem` 与
  `INVEN_GetBagSize` hook 虚拟化，控件第一帧直接读到扩展物品。
- EB 向 `core.item` 注册投影回调（`inventory.projection` 1.0），包括
  `projection_drop`、`module_view_installed`、同步槽/袋与 observation；`core.item`
  在读原版空间时跳过投影槽。
- EB 不再提供 `inventory.merge_projection`；`core.item` 只消费注册表中的回调。

## 7. 删除顺序（EB 侧）

1. 新增 `QolBagSource` 1.1 适配层（读 + export/import/remove + identify/use），
   注册但暂不切换旧路径。
2. 删除中立 provider、Host facade 引用、`contracts/inventory_extension.h` 提供面。
3. 删除 `ExtensionBagApi` / `extension_bag` 能力、全部 HTTP 路由和 typed command。
4. 删除全库存移动、`usecases/move`、`ext_orig` / `ext_ext`、pending/journal 恢复。
5. 删除 `inventory_json_fragment`、`remove_for_sale`、旧 `use_item` 包装。
6. 删除原版/改版袋直接读写与物品域 hook 行为（迁入 `core.item`）。
7. 投影回调反转：`core.item` 提供 `inventory.projection` 注册面，EB 注册回调；已实现。
8. 文档、静态检查、真机矩阵。

## 8. 对 `core.item` 的对接检查

1. descriptor 提供 `inventory.bag_source` 1.1，iface 为 `QolBagSourceRegistryApi`。
2. 已删除对 `extension_bag` / `inventory.merge_projection` 的 requires；扩展背包不再提供这些能力。
3. 读路径 space>=2 走注册源；写路径分派 `export_item/import_item/remove/identify/use`。
4. 跨空间移动按契约 §6 顺序执行，并落 `item.transfer.journal`。
5. 提供给 EB UI 的操作面：`move/use/sell/equip/put_jewel/unequip_bag`（能力名自定）。
6. 提供 `inventory.projection` 回调注册面，并在读原版空间时跳过投影槽；已实现。
7. Host `serialize_item/load_item/place_item` 缺口由 Host owner 补齐前，原生↔扩展移动 fail-closed。

## 9. 待确认

1. `import_item` 的 `payload_size == 0`：本文按“不支持、先原生创建再导出”处理。
2. `export_item` 的 `out->count`：本文按“请求的转移数量”处理。
3. 投影注册面的具体头文件/能力名由 `core.item` 定。
4. Host `serialize_item/load_item/place_item` 缺口导致原生↔扩展移动 fail-closed；
   集成前需 Host 批准。
5. 旧 sidecar `pending/journal` 的清理/回滚策略需在删除事务前定。
