# `attribute_range_display` 插件参考

## 摘要

功能插件（纯消费者、叶子）：在物品详情（`UIDesc`）的属性描述行中，仅对装备**原生词条**，按 `attribute.quality` provider 提供的质量等级，把属性**数值**染成对应品质的内联颜色码；宝石词条或无法可靠判定的行不染色。不提供能力、无 HTTP 路由、不写物品、不聚合装备、不做范围或质量计算。

除特别说明，文中 `plugins/...`、`contracts/...`、`game/...`、`host/...` 路径均相对 `module/app/src/main/cpp/`。

## 定位与版本

`attribute_range_display`，插件版本 `0.1.0`（`plugins/attribute_range_display/plugin.cpp:10-11`）。descriptor：2 项 required 依赖 `attribute.quality` v2.0 与 `item.facts` v3.6（`plugin.cpp:13-18`）；`provides_count=0`，`port_count=0`，无 HTTP query、endpoint 或 route metadata（`plugin.cpp:84-107`），无 `routes.inc`。注册于 `host/assembly.cpp:35,215`。

## 公开能力与路由

无对外能力：不 `provides` 任何 capability，不注册 HTTP 路由、快照、事件或命令。

唯一用户可见 feature 为 `attribute_range_display.ranges`，版本 1，默认 `1`，分类 `kBasicOptimization`，类型 `kToggle`，名称「属性范围显示」（`plugin.cpp:75-79`）。其 `on_toggle` 回调为 no-op：运行时门禁由 Host 的 feature 布尔值 → `enable` 派生驱动生命周期（ARCHITECTURE.md §8.2），回调本身不需动作。

## 游戏内行为

两个声明式入口 hook（`plugin.cpp:17-46`），均声明为 `private / optional / fail_closed`，`resumable=true`，`step_budget=120`：

| 资源 id | 符号（`game/symbols/game_symbols.h:604-605`） | 职责 |
|---|---|---|
| `hook:UIDesc_AddOption` | `F_UIDESC_ADD_OPTION_VMA` = `0xb343c` | 详情选项行渲染；wrapper 在本行写入前后各取一次文本游标，确认本行来自装备原生词条后，再就本行 span 插入颜色 |
| `hook:UIDesc_MakeItem` | `F_UIDESC_MAKE_ITEM_VMA` = `0xb36a0` | 详情构造入口；wrapper 把 `item`（`x0`）存入 thread-local 上下文供 AddOption 使用 |

等级 → 内联颜色码映射（`feature.cpp:58-74`）：Grey→`'G'`、White→`'W'`、Green→`'C'`、Blue→`'L'`、Purple→`'V'`、Gold→`'Y'`。

### 词条来源判定与缓存

AddOption wrapper 只拿到 `(option_id, value)`，不能凭 option id 所属类别推断来源。wrapper 通过 `item.facts` 的 `ItemFactsApi::view` 读取当前 item 的 `native_options[]` / `jewel_options[]` 与完整性字段：

- `view` 必须成功，且 `kQolItemInventoryHasOptionCompleteness`、`HasNativeOptions`、`HasJewelOptions` 同时存在；`kQolItemInventoryOptionsTruncated` 置位或计数超过各自容量时不染色。
- 在 `native_options[]` 与 `jewel_options[]` 中按 `id` 和 `value` 匹配当前行；两个列表条目自带 type，若 type 与所属列表不一致则视为数据异常，不染色。
- native 命中数必须大于 0；同 `id/value` 若同时命中 jewel，则当前行无法区分是原生还是宝石 → 不染色。
- 只命中宝石、无任何匹配、非装备 item、`item.facts` 能力/API/view 失败 → 不染色。
- 重复的相同原生条目仍属于确定的原生来源，可正常染色；重复 id 的不同 value 由当前显示值区分。
- `view` 只暴露 native/jewel/enchant 三类，未知 option type 不在其中，插件**无法判定**未知来源行；因此不能声称未知 type 行一定不会染色——它只在与某条原生条目同 id/value 时才可能被误认为原生。

view 惰性读取：`UIDesc_MakeItem` 建立 thread-local 当前 item 并清空缓存，首次 AddOption 读取一次 view，同一 MakeItem 周期内后续行复用；嵌套 MakeItem 会保存并在返回时恢复外层 item 与 view 缓存。

## 使用的 Host SDK 与 Game 面

- `ctx.sdk->capabilities.require`：每次调用重新 require 能力，不缓存接口指针。`item.facts` 用于 `ItemFactsApi::view`（当前 item 的 native/jewel 分类与完整性）；`attribute.quality` 用于 `query_option_context`（当前行显示值的范围/质量）。不调用 `attribute.quality.query_item`，避免重复探测整件装备范围。
- `ctx.sdk->item_handle.register_item`：`view` 读取需要 item 句柄时，把当前 item 注册为 `kQolItemHandleOwnerGame`。
- view 缓存：首次 AddOption 惰性读取一次 `QolItemInventoryView`，句柄与 view 一起缓存在 thread-local `ItemViewCache` 中；同一 MakeItem 周期的后续行复用。
- `ctx.sdk->ui`（`QolUiAdapter`）：
  - `ui_desc_text_cursor(builder, &offset)`：读取 builder 的**写游标**相对 UIDesc 文本缓冲基址的 offset（`contracts/ui.h:551`）。
  - `insert_inline_color_span(builder, before_offset, after_offset, code)`：在 `[before, after)` 本行 span 内插入颜色码并推进游标（`contracts/ui.h:552-553`）。
  - 当前 Adapter ABI 为 1.23；该插件按接口尾部 size 与函数指针检查游标/span 槽（minor 22 起提供），不要求调用方恰好使用最新 minor（`feature.cpp:94-101`）。
- 不使用任何 Game Host 面：插件不保存 `ctx.game`。
- 说明：`QolUiAdapter::insert_inline_color`（`contracts/ui.h:437`）已标注 deprecated——它没有本行 span，无法用于行内着色；本插件不再调用。

## 消费者与职责边界

本插件不作为 provider，**无下游消费者**；级联影响面仅在依赖方向：

- required 依赖 `attribute.quality` 与 `item.facts`：任一 provider 缺失时本插件 `blocked`；provider 撤销时作为 required consumer 被级联停用（`cascade_stop`），provider 恢复后按 Host 生命周期重新激活。
- 本插件停用不影响任何其他插件（叶子）。

边界归其他 owner：属性范围与质量计算归 `attribute_quality` provider（见 `docs/reference/plugins/attribute_quality.md`）；词条来源分类归 `item.facts` provider（见 `docs/reference/plugins/core.item.md`）；本行 span 与内联颜色串的物理插入归 Game Adapter（`host/ui_adapter.cpp:93-161`、`game/access/attr_range_ui.cpp:136-171`）；item 句柄注册归 infra item-handle registry。

## 失败语义（fail-closed）

资源就绪 `on_resource_ready`（`feature.cpp:176-195`）：匹配 `kResourceIdAddOption` / `kResourceIdMakeItem` 任一，若 `backup == nullptr` → 清空对应 backup 指针、`set_active(false)`、返回非零（宿主据此按还原释放资源并置该插件 failed）；非空则保存并返回 0；未匹配 id 返回 0。语义与 `attribute_quality` provider 一致。

`uid_desc_add_option_wrapper`（`feature.cpp:197-247`）：

1. `backup` 为空直接返回；
2. 不 active 时只透传 backup；
3. active 时在调用 backup **之前**快照 `before_offset`（仅当 adapter 支持新槽）；
4. 调用 backup（游戏写入本行）；
5. 快照 `after_offset`；
6. 惰性读取或复用当前 MakeItem 周期的 `item.facts` view（句柄、完整性、截断、容量校验）；
7. 按 `native_options[]` / `jewel_options[]` 的 `id/value` 判定当前行来源为 native（规则见上一节）；失败、截断、歧义或仅宝石均返回；
8. 再请求 `query_option_context`（`kind == OptionContext`、`status == Ok`、`0 <= level <= kAttributeQualityHighestLevel`）；
9. 任一不满足即返回，**不插色**；满足则调 `insert_inline_color_span`。

`uid_desc_make_item_wrapper`（`feature.cpp:249-262`）在 `active()` 时保存外层 item 与 view 缓存，清空后以 thread-local 设置当前 item；backup 返回后恢复外层状态；不 active 时直接透传。

## 生命周期

`install`/`on_enabled` → `set_active(true)`；`deactivate`/`on_disabled` → `set_active(false)`（`plugin.cpp:48-72`）。`fill_status` 输出 `active()`（`plugin.cpp:73-76`）。

## 文件地图

| 文件 | 事实位置 |
|---|---|
| `plugins/attribute_range_display/plugin.cpp:10-18,20-49,51-75,78-107` | 版本、required 依赖（attribute.quality + item.facts）、资源声明、lifecycle、feature、descriptor。 |
| `plugins/attribute_range_display/feature.h:9-19` | 资源 id 常量与对外函数声明。 |
| `plugins/attribute_range_display/feature.cpp:18-32,34-56,58-74,76-166` | 运行状态、view 缓存、能力 require、颜色映射、view 读取与原生来源判定。 |
| `plugins/attribute_range_display/feature.cpp:176-195,197-247,249-261` | resource ready（含空 backup 拒绝）、AddOption wrapper（缓存 view + 来源判定 + 游标快照 + span 插入）、MakeItem wrapper（嵌套缓存保存/恢复）。 |
| `contracts/ui.h:17,455,547-559` | adapter minor 23、deprecated `insert_inline_color`、minor 22 游标/span 槽及 minor 23 尾槽。 |
| `contracts/attribute_quality.h:14-19,31-104`、`contracts/item_facts.h:11-14,35-73` | 质量能力与 item facts 能力 id/版本、view/结果结构与尺寸校验。 |
| `game/access/attr_range_ui.h:45-46`、`attr_range_ui.cpp:128-135` | 文本缓冲基址只读访问器。 |
| `game/access/attr_range_ui.cpp:136-171`、`host/ui_adapter.cpp:93-161` | 本行 span 插入与 0x200 边界/游标一致性校验。 |
| `game/symbols/game_symbols.h:604-607` | 两个入口 hook 的 VMA、UIDesc 文本缓冲 VMA/容量。 |
| `tests/test_attribute_range_display.cpp`、`tests/test_ui_adapter.cpp` | host 侧 descriptor/wrapper/颜色/原生来源判定（宝石、歧义、截断、缺完整性、能力/view 失败）/view 缓存与嵌套恢复/行尾游标 fixture。 |
