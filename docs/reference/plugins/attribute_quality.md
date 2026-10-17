# `attribute_quality` 插件参考

## 摘要

职责是查询装备/独立宝石属性范围并用共享规则计算质量百分位与等级；不聚合整件装备、不渲染 UI、不决定 craft/autosell 策略，也不写物品。

## 定位与版本

`attribute_quality` 是只读属性质量 provider，插件版本 `0.1.0`；提供 `attribute.quality` v2.0，必需 `item.facts` v3.3。当前 descriptor 声明一项 private/required/fail-closed `MATH_GetRandom` entry hook、`jni_count=0`，无 HTTP query、endpoint 或 route metadata。见 `plugins/attribute_quality/plugin.cpp:12-65,115-131`、`contracts/attribute_quality.h:14-19,92-98`、`contracts/item_facts.h:11-14`。

接口是带 `struct_size` 的三槽表：`query_item`、`query_option_context`、`evaluate`。`attribute_quality_api_size_valid` 同时检查完整到 `evaluate` 的结构尺寸和三个非空回调；不能用 v1 的 `probe_jewel/probe_option/classify/percentile` 解释当前 ABI。三槽 typedef 见 `contracts/attribute_quality.h:70-90`，API struct 见 `92-98`，size validator 见 `100-104`。

| 入口 | 输入 | 输出 |
|---|---|---|
| `query_item` | `QolItemHandle`、可选 `AttributeQualityOptions` | `QolResult`；`AttributeQualityItemResult{count, complete, reserved, attrs[32]}`。每项含 `kind, ordinal, attribute_id, option_type, value, range{min,max}, quality{percentile,level}, status`。 |
| `query_option_context` | item handle、option id、当前 display value、可选 options | `QolResult` 与一个 `AttributeQualityItem`，`kind=OptionContext`、`ordinal=UINT32_MAX`。 |
| `evaluate` | value、`AttributeRange`、可选 options | `QolResult` 与 `AttributeQuality{percentile, level}`。 |

结构和字段定义见 `contracts/attribute_quality.h:31-75`；质量 capability 是 native API，不是 HTTP/JNI 接口。

除特别说明，文中的 `plugins/...`、`contracts/...`、`game/...` 路径均相对 `module/app/src/main/cpp/`。

## 质量计算：`evaluate`

`evaluate(value, range, options, out)` 是纯数值运算，不读取 item/facts/Host，不要求插件 active、hook 或 backup 就绪。输出为 `{percentile, level}`，最高级常量为 `5`；v2 对外的质量级别是整数 `level`，旧 `AttributeQualityTier` 名称不参与 v2 结果。见 `contracts/attribute_quality.h:18-34,75-90`、`plugins/attribute_quality/rules.cpp:54-100`。

- `options == nullptr` 等价于 `allow_outside_range=0`，即百分位夹在 `0..100`。
- 非空 options 只接受 `allow_outside_range` 为 `0` 或 `1`，三个 `reserved` 字节必须全为零；否则返回 `InvalidArgument`。
- `range.max <= range.min` 固定返回 `percentile=100, level=5`。
- 正常区间按 `(value - min) * 100 / (max - min)` 计算；中间量使用 `int64_t`。C++ 有符号除法向零截断：非负值向下取整，负值向上取整。
- 默认夹取模式中，`value < min` 为 `0`、`value >= max` 为 `100`。允许范围外值时不夹百分位；若最终百分位超出 `int32_t`，返回 `InvalidArgument`。
- level 阈值：`>=100 → 5`、`>=90 → 4`、`>=75 → 3`、`>=60 → 2`、`>=30 → 1`，其余（含负百分位）为 `0`。

以下纯 `evaluate` 示例使用假设输入，不是实测游戏范围：

| 输入 | 输出 |
|---|---|
| `value=-1, range={0,3}, allow_outside_range=1` | `percentile=-33, level=0` |
| `value=1, range={0,3}, options=nullptr` | `percentile=33, level=1` |
| `value=125, range={0,100}, options=nullptr` | `percentile=100, level=5`（clamp） |
| `value=125, range={0,100}, allow_outside_range=1` | `percentile=125, level=5` |
| `value=9, range={10,10}, options=nullptr` | `percentile=100, level=5`（退化区间） |

## Item 查询

`query_item` 通过 `item.facts` 依序读取 category、jewel/equip 分类和一次 `QolItemInventoryView`；属性条目从该 view 的 `options[]` 按原序处理，不按 id 去重，也不对失败条目重读或跳过。category 与分类字段是该调用中的独立 facts 读取，view 本身只取一次。见 `plugins/attribute_quality/feature.cpp:142-165,267-332`。

一次 `query_item` 只取一次 facts view；每个受支持的 Jewel/StoredOption 范围仍分别运行 capture，不共享一次范围捕获覆盖整批条目。

| 输入对象 / 条目 | v2 行为 |
|---|---|
| 独立宝石 | `count=1`；`kind=Jewel`、`ordinal=0`，从 jewel value/type 捕获一次范围并计算质量。item view 缺 jewel 有效位时为 NotReady。 |
| 装备内储存词缀 | `count=view.option_count`，最多 32 项；按数组顺序记录 `ordinal`、id、type、value。只有存储 `type==0` 会探测范围；其他 type 原位返回 `Unsupported` status，不会挤掉后续项。ability-level 符号缺失由 adapter 返回 `-1`/NotReady；provider 缺 `HasNeedLevel` 有效位也返回 NotReady。 |
| 非宝石、非装备 | 返回 Unsupported。 |

`ItemFactsApi::view` 需要 `kQolItemInventoryHasOptionCompleteness`；缺位返回 NotReady。`complete` 只由 `kQolItemInventoryOptionsTruncated` 决定：它表示枚举是否完整，不代表每个属性探测成功。单项探测或固定表失败保留在该项 `status` 中，后续项仍处理；结构/输入/事实快照错误则由外层 `QolResult` 返回。`ordinal` 只表示本次 view 的遍历位置，不是跨查询身份，也不代表 UI 行号。view 容量和 bit7/bit8 定义见 `contracts/game_ports/item_port.h:41-93`；生产适配器在达到 32 项且 `next != nullptr` 时设置 truncated，见 `game/adapter_ports/item_port.cpp:302-318`。

## 当前词缀上下文查询

`query_option_context(item, option_id, display_value, ...)` 用调用方传来的 option id 和**当前显示值**计算 `kind=OptionContext`；`ordinal=UINT32_MAX`、`option_type=-1`。它不会在 item 的储存词缀列表中搜索或匹配该 id/value，因此重复 id 的展示行由当前调用参数区分。见 `plugins/attribute_quality/feature.cpp:334-381`。

`query_option_context` 和 `query_item` 都先校验 options，再经同步 Game Host 探测范围。范围调用的 Host 错误按原 `QolResult` 保留；上下文查询同时把错误码写入 `out.status`。插件内部对每个 stored option 都写出 `status`，不以一个失败项中止整批。

## 固定词缀表与范围探测

普通范围来自 Game Host 的同步 `option_value` / `jewel_value` 调用。装备词缀查询的 `flag` 当前为 `1`；批量存储词缀与当前 UI 上下文都用该值。固定表只在 `uses_fixed_option_table` 成功返回 `1` 时应用：返回 `0` 是明确的 non-applicable，保留捕获范围；成功但返回非 0/1 为 InvalidArgument；该查询本身出错、固定表 NoMatch 或 NotReady 都作为实际错误保留，**不会回退到捕获范围**。单项批量查询将该错误写入该条 status 并继续；上下文查询向调用方返回错误。见 `plugins/attribute_quality/feature.cpp:100-140,192-223,307-330,363-379`。

底层 `attr_fixed_option_percent_pair` 按 category + option id 搜索 `ITEMSTATICOPTBASE`；重复匹配时最后一行生效。NoMatch 返回 `kQolErrNotFound`，数据/stride 尚不可用返回 `kQolErrNotReady`。成功后以捕获到的最大值作 base，按 `[base + base*p1/100, base + base*p2/100]` 转换；`p1/p2` 是 signed 8-bit 百分比，`base <= 0` 按 1 处理。计算使用 `int64_t`，结果越界 `int32_t` 时返回 InvalidArgument。见 `game/access/attr_range_ui.cpp:21-48,51-96`、`plugins/attribute_quality/rules.cpp:102-120`。

## 捕获资源、启动与撤销

- `MATH_GetRandom` 是唯一捕获资源，声明 `private`、`required`、`fail_closed`；安装时校验 Game Host major 2、minor 至少 3 及所需 `attr_range` 函数，并要求 `item.facts` v3.3 提供 category、jewel/equip 分类和 view 成员。`QOL_PORT_ITEM` 当前 v3.3 是 item.facts/adapter 侧版本；provider descriptor 本身声明的是 `item.facts` capability，不直接声明该 port。见 `plugin.cpp:19-33,62-85`、`contracts/game_host.h:216-217`、`contracts/game_ports/ports.h:42-45`。
- 捕获状态为 thread-local RAII scope。范围探测在同线程 armed；第一次 wrapper 命中记录 `[min,max]`、返回 `max` 并 disarm，本次宿主读取中的后续随机调用透传 backup；未命中的 capture 返回 NotReady。嵌套 capture 返回 Busy，scope 退出清除状态。见 `feature.cpp:20-23,54-89,389-398`。
- Resource ready 对匹配 hook id 的空 backup 返回失败并关闭 active；非空 backup 被保存。`clear_runtime` 先关 active，再清除 facts、backup、Game Host 指针。`on_enabled` 打开门禁、`on_disabled` 关门禁；隐藏 feature `attribute_quality.provider` 默认 1，toggle callback 是 no-op，不是用户业务开关。见 `feature.cpp:248-264`、`plugin.cpp:67-113`。
- **错误边界**：API 在 active/Host/facts/backup 未就绪时拒绝范围查询；纯 `evaluate` 的数值运算不依赖这些状态，但 API function pointer 仍只在能力生命周期内可借用，不能跨 revoke 缓存。wrapper 若实际在 backup 为空时被调用会返回 `INT_MIN` sentinel；这不是游戏定义的错误值，不能据此宣称游戏调用可安全处理。正常可用性依赖 Resource ready 拒绝空 backup、required/fail-closed 资源及查询门禁。见 `feature.cpp:35-39,225-230,255-264,389-398`。

## 消费者与职责边界

| 消费者 | 依赖 | 当前调用与失败行为 |
|---|---|---|
| `autosell` | required `attribute.quality` | 查询宝石 `query_item`，只接受完整的单项 Jewel 且 status=0；不可用或探测失败保持 `jewel_percentile=-1`，不以默认 0 命中误售规则。见 `plugins/autosell/plugin.cpp:38-43,254-262`、`plugins/autosell/view.cpp:64-85`。 |
| `attribute_range_display` | required `attribute.quality`（插件审计批 2026-10-17 由 optional 改硬依赖） | 以 option id + 当前 display value 调 `query_option_context`，由本消费者将 `level` 映射成内联颜色；能力/事实错误时不插入颜色。见 `plugins/attribute_range_display/plugin.cpp:13-16`、`plugins/attribute_range_display/feature.cpp:24-34,78-112`。 |
| `custom_recipe` | optional `attribute.quality` | `query_item` 取宝石现态，`evaluate` 纯计算候选档位；能力或质量查询失败会在候选随机、宝石写入、材料数写回之前退出。成功的策略、随机候选、宝石位写入及材料列表更新由 custom_recipe 自己负责。见 `plugins/custom_recipe/plugin.cpp:233-250`、`plugins/custom_recipe/feature.cpp:171-249,614-669`。 |

provider 只提供范围事实与质量计算；不做整件装备聚合、UI 颜色/文本写入、物品写入、craft 候选策略、自动出售策略或材料策略。Game Host 读取原语只同步调用；provider 不创建后台任务，不保证后台调用 `capabilities.require` 或能力撤销期间借用指针的 lease 并发安全。消费者应按调用重新 require，并且不得缓存能力指针跨撤销。Game Host item-handle 和 fixed-table adapter 的 wrong-thread 路径返回 Blocked，见 `game/access/attr_range_ui.h:7-10`、`game/adapter_ports/game_host_port.cpp:672-724`。

## 文件地图

路径相对 `module/app/src/main/cpp/`。

| 文件 | 事实位置 |
|---|---|
| `contracts/attribute_quality.h:14-19,31-75,79-98,100-104` | capability v2.0、三槽 API、结果/选项/尺寸校验。 |
| `contracts/item_facts.h:11-14,35-65`、`contracts/game_ports/item_port.h:41-93` | item.facts 3.3 与 item view 的容量/有效位。 |
| `contracts/game_host.h:39-47,205-234` | Game Host attr_range 面与 ABI 2.4（v2.4 追加 `game_identity` 尾槽）。 |
| `plugins/attribute_quality/plugin.cpp:12-131`、`plugins/attribute_quality/feature.h:24-25` | 版本、requirements/provides/resource、Game Host 检查、lifecycle 与空 backup 拒绝说明。 |
| `plugins/attribute_quality/feature.cpp:41-398`、`feature.h:13-43` | options 校验、查询、范围捕获、固定表、错误与 wrapper。 |
| `plugins/attribute_quality/rules.h:11-35`、`rules.cpp:8-120` | 质量运算及固定表范围转换。 |
| `game/access/attr_range_ui.cpp:21-95`、`game/adapter_ports/game_host_port.cpp:672-735` | 固定表/ability 原语及 Game Host 错误、线程转发。 |
| `game/adapter_ports/item_extras_port.cpp:17-34`、`game/adapter_ports/item_port.cpp:302-318` | ability level 未就绪与 32 项完整性标记。 |
| `plugins/autosell/view.cpp:64-85` | required consumer 的 Jewel 质量条件。 |
| `plugins/attribute_range_display/feature.cpp:24-111` | optional consumer 的当前展示值上下文与颜色呈现。 |
| `plugins/custom_recipe/feature.cpp:171-249,614-669` | optional consumer 的当前宝石查询、纯候选 evaluate 及写入顺序。 |
