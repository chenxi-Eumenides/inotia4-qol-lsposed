# `special_equip` 插件参考

## 摘要

功能插件（纯消费者、默认开启的叶子）：解除 ITEMDATABASE 记录 `+7` bit4 这道「NPC 专属保护」对**非特殊 NPC** 的约束，使玩家侧角色能够穿脱全部带该位保护的装备；同时为装备详情出售按钮登记一个布尔门谓词，按菜单角色的装备权限拦截出售。

本域只在原版判定**已经拒绝**时跳过 bit4 一项并重放其余检查，不直接放行，也不写 ITEMDATABASE。角色侧闸门完全交给原版 `CHAR_CanChangeEquip`，因此特殊 NPC 的穿脱限制与原版一致。插件不提供 capability、无 HTTP 路由、无 JNI 绑定。

除特别说明，文中 `plugins/...`、`contracts/...`、`game/...`、`host/...` 路径均相对 `module/app/src/main/cpp/`。

## 定位与版本

`special_equip`，插件版本 `0.1.0`（`plugins/special_equip/plugin.cpp:17-18`）。descriptor（`plugin.cpp:168-186`）：

- `requires` 1 项：`item.facts` v3.6（`contracts/item_facts.h:12-15`）。
- `optional_requires` 1 项：`character.facts` v1.0（`contracts/character_facts.h:12-14`），仅用于拒绝日志中的角色类型诊断；缺失不阻断安装。
- `provides` 0、`jni` 0、HTTP 路由 `nullptr, 0`。
- `resources` 2 个入口 hook（见下节）；`features` 1 个。
- `ports` 1 项：`QOL_PORT_CHARACTER_EXTRAS` v1.3（`plugin.cpp:32-36`；`contracts/game_ports/ports.h:54`）。
- 生命周期回调：`plugin_install`、`plugin_deactivate`、`plugin_on_enabled`、`plugin_on_disabled`、`plugin_fill_status`、`on_resource_ready`（`plugin.cpp:176-181`）。

注册位于 `host/assembly.cpp:225`。

## 公开能力与路由

无对外能力：不 `provides` 任何 capability，无 HTTP query/endpoint/route metadata，无 `routes.inc`。

唯一 feature 为 `special_equip.equipment_rules`，版本 1，默认 `1`，分类 `Hidden`，类型 `kToggle`，名称「NPC装备」，描述「允许获得、使用NPC装备」（`plugin.cpp:162-166`）。其回调为空操作（返回 `ok()`），运行时行为由 lifecycle 回调与 `g_gate` 驱动，不由 feature 回调驱动。该 feature 在 Debug 可见、Release 不显示 Hidden 分类（见 `docs/reference/plugins/settings.md` §7）。

出售门禁不产生公开能力，而是通过 Host 的 `hooks.set_intercept` 把谓词登记到逻辑 resource id `hook:UIEquip_ButtonDestroyExe`（`plugin.cpp:19,125-129`）。该资源的 hook owner 是 `core.item`；owner wrapper 在入口查询 `intercept_blocks`，命中即不调用原函数（`plugins/core.item/inventory/native_inventory_hook.cpp:1294-1310`）。布尔门契约见 `hooks/dispatch.h:64-79`：同一资源最多 1 个谓词，容量 8，`blocks` 在主线程先持锁快照再锁外调用。

## 游戏内行为

### 入口 hook

两个资源均为 `kEntryHook`、`private`、`required`、`failClosed`、`resumable=true`、`step_budget=120`、`gate=nullptr`（常驻，不可由 feature 关闭），handler 分别为两个 wrapper（`plugin.cpp:41-71`）。

| 资源 id | 符号 / VMA | wrapper |
|---|---|---|
| `hook:CHAR_CanEquipItem` | `F_CAN_EQUIP_VMA` = `0xe4eb4`（`game/symbols/game_symbols.h:622`） | `can_equip_wrapper`（`feature.cpp:211-242`） |
| `hook:CHAR_CanUnequipItem` | `F_CAN_UNEQUIP_VMA` = `0xe4e2c`（`game/symbols/game_symbols.h:626`） | `can_unequip_wrapper`（`feature.cpp:244-264`） |

符号解析表映射见 `game/access/symbols.cpp:52-54`。

### 判定重放

原版判定链（`plugins/special_equip/feature.h:22-26`）：

- `CHAR_CanEquipItem`：① `ITEM_IsRealEquip(item)` → ② `!(rec[+7] & 0x10)` → ③ `CHAR_CanChangeEquip(ch)` → ④ `ch[C_LEVEL] >= ITEM_GetEquipLevel(item)`。
- `CHAR_CanUnequipItem`：① `!(rec[+7] & 0x10)` → ② `CHAR_CanChangeEquip(ch)`。

wrapper 的执行顺序：

1. backup（原函数指针）为空时返回 `0`，不放行。
2. 原版返回非 0 时直接返回 `1`，不介入快速路径。
3. `gate_enabled()` 为假时返回 `0`（不覆写原版拒绝）。
4. `ch`/`item` 为空返回 `0`。
5. 重放依赖缺失、真装备/需求等级/角色权限/角色等级任一读取失败均返回 `0`（fail-closed）。
6. 进入纯规则：装备侧要求「真装备 ∧ 角色可更换装备 ∧ 角色等级 ≥ 装备需求等级」；脱下侧只要求「角色可更换装备」（`rules.cpp:5-15`）。
7. 仅当原版拒绝而重放允许时返回 `1`，debug 日志记 `overridden`。

规则输入使用 `EquipReplayInputs` / `SellGateInputs` 命名结构，wrapper 读取一次后同时交给纯规则与诊断日志，避免位置参数误传和重复读取。

两个 wrapper 入口都建立 `InFlightGuard`（resource id 分别为 `hook:CHAR_CanEquipItem` / `hook:CHAR_CanUnequipItem`），使 Host 在释放资源前能看到在途调用。

重放的实际取值来源：

- 「真装备」使用 `ItemFactsApi::is_real_equip(handle, out)`，与原版 ① 的 `ITEM_IsRealEquip` 是同一游戏函数；句柄经 `lookup_item` 或 `register_item(kQolItemHandleOwnerGame)` 解析。`QolItemInventoryView.equip` 是另一个判据（数量未编码），本插件不使用。
- 装备需求等级使用 `game->read_only.item.equip_level(item, out)`，与原版 ④ 的 `ITEM_GetEquipLevel` 是同一游戏函数；该函数包含 chaos 等级加成、负值归零与上限裁剪。`QolItemInventoryView.need_level`（`ITEM_GetAbilityLevel`）只读类别记录基础值，不用于重放。
- 角色等级按 u8 零扩展读取，比较为 32 位有符号 `>=`（`rules.cpp:9-10`）。

bit4 的读取与语义由 `game/access/item_class.cpp:11-26` 收口：ITEMCLASS GOT 双层解引用、按 category 乘 stride、零指针/零 stride 返回空，bit4 = `(rec[7] >> 4) & 1`。`game->read_only.item.class_no_equip_bit` 只用于 `log_reject` 的诊断输出（`feature.cpp:119-126`）。

### 出售门禁

谓词 `sell_gate_should_block`（`feature.cpp:266-286`）：

1. `g_gate` 关闭时返回 0（插件未启用，谓词常驻但空转）。
2. `game_host`、`menu_character`、`can_change_equip`、`equip_desc_role_type` 任一不可用时返回 1（拦截）。
3. 读取菜单角色失败或句柄为空、`can_change_equip` 读取失败返回 1。
4. 读取 `desc_type` 失败返回 1。
5. 交给 `sell_gate_decides_block`（`rules.cpp:17-24`）：`desc_type < 0` 或 `> 2`（未知）或角色/原语缺失 → 拦截；`desc_type` 为 1/2（背包、扩展袋等非装备详情）→ 放行；`desc_type == 0`（装备详情）→ 按 `CHAR_CanChangeEquip` 的否定拦截。

门禁**只使用 `desc_type` 与菜单角色的装备权限**，不读取当前物品的类别、`bit4` 或其它属性。因此菜单角色为特殊 NPC 时，其装备详情中的**所有**装备都会在出售前被拦截，而不区分是否为专属位物品；背包/扩展袋中的物品不受影响。

`CHAR_CanChangeEquip` 的语义为 `([ch+0x352] >= 0) && !CHAR_IsSpecialNPC(ch)`（`feature.h:38-40`）。因此售出拦截与穿脱拦截共用同一角色判定，插件不自行判断 NPC 类型。

### 行为边界

- 放行不改变「装备在谁身上」：非特殊 NPC、主角可穿脱 26 条带 bit4 的专属位装备（cat 485-506、785-787、948，`feature.h:12-17`）。
- 特殊 NPC 的原生装备仍不可脱下、不可穿上。
- 特殊 NPC 装备详情中的出售会被拦截（范围见上节）；非特殊 NPC 与主角的装备详情出售不受影响。
- 背包与扩展袋中的物品出售不经过本插件的门禁。
- 插件不写 ITEMDATABASE：wrapper 对游戏状态只做纯读（`feature.h:28-31`）。
- 不 hook `CHAR_CanChangeEquip`：该函数拿不到 item，无法把放行限定到具体装备，且它是必须保留的检查（`feature.h:32-36`）。

## 使用的 Host SDK 与 Game 面

- `ctx.sdk->capabilities.require`：`plugin_install` 每次重新 require 两项能力，并校验 `item.facts` 的 API 尺寸（`plugin.cpp:88-113`）。运行时 wrapper/MakeItem 路径不缓存接口指针，直接使用全局 `g_item_facts`/`g_character_facts`（`feature.cpp:24-25,207-209`）。
- `item.facts`：重放读取 `is_real_equip`；`log_reject` 诊断读取 `category`（不再使用 `view`）。`character.facts`：`type`，仅用于 `log_reject` 诊断；未解析时不记录角色类型，其余功能继续。
- `ctx.sdk->item_handle`：`lookup_item` 失败时以 `kQolItemHandleOwnerGame` 调 `register_item`，再用返回句柄调用 `is_real_equip` / `category`。
- `QolCharacterExtrasPort::read`：`can_change_equip`（`feature.cpp:228,256,278`）、`level`（`feature.cpp:229`）、`merc_slot`（诊断，`feature.cpp:104`）、`menu_character`（`feature.cpp:277`）。
- `QolGameHost::read_only.item`：`equip_level`（重放 ④，`feature.cpp`）、`class_no_equip_bit`（诊断，`feature.cpp:119-126`）、`equip_desc_role_type`（出售门禁，`feature.cpp:282`）。
- `ctx.sdk->log`：`write`、`debug_enabled`（`feature.cpp:54-74`）；`ctx.sdk->hooks`：`set_intercept` / `clear_intercept`（`plugin.cpp:125-129,140-142`）。

## 职责边界

- 插件负责：hook wrapper、按契约读取 item/character 事实、纯规则重放、出售谓词登记与注销、诊断日志。
- `game/access/item_class`：ITEMDATABASE/GOT 表读取与 bit4 语义；注释标记为纯读，可由游戏线程与 HTTP 线程进入（`game/access/item_class.h:7-20`）。
- `core.item`：持有 `UIEquip_ButtonDestroyExe` 的原生 hook/wrapper，查询布尔门决定是否调用原函数；R-55 预演标记也在此实现（`native_inventory_hook.cpp:1288-1310`）。
- `custom_recipe`：自己实现特殊装备类别谓词并注入配方匹配，使用同一组 26 条类别，但不调用本插件 API，也不依赖其呈现状态（`plugins/custom_recipe/feature.cpp:370,644,722`；`plugins/custom_recipe/catalog.h:82,143`）。
- 本插件不拥有角色判定、装备数据写入口，也不拥有出售按钮的 hook。

## 生命周期与线程模型

- `plugin_install`：写入 `g_sdk`/`g_game`，校验 `PluginContext` 尺寸、SDK major、`ports` 数量与指针；获取 `item.facts`（必需）与 `character.facts`（可选，失败仅 WARN）；`deps_ready()` 通过后 `set_gate(true)` 并登记出售谓词。出售谓词登记失败被视为功能不变量失效：记 ERROR、清理运行时全局并返回非 0，插件不安装资源（`plugin.cpp:73-140`）。
- `deps_ready()` 要求 `item.facts.is_real_equip`、`read_only.item.equip_level`、`character_extras().read.can_change_equip`、`menu_character` 与 `game_host` 均可用；缺失时安装返回非 0，宿主不安装资源并置 `failed`。
- `plugin_deactivate`：`set_gate(false)`、清空事实 API 指针与 port、`clear_intercept`（`plugin.cpp:133-143`）。
- `plugin_on_enabled` / `plugin_on_disabled`：只切换 `g_gate`，不做资源操作（`plugin.cpp:145-154`）。
- `plugin_fill_status`：`enabled = gate_enabled()`，`error = nullptr`（`plugin.cpp:156-159`）。
- `on_resource_ready`：按 resource id 保存 backup 到 `std::atomic<void*>`；未知 id 记一次告警后返回 0（`feature.cpp:192-205`）。
- 线程模型：两个 wrapper 只调用纯读游戏函数（`feature.h:51-52`）。但 item 句柄的 `lookup_item`/`register_item` 要求主线程（`game/access/item_handle_registry.cpp:34-36,72-76,92`）；非主线程进入时句柄解析失败并返回原版拒绝，wrapper 因此保持原版拒绝（`feature.cpp:43-51,226-231`）。当前仓库内未发现经 HTTP 线程调用这两个入口的调用点。
- 事实 API 指针与 port 指针为普通全局变量，门禁、backup、拒因日志预算为 atomic。`log_reject` 先判 debug 是否开启，再消耗 120 次预算并读取诊断事实；debug 关闭时不产生事实读取与日志（`feature.cpp:104-110`）。

## 文件地图

| 文件 | 职责 |
|---|---|
| `plugins/special_equip/plugin.cpp` | descriptor、requires/resources/ports/features、install/deactivate/启停/status、出售谓词登记 |
| `plugins/special_equip/feature.cpp` | 两个入口 wrapper、事实读取、fail-closed 判定、出售谓词、诊断日志 |
| `plugins/special_equip/feature.h` | 业务层声明与原版判定链/取舍说明 |
| `plugins/special_equip/rules.cpp` / `rules.h` | 纯规则：装备重放、脱下重放、出售门禁决策 |
| `plugins/special_equip/CMakeLists.txt` | `qol_plugin_special_equip` 静态库 |

## 测试

Host target `special_equip_rules_tests`（`tests/CMakeLists.txt:1033-1043`）只链接 `rules.cpp` 与 `tests/test_special_equip_rules.cpp`：

- 装备重放：非真装备、角色无权限、等级低于/等于/高于需求、0 级需求、依赖缺失占位。
- 脱下重放：权限有/无。
- 出售门禁：非装备详情（2/1）、未就绪（-1）、角色缺失、原语未解析、可动装备、特殊 NPC。

未覆盖：wrapper 本体、真实 SDK/port 调用、hook 安装与 backup、ITEMDATABASE/GOT 读取、`set_intercept`/`clear_intercept` 生命周期、线程行为。
