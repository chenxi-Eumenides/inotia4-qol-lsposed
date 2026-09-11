# 扩展背包运行时架构

> 状态：CURRENT（实现与静态证据架构说明，不代表 P7 Overall 已验收）
>
> 取证基线：当前工作树；写作任务书 `archive/extension-bag/writing-materials/doc-set-plan.md`；交叉影响素材
> `archive/extension-bag/writing-materials/impact-matrix-source.md`。所有源码行号均按本工作树逐一打开核对。
>
> 本文只负责代码层级、操作调用链、技术选型、替代方案和未知限制。范围状态以
> `control-plane.md` 为准，函数决策以 `inventory-integration-decision-plan.md` 为准，
> 交互事务以 `drag-protocol.md` 为准，sidecar 以 `module-save-store.md` 为准。

## 0. 阅读规则与结论边界

1. 文件中的 `文件:行` 是当前源码锚点，不是版本锚点。
2. `game_symbols.h` 中的 VMA 是当前目标 `libgame.so` 的证据索引；代码通过
   `fn_resolve` 和 `game_access` 使用符号，域文件不应复制裸地址。
3. “当前路径”描述源码已经形成的路径；“已验证”仅指文档中登记的 Host、静态或
   真机证据。源码存在不等于 P7 全局库存接入完成。
4. 任务袋是原版物理索引 `5`；扩展逻辑袋是对外编号 `6..10`，内部数组索引为
   `0..4`。两套编号空间不能互换。
5. 问题 B（扩展↔扩展交换后原版同号槽物品消失并被保存固化）仍未解决；本文不以
   已有快照守卫、单一 owner 或回滚代码推导“已修复”。

## 1. 代码层级

### 1.1 实际依赖图

当前 native 目标由 `module/app/src/main/cpp/CMakeLists.txt:4-48` 编译成单一
`gamebridge` shared library。CMake 没有把每个目录编译成独立库，因此层级结论以
include 图、稳定头文件和实现归属核对，而不是以链接目标名称臆测。

```text
Kotlin controller
    ↓ ApiServices.action/info/config/op
Kotlin Service
    ↓ NativeBridge external（JNI 名称和签名冻结）
bridge/native gamebridge*.cpp
    ├─→ API native 域 / core / data
    ├─→ feature/patch
    ├─→ feature/ui
    └─→ feature/extension_bag

feature/extension_bag ──→ core/native 稳定端口、JSON/几何/存档端口
                         ├→ data/game_access/game_state
                         ├→ feature/patch（现存的补丁协作边）
                         └→ feature/ui（现存的控件工具协作边）

feature/patch ──────────→ core/native、data/game_access、extension_bag port
API native ─────────────→ core/native extension_bag_port（库存 API 的可选适配）
core/native ────────────→ 不 include feature/；由 feature 提供 port 的实现
```

证据：统一头 `bridge/native/gamebridge_internal.h:7-24` 和入口
`gamebridge.cpp:1-35`；扩展 include 图 `feature/extension_bag/game_ui_virtbag.cpp:3-19`；
patch wrapper `feature/patch/native_inventory_hook.cpp:1-8`；API port
`api/native/game_inventory.cpp:1-15`。`core/`→`feature/` 无 include 命中，
`extension_bag_port.h` 是由 feature 实现的反向稳定契约；扩展→patch/UI 是既有 feature
内部协作边，不改变 core 不依赖 feature 的边界。

### 1.2 CMake 编译归属

`CMakeLists.txt:4-48` 的扩展相关源文件是：

| 编译单元 | 归属 | 作用 |
|---|---|---|
| `feature/extension_bag/game_ui_virtbag.cpp` | feature | 扩展背包运行时主 TU，状态、投影、输入、事务、生命周期、UI 分流 |
| `feature/extension_bag/extension_bag_port.cpp` | feature adapter | 实现 core 的 `extension_bag_port.h` 稳定端口 |
| `feature/extension_bag/extension_bag_persistence.cpp` | feature persistence | 通过 JNI 回调 sidecar 的 load/prepare/commit/abort |
| `feature/extension_bag/extension_bag_geometry.cpp` | feature geometry | 扩展网格命中和控件几何适配 |
| `feature/extension_bag/extension_bag_api.cpp` | feature API facade | 把稳定的 `data_op_extension_bag_*` 转给主 TU 内部实现 |
| `feature/extension_bag/extension_bag_logging.cpp` | feature logging | Android log 与文件日志 |
| `feature/patch/native_inventory_hook.cpp` | feature/patch | LSPosed Native Hook API 安装、backup 和函数级分流 |
| `feature/patch/inventory_hook_stage4.cpp` | feature/patch | 可测试的 dispatcher seam 与递归/失败语义 |
| `feature/patch/inventory_find_item_poc.cpp` | feature/patch | `FindItem` PoC 适配 |

同一目标中的 core 源文件包括 `core/native/game_json.cpp`、`game_ops_common.cpp`、
`grid_geometry.cpp`、`game_nav.cpp`、`game_cache.cpp`、`module_save.cpp`、
`game_motion.cpp`（`CMakeLists.txt:14-20,29`）。data 和 API native 源文件的归属以
`CMakeLists.txt:12-33` 为准。

### 1.3 所有 `.inc` 的物理归属

`.inc` 不是独立 CMake 编译单元，而是在宿主 `.cpp`/`.h` 的 namespace、静态状态和
include 顺序中展开；以下按当前 include 点核对。

#### API / parse 域

库存 `basic/equipment/use/read.inc` 属于 `api/native/game_inventory.cpp:12-15`；UI
`read/operations.inc` 属于 `game_ui.cpp:9-10`；world 的
`story/navigation/movement/operations/readers.inc` 属于 `game_world.cpp:21-25`；
`game_dialog_content/operations.inc` 属于 `game_dialog.cpp:12-13`。对外函数仍是宿主的
`build_*`/`data_op_*`，不单独形成库边界。

#### data / patch / UI 域

`game_access_globals.inc` 属于 `game_access.cpp:15`；`game_patch_core/move_merge/craft.inc`
属于 `feature/patch/game_patch.cpp:74-76`；设置 `geometry/config/render/panel/injection/api.inc`
属于 `feature/ui/game_ui_settings.cpp:93-99`；`game_ui_kit_controls/render.inc` 属于
`game_ui_kit.cpp:44-45`；`game_ui_custom_panel/injection.inc` 属于
`game_ui_custom.cpp:101-102`。

#### extension-bag feature 域

model 的 `virtual_bag_drag_model/transaction_rules/state_ops/serialization/state_json.inc`
展开于 `model/virtual_bag_state.h:54,105-108`，提供 session、谓词、normalize、payload、
JSON；`virtual_bag_transaction_types.h` 是同头展开的类型头，不是 `.inc`。

主 TU `game_ui_virtbag.cpp` 的 include 点：`observation.inc:187`、`tabs.inc:191`、
`drag_session.inc:216`（观测/标签/session）；`runtime.inc:218` 并在
`:784,791,819,894-895` 展开 ownership/input/transaction/render/lifecycle；
`public_runtime.inc:405`（门面）；`equip.inc:677`、`store.inc:678`、`api_impl.inc:1161`。

接口清单见 `extension_bag_port.h:6-51`、`game_ui_virtbag.h:10-30`、
`extension_bag_context.h:10-30`；其中 context 是内部访问点，真正对外边界是
`game_ui_virtbag.h:10-24` 和 `core/native/extension_bag_port.h:8-51`。`Item` 不保存
native 指针，见 `model/virtual_bag_state.h:53-67`。

### 1.4 port 与对外接口

#### core port

`core/native/extension_bag_port.h:6-51` 对 API/patch 暴露以下稳定能力：

- 查询：`extension_bag_for_each_logical_item`、`extension_bag_item_at`、
  `extension_bag_view_item_at`、`extension_bag_identify_native_item`、
  `extension_bag_find_native_item`。
- 状态/投影：`extension_bag_enabled`、`extension_bag_module_view_installed`、
  `extension_bag_inventory_bags_json`、`extension_bag_sync_projected_*`。
- 事务：`extension_bag_remove_native_item`、`extension_bag_consume_native_item`、
  `extension_bag_equip_projected_item`、`extension_bag_adopt_native_item`。
- 门禁/生命周期：`extension_bag_has_empty_slots`、
  `extension_bag_adopt_unequipped_item`、`extension_bag_prepare_main_menu`、
  `extension_bag_prepare_save_slot_load`。
- API 语义：`extension_bag_use_item`、`extension_bag_move_item`、
  `extension_bag_put_jewel`。

`extension_bag_port.cpp:16-29` 先实现投影和开关端口；其余端口继续转给主运行时。
因此 core 只持有声明，不能反向 include extension-bag 内部实现。

#### feature 内部头

- `game_ui_virtbag.h:10-24`：UI 注入、视图状态、投影刷新、事件门禁。
- `extension_bag_geometry.h:5-9`：网格命中、slot 计算、子控件和绝对位置。
- `extension_bag_context.h:10-30`：JNI env/class、内部 State、API 实现、sidecar
  prepare/commit/abort。
- `native_inventory_hook.h:5-23`：`NativeHookFunType`、`NativeUnhookFunType`、
  `NativeAPIEntries`、`native_init`。

### 1.5 Controller/API 到 native

正式库存调用的当前路径是：

```text
HTTP POST
  → InventoryActionController（路由、body、参数校验）
  → ApiServices.action
  → ActionApiServiceImpl : ActionApiServiceCore
  → InventoryActions
  → NativeBridge external
  → bridge/native Java_* JNI
  → data_op_* / extension_bag port
  → game_access fn_* 或扩展事务
```

证据：

- `InventoryActionController.kt:18-61,64-107` 只解析参数并调用
  `ControllerGuard.guard { ApiServices.action... }`。
- `ApiServices.kt:10-18` 注册 action/info/op/config 服务；
  `ActionApiServiceImpl.kt:3-5` 委托给 `ActionApiServiceCore`。
- `ActionApiServiceCore.kt:31-53` 将 use/sell/move/equip/unequip/jewel 等转给
  `InventoryActions`。
- `InventoryActions.kt:16-53` 调用对应 `NativeBridge`；扩展参与 move 时在
  `:24-31` 分流至 `nativeOpExtensionBagMoveItem`。
- JNI 薄层 `gamebridge_domain_operations.cpp:95-123` 将 use/discard/sell/move 的
  参数转换给 `data_op_*`；扩展专用 JNI 在
  `gamebridge_extension_bag.cpp:4-46`。
- `data_op_use_item`、discard、sell、move 分别见
  `api/native/game_inventory_use.inc:1-107,148-217`；装备/宝石见
  `api/native/game_inventory_equipment.inc:1-83`。

### 1.6 NativeBridge 冻结面

`NativeBridge.kt:32-146` 声明 112 个 external，扩展声明在 `:128-146`；初始化顺序
`NativeBridge.kt:19-29` 为注册两个 Java bridge 后调 `nativeInit()`。`JNI_OnLoad` 缓存
JVM，注册函数只把 class 交给 feature，`nativeInit` 再执行 bridge、库存 Hook、cache 和
设置 UI，见 `gamebridge.cpp:8-35`。`native_init.list:1` 只证明库注册，不证明 Hook 已
运行时安装；JNI 名称、HTTP 成功响应和 external 签名冻结，见 `architecture.md:485-487`。

## 2. 逐操作：原版实现路径 vs 模块当前路径

### 2.1 入口和 VMA 对照

以下 VMA 均由 `data/native/game_symbols.h` 当前行逐条打开核对；它们只用于当前
版本事实、解析 fallback 和反汇编索引，正式调用走 `game_access`。

| 函数 | 当前 VMA | 证据 |
|---|---:|---|
| `INVEN_FindItem` | `0x10438c` | `game_symbols.h:388` |
| `INVEN_HaveItem` | `0x104870` | `game_symbols.h:389` |
| `INVEN_GetItemCount` | `0x104260` | `game_symbols.h:390` |
| `INVEN_FindItemSlot` | `0x103704` | `game_symbols.h:391` |
| `INVEN_RemoveItem` | `0x104044` | `game_symbols.h:392` |
| `INVEN_PutJewel` | `0x10bcb4` | `game_symbols.h:337` |
| `INVEN_ConsumeItem` | `0x1047bc` | `game_symbols.h:434` |
| `CHAR_UseItemEx` | `0xeb670` | `game_symbols.h:435` |
| `CHAR_EquipItemFromInvenToSlot` | `0xe5368` | `game_symbols.h:406` |
| `CHAR_UnequipItemToInven` | `0xe2f68` | `game_symbols.h:407` |
| `INVEN_MoveItem` | `0x104934` | `game_symbols.h:400` |
| `INVEN_SaveItem` | `0x104528` | `game_symbols.h:398` |
| `INVEN_SaveItemOnEmpty` | `0x104be0` | `game_symbols.h:459` |
| `UIEquip_ButtonEquipExe` | `0xb7c18` | `game_symbols.h:569` |
| `UIEquip_ButtonUseExe` | `0xb80b8` | `game_symbols.h:570` |
| `UIEquip_OKConfrimUseItem` | `0xb8478` | `game_symbols.h:573` |
| `UIEquip_ButtonDestroyExe` / OK | `0xb6240` / `0xb83d0` | `game_symbols.h:574,576` |
| `UIEquip_ButtonUnequipExe` | `0xb7e14` | `game_symbols.h:575` |
| `UIEquip_MakeDesc` | `0xb8980` | `game_symbols.h:567` |

#### 2.1.1 数量位段与上限 patch 点

当前工作树 `game_patch_core.inc` 的原版数量位段/上限 patch 表（S2 布局）：常驻位段表
共 2 点，可逆上限 clamp 表共 6 点。当前登记点如下，地址均为函数 VMA 加函数内偏移：

常驻位段表（`g_stack_layout_patches`）：

| 地址 | 函数+偏移 | 原指令 | 新指令 | 语义 | 门控 |
|---:|---|---|---|---|---|
| `0x127e04` | `SAVE_SaveInventory+0x78` | `0x52800301` | `0x528002A1` | 子物品检查位段缩窄 bitEnd 24→21（避开数量高位段 a） | 常驻 |
| `0x127f4c` | `SAVE_LoadInventory+0xa8` | `0x52800301` | `0x528002A1` | 同上（读档侧） | 常驻 |

S2 布局（`a=bits22-24`、`b=bits25-31`、`count=128a+b`）下 bits25-31 就是原版 7 位
字段 b：原版 `UTIL_Get/SetBitValue(start=25,end=31)` 天然读写 b 且不触碰 a，S1 时代
「把 bitStart 25 改 22 的单一 10 位段布局 patch」（含 `ITEM_GetCumulateCount+0x78` 读
位段与 fix-15 的 `UIStore_BuyItem+0x1a8` 写位段）与 S2 编码冲突，已全部移除；数量
读取统一依赖 `ITEM_GetCumulateCount` getter hook（S2-P2），写侧进位由调用方完成
（§3.7）。存档子物品检查以 `GetBitValue(+0x10, end=24, start=0)` 判断袋容量位非零，
`count≥128` 的普通物品会让 bits22-24（a 段）误判出「子物品」，因此 end 24→21 的缩窄
在 S2 下仍然必要。

可逆 clamp 表（`g_stack_limit_clamp_patches`，受 `set_stack_limit_enabled()` 门控）：

| 地址 | 函数+偏移 | 原指令 | 新指令 | 语义 |
|---:|---|---|---|---|
| `0x104a84` | `INVEN_MoveItem+0x150` | `0x71018C1F` | `0x710F9C1F` | 合并总量 ≤999（getter hook 完整数） |
| `0x104a8c` | `INVEN_MoveItem+0x158` | `0x52800C77` | `0x52807CF7` | 可转移量填满 999−目标总量 |
| `0x103ccc` | `INVEN_SaveItemDirect+0xdc` | `0x71018C7F` | `0x710F9C7F` | 合并总量 ≤999（getter hook 完整数） |
| `0x103ce0` | `INVEN_SaveItemDirect+0xf0` | `0x71018E9F` | `0x710F9E9F` | 源堆叠总量 ==999 判定 |
| `0x104690` | `INVEN_SaveItemData+0x7c` | `0x71018A9F` | `0x710F9A9F` | 入参创建总量 >998 走拆堆分支 |
| `0x103b98` | `INVEN_FindSaveSlot+0x238` | `0x71018C1F` | `0x710F9C1F` | 两堆总量之和 ≤999（getter hook 完整数） |

clamp 口径：999 只允许作用于「数量完整值」判定点（比较数来自 getter hook 解码总量
或函数入参总量）。直接读取 b 段残量（mod 128）的判定点不进 clamp 表——
`INVEN_CheckSaveInNotEmptySlot+0xa4` 与 `INVEN_GetCumulateSaveSlotEx+0x164/0x17c` 在
S2 下保持原版 99 的 b 满判定（999 对 b 值无意义且高估剩余空间，不 fail-closed）；
`INVEN_SaveItemData+0x8c` 的 fill 常量同理保持原版 99（fill 写的是 b 段值，拆堆配对
指令 `sub #0x63` 不可独立改写）。

`0x105b58`、`0x105c18`、`0x105b60`、`0x105c20` 已复核为**非数量，保持撤销**：两处
`ITEM_IsRealEquip/ITEM_IsRealBroken` 均先读取物品 `+0x10`，再以
`UTIL_GetBitValue(start=25,end=31)` 读取 `ITEMSYSTEM_CreateItem@0x10c030..0x10c038`
（`0x10be9c+0x194..0x19c`）
写入的装备 marker `100`，后续比较只是装备/损坏判定，不是堆叠数量读取或上限。

已知边界（写侧收口前开放点）：启用态下原版写点 `SetBitValue(start=25,end=31)` 只能
写 b，合并/创建总量 >127 时无法对 a 进位；总量 ≤127 全部正确，>127 的原版袋内进位
由调用方级 hook（S2-P3 H-18..H-21）闭合（禁止 hook `UTIL_SetBitValue`，
见规则册 R-46/R-49）。

16 函数 / 24 写点收口状态（2026-09-11 反汇编冻结，基线 `.tmp/dualmode-full.asm`，逐函数
证据见 `native_inventory_hook.cpp` 语义表）：已接管 5 个（MoveItem、SaveItemDirect、
Divide、ConsumeItem、RemoveItemData——H-21 快照/重扫修正部分删堆）；数量回写已撤销
1 个（`ITEMSYSTEM_MakeItem`——arg2 是静态表查找/品质参数非数量，产物量由原版
`CAL_Calculate` 公式写点 `0x10ca3c` 生成且 ≤99，b 写即全量，R-52/VM-38）；无需
接管 8 个（UIStore_BuyItem 购买量 max=99、ITEMSYSTEM_CreateItem 数量恒 1、
UIMix_StartMix/ITEMSYSTEM_ProcessUnpack 产物量=只读静态表、DEALSYSTEM_MakeSale
唯一 31/25 写点是 marker=126、GAME_StartNewGame 初始量 5、INVEN_SaveItemData 每笔
≤99、MAPITEMSYSTEM_CreateItem count=任务/事件表只读数据）；待勘察 2 个
（NetworkStore_InitializeMenuData、NetworkStore_AddItem——数量来自网络数据，离线
不可达）。已知开放边界：丢弃/移仓若存在把 S2 全量（>127）作为 count 传入
MAPITEMSYSTEM_CreateItem 的路径（当前全量反汇编未见），以及 UIMix/ProcessUnpack
静态表若含 >99 产物量（原版数据域设计 ≤99），需数据审计复核。

### 2.2 18 项逐条核实

| # | 操作 | 原版实现路径 | 模块当前路径 | 核实结论 |
|---:|---|---|---|---|
| 1 | 使用 | `UIEquip_ButtonUseExe@0xb80b8` → `CHAR_UseItemEx@0xeb670` → `INVEN_ConsumeItem@0x1047bc` | API 先用 `inventory_item_ref_at` 分流；扩展走 `extension_bag_use_item`，普通物品走 `fn_char_use_item_ex`，见 `game_inventory_use.inc:1-7,98-106` 和 `extension_bag_api_impl.inc:151-189` | 一致；消费后端由 Hook/port 适配 |
| 2 | 确认使用 | `UIEquip_OKConfrimUseItem@0xb8478` → `INVEN_FindItemSlot@0x103704` → `CHAR_UseItemEx` | Native Hook wrapper 先给 `virtual_bag_handle_confirm_use_item`；扩展在 `extension_bag_equip.inc:688-770` 校验 token 后调用原版 `fn_char_use_item_ex`，普通物品 backup | 机制一致；扩展避免物理槽反查 |
| 3 | 原版袋→扩展袋装备 | 原版袋物品拖到袋控件，由 UI/TouchHandle 的 drop 事件处理 | `virtual_bag_event`/标签 proc 判断类别 1..4，调用 `try_equip_on_extension_tab_drop_locked` → `equip_extension_bag_item_locked`，见 `extension_bag_lifecycle.inc:194-229`、`extension_bag_runtime.inc:38-70`、`extension_bag_equip.inc:20-35,303-317` | 素材所称“ButtonEquipExe 主入口”不准确；实际是 tab/drop 路径 |
| 4 | 扩展物品→角色装备 | `CHAR_EquipItemFromInvenToSlot@0xe5368` 读取物理 `INVEN[bag][slot]` 并交换 | Hook `equip_item_from_inven_to_slot_wrapper` → `stage4_equip_item`；API 也调用同一原版入口，扩展适配临时投影槽，见 `native_inventory_hook.cpp:201-221`、`game_inventory_equipment.inc:51-72`、`extension_bag_equip.inc:155-270` | 一致；扩展对象不直接伪造永久物理槽 |
| 5 | 装备按钮 | `UIEquip_ButtonEquipExe@0xb7c18` 内联删除源并调用装备链 | 函数级 Hook 返回三态：扩展由 `virtual_bag_handle_backpack_button_equip_result` 接管，Blocked 吞掉原版，其他 backup，见 `native_inventory_hook.cpp:305-318` | 一致；三态不能压成 bool |
| 6 | 卸下装备/袋 | `CHAR_UnequipItemToInven@0xe2f68`；袋详情经 `UIEquip_ButtonUnequipExe@0xb7e14` | 两个函数均 Hook；`stage4_unequip_item_to_inven` 失败后扩展收编，按钮由 `extension_bag_handle_original_bag_unequip` 接管，见 `native_inventory_hook.cpp:133-143,246-260` | 一致；任务袋仍排除 |
| 7 | 宝石/镶嵌 | `ITEMSYSTEM_PutJewel@0x10bcb4` → 成功后原版 UI/库存删除 | `data_op_jewel` 先识别扩展宝石；普通装备调用 `fn_put_jewel`，成功后统一 `fn_remove_item`，见 `game_inventory_equipment.inc:1-27`；Hook 适配见 `native_inventory_hook.cpp:224-229` | 一致；扩展宝石不走 Direct remove |
| 8 | 拖拽移动 | TouchHandle 的 `0x17` press、`0x19` move、`0x18` release → 原版 `INVEN_MoveItem` | `virtual_bag_event@extension_bag_lifecycle.inc:194`；投影 session 在 `0x17/0x19` 握手，`0x18` 在 `:297-315` 唯一提交并吞掉原版二次移动 | 一致；问题 B 仍未解决 |
| 9 | 双非空交换 | `INVEN_MoveItem@0x104934` 按原版物理槽交换/合并 | `swap_extension_slots_locked` 校验 descriptor/cache/token，交换 object/hash/handle/generation 并同步投影，见 `extension_bag_transaction.inc:352-538` | 逻辑扩展存在；真机问题 B 未解决 |
| 10 | 合并堆叠 | `INVEN_MoveItem` 按原版堆叠身份和上限处理 | `move_extension_to_extension_locked` 在 `extension_bag_transaction.inc:541-708` 按 payload/同袋/开关/stack limit 判定，失败再选择目标或交换 | 一致到扩展事务；不应推导原版函数已覆盖逻辑袋 |
| 11 | 销毁 | `UIEquip_ButtonDestroyExe@0xb6240` → popup OK `0xb83d0` → 物理删除/释放 | 详情按钮 PtrHook 安装在 `extension_bag_equip.inc:620-682`；`extension_destroy_ok` 清 descriptor、释放模块对象并刷新，见 `:528-575`；全局 popup 槽只用于确认回调 | 一致；现存 popup 槽劫持仅卖出/销毁 |
| 12 | 卖出（装备页） | 详情按钮 → 原版确认 popup → 价格函数 `ITEM_GetSellPrice@0x10a500` → 删除 | `extension_sell_ok` 在 `extension_bag_equip.inc:418-526` 加钱、删除扩展对象，失败退款；详情按钮由 `:620-682` 观察 hook | 一致；与商店 popup 状态分离 |
| 13 | 卖出（商店页） | `UIStore_*` 详情/确认 → 商店卖出回调 | 商店单独保存 `store_desc_*`，`store_make_desc_gate` 后由 `store_desc_sell_execute` 建 popup，OK 在 `extension_bag_store.inc:439-615` 完成；刷新只走 `store_refresh_projection_locked` | 一致；不调用会覆盖整列的原版刷新 |
| 14 | 商店购买收编 | `UIStore_BuyItem` → `FindSaveSlot` → `SaveItem`；原版满则购买失败 | 两个 BL 在 `extension_bag_store.inc:714-726` patch 到 `store_buy_find_slot_gate@635-644`；原版 `SaveItem` 失败由 `save_item_wrapper@native_inventory_hook.cpp:184-198` adopt | 当前实现存在；SaveItem 函数处置以库存册 §2.2、§3.2 为准，caller 覆盖仍待 P7 stage-5 |
| 15 | 拾取/生产者兜底 | 拾取、奖励、开箱、生产等 caller 各自创建/保存物品，最终部分进入 `INVEN_SaveItem` | 当前 `save_item_wrapper` 在 backup 失败时尝试扩展 adopt；注释列出 18 个漏斗，见 `native_inventory_hook.cpp:184-198`。caller 级所有权/失败回滚仍待 P7 stage-5 | 部分实现；不能称所有生产者已覆盖 |
| 16 | 视图切换/页签 | 原版 inventory event、袋控件、TouchHandle 回调和原版绘制 | 扩展标签在 `extension_bag_runtime.inc:81-160` 创建；点击 proc 在 `:1-15` 排队，生命周期 `commit_pending_extension_tab_locked@runtime.inc:380-392` 执行；H-16 `UIEquip_RefreshItemArea` 负责原版刷新后的 post-projection，模块主动刷新走 raw-original dispatcher，draw-end 不再承担帧级 heal | 当前代码对码成立；页签拖放、四象限和同袋 apply 分支已按验收册真机状态确认，S-03 另由 VM-10 维护 |
| 17 | 详情弹窗 | `UIEquip_InvenItemControlEventProc` 事件 `0x80` → `UIEquip_MakeDesc@0xb8980` → `SetDescMenu` | `extension_bag_lifecycle.inc:728-770` patch 唯一 BL 到 `make_desc_equip_gate`；扩展详情按钮 PtrHook 在 `extension_bag_equip.inc:620-682`，确认使用/装备/卖出/销毁分流 | 一致；装备/卸下 proc 明确跳过双层包装 |
| 18 | 存档/sidecar | 原版保存入口 → `SAVE_Save@0x129600` → `SAVE_SaveInventory` 等原版序列化 | 8 个保存 callsite 在 `extension_bag_lifecycle.inc:54-68` patch 到 `module_save_game`；core 先 participant prepare，再 `fn_save`，成功 commit，见 `module_save.cpp:62-111`；扩展 JNI sidecar 在 `extension_bag_persistence.cpp:161-241` | 一致；不是 Hook `SAVE_*` 函数本体 |

### 2.3 复杂操作：拖拽

#### 原版链

原版以 `TouchHandle` 消费 press/move/release；模块保留 press/move 握手，事件 wrapper
为 `call_original_event_with_inventory_guard`，无扩展 session 时返回原版结果，见
`extension_bag_lifecycle.inc:116-166`。

#### 模块链

1. `0x17` 识别投影 child 并开始 session，`0x19` 只更新 session，见
   `extension_bag_lifecycle.inc:257-279,367-388`、`extension_bag_drag_session.inc:1-10`。
2. `0x18` 由 `consume_projected_release_locked` 唯一提交，目标分流到原版或扩展网格，
   三方向调用对应 `move_*` 事务；扩展源目标解析先判页签后判网格，见
   `extension_bag_lifecycle.inc:297-315`、`extension_bag_transaction.inc:791-872`。
3. 失败时仍吞掉 release，避免原版 `INVEN_MoveItem` 二次改真实槽；guard 做物理数组
   pre/post digest，见 `extension_bag_lifecycle.inc:120-165`。
4. 触摸窗口内的 custody release 进入 `extension_bag_ownership.inc:35-53` 的延迟队列，
   draw-end 由 `extension_bag_render.inc:712-770` 调用排空；排空逐项检查
   `g_module_objects` 和 projection 控件引用，无引用时才调用 `ITEMPOOL_Free`
   （`extension_bag_ownership.inc:71-119`）。
5. ext→orig 事务的投影宿主守卫（R-54/VM-40）：目标袋 == 投影宿主袋时，事务期用
   `HostCapacityGuard` 临时恢复被投影替换的宿主容量字（`INVEN_GetBagSize` /
   `INVEN_SaveItemOnEmpty` 按真实容量判定），任何出口写回投影值；投影中不改写
   CUR_BAG（宿主身份 R-26/R-27），显示袋切换仅非投影/API 路径，见
   `extension_bag_transaction.inc:move_extension_to_original_locked`、
   `model/virtual_bag_transaction_rules.inc:ext2orig_requires_host_capacity_restore`。

“唯一 owner”只是控制流设计，不是问题 B 的修复证据；H3/H4 的
`ERROR physical inventory mutation` 复测日志仍缺。

### 2.4 复杂操作：确认使用

#### 原版链

`UIEquip_ButtonUseAfterConfirmExe@0xb95f8` 触发确认框；OK 回调
`UIEquip_OKConfrimUseItem@0xb8478` 反查 `INVEN_FindItemSlot`，再调用
`CHAR_UseItemEx`，见 `game_symbols.h:571-573`。

#### 模块链

1. 详情阶段捕获扩展对象身份，见 `extension_bag_equip.inc:620-682,987-1040`；wrapper
   先调 `virtual_bag_handle_confirm_use_item`，非扩展才 backup，见
   `native_inventory_hook.cpp:263-271`。
2. 扩展分支校验 enabled、详情对象、bag/slot、指针、generation/token 和角色，见
   `extension_bag_equip.inc:688-770`。
3. 释放 `g_virtual_bag_mtx` 后调用原版效果函数，由 `ConsumeItem` wrapper 承接消费，
   成功后刷新区域并清理详情 hook，见 `extension_bag_equip.inc:713-733,760-769`。

这条链保留原版效果和消费时机，只替换“如何从详情身份找到可控对象”的入口。

### 2.5 复杂操作：商店购买收编

1. 商店独立保存 state、详情和 draw hook，见 `extension_bag_store.inc:675-731`。
2. 两处 `FindSaveSlot` BL 改到 `store_buy_find_slot_gate`；物理有空槽返回原值，物理
   满且扩展有空槽才让调用者继续 `SaveItem`，见 `extension_bag_store.inc:630-644,714-726`。
3. `save_item_wrapper` 恢复容量窗口后调 backup，失败才 adopt；卖出 state 独立，失败
   退款，见 `native_inventory_hook.cpp:184-198`、`extension_bag_store.inc:488-556`。

购买路径的静态 gate、SaveItem fallback 和完整 caller 运行时覆盖是三个不同命题；
当前只可以说前两者有代码，不能说所有购买/生产 caller 均已完成 stage-5 验证。

### 2.6 复杂操作：生产者兜底

拾取、任务奖励、开箱、拆包、合成、商店、脱装备和使用结果不是同一 caller；静态决策
要求上层分发并逐 caller 验证。

- 拾取静态链为 `CHAR_PickItemAll` → `NOTIFIER_Add` → `CHAR_ActivePickupEvent` →
  `INVEN_SaveItem`，见 `inventory-integration-decision-plan.md:142-153`。
- wrapper 只负责“原版已创建对象入袋失败后的 adopt”，不重建对象、不判断 caller 的
  任务袋语义、不完成多产物回滚；成功返回原值，失败且有扩展空位才返回 `1`，见
  `native_inventory_hook.cpp:192-198`。
- adopt 收编顺序（R-53/VM-39）：先 `adopt_merge_into` 遍历扩展袋已有同类堆合并
  （判据与移动合并同源，不受 `move_merge_enabled` 门控——原版 `SaveItem→FindSaveSlot`
  自带自动合并语义），无可合并堆才新建扩展空槽；见
  `game_ui_virtbag.cpp:virtual_bag_adopt_native_item`、
  `model/virtual_bag_transaction_rules.inc:adopt_merge_into`。
- 合成仍有 `game_patch_craft.inc` 旁路，部分回滚和五个 caller 属 stage-5/P6；任务物品
  仍只能进原版任务袋 `5`。

### 2.7 复杂操作：存档

1. `patch_all_save_callsites` 枚举八处并校验原指令后替换 BL，见
   `extension_bag_lifecycle.inc:13-68`；target 是 core `module_save_game`，不是
   `SAVE_Save` Hook，见 `:15-18`。
2. `module_save_game` 检查 world、`fn_save`、slot、重入并生成 transaction id；participant
   逐个 prepare，失败逆序 abort，见 `module_save.cpp:31-87`。
3. 调原版 `fn_save()`；失败 abort，不提交 sidecar；成功后逐 participant commit，扩展
   通过 JNI 调 prepare/commit/abort，见 `module_save.cpp:89-111`、
   `extension_bag_persistence.cpp:161-241`。

保存 callsite patch、sidecar participant 和物理库存保存是不同边界；不得把其中任一
项描述成 Hook 全部 `SAVE_*` 或 `INVEN_*` 保存函数。

## 3. 选择当前路径的原因与证据

### 3.1 确认使用：全局 OK 槽 → `0xb8478` 函数本体 Hook

选择函数级 LSPosed Hook；普通对象走 backup，扩展详情对象走
`extension_confirm_use_item`。全局 OK 槽由确认使用、卖出、销毁共享，回调和身份会互相
覆盖；函数级 original-first 让原版物品逐指令走原版。证据：`inventory-integration-decision-plan.md:323-329`；
wrapper `native_inventory_hook.cpp:263-271`；扩展 token `extension_bag_equip.inc:688-770`；
原版 VMA `game_symbols.h:571-573`。当前 popup 槽只剩卖出/销毁，分别见
`extension_bag_equip.inc:361-386,418-575` 和 `extension_bag_store.inc:589-606`。

### 3.2 不把 `FindItemSlot` / `CHAR_UseItemEx` 做成通用底层接入

选择保留 `FindItemSlot` 原版物理契约；扩展通过上层分发、逻辑映射和受控物化使用
`CHAR_UseItemEx`。原因是 `FindItemSlot` 的 `int8_t*` 输出不能安全承载 bag `6..10`
和逻辑 slot，折叠扫描还会误伤确认、装备、删除等 caller。证据：ABI/VMA
`game_symbols.h:391`；
袋域裁定 `control-plane.md:20-26`、`inventory-integration-decision-plan.md:81-89,105-118,208-224`；
当前 API 分流 `game_inventory_use.inc:1-8`、确认 token `extension_bag_equip.inc:698-733`；
原版效果优先 `inventory-integration-decision-plan.md:267-289`。

### 3.3 采用 LSPosed 官方 Native Hook，不采用 Dobby/ShadowHook/字节系

`native_init` 接收官方 `hook_func`/`unhook_func`，框架提供 backup/trampoline，安装链
见 `native_inventory_hook.cpp:310-456,461-499`。不新增依赖的理由是框架已内置；当前安装链使用动态符号优先、VMA fallback、可执行
校验和逆序回滚分别见 `native_inventory_hook.cpp:328-360,286-307,378-395`；本地 ABI
见 `native_inventory_hook.h:5-23`，注册清单见 `native_init.list:1`。

### 3.4 逻辑袋+投影，不伪造 11 个物理袋

选择物理 inventory `0..5`、5 个逻辑袋/每袋 16 槽，对外 `6..10`，视图打开时投影到
原版 ControlItem。物理数组、任务袋、原版保存和对象池不认识 11 袋；伪造会破坏槽
写入、存档和 `ITEMPOOL_Free` 所有权。证据：状态和编号
`virtual_bag_state.h:14-35,47-67`，payload 不保存 native 指针
`virtual_bag_state.h:53-67`、`virtual_bag_payload.h:3-27`，编号裁定
`inventory-integration-decision-plan.md:32-44,59-64`，投影/借用
`extension_bag_runtime.inc:81-160`、`extension_bag_equip.inc:176-226`，物理快照
`extension_bag_lifecycle.inc:71-103,120-165`。

### 3.5 sidecar，不 Hook `SAVE_*`

保存 callsite 进入 `module_save_game`；扩展作为 participant 写 `extensionbags.items`，
原版仍由 `fn_save()` 执行。这样维持“prepare→原版保存→commit”，避免重复/提前提交
和原版失败时留下扩展状态。证据：保存 Hook 暂缓裁定
`inventory-integration-decision-plan.md:44,118,291-301,335-343`；协调器
`module_save.cpp:62-111`；JNI participant `extension_bag_persistence.cpp:161-241`；
Java section `ExtensionBagUiBridge.kt:11-24,110-168`；8 处 callsite
`extension_bag_lifecycle.inc:54-68`。这是 callsite patch，不是 `SAVE_*` 函数 Hook。

### 3.6 SaveItem 当前处置

当前代码安装 `SaveItem` Native Hook，安装链见
`native_inventory_hook.cpp:186-201,472-546`，wrapper 在 backup 失败后按 item 身份
尝试 adopt，见 `:184-198`。库存册 §2.2、§3.2 是函数处置的唯一裁定源。

现行“禁止宽 Hook”只表示不得继续扩大到 `SaveItemDirect`、`SaveItemData` 或所有保存
辅助；它不表示不存在 H-13。wrapper 的存在也不证明 18 条 caller 已完成身份、失败释放
和真机覆盖，生产者验收仍属 P7 stage-5。问题无法仅靠静态安装链闭合时，按验收册
VM-19/VM-20/VM-21/VM-22 取证。

### 3.7 S2 数量编码的 hook 分层（S2-P2 读侧、S2-P3 写侧、S2-P5 关闭态语义已落地）

> 本节记录 S2 数量编码选型与分层现状。规则正文见规则册 R-45..R-49，存档侧数量语义见
> 存档册 §6.4；patch 点登记见 §2.1.1。
> 数量位无版本标识、无迁移代码：sidecar 与原版袋 `+0x10` 统一 S2 布局。
> 已落地：patch 表 S2 重构（§2.1.1）、模块写路径编码（本节「模块写侧」）、
> 读侧 getter hook（S2-P2，H-17）、模式感知读写层与关闭态低 7 位语义
> （S2-P5，R-47 决策 b，`stack_codec::effective_*`）。

- **编码层**：可堆叠数量采用 S2 布局 `a=(v>>22)&0x7`（bits22–24 高位段）、
  `b=(v>>25)&0x7F`（bits25–31 低位段，即原版字段），`count=128a+b`，范围 `0..1023`
  （业务上限 999）；高位段读写只对 count-encoded 类别生效（R-45、R-46）。
- **读侧 = 统一 getter hook（S2-P2，已落地）**：数量读取统一经
  `ITEM_GetCumulateCount@0x106094` 的 H-17 wrapper（`native_inventory_hook.cpp`
  `get_cumulate_count_wrapper`，分流纯函数 `inventory_hook_stage4.cpp`
  `stage4_get_cumulate_count`）按模式视图解码，调用方无需各自拆位；这是读路径的
  唯一统一入口。门控判据沿用 R-40/R-41 的 `item_count_encoding`（ITEMCLASSBASE +6 bit0）：
  kEncoded 返回 `stack_codec::effective_read_count(*(uint32_t*)(item+I_COUNT),
  stack_limit_enabled())`（启用态 S2 全量 `128a+b`、关闭态只读 b），非可堆叠与
  kUnknown fail-closed 直通 backup；wrapper 不取 `g_virtual_bag_mtx`、不设 TLS 直通
  （原版 getter 反汇编 `0x106094–0x106124` 为纯读：空指针 0、非可堆叠 1、可堆叠
  `GetBitValue(31,25)`）。
- **模式感知读写层（S2-P5，R-47 决策 b）**：`core/native/stack_codec.h` 提供
  `effective_read_count(value, enabled)`（启用态 `s2_read_count` 全量、关闭态只读
  b 段）、`effective_write_count(value, count, enabled)`（启用态 `s2_write_count`
  全量拆段、关闭态只写 b 段且 a（bits22–24）与 bits0–21 原样保留）、
  `effective_clamp(count, enabled)`（999/99）与 `effective_view_count(canonical,
  enabled)`（descriptor int 域视图：关闭态 = `count mod 128`）。运行时操作面
  （展示、查询、消耗、合并、卖出、创建、拆堆回写）统一走该层；布局层 `s2_*`
  本身与模式无关。
- **descriptor canonical 边界**：descriptor `Item.count` 与 sidecar 持久化恒为
  canonical `128a+b`（R-38/R-48 不变）；关闭态只改变运行时视图与操作算术，
  读档不 clamp、不按配置重编码。凡从原版对象物化/重同步 descriptor 的写点
  （adopt、unequip 收编、apply 同步、desc 重同步、orig→ext 收编）使用
  `canonical_item_count`（`game_inventory_read.inc`，绕过 getter 直接 S2 解码），
  关闭态模块侧数量写回后 descriptor 回写 `s2_read_count` 解码的 canonical，
  保证重开后读回完整值。
- **非 getter 读点白名单（反汇编盘点 `0x103000–0x106000`；处置口径=R-47，保持 b 段
  原生语义，不新增指令 patch）**：
  `INVEN_CheckSaveInNotEmptySlot@0x103d78` 的 `0x103ec8`（`+0x14c` 起 bit25，堆叠剩余
  空间按 b 段残量）、`INVEN_GetCumulateSaveSlotEx@0x1051b0` 的 `0x105310`（`+0x160`，
  b≥99 满判定）与 `0x105328`（`+0x178`，候选槽余量 99−b）、`INVEN_ConsumeItem@0x1047bc`
  的 `0x104818`（`+0x5c`，count>1 走递减判定）与 `0x104844`（`+0x88`，递减前读，
  配套 `0x104858` 只写 b）。b 段判定对 `count≥128`（b 段残量小）会高估剩余空间，
  由后续入库路径的 getter 域 clamp（`INVEN_FindSaveSlot+0x238`、
  `INVEN_SaveItemDirect+0xdc/0xf0`，§2.1.1）兜底；其余 `INVEN_*` 的
  `UTIL_GetBitValue` 调用读类别 bit6..15 或袋容量 bit0..24，不属数量读点。
  全量反汇编（`.tmp/dualmode-full.asm`，扫描判据 `ldr w,[x,#0x10]` 后 30 行内出现
  `UTIL_GetBitValue(_,31,≥22)`/`lsr/ubfx ≥22`）补充 `0x103000–0x106000` 区间外的
  数量位读点。**出售/拆堆直接位读组（S2 缺陷修复，R-51，已重定向）**：
  `UIStore_ButtonSellExe` 的 `0xd18d0/0xd18e8`（b≤1 整堆判定 + 数量输入框上限）、
  `UIStore_SellItem` 的 `0xd26b8`（b≤1 分批卖出）、`ITEMSYSTEM_Divide` 的
  `0x108458/0x10848c`（拆堆满量守卫与 remain 计算）经
  `g_stack_getter_redirect_patches` 重定向为 `mov x0,xN; bl ITEM_GetCumulateCount`，
  读模式感知全量（启用 128a+b / 关闭 b）；安装日志 5 条 `redirect ...`。**注意**：
  不可用「GetBitValue start 25→22」单指令窗口伪造全量——十位窗口 `(v>>22)&0x3FF`
  是 `a+8b`，与 `a*128+b` 不等价。仍登记为 b 段原生语义的读点：装备页详情出售结算
  函数 `0x1261c4`（由 `UIEquip_OKDestroyItem@0xb8468` 调用；`0x1261fc` 取 `+0x10`、
  `0x126208` `bl UTIL_GetBitValue(_,31,25)` 内联位读 b 段，clamp `b∈[1,99]?b:1`）——
  该点当前不在 `g_stack_getter_redirect_patches`（表内 5 条仅 `UIStore_ButtonSellExe`×2、
  `UIStore_SellItem`×1、`ITEMSYSTEM_Divide`×2）；启用态背包详情出售由 R-55 H-23
  `UIEquip_OKDestroyItem` 接管，关闭态与原版装备详情仍走该 b 段语义。`EVTSYSTEM_Process`
  的 `0xfd1a0`（b==126 魔数分支，语义未定，仅登记）。以上登记项在 `count≥128` 时作用于
  b 段残量（低估数量/上限），属调用方级借位与显示收口的同一开放边界。
  **装备 marker 判定组（P4 必须收口）**：`ITEM_IsRealEquip` 的 `0x105b5c`、
  `ITEM_IsRealBroken` 的 `0x105c1c`、`ITEM_GetDamage` 的 `0x109c44`、
  `ITEM_GetDefense` 的 `0x109f14`、`ITEMSYSTEM_IsEnchantable` 的 `0x10b270` 均以
  `GetBitValue(+0x10,31,25) > 99` 判定装备 marker 或触发强化分支；S2 下可堆叠
  `count mod 128 ∈ [100,127]`（含 `count=100..127`）会与 marker `100` 冲突而被误判，
  需按类别门控或改经 getter 完整值判定（对应 VM-33 的 100 边界用例）。
- **写侧 = 调用方级 patch/hook + 进位**：原版不存在单一「写数量」函数，各 caller 直接
  改 `+0x10` 位段；因此写侧只能在知道物品身份的调用方（既有 patch/hook 点）完成类别
  门控与 `128a+b` 进位/借位，没有也不设统一 setter。
   模块写侧当前实现（S2-P3 + S2-P5 模式感知）：全部数量产生点（创建
   `data_op_add_item`、合并 `patch_payload_count`/`merge_count`、消耗写回
   `consume_extension_item_after_native_locked`）统一走
   `stack_codec::effective_write_count/effective_clamp`——类别门控 fail-closed（非
   count-encoded 类别拒绝写）；启用态总量算术在解码域完成后由 `s2_write_count`
   拆段写回，b 溢出 127 自动进位（a+1、b 归零），a 借位自动落 b+128；关闭态
   只写 b 段、保留 a（R-47 决策 b）；袋对象 marker 写入保持
   `write_native_bag_object_marker`（仅 bit25，不触碰容量位）。登记点：
   `model/virtual_bag_transaction_rules.inc`（`patch_payload_count`、`merge_count`）、
   `extension_bag_equip.inc`（消耗写回）、`api/native/game_inventory_basic.inc`
   （`data_op_add_item`）。
   原版写点现状：S2 下原版指令保持 b 段原生读写且不破坏 a（S1 布局 patch 已移除，
   §2.1.1）；启用态 clamp 作用于完整数量判定点。已知开放边界：原版写点对 >127 的
   合并/创建无法对 a 进位（≤127 全部正确），由调用方级写点（S2-P3 H-18..H-21）闭合，
   无迁移扫描路径（原版袋 `+0x10` 迁移方案已删除，不存在按版本触发的字段重写）。
   H-21 `INVEN_RemoveItemData`（`remove_item_data_wrapper` + 纯函数
   `stage4_remove_item_data_plan`，Host `test_remove_item_data_plan`）：原版
   0x1040a8 顺序遍历 6 袋，整删累计后对最后一堆写 `b = cum(Getter 全量) + w22 -
   count`；启用态 getter 算术正确但 b 写对 remain>127 或旧 a 残留丢进位，wrapper
   在 backup 前快照全部匹配类别的 count-encoded 堆、backup 后重扫（消失堆只求和、
   唯一缩减堆回写 a+b=remain），多缩减/越域/count≤0 一律 fail-closed 保持原版结果；
   关闭态原版 b 写天然正确（getter 返回 b 域、只写 b、a 保留），wrapper 不启用修正
   （R-47）。其余 10 个原版写函数的反汇编证据与「无需接管/待勘察」判定见
   `native_inventory_hook.cpp` 语义表（2026-09-11 冻结）。
- **不 hook `UTIL_SetBitValue` 的原因**：它是无物品指针的通用位工具，hook 点拿不到
  所属物品，无法判定类别（可堆叠数量、宝石选项、袋容量、装备 marker 共用该工具），
  也无法计算进位；hook 它会把数量语义强加到所有位写入上。
- **关闭态（决策 b，R-47）**：堆叠上限关闭态下，模块与原版的数量读写、操作与展示
  统一按低 7 位视图——有效数量 = `b`（`count mod 128`），上限 99；`a`（bits22–24）
  不读、不写、不参与运算，只原样保留；重新开启后经 S2 解码读回完整 canonical 值。
  该语义由三层共同保证：①读侧 getter 经 `effective_read_count` 只返回 b；②原版写点
  只写 b（常驻 patch 表不含任何 a 段写点，clamp 表关闭态整体 revert 回原版 99/b
  语义）；③模块写点经 `effective_write_count` 只写 b，且 H-04 消耗借位预置仅在
  启用态生效（关闭态 b≤1 消耗到空按原版删除即关闭态语义，不为了保留 a 而让有效
  数量 0 的堆无法消耗）。关闭期操作造成的总量变化是决策 b 的既定语义，不做反向
  同步、不清零 `a`。S2 下 descriptor 与 sidecar 恒保持 canonical（见上文
  「descriptor canonical 边界」），重开即恢复完整视图。

- **S2 缺陷修复映射（2026-09-11，规则册 R-51..R-55 / 验证矩阵 VM-37..VM-41）**：
  ①出售/拆堆直接位读重定向统一 getter（R-51，`g_stack_getter_redirect_patches`，
  §3.7 上文）——修复整堆出售按 b 残量结算；②`ITEMSYSTEM_MakeItem` arg2 非数量、
  数量回写撤销（R-52，`stage4_make_item_writeback_count` 恒 0）——修复拾取 1 个得
  2/3/4 个；③`INVEN_SaveItem` 漏斗 backup 前 `virtual_bag_merge_native_item` 与
  backup 失败后 `virtual_bag_adopt_native_item` 都先经 `adopt_merge_into` 并入扩展
  同类堆（R-53）——修复拾取不并入扩展堆；④`move_extension_to_original_locked` 在
  目标袋为投影宿主时事务期恢复宿主真实容量字（RAII 写回投影值）且投影中不切换
  显示袋（R-54，`ext2orig_requires_host_capacity_restore`/
  `ext2orig_should_switch_display_bag`）——修复扩展→原版袋 0 移动误判满/宿主分叉
  错位视图；⑤`UIEquip_OKDestroyItem` 背包详情出售由 H-23 接管（R-55）——启用态取
  canonical 全量、按 `unit×count×7/10` 加钱删堆刷新，按钮预演只回填金额，关闭态/
  非背包详情 backup，修复原版 `0x1261c4` 内联 b 段读把 `199` 按 `71` 结算（真机
  实得 546）。五者行为断言均待真机 VM-37..VM-41 取证。

验证锚：真机锚归验证矩阵 VM-33/VM-34/VM-35/VM-36；读侧门控分流由
Host `test_get_cumulate_count_s2` 覆盖（kEncoded 直达不调 backup、kNotEncoded/
kUnknown/空指针 fail-closed 走 backup、启用/关闭两态视图与切换不丢值）；编码层与
模块写侧的编码/模式感知语义由 Host `test_stack_codec_s2`、
`test_stack_codec_effective_mode`、`test_virtual_bag_mode_aware_ops` 覆盖，patch 表
指令匹配待 debug 构建与真机回归取证。

## 4. 可替代的更好方法与不采用原因

以下方案在局部目标上可能更统一或更容易理解，但当前不采用。每节同时说明成本
和风险，不把现有方案包装成没有代价的最优解。

### 4.1 统一事务解释器

设想：把 use/equip/move/sell/craft/store/save 统一成
`Prepare→Apply→Commit→Abort`。收益是日志、阶段和回滚接口统一；成本是为原版同步
函数、popup、生产者多产物和保存 participant 建 effect adapter，并重做线程锁序。
原版返回值并不总是成功标志，`CHAR_UseItemEx` 还会同步进入消费 Hook；错误抽象会
造成重复消费、提前释放或错误回滚。问题 B 也说明统一 owner 不能替代物理取证，故
当前仍按操作分流，证据为 `extension_bag_transaction.inc` 与 `module_save.cpp`。

### 4.2 升宽槽编码

设想：把 `int8_t* out_slot` 改成能编码 `6..10` 的结构，让原版 caller 直接识别逻辑袋。
成本是同步修改 ABI、所有 caller、保存辅助、装备、删除和 TouchHandle，并证明现有 caller
不会截断。风险是游戏二进制栈/寄存器契约不可安全改写，任务袋 5 与扩展域也会折叠。
当前 `FindItemSlot` 只返回物理编码，证据 `inventory-integration-decision-plan.md:208-224`、
`game_symbols.h:391`，故不采用。

### 4.3 全量自绘 UI

设想：不再借用 `ControlItem`，模块自绘标签、格子、详情、弹窗和触控。收益是减少
投影覆盖和 data[0] 借用；成本是重建字体、动画、命中、按钮、详情、分辨率适配和
线程同步；风险是 UI 回归面大且会产生第二套 popup 状态机。当前复用原版容器、draw
和 ControlItem，见 `extension_bag_runtime.inc:81-160`、`extension_bag_render.inc:1-30`，
因此全量自绘不是低风险替代。

### 4.4 只读模式

设想：只显示 sidecar，不接管消费、装备、跨域移动和生产者。收益是降低对象释放、
物理写入和保存冲突风险；成本是每个入口都要稳定拒绝，API/验收需拆成双模式；风险
是它仍不解决投影期间 TouchHandle 的物理干扰，也不解决问题 B。当前已有 P4 事务、
原版效果复用和 participant，除非运行时无法维持所有权不变式，否则不退回只读。

### 4.5 Hook `SAVE_*` 承载 payload

设想：把 payload 编进原版保存记录，Hook `SAVE_SaveInventory`/`SAVE_SaveItem`/
`SAVE_LoadItem` 并取消 sidecar。成本是重做记录格式、长度、自动保存和崩溃恢复；
风险是原版记录和对象池是固定契约，格式变化破坏存档兼容，扩展对象也不能交给
`ITEMPOOL_Free`。当前 section version=4，见 `ExtensionBagUiBridge.kt:18-24`；sidecar
participant 风险更小，故不采用。

触摸窗口内的 custody 释放由 `extension_bag_ownership.inc` 的 16 项定长环形队列接管；
draw-end 在窗口关闭后逐项检查 `g_projected_item_root` 控件 `data[0]` 与
`g_module_objects` 引用，只有无引用对象才调用 `ITEMPOOL_Free`，满队列或仍有引用时保持
队列项以 fail-safe 避免 UAF。

## 5. 未知与限制

### 5.1 问题 B：未解决

固定口径：扩展袋 0 的 a↔b 交换后，原版袋 0 同号 b 格物品消失并被保存固化；当前使用
0x18 单一 owner、物理 `0..5×16` 快照守卫和 pre/post 插桩。代码防线为
swap 状态同步 `extension_bag_transaction.inc:374-449,487-538`、物理快照
`extension_bag_lifecycle.inc:71-165`、唯一 owner `:297-315`；H3/H4 复测日志缺口为
`archive/extension-bag/writing-materials/impact-matrix-source.md:79-83`。因此不能由守卫代码或 API 成功返回推导
交换安全。

### 5.2 P7 stage-5 边界

拾取、任务奖励、开箱、拆包、合成、商店购买、脱装备和使用结果仍需逐 caller 真机
验证；静态表见 `inventory-integration-decision-plan.md:142-155`。开箱/解封/骰子的
消费时机和多产物回滚仍有独立路径，见 `game_inventory_use.inc:17-95`；生产物释放、
材料删除、扣款回滚和脱装备满包也未由本架构关闭。SaveItem H-13 是当前代码事实，
但 caller/所有权/真机覆盖仍未闭合，不能据此宣称生产者全覆盖。

### 5.3 `ExtensionBagUiBridge` v2/v3/v4 现状

当前仍是兼容分支，不是最终纯 v4：主 section/版本见 `ExtensionBagUiBridge.kt:18-24`；
load 兼容 legacy 见 `:59-67`；v2/3/4 分支、回写处理见 `:69-94`；payload-less
隔离见 `:180-240`。Hub 的 v4-only 是最终目标而非当前实现状态，见
`control-plane.md:118`。本文记录现状，不宣称兼容分支属于最终发布契约。

**未定-需真机证据**：v2/v3/legacy 在进程中断、切档和跨进程并发下的回写安全性，不能由
`ExtensionBagUiBridge` 的静态分支判定。取证应使用验收册 VM-25：分别准备兼容 section、
在回写窗口 force-stop，随后核对 v4 section、last-good、原版存档和隔离日志。

### 5.4 API、UI 与锁限制

扩展 bag `6..10` 才走扩展 move，原版→原版仍走 `nativeOpMoveItem`，见
`InventoryActions.kt:24-31`；任务袋 5 和扩展视图打开时的原版移动被拒绝，见
`game_inventory_use.inc:189-216`。视图入口/选择/点击仍有 debug 路径，见
`ExtensionBagActions.kt:16-29`，不能代替 UI 验收。对象四态账本见
`model/ownership_ledger.h:3-35`；持 `g_virtual_bag_mtx` 时不得调用会刷新的 `op_ok()`，
见 `AGENTS.md:34-42`。

#### 5.4.1 持锁原版回调的受控入口（R-44 同类点清单）

`g_virtual_bag_mtx` 是非递归 `std::mutex`；持锁期间调用可能回调库存 Hook 的原版函数，
会形成同线程重入取锁的自死锁。当前实现只允许经下列受控入口：

**TLS original-only 守卫本体**：

- `g_in_native_call`（thread_local）+ RAII `NativeCallScope`：`game_ui_virtbag.cpp:187-194`。
- 查询口 `virtual_bag_native_call_active()`：`game_ui_virtbag.cpp:279-281`。

**dispatcher 本体**：

- 删除：`remove_item_direct_unlocked` 解锁 → `NativeCallScope` 内调 `fn_remove_item_direct`
  → 重锁 → `original_to_extension_source_slot_postcondition` 复核源槽
  （`game_ui_virtbag.cpp:246-269`）。
- 刷新：`virtual_bag_call_original_refresh_item_area_with_guard` 经 `NativeCallScope` 调
  raw-original trampoline（`game_ui_virtbag.cpp:283-286`）；
  `inventory_native_hook_call_refresh_item_area_original`（`native_inventory_hook.cpp:608`）。

**查询 Hook 的 original-only 直通分支**（守卫置位时只走原版 backup）：

- `FindItem`：`native_inventory_hook.cpp:91-101`；`HaveItem`：`:125-132`；
  `GetItemCount`：`:134-142`；`IsHavingEmptySlot`：`:144-154`。
- `stage4_*_original_only` 实现：`inventory_hook_stage4.cpp:25-63`。

**受控调用点**（新增持锁原版调用必须改走上述 dispatcher，不得自开裸调用）：

| 调用点 | 入口 | context |
|---|---|---|
| `extension_bag_transaction.inc:102` | orig→ext 删除源（`move_original_to_extension_locked`） | `original-to-extension` |
| `extension_bag_transaction.inc:118` | orig→ext 提交后原版刷新 | `NativeCallScope` |
| `extension_bag_transaction.inc:269`、`:287` | ext→orig 失败回滚删除 | `ext2orig release rejected rollback` / `ext2orig persist rollback` |
| `extension_bag_equip.inc:108` | 装备路径删除源 | `extension equip` |
| `game_ui_virtbag.cpp:861` | 卸下回滚删除 | `unequip rollback` |
| `extension_bag_runtime.inc:796`、`:799` | pending recovery 删除与刷新 | `pending recovery` |

### 5.5 结论边界

本文能证明 include/port 层级、18 项代码路径、VMA/选型证据和素材差异；不能证明
P7 生产者全覆盖、问题 B 已修复、全部 Hook 已通过 LSPosed 真机验收，或仅凭静态链
关闭消费时机、失败回滚和跨进程恢复。

## 6. 无法从仓库直接取证、标为“推断”的点

以下内容标为推断，未作为实现事实：

1. **推断**：未逐一打开的二进制 caller 一定走 `INVEN_SaveItem`；静态 caller 表不能
   推导 SaveItem wrapper 全覆盖。
2. **推断**：所有目标 LSPosed 环境都会触发 `native_init` 并提供 ABI version 2；清单只
   证明注册，触发和 backup 仍需设备证据。
3. **推断**：物理快照恢复覆盖问题 B 的全部来源；H3/H4 缺日志，只能证明代码有守卫。
4. **推断**：所有 `CHAR_UseItemEx` 类别成功时都同步消费；开箱、解封、骰子有独立分支，
   需 stage-5 取证，见 `game_inventory_use.inc:17-95`。
5. **推断**：v2/v3 在异常、进程中断、跨进程场景均可安全回写/恢复；最终仍受 P6 约束。
