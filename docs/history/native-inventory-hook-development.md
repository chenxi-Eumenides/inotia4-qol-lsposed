# 扩展背包原生库存函数适配开发文档

历史归档（2026-09-08）：仅供追溯，不得作为当前实现依据；当前权威见 docs/development/features/extension-bag/

> 文档状态：开发规格（未包含实现）  
> 适用范围：P7 原生库存查询、消耗、删除入口适配  
> 目标版本：当前 `libgame.so`，并兼容后续动态符号地址漂移  
> 最后核对：2026-09-05

## 1. 文档目的

本文档用于指导开发代理实现以下功能：

1. 通过 LSPosed 官方 Native Hook API 拦截游戏原生库存函数入口。
2. 让原版物品和扩展背包物品在查询、消耗、删除时正确分流。
3. 保持原版 `libgame.so` 的 ABI、返回值和对象所有权语义。
4. 不让扩展对象进入原版 `ITEMPOOL_Free` 或原版物理槽写入路径。
5. 以动态符号解析为首选，`game_symbols.h` 中的 VMA 只作为 fallback。

本文档不授权一次性替换全部库存函数，也不允许以三个入口 hook 已完成 P7 为结论。P7 仍要求继续审计查询、入库、生产者、装备、合成、商店、存档和 UI 旁路。

## 2. 开发前必须阅读的文档

按以下顺序阅读，不重复尝试文档已经否定的方案：

1. `README.md`
2. `docs/development/architecture.md`
3. `docs/development/features/extension-bag/control-plane.md`
4. `docs/development/features/extension-bag/module-save-store.md`
5. `docs/development/features/extension-bag/drag-protocol.md`
6. `docs/reference/game/bag.md`
7. `docs/guides/build-and-deploy.md`

本任务直接相关的权威章节：

- `architecture.md §2.2.1 C++ Hook 技术选型结论`
- `control-plane.md §P7.1 背包相关函数清单与适配边界`
- `control-plane.md §P7.2 Hook 与调用机制选择`
- `control-plane.md §P7.3 实施顺序`

## 3. 当前已确认的游戏原生实现

当前输入文件：

```text
apk/decoded/lib/arm64-v8a/libgame.so
```

动态符号确认：

| 函数 | 当前 VMA | 动态符号 | 当前大小 | ABI |
|---|---:|---|---:|---|
| `INVEN_FindItem` | `0x10438c` | `GLOBAL FUNC` | 208 bytes | 反汇编显示单个整数类别参数，正式 typedef 需在 PoC 中冻结 |
| `INVEN_RemoveItem` | `0x104044` | `GLOBAL FUNC` | 100 bytes | `int (void* item)` |
| `INVEN_ConsumeItem` | `0x1047bc` | `GLOBAL FUNC` | 180 bytes | `void (void* item)` |
| `INVEN_FindItemSlot` | `0x103704` | `GLOBAL FUNC` | 604 bytes | `int (void* item, int8_t* out_slot)`；成功写 `out_slot[0]` 并返回 `1`，失败返回 `0` |
| `INVEN_RemoveItemDirect` | `0x103fd8` | `GLOBAL FUNC` | 108 bytes | `int (int bag, int slot)`，返回值不可信 |
| `INVEN_HaveItem` | `0x104870` | `GLOBAL FUNC` | 196 bytes | `int (int32_t category)` |
| `INVEN_GetItemCount` | `0x104260` | `GLOBAL FUNC` | 300 bytes | `int (int32_t category)` |
| `INVEN_GetBagSize` | `0x103250` | `GLOBAL FUNC` | — | `int (int32_t bag)` |
| `INVEN_GetEmptyBagSlot` | `0x103280` | `GLOBAL FUNC` | 96 bytes | `int ()`，返回原版物理槽编码 |
| `INVEN_IsEmptyBag` | `0x1032e0` | `GLOBAL FUNC` | — | `int (int32_t bag)` |
| `INVEN_IsHavingEmptySlot` | `0x103460` | `GLOBAL FUNC` | 676 bytes | `int (int32_t needed, int32_t include_task_bag)` |

上述 VMA 只用于反汇编、特征校验和当前版本记录。正式代码不得在域文件写裸地址，必须使用：

- `game_symbols.h` 中的 `F_*_VMA` 常量；
- `symbol_registry.h` 中的符号登记；
- `symbol_resolver` 的动态符号优先解析；
- 必要时才回退 VMA。

当前事实：上述查询与容量入口均已完成正式 typedef、符号登记和函数指针解析；只有
`INVEN_FindItem`、`INVEN_HaveItem`、`INVEN_GetItemCount`、`INVEN_IsHavingEmptySlot`
已接入 Native Hook。`GetBagSize`、`GetEmptyBagSlot`、`IsEmptyBag` 保留原版物理袋语义，
扩展袋容量和空槽由逻辑接口独立提供；不得用扩展 bag/slot 编码伪装原版返回值。

### 3.1 `INVEN_FindItem`

当前实现：

```text
参数 w0 = category
├─ 负值/越界 → 返回 nullptr
├─ 遍历原版袋 0..5
├─ 每袋按 INVEN_GetBagSize(bag) 遍历物理槽
├─ 从 item + I_TYPE 读取类别位域
└─ 找到匹配项 → 返回原版 native item 指针
```

原版任务袋 `5` 当前会被原版函数扫描，但扩展背包逻辑要求任务袋 `5` 不参与扩展库存聚合。不能伪造扩展袋为原版物理袋。

入口 wrapper 的建议语义：

1. 先调用原函数，保留原版优先顺序。
2. 原函数返回非空时直接返回原版对象。
3. 原函数返回空时，在扩展逻辑袋中按 category 查找。
4. 扩展命中时返回模块持有的、可受控借用的 native 对象。
5. 扩展对象只能在同步游戏操作链中使用，不得交给原版对象池释放。

### 3.2 `INVEN_ConsumeItem`

当前反汇编链路：

```text
INVEN_ConsumeItem(item)
├─ 读取 item + I_TYPE 的类别位域
├─ 从静态物品数据读取可堆叠标记
├─ 不可堆叠 → 尾跳 INVEN_RemoveItem(item)
└─ 可堆叠
   ├─ 读取 item + I_COUNT 的数量位域
   ├─ 数量 > 1 → 数量减 1
   └─ 通过 UTIL_SetBitValue 写回 item + I_COUNT
```

内部使用的基础函数包括：

- `UTIL_GetBitValue`
- `MEM_ReadUint8`
- `UTIL_SetBitValue`
- `INVEN_RemoveItem`

其中前三个是通用位域/内存工具，不得为了扩展背包而全局 hook。真正具有库存语义的内部依赖只有 `INVEN_RemoveItem`，因此：

- 必须 hook `INVEN_ConsumeItem` 入口；
- 必须同时 hook `INVEN_RemoveItem` 入口；
- 不需要 hook `UTIL_GetBitValue`、`MEM_ReadUint8`、`UTIL_SetBitValue`；
- 不应把扩展对象交给原版 `INVEN_RemoveItemDirect`。

为什么不能只做尾部 hook：不可堆叠分支在 `0x104830` 尾跳到 `INVEN_RemoveItem`，不会返回普通 `INVEN_ConsumeItem` 尾部；因此尾部只能覆盖可堆叠分支，无法覆盖完整消耗语义。

### 3.3 `INVEN_RemoveItem`

当前实现：

```text
INVEN_RemoveItem(item)
├─ 调 INVEN_FindItemSlot(item, out_slot)
├─ 未找到 → 保持失败/原版后续状态
├─ 找到 → 解码物理 bag/slot
├─ 调 INVEN_RemoveItemDirect(bag, slot)
└─ 执行后续 shortcut 更新/状态检查
```

`INVEN_RemoveItemDirect` 当前按原版物理 `bag/slot` 访问 `INVEN_pItem`，随后调用 `ITEMPOOL_Free(item)`。扩展对象没有合法的原版物理槽，禁止伪造扩展袋号或扩展槽号进入此函数。

`INVEN_RemoveItem` 必须在入口根据 item 指针识别扩展对象：

- 扩展对象：执行扩展逻辑槽删除，返回原版成功语义 `1`；
- 原版对象：调用 LSPosed backup 原函数；
- 非法/已失效对象：不得盲目调用原版释放路径，应按现有错误语义拒绝并记录原因。

尾部不适合作为主分流点，因为原版调用 `INVEN_FindItemSlot` 后，原始 item 指针契约和失败路径都不适合再推断扩展逻辑槽。

## 4. LSPosed Native Hook API 结论

### 4.1 官方能力

LSPosed 官方 Native Hook API 提供：

```cpp
using HookFunType = int (*)(void* func, void* replace, void** backup);
using UnhookFunType = int (*)(void* func);
using NativeOnModuleLoaded = void (*)(const char* name, void* handle);

struct NativeAPIEntries {
    uint32_t version;
    HookFunType hook_func;
    UnhookFunType unhook_func;
};

using NativeInit = NativeOnModuleLoaded (*)(const NativeAPIEntries* entries);
```

官方实现会在目标函数入口进行替换并提供 backup/trampoline。模块不需要自己实现 ARM64 trampoline、指令搬运、跳板分配或 LR 修复。

官方参考：

- <https://github.com/LSPosed/LSPosed/wiki/Native-Hook>
- <https://github.com/LSPosed/LSPosed/wiki/Develop-Xposed-Modules-Using-Modern-Xposed-API>
- <https://github.com/LSPosed/LSPosed/blob/master/core/src/main/jni/src/native_api.cpp>

### 4.2 当前项目缺失项

当前项目已有：

- `module/app/src/main/cpp/bridge/native/gamebridge.cpp` 中的 `JNI_OnLoad`；
- `NativeBridge.nativeInit()`；
- `libgamebridge.so`；
- `game_access` 动态符号解析；
- C++17 Android NDK 构建。

当前项目已具备：

```text
module/app/src/main/resources/META-INF/xposed/native_init.list

当前仍需验证：

```text
LSPosed native_init 回调是否在目标环境触发
目标函数是否全部成功安装 Hook
backup 调用、递归保护、失败回滚和业务调用链
```
```

Modern API 路径应增加 `META-INF/xposed/native_init.list`，内容为：

```text
libgamebridge.so
```

旧 Legacy/LSPatch 路径可能要求 `assets/native_init`；该路径不作为当前 LSPosed 模块版 Native Hook 的实现依据。不得假定 API 93、101、102 的 Java 版本号等同于 Native Hook 协议版本。

### 4.3 API 版本兼容判断

必须区分三种版本：

| 名称 | 含义 | 当前判断 |
|---|---|---|
| Java API 93 | Legacy Xposed API | NPatch/LSPatch 常见兼容路径；不代表 Native Hook ABI |
| Java API 101 | Modern LibXposed API | 当前项目 `compileOnly` 版本；可使用 Modern native entry 清单 |
| Java API 102 | Modern API 后续版本 | 需在目标 LSPosed/NPatch 版本上实机验证 |
| `NativeAPIEntries.version` | Native Hook 接口协议版本 | 当前 LSPosed 源码实现为 `2` |

LSPosed 官方 Native Hook 文档没有给出“API 93/101/102 对应 Native Hook 可用性”的完整兼容矩阵。因此：

- LSPosed 模块版：先按 Modern API 101 + `native_init.list` 实现并验证；
- 后续 API 102：重新构建并真机验证 `native_init` 回调、hook 安装和 backup 调用；
- NPatch API 93：单独做加载 PoC，不得因 Java API 93 可加载模块就宣称 Native Hook 可用；
- NPatch 新版本已公开声明支持 Modern/native-only 模块的版本，需要按实际安装版本验证，不以项目名称推断。

## 5. 动态符号和目标地址策略

### 5.1 地址解析优先级

正式 hook 目标采用以下优先级：

```text
实际加载的 libgame.so
    ↓
symbol_resolver.resolve(符号名, fallback_vma)
    ↓
ELF .dynsym 命中 → 使用动态地址
    ↓ 未命中
game_symbols.h VMA fallback
    ↓
入口特征/函数大小/可执行映射校验
    ↓
校验失败 → 不安装 hook，保留原版路径并记录错误
```

`symbol_resolver.cpp` 已实现：

- 根据 `/proc/self/maps` 定位 `libgame.so` 基址；
- 解析 ELF `.dynsym`；
- SysV ELF hash 查找符号；
- 地址范围校验；
- VMA fallback；
- 符号来源统计。

不得在新文件中重新实现另一套基址或符号解析。

### 5.2 Hook 安装目标

建议在 `bridge_init()` 成功、全部 `fn_*` 函数指针已经解析后，从 JNI 顶层或 patch feature 调用安装函数。不要让 `game_access` data 层直接依赖 `feature/extension_bag`，以免违反 native 依赖方向。

推荐调用关系：

```text
Java NativeBridge.nativeInit()
    → bridge_init()
    → inventory_native_hook_install()
```

如果 Native Hook API 先于 `bridge_init()` 获得，则只保存 `hook_func`；等 `bridge_init()` 完成后再安装目标 hook。

### 5.3 backup 指针要求

必须保存独立 backup 指针：

```cpp
FindItemFn g_backup_find_item = nullptr;
ConsumeItemFn g_backup_consume_item = nullptr;
RemoveItemFn g_backup_remove_item = nullptr;
```

wrapper 对原版对象调用 backup，不得调用已经被 hook 的 `fn_find_item`、`fn_consume_item` 或 `fn_remove_item`，否则会递归进入 wrapper。

## 6. Dispatcher 设计

### 6.1 统一对象识别

扩展对象识别必须基于当前逻辑槽映射：

```text
item 指针
    ↓
遍历 g_module_objects[bag][slot]
    ↓ 命中
返回 extension bag/slot
    ↓ 未命中
按原版对象处理
```

不能只通过 item category 判断扩展对象，因为原版和扩展可以持有同一 category。

识别函数必须：

- 在 `g_virtual_bag_mtx` 保护下读取扩展对象表；
- 校验 bag/slot 范围；
- 校验对象指针仍等于 `g_module_objects[bag][slot]`；
- 处理 UI 重建、卸载和对象重物化后的旧指针；
- 不把装备槽对象自动当成扩展逻辑背包槽对象。

### 6.2 `FindItem` dispatcher

推荐伪代码：

```text
find_item_wrapper(category):
    if recursive_guard:
        return backup_find_item(category)

    original = backup_find_item(category)
    if original != nullptr:
        return original

    enter recursive_guard
    lock g_virtual_bag_mtx
    ensure extension state is ready
    find first valid extension descriptor with category and count > 0
    item = module_item_locked(bag, slot)
    unlock
    leave recursive_guard
    return item
```

注意：`module_item_locked()` 可能物化 native 对象。物化失败时必须返回 `nullptr` 并记录失败原因，不得返回 payload 地址强转指针。

### 6.3 `ConsumeItem` dispatcher

`CHAR_UseItemEx` 成功后通常会调用 `INVEN_ConsumeItem`。因此扩展 wrapper 的正常时序是：

```text
CHAR_UseItemEx(leader, extension_item, flag)
    → 游戏完成效果
    → INVEN_ConsumeItem(extension_item)
        → wrapper 识别扩展对象
        → 扩展数量减 1 / 清槽
        → payload 回写和 dirty 标记
        → 不调用 backup_consume_item
```

扩展分支要求：

- 读取 descriptor 的 `before_count`；
- 从 native 对象读取观察数量，但不能把原版函数调用结果当成扩展状态唯一来源；
- 数量大于 1 时更新 `I_COUNT` 并重新序列化 payload；
- 数量归零时清理 descriptor，再释放模块持有对象；
- 失败时恢复 native count 和逻辑 descriptor，记录 context；
- 不在持有 `g_virtual_bag_mtx` 时调用会触发缓存刷新的 `op_ok()`；
- 不在该底层 hook 中擅自改变项目规定的保存时机。

原版分支：

```text
return g_backup_consume_item(item)
```

### 6.4 `RemoveItem` dispatcher

扩展删除语义默认是“删除整个逻辑槽”，与原版 `INVEN_RemoveItem(item)` 的删除意图对应。实现要求：

1. 根据 item 指针找到扩展 bag/slot。
2. 再次确认槽内仍持有同一 item 指针。
3. 清除逻辑 descriptor。
4. 清除类别、hash 和对象映射。
5. 释放模块对象，但不能调用 `ITEMPOOL_Free`。
6. 标记 dirty，按既有扩展背包刷新协议刷新 UI/投影。
7. 返回 `1` 表示扩展删除已完成。

原版分支必须调用 `g_backup_remove_item(item)`，而不是重新调用 `fn_remove_item(item)`。

### 6.5 递归与线程安全

wrapper 必须加入：

- `thread_local` 递归保护，至少分别保护 Find/Consume/Remove；
- 安装状态原子标志，防止重复安装；
- 目标函数 backup 非空检查；
- 扩展状态未初始化时，原版对象走 backup；
- 扩展对象表读写使用 `g_virtual_bag_mtx`；
- 不在游戏线程执行期间卸载 Native Hook；
- 不提供会导致 backup 失效的运行时热卸载路径。

## 7. 其他必须适配的原版函数

### 7.0 本任务实施顺序（冻结）

按依赖和风险从低到高执行，前一项未完成 ABI、实现和真机验收前，不进入后一项：

1. **ABI 与调用链确认**：先确认 `INVEN_FindItemSlot`，再确认 `CHAR_UseItemEx`、`CHAR_ProcessShortcut` 及其直接物理库存检查；不得猜签名或返回值。
2. **逻辑查询层**：统一 `FindItem`、`HaveItem`、`GetItemCount`、`FindItemSlot` 的原版优先/扩展 fallback 语义；扩展对象不得伪造原版物理槽。
3. **使用与消耗**：恢复原版 `UIEquip_ButtonUseExe` 主路径；由 `CHAR_UseItemEx` 执行效果，由 `INVEN_ConsumeItem`/`INVEN_RemoveItem` 负责扩展扣减和删除。
4. **快捷键使用**：审计 `CHAR_ProcessShortcut` 的直接 `BL INVEN_FindItem` 调用；仅在上层无法覆盖时做单点调用点适配。
5. **装备与脱装备**：依次处理 `CHAR_CanEquipItem`、`CHAR_FindEquipSlot`、`CHAR_EquipItem*`、`CHAR_UnequipItemToInven*`，复用扩展事务和所有权账本。
6. **镶嵌与强化**：适配 `ITEMSYSTEM_PutJewel`、`ITEMSYSTEM_EnchantItem` 及其材料消耗，不让扩展材料进入物理槽写入路径。
7. **堆叠、拆分、移动、丢弃**：处理 `INVEN_GetCumulateSaveSlotEx`、`INVEN_MoveItem`、`ITEMSYSTEM_Divide`、`INVEN_RemoveItemData`，扩展侧统一进入逻辑事务。
8. **出售、合成和生产者**：处理 `UIStore_*`、`DEALSYSTEM_*`、`MIXSYSTEM_*`、掉落、奖励、开箱和拆包；网络商店不属于当前 P7 接入范围。
9. **独立 UI 面板**：最后接入商店、合成器等独立控件树；不改变保存协调器和 sidecar 提交时机。
10. **逐项验收**：每项记录 ABI、调用链、原版/扩展成功与失败、数量、payload、所有权、回滚、日志和真机证据；未完成项不得标记为 P7 完成。

当前执行项：第 5 项装备与脱装备原版入口 Hook/事务验收。

第 1 项 ABI 已完成：`INVEN_FindItemSlot` 为 `int (void*, int8_t*)`，成功写回
`out_slot[0]` 并返回 `1`，失败返回 `0`。第 4 项快捷键调用链也已核实：
`CHAR_ProcessShortcut`（`0xec028`，ABI `int (void*, int)`）在 `0xec1a4`
直接调用 `INVEN_FindItem`，随后在 `0xec1f8` 调用 `CHAR_UseItemEx`。因此快捷键
扩展物品使用复用 `INVEN_FindItem` Hook，不另加 `CHAR_ProcessShortcut` Hook；仍需
单独完成真机快捷键成功、冷却失败和数量扣减验收。

第 7.3 项 ABI 已开始确认：`ITEMSYSTEM_Divide`（`0x1083f8`）为
`void* (void*, int32_t)`，调用点用于按数量拆分堆叠物品，返回新物品指针，失败返回
`nullptr`。正式扩展拆堆实现必须接管返回对象所有权，不能直接让其进入原版物理槽。

查询与容量组的剩余 ABI 已由 `libgame.so` 符号入口和 AArch64 调用点确认：

- `INVEN_CalculateEmptySlotCountForSave`（`0x103f10`）：`int (int32_t, int32_t)`；
  两个参数分别在入口以 `w0`、`w1` 使用，返回待保存物品所需的空槽数量。
- `INVEN_GetEmptySaveSlotEx`（`0x105070`）：
  `int (int32_t, int32_t, int8_t*, int32_t, int32_t*)`；入口使用
  `w0/w1/x2/w3/x4`，向 `x2` 写槽编码，并通过 `x4` 更新已写入数量。
- `INVEN_GetNeededSaveSlotEx`（`0x105440`）：
  `int (int32_t, int32_t, int8_t*, int32_t, int32_t*, int8_t*, int32_t)`；入口使用
  `w0/w1/x2/w3/x4/x5/x6`，内部同时调用 `INVEN_GetEmptySaveSlotEx` 和
  `INVEN_GetCumulateSaveSlotEx`。
- `INVEN_GetCumulateSaveSlotEx`（`0x1051b0`）：
  `int (int32_t, int32_t, int8_t*, int32_t, int32_t*)`；向第三个参数写入可堆叠槽编码，
  通过第五个参数更新结果数量。

以上四个函数目前已登记到 `game_symbols.h`、`symbol_registry.h` 和运行时解析表；扩展
逻辑分流与保存事务接管仍待实现，不能标记为容量组完成。

文档已登记完整范围，见 `control-plane.md §P7.1`。以下是开发顺序，不代表全部都要 Native Hook。

### 7.1 第一组：查询

```text
INVEN_FindItem
INVEN_HaveItem
INVEN_GetItemCount
INVEN_FindItemSlot
```

处理方式：逻辑库存查询服务。`FindItemSlot` 不得用扩展 bag 号伪造原版物理槽编码。

### 7.2 第二组：袋容量和空槽

```text
INVEN_GetBagSize
INVEN_GetEmptyBagSlot
INVEN_IsEmptyBag
INVEN_IsHavingEmptySlot
INVEN_CalculateEmptySlotCountForSave
INVEN_GetEmptySaveSlotEx
INVEN_GetNeededSaveSlotEx
```

处理方式：原版容量和扩展逻辑容量分别计算，由统一入库策略聚合；任务袋 `5` 不得被扩展逻辑占用。

### 7.3 第三组：堆叠和移动

```text
INVEN_GetCumulateSaveSlotEx
INVEN_MoveItem
ITEMSYSTEM_Divide
```

处理方式：原版↔原版保留原函数；涉及扩展袋时走逻辑事务、payload 序列化和所有权账本。

### 7.4 第四组：删除和消耗

```text
INVEN_RemoveItem
INVEN_RemoveItemDirect
INVEN_RemoveItemData
INVEN_ConsumeItem
```

处理方式：只有 item-pointer 入口负责对象分流。`RemoveItemDirect` 是物理释放原语，不作为扩展对象入口；扩展对象必须在更高层被截住。

### 7.5 第五组：入库和创建

```text
INVEN_FindSaveSlot
INVEN_SaveItem
INVEN_SaveItemDirect
INVEN_SaveItemOnEmpty
INVEN_SaveItemData
INVEN_CheckSaveInNotEmptySlot
ITEMSYSTEM_CreateItem
ITEMSYSTEM_MakeItem
ITEMSYSTEM_ProcessUnpack
```

处理方式：原版背包 `0..4` 优先，原版满后进入扩展逻辑袋。不得返回虚假的原版物理槽让调用者继续写 `INVEN_pItem`。

### 7.6 第六组：物品语义

```text
ITEM_GetCumulateCount
ITEM_GetPrice
ITEM_GetSellPrice
ITEM_GetBuyPrice
ITEM_GetAbilityLevel
ITEM_GetRarity
ITEM_GetDamage
ITEM_GetDefense
ITEM_GetMagicDamage
ITEM_GetName
ITEMDATABASE_IsUse
ITEMSYSTEM_Is*
ITEMSYSTEM_CanPutJewel
```

处理方式：优先用 category/静态表逻辑；只有确实需要原版复杂计算时才受控物化 native 对象。

### 7.7 第七组：使用和装备

```text
CHAR_UseItemEx
CHAR_ProcessShortcut
ITEMSYSTEM_OpenItemBox
ITEMSYSTEM_ReleaseSealed
CHAR_CanEquipItem
CHAR_FindEquipSlot
CHAR_EquipItem
CHAR_EquipItemFromInven*
CHAR_UnequipItemToInven*
```

处理方式：以业务 dispatcher 为主。快捷键、物品描述、装备按钮等直接旁路必须单独审计，不能用 API 端点成功代替游戏内调用链验证。

### 7.8 第八组：合成、商店和掉落

```text
MIXSYSTEM_CheckMixture
MIXSYSTEM_GetStuff*
MIXSYSTEM_UseStuff
MIXSYSTEM_MakeItem
UIStore_*
DEALSYSTEM_*
MAPITEMSYSTEM_*
CHAR_PickItemAll
```

处理方式：分别适配材料槽、商品槽、掉落对象和入库流程；不能只 hook `INVEN_GetItemCount` 解决。网络商店已移出当前 P7 接入范围，相关原版函数仅保留运行时和历史审计用途。

### 7.9 第九组：存档和对象生命周期

```text
SAVE_SaveInventory
SAVE_SaveItem
SAVE_LoadItem
ITEMPOOL_Allocate
ITEMPOOL_Free
```

处理方式：原版存档保持原语义；扩展 payload 由 sidecar/participant 处理。扩展物化对象的 allocate/free 必须由所有权账本控制。

### 7.10 第十组：UI

```text
UIEquip_*
ControlItem_SetItem
ITEM_Draw*
UIStore_* inventory controls
UIMix_* inventory controls
```

处理方式：稳定间接回调继续使用 `PtrHook`；独立商店/合成控件树分别接入，不把 debug 端点当正式 UI 操作面。

## 8. 实施阶段

### 阶段 A：Native Hook 最小 PoC

本阶段是项目架构中允许的“先验证后进入正式功能”闸门。PoC 通过前，不得修改扩展物品数量、删除逻辑或把全局 hook 宣称为正式功能；PoC 失败时回退到直接调用、`PtrHook` 或有限指令 patch，并更新架构决策记录。

修改范围建议：

```text
module/app/src/main/resources/META-INF/xposed/native_init.list
module/app/src/main/cpp/feature/patch/native_inventory_hook.h
module/app/src/main/cpp/feature/patch/native_inventory_hook.cpp
module/app/src/main/cpp/bridge/native/gamebridge.cpp
module/app/src/main/cpp/CMakeLists.txt
```

PoC 只做以下事情：

1. 导出 `native_init`。
2. 保存 `NativeAPIEntries::hook_func`。
3. 在 `libgame.so` 已解析后安装三个 hook。
4. wrapper 只记录调用次数、参数和 backup 地址，然后调用 backup。
5. 不接入扩展业务，不修改物品数量，不修改 payload。

PoC 成功标准：

- `native_init` 被调用；
- `hook_func` 非空；
- 三个目标地址解析成功；
- 三个 backup 非空；
- 原版背包打开、使用、删除、镶嵌流程无崩溃；
- 无重复安装日志；
- 进程重启后仍能安装；
- LSPosed 模块版真机通过。

### 阶段 B：扩展对象识别

1. 暴露或复用扩展对象到逻辑槽的受控查询接口。
2. 为 Find/Consume/Remove 各自增加扩展对象识别。
3. 先只记录“命中扩展对象但仍回退原版”的观察日志。
4. 验证物化对象、重建对象和旧指针均可区分。

### 阶段 C：`RemoveItem` 扩展分支

先实现删除，再实现消耗。原因是删除语义不依赖游戏效果链，更容易验证对象所有权。

验证：

- 扩展单件删除；
- 扩展堆叠物整槽删除；
- 删除后对象不再出现在 `g_module_objects`；
- 未调用 `ITEMPOOL_Free`；
- 原版删除行为不变；
- 返回值与删后状态一致。

### 阶段 D：`ConsumeItem` 扩展分支

1. 扩展可堆叠物数量减 1。
2. 数量归零清理逻辑槽。
3. 扩展不可堆叠物通过 Consume 入口删除。
4. `CHAR_UseItemEx` 成功和失败路径分别验证。
5. 原版药水、卷轴、技能书、骰子、解封、开箱路径回归。

### 阶段 E：`FindItem` 扩展分支

1. 原版优先。
2. 原版无匹配时扩展 fallback。
3. 扩展对象只返回模块持有借用对象。
4. 验证后续 `CHAR_UseItemEx`、`INVEN_ConsumeItem`、`INVEN_RemoveItem` 能连续完成。
5. 验证调用者不会把扩展对象送入原版物理槽或对象池。

### 阶段 F：旁路审计

按以下顺序审计：

```text
INVEN_HaveItem / INVEN_GetItemCount
CHAR_ProcessShortcut
CHAR_UseItemEx
配方材料检查
装备/卸装备
掉落/奖励/开箱/拆包
合成/商店
保存/加载/对象释放
UIEquip/UIStore/UIMix
```

能在逻辑服务或上层业务入口解决的，不增加 Native Hook。确实必须 patch 的直接 `BL` 调用点，必须单独登记地址来源、ABI、寄存器、返回路径和回滚方案。

## 9. 禁止事项

以下行为会使实现失去验收资格：

1. 手写 ARM64 trampoline 替代 LSPosed 官方 Native Hook API。
2. 引入 ShadowHook/Dobby 等新依赖而不做专项兼容 PoC。
3. hook `UTIL_GetBitValue`、`MEM_ReadUint8`、`UTIL_SetBitValue` 等通用基础函数。
4. hook `ITEMPOOL_Free` 后试图靠猜测判断扩展对象。
5. 让扩展对象进入 `INVEN_RemoveItemDirect`。
6. 用扩展 bag/slot 伪造原版物理槽编码。
7. wrapper 内调用已被 hook 的 `fn_*` 作为 backup。
8. 在持有 `g_virtual_bag_mtx` 时调用 `op_ok()` 或触发缓存刷新的函数。
9. 以 `game_symbols.h` 当前 VMA 代替动态符号解析。
10. 未完成真机验证就宣称“游戏所有地方支持扩展背包”。
11. 直接修改无关业务逻辑、版本号、API 契约或保存时机。
12. 使用 `as any`、`@ts-ignore`、空异常处理；C++ 必须保持明确的错误和所有权语义。

## 10. 测试矩阵

### 10.1 Host 测试

Host 测试覆盖纯逻辑部分：

- category 查询优先级；
- 扩展对象指针到逻辑槽映射；
- 数量减 1；
- 数量归零；
- 重复删除；
- 失效指针；
- 原版/扩展混合库存；
- 任务袋 `5` 排除；
- payload 回写失败时回滚；
- hook 未安装时原版 fallback。

### 10.2 Native Hook PoC 真机测试

必须在唯一验收设备 `<设备IP>` 上进行：

1. 冷启动，确认 `native_init` 和 hook 安装日志。
2. 原版物品查询、使用、消耗、删除。
3. 原版宝石镶嵌后宝石正常消耗。
4. 扩展普通消耗品使用后数量减少。
5. 扩展不可堆叠物使用后槽位清空。
6. 扩展物品删除后不触发原版 `ITEMPOOL_Free`。
7. 原版和扩展同 category 同时存在时，确认原版优先。
8. 重启游戏并重新进入背包，确认没有重复 hook 或旧指针。
9. 关闭/打开背包面板，确认 UI 重建不产生旧对象误删。

### 10.3 构建和静态验证

修改后必须执行：

```text
git diff --check
相关 host tests
scripts/build-debug.sh
唯一真机 API smoke / 功能验收
```

Android 构建禁止直接执行 `./gradlew`、系统 `gradle` 或手写 Gradle 路径。

日志和一次性输出写入：

```text
.tmp/native-inventory-hook/<file>
```

任务完成后清理该临时目录，不把构建产物、日志、缓存和截图加入源码变更。

## 11. 验收标准

只有以下条件全部满足，三函数入口适配才算完成：

1. LSPosed Native Hook API 在模块版真机成功初始化。
2. 三个函数都使用动态符号地址优先安装，VMA fallback 有校验和日志。
3. 原版对象始终走 backup 原版逻辑。
4. 扩展对象不会进入原版 `INVEN_FindItemSlot`、`INVEN_RemoveItemDirect` 或 `ITEMPOOL_Free`。
5. `INVEN_FindItem` 能按原版优先、扩展 fallback 返回正确对象。
6. `INVEN_ConsumeItem` 能覆盖堆叠与不可堆叠扩展物品。
7. `INVEN_RemoveItem` 能删除扩展逻辑槽并返回可验证成功值。
8. 原版宝石镶嵌后的消费链路不回归。
9. wrapper 无递归、死锁、重复安装和旧指针误删。
10. Host 测试、Debug 构建和 `<设备IP>` 真机验收全部通过。

注意：上述条件只证明这三个入口适配完成，不代表整个 P7 已完成。P7 仍需完成本文第 7 节列出的旁路和生产者审计。

## 12. 开发代理交付格式

每个开发阶段交付以下内容：

```text
1. 修改文件清单
2. 动态符号解析和 hook 安装日志
3. wrapper ABI 与 backup 指针说明
4. Host 测试命令及结果
5. Debug 构建命令及结果
6. <设备IP> 真机步骤、日志和结果
7. 未完成项和风险
```

不得只报告“编译通过”。Native Hook 任务必须同时提供运行时安装证据、原版回归证据、扩展对象所有权证据和真机功能证据。
