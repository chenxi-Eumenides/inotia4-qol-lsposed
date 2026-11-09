# `custom_recipe` 插件参考

> 本文是该插件的设计、架构、能力与职责的唯一权威文档。实现事实以本文件为准；验收状态见
> `docs/refactor/verification.md`，问题台账见 `docs/refactor/issues.md`。路径相对
> `module/app/src/main/cpp/`。

## 摘要

`custom_recipe` 是自定义合成配方与合成结果的唯一 owner：判定三格配方结果、执行扣料与产物
创建/保存、升阶（宝石与装备）、管理静态/动态配方目录，并持有 RECIPEBASE/MIXTUREBASE 注入记录的
行为所有权。界面部分（原 `craft_ui`）已并入本插件的 `ui/` 子层，不再是独立 descriptor。

- 插件 id/version：`custom_recipe` / `0.1.0`（`plugins/custom_recipe/plugin.cpp:36-37`）。
- 唯一可见开关：descriptor feature `custom_recipe.recipes`（默认关闭、Gameplay/toggle、
  priority `-1`，`plugin.cpp:350-357`）。
- 不提供 capability、不声明 HTTP handler/route、JNI count 为 0。
- 目录内不出现游戏基址或 VMA：槽位由 game 侧端口装配，`check_offsets` 的
  `offsets_plugin_base` 规则持续守护。

## 1. 职责边界

| 在本插件内 | 不在本插件内 |
|---|---|
| 两条注入配方入口（「合成」「宝石强化」）与三格匹配目录 | 原版背包/物品读写原语（`inventory_hooks`、`core.item`） |
| 三格 place/prepare/execute、确认与取消、扣料与产物保存 | 只读配方事实 `craft.facts` 与 `/api/op/craft/mix-direct`（归 `core.craft`，当前 501） |
| 升阶的 gate、候选值、need/数值写入顺序 | 属性档位与数值区间查询（`attribute.quality`，optional 依赖） |
| 动态特殊装备配方的播种、缓存与代际 | 特殊装备类别判定数据来源（`ITEMCLASSBASE` 静态结论，由 `special_equip` 共用同一组） |
| RECIPEBASE/MIXTUREBASE 注入与还原（ownership） | 文本层的 `MEMORYTEXT_GetText` hook 本体（`core.custom_ui`） |
| 配方呈现文案与提示 scope（2–11） | 通用 UI 控件/状态服务（`core.custom_ui`、`core.ui_state`） |

## 2. Descriptor 面

| 面 | 当前声明 |
|---|---|
| `provides` | 无。结果 API `result::CraftResultApi` 只在插件内部由 UI 子层调用。 |
| `requires` | `item.facts` v3.6、`craft.facts` v1.0、`save.facts` v1.1、`text_layer` v1.0、`inventory.action` v3.3、`inventory.slot` v1.0、`item.payload` v1.0、`inventory.transfer` v1.1（`plugin.cpp`）。 |
| `optional_requires` | `attribute.quality` v2.0、`inventory.bag_source` v1.2；缺失/失效只置 degraded，`attribute.quality` 只影响升阶路径，`inventory.bag_source` 使扩展袋物品的扣料回退原版类别入口。 |
| `ports` | 三条：`QOL_PORT_CUSTOM_RECIPE_WRITE` v1.5、`QOL_PORT_CRAFT_UI_WRITE` v1.1、`QOL_PORT_RECIPE_TABLE` v1.0。 |
| `resources` | 13 条：5 条核心 UI hook（required/fail-closed）+ 7 条装饰 UI hook（optional/fail-soft）+ 1 条 `own:RECIPEBASE_MIXTUREBASE`（required/fail-soft）。顺序即释放逆序，ownership 最后声明、最先释放。 |
| `jni` / HTTP | 均为 0（`plugin.cpp:359-377`）。 |
| `features` | 仅 `custom_recipe.recipes`。 |

## 3. 分层与目录

| 层 | 目录/文件 | 职责 |
|---|---|---|
| L0 纯模型与规则 | `model/rules.*`、`model/catalog.*`、`model/result.h` | 档位/区间、材料数量、三格匹配、静态与动态目录、typed 结果 POD。不触碰游戏内存。 |
| L1 状态 | `result/feature.cpp` 文件级状态 | 门禁、ownership readiness、pending/token、动态目录缓存、日志预算。 |
| L2 持久化 | 无 | 只经 `save.facts` 读取创建时间 seed，不写盘。 |
| L3 事务 | `result/feature.cpp` 执行阶段 | place/prepare/execute 的阶段化写路径；明确不回滚扣料后失败。 |
| L4 用例 | `result/feature.cpp` | `api_place/prepare/execute/material_needs/make_item/craft_gate` 等。 |
| L5 核心适配 | `table/table_runtime.cpp`、`ui/runtime.cpp` | 端口→`TableAccess` 装配、表注入入口；12 条 hook wrapper 与 backups/claims。 |
| L6 接口 | `plugin.cpp`、`table/text_table.h`、`ui/feature.*` | descriptor/生命周期、文案表、UIMix 控件与事实读写封装。 |

唯一依赖规则：只允许高层 include 低层。当前 `model/` 不 include `result/`、`table/`、`ui/`；
`result/` 不 include `ui/`，也不使用控件类型；`ui/` 与 `plugin.cpp` 向下调用 L4/L5。

## 4. 能力与契约

### 4.1 内部结果 API

`result::CraftResultApi`（`model/result.h:92-106`）共 12 个成员，由
`craft_result_api()`（`result/feature.cpp`）返回，只供本插件 UI 子层调用：

`enabled`、`three_slot_mode_active`、`is_tier_up_recipe`、`form_ui_type_for_mix_type`、
`craft_gate`、`is_jewel_item`、`place`、`prepare`、`execute`、`material_needs`、`make_item`、
`desc_should_replace`。

结果类型全部为 typed enum/POD，不返回 JSON 字符串。UI 状态经 `CraftPlaceInput`/
`CraftPrepareInput`/`CraftExecuteInput`（`model/result.h:55-90`）显式入参携带，结果层不读
UIMix 控件。

### 4.2 端口

| 端口 | 用途 | 来源 |
|---|---|---|
| `QOL_PORT_CUSTOM_RECIPE_WRITE` v1.4 | 写 stuff 需求、槽位物品、宝石字、真实闭区间随机、装备原生词条值；产物生成/入库/回滚已退役，改由 `inventory.action` 承担 | `contracts/game_ports/custom_recipe_write_port.h` |
| `QOL_PORT_CRAFT_UI_WRITE` v1.1 | UIMix 控件状态、提示/YesNo、费用归零 | `contracts/game_ports/craft_ui_write_port.h` |
| `QOL_PORT_RECIPE_TABLE` v1.0 | RECIPEBASE/MIXTUREBASE 槽位（base + 5 个字段指针） | `contracts/game_ports/recipe_table_port.h` |

表槽位契约要求全有或全无：`base == 0` 时 6 个字段全部为 0；实现位于
`game/adapter_ports/recipe_table_port.cpp`，是这些 VMA 的唯一来源。

### 4.3 与 `core.craft` 的边界

`core.craft` 继续提供只读 `craft.facts` v1.0，并持有 `/api/op/craft/mix-direct` owner
（Kotlin `OpController` 返回 501）。本插件不实现该 route、不新增 JNI、不聚合通用 craft API。

## 5. 配方目录

### 5.1 注入入口

`model/catalog.cpp:54-65` 的 `kCatalog` 有两条，均挂在宝石强化页（group 3 = `1<<3`）：

| 入口 | Kind | 形式 | 说明 |
|---|---|---|---|
| 「合成」（label wordId 35216，记录下标 69） | `kThreeSlotCraft` | `kMultiInputCreate`（type 1） | 三格隐式配方；材料显示镜像原版 record 12，费用由 hook 强制 0。三同类档位宝石命中规则 3-6 时**委托原版 `UIMix_StartMix` 执行**（见 §6.3 委托分支），其余规则走插件管线。 |
| 「升阶」（label wordId 35291，记录下标 70） | `kTierUp` | `kTargetAndCreate`（type 3） | 目标为宝石或真实装备；材料数量 = `max(0, 品质档位 + 属性等阶 − F)`，`F = floor((等级 − 25) × 8 / 70)`；品质档位宝石取类别（28..32 → 0..4）、装备取稀有度（0..4 → 0..4），属性等阶宝石取 `quality.level`、装备取可升阶词条最低 `level`；25 级起最低组合 0 瓶、95 级起全部组合 0 瓶。 |

注入记录布局 12 字节（RECIPEBASE）与 3 字节（MIXTUREBASE），由 `table/table.cpp` 的纯构造器
生成；组位只保留模块自己的位，停用时还原原版段组位。

### 5.2 静态三格规则（8 条，表序即优先级）

`model/catalog.cpp:86-111`；匹配键是物品类别（本项目内类别 == itemId）。

| # | 规则 | 匹配语义 |
|---|---|---|
| 1 | 皮革 + 空槽 + 魔法衣料 | 有序；产物大背包 |
| 2 | 小恢复药水 ×2 + 低级武器强化卷轴 | 无序；产物顶级宝石 |
| 3 | 低级宝石 ×3 | 无序；产物中级宝石；**委托原版配方 12** |
| 4 | 中级宝石 ×3 | 无序；产物高级宝石；**委托原版配方 13** |
| 5 | 高级宝石 ×3 | 无序；产物顶级宝石；**委托原版配方 14** |
| 6 | 顶级宝石 ×3 | 无序；产物混沌宝石；**委托原版配方 15** |
| 7 | 任意宝石 + 混沌武器卷轴 + 混沌防具卷轴 | 有序；产物为第 1 格宝石本身，数值 ×1200/1000（向上取整），保留随机等级与属性类型 |
| 8 | 两件同类特殊装备 + 元气恢复药水 | 有序且槽 0/2 同类；产物同类别新建，孔总数与剩余强化次数拉满 |

通配符：`kAnyJewelSlot`（任意宝石 28–32）、`kAnySpecialEquipSlot`（26 类特殊装备，谓词由
调用方注入；谓词为空时该通配不命中）。

### 5.3 动态特殊装备配方

- 26 个特殊装备类别（`model/catalog.h:165-168`）各生成一条三格有序配方；材料从 14 条材料池
  （`catalog.h:157-160`，含一个空槽候选）抽取且三者互不相同。
- 每次存档就绪（事件状态 `kEventStateSaveReady` 的 ready 发布/快照）：经 `save.facts` v1.1 读取当前槽 `slot_times` 的**角色创建时间**作
  seed（`result/feature.cpp:455-502`）；自包含 SplitMix64 PRNG 按类别派生独立流，不使用共享
  游戏 RNG。同 seed + 同类别 + 同代码版本生成完全相同目录。
- 候选若命中静态配方或已接受的动态配方，只重抽当前类别，不改变其它类别结果。
- 进程内按 `std::map<creation_time, std::vector<ThreeSlotRecipe>>` 缓存成功目录直到进程结束：
  无淘汰、不写磁盘；cache hit 不调用生成器，但仍发布完整动态表、推进 generation 并使 pending
  失效（`model/catalog.cpp:342-396`、`399-413`）。
- 创建时间为 0、槽位无效、能力读取失败或生成失败 → 清空动态表并记 WARN，不发布旧表。
- 匹配顺序：**静态表优先，其后动态表**；容量上限 64（`catalog.h:139`）。

## 6. 三格合成流程

`place`（`result/feature.cpp:653`）→ `prepare`（`:691`）→ UI 确认弹窗 → `execute`（`:750`）。

### 6.1 放料

| 结果 | 触发 |
|---|---|
| `kNotMine` | 输入为空或插件未启用（三格模式判定在 UI 子层） |
| `kPlaced` | 有效写入；**无效槽/无效物品也返回 `kPlaced`（无事发生）** |
| `kAlreadyPlaced` | 重复不可堆叠对象 |
| `kNotEnoughHeld` | 可堆叠类别持有数不足 |
| `kWriteFailed` | 槽位写入失败 |

`ui/feature.h` 的 `craft_place_disposition` 把结果映射为推进选格/提示，不自行改状态。

### 6.2 prepare

1. 先作废旧 pending；
2. 按静态→动态表匹配；真正未命中 → `kNoMatch`；
3. 命中后复核每格 live identity 与类别，通过才写 pending（recipe、items、categories、
   generation、token）→ `kConfirmPending`；
4. 已匹配但读取槽位身份/类别失败（`stuff_item` 错误、类别错误、传入类别与槽内不符、声称空槽
   却有对象）→ `kUnavailable`，与 `kNoMatch` 区分。

槽位读取返回的 handle 仅当 `slot == 0 && generation == 0` 才算合法空槽；半无效 handle 视为
畸形身份，返回 `kQolErrInvalidArgument` 且不进入配方判定。

### 6.3 execute 阶段

| 阶段 | 失败结果 | 是否已扣料 |
|---|---|---|
| 1 precheck：pending/token/generation、live handle/类别/可堆叠/持有数、扣料入口就绪 | `kStaleSlots` / `kPrecheckFailed` / `kNotEnoughHeld` | 否 |
| 1.5 委托判定（在三同类档位宝石命中规则 3-6 时，位于端口就绪检查之前）：`delegate_mix_type_for` 推导原版配方 `mixType = product − 17`（RECIPEBASE 12..15）→ 返回 `kOkDelegated`，UI 层经 `craft_ui_write_port` v1.2 `delegate_start_mix` 调 `MIXSYSTEM_GetCost` 校验配方合法并按原版口径重算 `[g_uimix+0xf8]` 费用，写 `[g_uimix+0x48]` 后调原版 `UIMix_StartMix`；委托期间 `MIXSYSTEM_MakeItem` hook 旁路到原版函数（原版 type1 传入的 out 槽是未初始化栈内存，垃圾非空指针会误入升阶分支）。产物词条（含大修 20260810 定向合成、c18 补掷、原版随机）、扣料、清格、刷新、音效、弹窗全部按该变体原版行为执行；费用按各变体口径——变体不扣钱则重算的费用不会被扣。StartMix 全程只读 `[+0x48]` 且不还原，委托返回后恢复调用前的模块「合成」记录并重算 stuffList/费用，面板保持可继续放料；否则会停在原版宝石配方（12..15），后续放料按原版校验、无法填入其他宝石。端口缺失或配方非法时提示不可用、材料未扣 | `kOkDelegated`（委托失败在 UI 层收尾） | 否（原版链自行扣料） |
| 2 产物准备：按配方 `product_origin` 分支——`kNewItem` 走 `inventory.action.create`（perfect 不可用时以 flags=0 回退普通创建）并改写；`kUpdateFirstItem` 在单件时把第 1 格原物品作为产物，原地 `item.payload.update` 并提交 descriptor，不创建/不 place/不消耗第 1 格；堆叠时回退为 `kNewItem` 执行 | `kProductPrepareFailed` | 否 |
| 3 扣料：按填入槽 `inventory.action.discard` 扣该槽物品 1 个单位（句柄经 `inventory.slot.view` 定位，扩展袋物品经 `inventory.bag_source` 定位）；不可定位时可堆叠回退原版 `remove_item_data`（R-56 扩展袋补扣）、不可堆叠回退按句柄 `remove_item` + 数量前后差观察 | `kMaterialRemoveFailed` / `kMaterialObservationFailed` | **是，可能** |
| 4 保存：`inventory.action.place`（`ref={原生空间,-1}`、`slot=-1`）走 `INVEN_SaveItem` 自动找槽与合并（H-13），返回 `kOk` 即保存成功 | `kProductSaveFailed` | 是 |

阶段 1.5 仅命中规则 3-6（三格同为类别 28..31 且产物 = 高一档）；其余规则不进委托分支。委托
路径不做产物准备/扣料/保存（阶段 2-4 全部跳过），扩展袋扣料兼容由原版 `INVEN_RemoveItem` 的
H-05 补扣承担。

前置校验全部在任何扣料之前；产物在扣料前准备、失败即丢弃。扣料后的失败**不回滚**，提示
「合成失败，材料可能已扣，请检查背包。」；`inventory.action.place` 返回 `kOk` 后 handle 归
原生库存，代码不 free 或回读该 handle；自动保存失败时对象仍为 detached，但失败路径保持与
迁移前一致的「不承诺清理」。

### 6.4 确认与取消

- `kConfirmPending` 时 `craft_show_yesno` 必须提供非空 `ok` 与 `cancel`（任一为空即拒绝创建）。
- 取消只调用 `cancel_pending_confirmation()`：作废 pending/token，不写物品、不随机、不扣料、
  不保存、不清空已填格（`result/feature.cpp:1216`）。
- 弹窗创建失败不静默：作废 pending，提示「合成暂不可用，材料未扣。」并保留已填格。
- 提示按错误分类各走独立 scope（见 §10）：未命中 8、上下文不可用/弹窗失败 9、格子或代际变化
  10、放料写入失败 11；真正库存不足仍走原版材料不足提示（94 原文）。

## 7. 升阶

目标可以是宝石或真实装备（`item.facts.is_real_equip` 为 1）。两者共用同一套档位/区间判定，
区别只在「被提升的数值」从哪里取、写回哪里。

- gate（`api_craft_gate`）返回 `CraftGate`：
  - 物品事实或 quality 查询失败、item word 与 facts 不一致、装备 view 缺失/截断 → `kUnavailable`；
  - 既非宝石也非真实装备 → `kNotJewel`；
  - 宝石已金档 → `kAlreadyGold`；
  - 装备没有原生词条，或所有原生词条都已满档 → `kNoUpgradableAffix`；
  - 其余 → `kPass`。
  - UI 把 `kUnavailable` 显示为「功能不可用」、`kNotJewel` 显示为「只有宝石或装备才可以。」、
    `kNoUpgradableAffix` 显示为「该装备没有可升阶的词条。」；这些只阻断升阶路径，不停用三格配方。
- 宝石候选值：以 `AttributeRange` 调 `evaluate` 搜索当前档之上首个有代表值的区间，再在闭区间内用
  调用方 `rand_inclusive` 取样；已是 Gold → `kAlreadyGold`，区间退化 → `kDegenerate`。
- 装备候选值：读取 `item.facts.view` 的 `native_options[]`（option node type 0）。逐条用
  `attribute.quality.query_option_context(item, id, value)` 取当前档位与闭区间；只有
  status 成功、level 0..4、`max > min` 的词条进入候选，再用同一个 typed 闭区间随机源抽一条。
  宝石词条（`jewel_options[]` / type 1）与 `enchant_options[]`（附魔）不参与候选，也不会被写。
- 新值：与宝石相同，`compute_tier_up_value` 在下一档闭区间内随机取值。
- 材料消耗输入 = 品质档位 + 属性等阶：品质档位宝石由 `jewel_quality_tier_from_category`
  （类别 28..32 → 0..4）给出、装备由 `equipment_quality_tier_from_rarity`（稀有度 0..4 → 0..4）
  给出；属性等阶宝石取 `quality.level`（0%→0、30%→1、60%→2、75%→3、90%→4，金档不可升阶）、
  装备取全部可升阶词条（type 0、status ok、level 0..4、`max > min`）的最低 `level`。
  数量 = `max(0, 品质档位 + 属性等阶 − F)`，`F = floor((等级 − 25) × 8 / 70)`：
  25 级时 F=0（最低组合免费）、95 级时 F=8（最高组合免费）。
  面板显示（`api_material_needs`）经 `tier_up_cost_inputs_for_item` 取输入；执行
  （`api_make_item`）从已查询的 `quality` / 候选词条取同一语义的输入，保证两边口径一致。
- `api_make_item` 的提交顺序：
  - 宝石：quality 查询 → 与 item word 交叉核对 → 材料需求前置校验 → 计算候选（随机）→
    **先提交 need 写入** → `item.payload.update(source=item, kJewelWord)` 原地更新对象并生成 payload
    → `inventory.transfer.replace_item` 提交 provider descriptor。
  - 装备：view 校验 → 候选随机 → 升阶计算 → int16 范围校验 → 材料需求前置校验 →
    **先提交 need 写入** → `item.payload.update(source=item, kNativeOptionValue)` 原地更新对象并生成
    payload → `inventory.transfer.replace_item` 提交 provider descriptor。
  - need 失败直接返回且不写数值；写值失败时 need 已写，不回滚；失败时已写入的展示值允许残留，
    不等价于强原子。
- 随机源必须是真实游戏闭区间随机：UI 侧直接调用 write port ABI 1.5 的 `random_inclusive`，
  不恒返回下界、不静默 clamp。
- 装备写回只改第 `native_index` 条 type 0 节点的 int16 存储值，不重建 option 链表、不改
  宝石词条、不改附魔字段。
- 三格产物的对象来源由 `ThreeSlotRecipe::product_origin` **逐条配方声明**，不由 `ProductMode` 推断：
  - `kNewItem`：先 `inventory.action.create` 新产物，再 `item.payload.update(source=product)` 原地配置；
  - `kUpdateFirstItem`：产物就是第 1 格原物品，原地 `item.payload.update(source=items[0])` +
    `inventory.transfer.replace_item`；单件时该格不扣料、不 place；堆叠时回退为 `kNewItem` 执行
    （扣 1 件、创建新产物），避免一次升级整堆。
  - 当前第 7 条（宝石 + 混沌卷轴）声明为 `kUpdateFirstItem`；其余 7 条与动态配方为 `kNewItem`。
    新增/修改配方时必须显式选择来源。
- `inventory.transfer.replace_item` 对原版袋只确认物品仍在槽内；对扩展袋更新 descriptor/hash/
  dirty/refresh。对象身份保持不变，craft UI/UseStuff 的原生指针继续有效。

## 8. 状态与线程

### 8.1 状态清单

| 状态 | 位置 | 类别 |
|---|---|---|
| `g_custom_recipe_enabled`（atomic） | `result/feature.cpp:121` | 业务门禁 |
| `g_ownership_ready` | `:122` | 生命周期/资源 |
| `g_dynamic_recipe_cache` | `:123` | 进程级缓存 |
| pending recipe/items/categories/generation/token/valid | `:364-371` | 待确认业务状态 |
| 动态目录表 + generation | `model/catalog.cpp:138-145` | 业务目录 |
| `g_state` / `g_dedupe` | `table/table_runtime.cpp` | 表注入状态机与日志去重 |
| TLS 文本 scope | `ui/feature.cpp` | UI 渲染控制状态 |
| hook backups、绘制槽位（atomic） | `ui/runtime.cpp:53-68` | UI/hook 控制状态 |

### 8.2 线程与重入

- 全部业务状态只在游戏主线程读写；注释显式声明（`model/catalog.cpp:138-141`、
  `result/feature.cpp:363`）。
- 无业务互斥量、无 `try_lock`；仅有 6 个 `std::atomic`：门禁、两处日志预算、绘制标志各 1，另有 2 个“线程告警只报一次”去重标志。
- 业务写跨线程入口只有两个 typed command：`write_stuff_need` 与 `set_stuff_item` 经
  `commands.post_typed` 入队（`ui/feature.cpp:205-219`），主线程 handler 校验 `enabled()`
  后执行。其余同步写端口调用只做 UI 表现（`set_ui_type`、`show_text`、`show_yesno` 等）。
- pending 在 `api_execute` 入口即被复制并清除 valid/token，后续阶段调用写端口时不再持有
  pending；动态 generation 与 token 共同防止确认期间目录或槽位变化。
- 主线程契约观测：`place`/`prepare`/`execute`/`make_item` 与表 `acquire`/`deactivate` 在
  `g_game->thread.is_main_thread()` 返回 0 时写一次 WARN `wrong thread entry=...`；只记录、
  不改返回值（`table/table_runtime.cpp:63`、`result/feature.cpp:645`）。

## 9. 表注入与 ownership

- 端口解析（`table/table_runtime.cpp:72`）：端口未绑定或 `base == 0` → `NotReady`；否则把 6 个
  字段映射为 `TableAccess` 交给便携核心。
- `TableAcquireStatus`（`table/table.h:114`）：`Ok / NotReady / Unsupported / Internal / Busy`。
  **只有 `NotReady` 是可自动恢复信号**；`Unsupported`/`Internal` 为固定错误；`Busy` 只表示本次
  调用未开始。
- 分类在任何模块状态改动之前完成：非法布局不得借「已有 buffer」判 OK，也不得在拒绝路径改写
  状态。书名册字节不变式在抓取组位快照之后、实际新注入之前校验。
- `ensure` 注入模块缓冲、清除模块占用组位并追加记录（`table/table.h`、`table/table.cpp`）。
- `deactivate` 还原原版段组位并把 RECIPEBASE 记录数还原为 base，但**保持游戏指针指向模块缓冲
  且不 free**，使残留面板持有的注入下标仍在缓冲界内；指针已被游戏改写时只清模块状态，绝不
  写回陈旧原表指针。
- ownership 资源 `own:RECIPEBASE_MIXTUREBASE` 为 required + fail-soft：`own_acquire` 调
  `custom_recipe_table_ensure` 并据结果置 `g_ownership_ready`，失败回滚表状态并返回原错误码
  （`result/feature.cpp:1188-1212`）；`own_release` 为 total。
- 启动期定向恢复：Host 仅在单 claim 恢复成功时以 `backup=nullptr` 回调
  `on_resource_ready(ctx, "own:RECIPEBASE_MIXTUREBASE", nullptr)`；插件只据此与 UI 核心 hook
  备份就绪重算业务门禁，不重装 hook、不改 desired/Store（`plugin.cpp:287-300`）。

## 10. 文本与作用域

- 消费者匹配键是 **MEMORYTEXT id**，不是弹窗索引：`craft_show_popup(popup_index, scope)` 仍以
  原版索引（94/97/99）调用 `show_text`，另记一条 debug 事实日志。
- 两个域在 `table/text_table.h:30-45` 显式分列：`kPopupIndex*`（94/97/99）与 `kMemoryText*`
  （35410/35413/35415）；未知索引返回 0，不猜测。
- scope 由 UI 子层本地 TLS 持有（`TextScopeGuard`），并在绘制回调内保存/恢复；仅 2/3/4 推入
  Host text scope，私有 scope 不推入。
- 映射表（`table/text_table.h:71-90`）：

| scope | 用途 |
|---|---|
| 2 | 配方按钮「升阶」（35291） |
| 3 | 面板标题「升阶」 |
| 4 | 页签：**不登记**，保持原版「宝石强化」 |
| 5 | 功能不可用（97） |
| 6 | 合成失败，材料可能已扣（94） |
| 7 | 产物准备失败，材料未扣（94） |
| 8 | 配方不匹配（94） |
| 9 | 合成暂不可用，材料未扣（94） |
| 10 | 合成状态已变化（94） |
| 11 | 放入失败，请重试（99） |
| 12 | 只有宝石或装备才可以（97） |
| 13 | 该装备没有可升阶的词条（97） |

真正的库存不足不覆盖 scope，落到原版 94 原文。

## 11. 生命周期与资源

**install**（`plugin.cpp:138-241`）

1. 校验 `sdk`/`game`/`capabilities.require`、`port_count == 3` 且三条端口的 ABI 与函数指针齐备；
2. require `text_layer`、`item.facts`（含 size 校验）、`craft.facts`、`save.facts`（含
   `current_slot` 与 v1.1 `slot_times`）；
3. 按序订阅文本消费者、注册两个 typed handler、`ui::bind`、注入 facts/write port、绑定表端口、
   注册动态配方回调；
4. 任一步失败立即幂等清理本次注册（退订、注销 handler、`ui::clear()`、解绑端口），不依赖 Host
   后帧清理；
5. install 完成时**不**打开业务门禁。

**门禁**：`plugin_on_enabled` 仅在 `ownership_ready()` 且 `ui::core_backups_ready()` 时
`set_enabled(true)`（`plugin.cpp:266-273`）；feature toggle 关闭时立即 `set_enabled(false)` 并
作废在途 token，打开时不提前开门禁。资源声明带 `gate=nullptr`，跟随插件生命周期而非 feature。

**停用顺序**：`plugin_on_disabled` 先 `set_enabled(false)`（阻止新副作用、作废 pending），
随后 Host 按资源逆序释放——ownership 先 release（`own_release` → `table_deactivate`），再释放
UI hooks。`plugin_deactivate` 再清理订阅、typed handlers、事件订阅、UI context/backups 与
facts/ports（含 `save.facts`），最后解绑表端口（`plugin.cpp:243-264`）。

**资源等级**：5 条核心 hook = required/fail-closed（任一失败整体回滚）；7 条装饰 hook =
optional/fail-soft（失败只 degraded）。`UIMix_ResetStuffItemControl` 保持 optional，且 wrapper
不受 feature 门控，因为它同时服务原版成功链（`ui/runtime.cpp:512-518`）。

## 12. 文件地图

| 子职责 | 文件 | 职责 |
|---|---|---|
| Descriptor/生命周期/接线 | `plugins/custom_recipe/plugin.cpp`、`plugin.h` | 依赖与端口校验、资源声明组装、安装事务与清理、门禁、text 消费者、typed handler。 |
| 纯规则 | `model/rules.h`、`model/rules.cpp` | 档位序数与区间搜索、材料数量、三格库存/token 判定、宝石数值缩放。 |
| 目录 | `model/catalog.h`、`model/catalog.cpp` | 两条注入 entry、8 条静态规则、26 类动态构造、匹配与进程缓存。 |
| 结果契约 | `model/result.h` | 结果枚举、Input/Output POD、`CraftResultApi`。 |
| 结果层 | `result/feature.h`、`result/feature.cpp` | place/prepare/execute/make-item、quality 消费、动态配方事件、材料需求、ownership ops、门禁与端口状态。 |
| 表纯核心 | `table/table.h`、`table/table.cpp` | 记录构造、组位快照/恢复、分类/注入/还原核心；不含平台或地址代码。 |
| 表运行时 | `table/table_runtime.cpp` | 绑定 `QolRecipeTablePort`、装配 `TableAccess`、ensure/deactivate、表日志与线程观测。 |
| 呈现文案 | `table/text_table.h` | scope 与 MEMORYTEXT id 的替换映射。 |
| UI 子层 | `ui/feature.h`、`ui/feature.cpp` | UIMix 控件/事实读写封装、放料处置映射、提示与成功收尾。 |
| UI runtime | `ui/runtime.h`、`ui/runtime.cpp` | 12 条 hook wrapper、claims/backups、bind/clear、确认/取消与错误提示、真实随机适配。 |
| 设置兼容源 | `settings_contribution.cpp` | 兼容入口不注册设置项；feature metadata 唯一来自 descriptor。 |
| 构建 | `CMakeLists.txt` | `model/`、`table/`、`result/`、`ui/` 与 `plugin.cpp`、`settings_contribution.cpp`。 |

## 13. 已知限制

- 扣料先经 `inventory.action.discard` 扣填入槽自身 1 个单位（`inventory.slot.view` 定位原生袋，
  `inventory.bag_source` 按句柄定位扩展袋物品）；填入堆已耗尽或句柄落在无法还原成袋引用的空间
  （任务袋、bag_source 缺失/无 identify 的注册源）时回退原版入口，因此仍保留 H-03
  （物理+扩展类别总数）、H-05（对象删除）、H-13（保存）、H-21（类别补扣）的扩展背包支持；
  条件是对应 hook `live`、EB capability 可用且 enabled，不限制为物理袋。
- 升阶的 Game-owner 写入限制与三格流程分开核验，仍为待核项，不作为三格拒绝 EB 的理由。
- 装备升阶路径当前只有 Host 测试与 Debug 构建证据；装备产物继承原生词条、词条值写回后的
  显示/属性重算后果未做真机验收。
- 放料对空/无效输入返回 `kPlaced` 并推进选格（历史行为，未裁决）。
- 扣料后阶段失败允许不回退（CR01 口径）；`inventory.action.place` 之后不再清理 handle。
- 动态配方缓存按创建时间永久保留到进程结束，不设淘汰；这是「重复进档配方不变」的设计代价。
- 无跨线程查询快照：本插件没有 HTTP/JNI 只读面，主线程契约由约定与日志观测保证，尚未做
  fail-closed 线程拒绝。
- 启动期表注入依赖原表 ready；26 条动态组合只存在于模块内部匹配表，不逐条注册进原版
  RECIPEBASE。

## 14. 相关文档

- 构建、打包与设备验收：`docs/guides/build-and-deploy.md`。
- 验收卡与当前欠账：`docs/refactor/verification.md`（本插件相关条目）。
- 历史静态审计与合并设计：`docs/refactor/audits/plugin-audit/custom-recipe-20261014/`。
- 架构整改方案与实施记录：`docs/refactor/custom-recipe/refactor-plan.md`。
- 插件分层通用准则：`docs/refactor/plugin-architecture-guide.md`。
