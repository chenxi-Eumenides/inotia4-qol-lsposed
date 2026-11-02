# Handoff：扩展背包、item_type 与礼盒/拾取/背包问题

日期：2026-10-28  
分支：`refactor/arch-v2`  
当前 HEAD：`dc1d7189 fix(core.item): SaveItem 兜底复用有序空槽查询`

## 1. 当前仓库与构建状态

- 工作区仅有一个非本任务改动：
  - `scripts/patch-apk.sh`（其他代理修改，未提交；不要混入本任务提交）。
- 最近提交：
  - `dc1d7189 fix(core.item): SaveItem 兜底复用有序空槽查询`
  - `21224564 fix(extension_bag): 修复礼盒解包与拾取扩展袋兜底`
  - `710095d3 fix(extension_bag): 修正背包槽位判断与礼盒开箱路径`
  - `41609ae6 refactor(patch-apk): 生成前整体重建集成包`
  - `5bf09cd8 docs(core.item): 记录权威 item_type 与使用路径`
  - `fc1b6803 refactor(extension_bag): 统一使用 item_type 并收紧背包装备来源`
- 设备上已安装模块 hash：
  `d219e8374bfc10da8a7b48005517843e7d867c0cfb9759fe4617a8202d5242e4`
- 当前输出 C18L v589 集成包：
  `output/Inotia4_C18L_v589_debug_lspatch.apk`
- 当前输出集成包 SHA-256：
  `790f74b5db36f173c1990c8f2cc2db224f7357eb8f695126cf6db0b68279e050`
- 设备：`192.168.3.54:5555`，API `8088`，C18L v589。
- 构建入口只能使用 `scripts/build-debug.sh`；集成包使用 `scripts/patch-apk.sh`。

## 2. 架构基线

EB（`extension_bag`）只拥有扩展 sidecar 数据和 UI 适配；原版 0..5、C18 袋及库存动作归 `core.item`。

关键契约：

- `item.facts` 4.0：
  - 唯一类型入口 `ItemFactsApi::item_type(handle, QolItemType*)`。
  - 旧 `is_real_equip`、`is_jewel_category` 已删除。
- `QolItemType`：
  `kUnknown / kConsumable / kEquipment / kBackpack / kJewel / kEnchantScroll / kMercenarySeal / kSkillBook / kItemBox / kSynthSet / kPotion / kDice / kReviveScroll / kLamp / kUseAfterConfirm / kOther`
- `inventory.bag_source` 1.2：EB `space=2`，提供 `count/info/item_at/for_each_item/export_item/import_item/remove/identify/use`。
- `inventory.action` 3.1：
  `move/use/discard/sell/equip/unequip/put_jewel/find_empty_slots`。
- `inventory.projection` 1.4：EB 注册投影/详情回调，包括 `handle_extension_sell`、`handle_character_equip`、`handle_backpack_equip`。
- `QolItemPort` 4.1：删除旧类型谓词，`read.item_type` 为唯一类型来源；新增 `read.free_item`。
- `QolInventoryExtrasPort` 1.2：新增 `read.empty_bag_slot`。
- `QolExtensionBagWritePort` 1.2：新增 `equipment.open_item_box`、`equipment.consume_item`。

## 3. 已完成修复

### 3.1 item_type 统一

- 新增 `contracts/item_type.h` 与 `item.facts` 4.0。
- 所有旧类型判断迁移到 `item_type`：
  - EB
  - `attribute_range_display`
  - `special_equip`
  - `autosell`
  - `attribute_quality`
  - `custom_recipe`
  - 测试桩
- EB 删除旧包装，统一 `item_type_is`。
- 文档：
  - `docs/refactor/core-item/item-type-and-use-paths.md`
  - `docs/reference/plugins/core.item.md`
  - `docs/INDEX.md`

### 3.2 背包装备

- `INVEN_IsEmptyBag` 语义是“袋内容为空”，不能用于判断空袋槽。
- 新增 `inventory_extras.empty_bag_slot`，内部调用 `INVEN_GetEmptyBagSlot`。
- `native_bag_slot_available()` 改为使用空袋槽查询。
- 当前目标顺序：
  - 原版袋来源：原生空袋槽优先；原生满后走扩展袋空槽。
  - 扩展袋来源：仍然直接走扩展袋空槽（见未完成问题 3）。
- C18 追加袋 `space=1` 不参与背包装备。

### 3.3 礼盒/开箱

- 礼盒 924 是 `ITEMSYSTEM_IsPackItem`，不是 `ITEMSYSTEM_IsItemBox`（1007..1009）。
- 旧代码对 924 调用 `ITEMSYSTEM_OpenItemBox`，在 `MEM_ReadInt8` 崩溃。
- 现在分流：
  - `IsItemBox` 1007..1009 → `OpenItemBox(category)`
  - 其他 pack → `ITEMSYSTEM_ProcessUnpack(category)`
  - 解包后 `INVEN_ConsumeItem` → `QolBagSource::remove`
- 新增符号绑定 `fn_process_unpack`。
- 但 924 的 `ProcessUnpack` 仍返回 0，未完成。

### 3.4 拾取/SaveItem

- `INVEN_SaveItem` 由 core.item hook。
- 原版 `INVEN_SaveItem` 返回 0 后，调用 `find_empty_slots(3)`：
  - `find_empty_slots` 本身就是按 `space 0→1→注册源`、bag/slot 升序扫描，达到 limit 立即返回。
  - 如果返回原生 `space=0/1` 空槽，则返回失败，不把物品错误塞进扩展袋。
  - 如果返回 `space>=2`，用返回的 `bag/slot` 调 `QolBagSource::import_item`。
  - 导入成功后 `free_item` 释放原版创建对象。
- 已提交 `dc1d7189`。

### 3.5 崩溃/卡顿/快捷栏

- 控件读取不再调用 `ControlObject_GetCount`，直接读 `CO_COUNT/CO_CHILD_LIST`。
- `/proc/self/maps` 使用 16ms thread-local 缓存，解决背包页长时间卡顿。
- 扩展物品详情快捷栏登记/移除已映射到原版按钮。

## 4. 未完成问题（当前三个）

### 4.1 冒险家礼盒 924 使用仍失败

最新真机日志：

```text
inventory resolve provider ref=2/0/4 ... cat=924
bag source use rejected ... body={"ok":false,"error":"on cooldown"}
```

结论：

- `OpenItemBox(924)` 会崩溃；已避免。
- `ITEMSYSTEM_ProcessUnpack(924)` 实际返回 0，EB 将其映射为 `on cooldown`。
- `CHAR_UseItemEx` 对 924 也返回 0。
- 924 的 `ITEMCLASSBASE +2` 为 0，`IsPackItem` 为 1；`IsItemBox` 为 0。
- 目前不能确认 924 应该走 `ProcessUnpack` 还是其他入口。需要继续检查：
  - `ITEMSYSTEM_ProcessUnpack` 对 924 返回 0 的具体原因（空槽数量、table 不存在、thread/context）。
  - `CHAR_UseItemEx` 的 0x39c 分支是否实际覆盖 924。
  - 如果 924 只是活动/任务礼盒，是否有独立打开协议。

相关代码：

- `game/adapter_ports/extension_bag_write_port.cpp`：`write_extension_open_item_box`
- `plugins/extension_bag/api/api_impl.cpp`：`is_item_box` 分支
- `plugins/extension_bag/runtime/item_access.cpp`：`extension_open_item_box`
- 符号：`F_ITEMSYSTEM_PROCESS_UNPACK_VMA=0x10ce50`，`F_OPEN_ITEM_BOX_VMA=0x10e970`

### 4.2 拾取仍提示背包空间不足

`INVEN_SaveItem` 已有扩展袋兜底，但用户实测原版袋满仍提示空间不足。高度怀疑拾取链在 `INVEN_SaveItem` 之前调用 `INVEN_FindSaveSlot` 并直接中止。

事实：

- `INVEN_IsHavingEmptySlot` 已 hook，且 `registered_source_has_empty_slots` 会计入注册源空槽。
- `INVEN_FindSaveSlot` **没有 hook**。
- `INVEN_FindSaveSlot` 返回原生物理槽编码 `bag<<5|slot`，不能表达 `space=2` 扩展槽。
- 商店购买已有专门 caller gate；拾取没有。
- 可能需要在拾取调用点加 gate，或让调用方在 `FindSaveSlot` 失败后继续走到 `SaveItem` 兜底。

相关代码：

- `plugins/core.item/hooks/inventory_hooks.cpp`：
  - `is_having_empty_slot_wrapper`
  - `adopt_save_item_to_registered_source`
  - `save_item_wrapper`
- `game/access/inventory.cpp`：`find_save_slot`、`empty_bag_slot`
- `game/symbols/game_symbols.h`：`F_INVEN_FIND_SAVE_SLOT_VMA=0x103960`

### 4.3 扩展袋背包仍优先装扩展袋

当前代码：

```cpp
if (!module_source && native_bag_slot_available()) return false;
```

行为：

- 原版袋来源：原生空袋槽优先，原生满才扩展。
- 扩展袋来源：直接走扩展袋空槽，不检查原生空袋槽。

用户要求：无论背包在原版袋还是扩展袋，都原生空槽优先，原生满才扩展。

难点：

- 扩展物品没有原生物理槽对象；要装入原生袋槽必须新增“扩展物品 → 原生袋槽”的实际安装路径。
- 不能只改 `native_bag_slot_available` 条件，否则会进入原版读不到扩展物品的路径。
- 不能为显示投影写 `INVEN_pItem`；但真正的背包装备安装可能需要在 core.item 内实现原生袋槽写入 API，并明确事务/回滚。

相关代码：

- `plugins/extension_bag/core/host_adapter.cpp`：`native_bag_slot_available`、`extension_handle_backpack_equip`
- `plugins/extension_bag/ui/equip.cpp`：`first_empty_extension_bag_locked`、`equip_extension_bag_item_locked`
- `plugins/core.item/usecases/item_action.cpp`：`find_empty_slots_impl`

## 5. 建议的下一步

### 优先级 1：礼盒 924

1. 在真机主线程记录 `ITEMSYSTEM_ProcessUnpack(924)` 的返回值、`INVEN_IsHavingEmptySlot` 的 needed/result。
2. 确认 924 的 `ITEMCLASSBASE +2`、`ITEMSYSTEM_IsPackItem`、`ITEMSYSTEM_IsItemBox`。
3. 若 924 不属于 `ProcessUnpack`，不要继续强行调用；改为原版入口或明确不支持提示。
4. 若返回 0 是因为输出物品数量大于扩展空槽，应返回“空间不足”，而不是“发生错误”。

### 优先级 2：拾取

1. 找到拾取链中 `INVEN_FindSaveSlot` 的调用点。
2. 在原版返回 0 且 `INVEN_IsHavingEmptySlot` 为真时，不直接弹错误，继续走 `INVEN_SaveItem` 扩展兜底。
3. 不要 hook `INVEN_FindSaveSlot` 返回假扩展物理槽；它不能表达 `space=2`。
4. 复测：原版 0..4 全满、扩展袋有空槽，拾取应进入扩展袋。

### 优先级 3：扩展袋背包装备原生优先

1. 设计 core.item 的“扩展物品安装到原生空袋槽”事务：
   - 目标：`INVEN_GetEmptyBagSlot` 返回的原生空袋索引。
   - 需要把扩展物品 payload 变成原生袋槽对象；不能只写 sidecar。
2. 事务需包含：
   - 原生空袋槽检查
   - 扩展 source 导出/移除
   - 原生袋槽安装
   - 失败回滚扩展 source
3. 不要用显示投影临时写 `INVEN_pItem` 作为方案。
4. 如果原生槽已满，再走扩展空槽。

## 6. 验证入口

```bash
scripts/build-debug.sh
python3 scripts/maintenance/check_arch.py
python3 scripts/maintenance/check_http_routes.py
```

当前已通过：

- `host_adapter_tests`
- `item_facts_plugin_tests` 274/0
- `inventory_action_sell_tests` 55/55
- `autosell_view_diagnostic_tests`
- `check_arch.py`
- Debug 构建

已知既有问题：

- `custom_recipe_tests` 仍有 5 个历史失败（`test_custom_recipe.cpp:599`）。
- `plugin_registry_tests`、`plugin_lifecycle_tests`、`feature_reconciler_tests` 未构建二进制。

## 7. 约束提醒

- 不要修改 `INVEN_pItem` 做显示投影；EB 只负责自身 sidecar/UI。
- 原版 0..5、C18 袋、库存动作归 core.item。
- C18L v589 当前测试目标；C18 追加袋 `space=1` 不参与背包装备。
- 一个功能一个提交；不要提交其他代理的 `scripts/patch-apk.sh` 改动。
- 真机设备不要依赖坐标点击；可用 adb、API、Frida 和日志。

## 8. 2026-10-28 修复记录

提交：

- `d9c06b61 fix(core.item): SaveItem 兜底继续尝试注册源槽`
  - 原版 `INVEN_SaveItem` 失败后不再因第一个原生空槽放弃，而是跳过原生槽继续尝试注册源（扩展袋）。
  - `adopt` 各阶段与 `INVEN_SaveItem` 结果补日志。
- `8493801f fix(extension_bag): 礼盒使用前检查产物槽位并区分失败原因`
  - 新增 ITEMPACKBASE 读取（`G_ITEMPACK_*_GOT`）、`Game Host 2.6 read_only.item.pack_required_slots`、
    `inventory_extras 1.3 read.is_having_empty_slot`。
  - EB 礼盒使用前按产物累计槽位（924 → 4）检查空间；不足返回 `no space`，解包失败返回 `unpack failed`。
- `a53c0848 feat(extension_bag): 背包装备优先占用原生空袋槽`
  - 扩展袋来源背包先反序列化为原生对象写入原生袋表空槽（容量校验、持久化、ownership handover）；
    失败按台账回滚并保留扩展源，原生槽满再回退扩展槽。
- `94c2ca25 fix(core.item): SaveItem 兜底直接扫描注册源空槽`
  - 根因：`find_empty_slots(3)` 只返回前 3 个空槽，未装备袋被误报 16 格时前 3 个可能全是不可写原生槽，
    扩展袋永远轮不到（真机日志 `adopt=0 empty_count=3 target_space=-1`）。
  - 改为按注册源顺序直接扫描 `info/item_at` 空槽并 `import_item`。
- `817aaf1f fix(core.item): 袋容量按 INVEN_GetBagSize 返回`
  - `native_bag_capacity` 不再对未装备袋固定返回 16，直接调用原版 `INVEN_GetBagSize`（未装备 → 0）。
  - 该修复同时阻止 `move_item` 写入未装备原生袋（此前写入后在存读档时丢失）。

验证：

- `scripts/build-debug.sh` 通过；`check_arch.py`、`check_http_routes.py` 通过。
- Host CTest：157 项中 3 项失败均为既有/环境问题（`custom_recipe_tests`、`item_handle_contract_redline`
  命中未改动的 `item_port.h`、`core_api_lifecycle_tests` 描述断言）。
- 真机 C18L v589（APK `Inotia4_C18L_v589_debug_lspatch.apk`，模块 `2610280256_3562c7f7d573`）：
  新档 `add(924)` + `use_item` 票据 `done`，产物 874 + 3×背包（大）正常入包；
  未装备的 C18 追加袋容量已返回 0（`GET /api/item/inventory`）。
- 拾取（原版袋满→扩展袋）、扩展袋礼盒使用、背包装备原生优先由用户触摸复测。

