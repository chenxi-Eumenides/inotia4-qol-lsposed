# `attribute_range_display` 插件参考

## 摘要

功能插件（纯消费者、叶子）：在物品详情（`UIDesc`）的属性描述行中，按 `attribute.quality` provider 提供的质量等级，把属性**数值**染成对应品质的内联颜色码。不提供能力、无 HTTP 路由、不写物品、不聚合装备、不做范围或质量计算。

除特别说明，文中 `plugins/...`、`contracts/...`、`game/...`、`host/...` 路径均相对 `module/app/src/main/cpp/`。

## 定位与版本

`attribute_range_display`，插件版本 `0.1.0`（`plugins/attribute_range_display/plugin.cpp:9-10`）。descriptor：1 项 required 依赖 `attribute.quality` v2.0（`plugin.cpp:12-15`）；`provides_count=0`，`port_count=0`，无 HTTP query、endpoint 或 route metadata（`plugin.cpp:81-103`），无 `routes.inc`。注册于 `host/assembly.cpp:35,215`。

## 公开能力与路由

无对外能力：不 `provides` 任何 capability，不注册 HTTP 路由、快照、事件或命令。

唯一用户可见 feature 为 `attribute_range_display.ranges`，版本 1，默认 `1`，分类 `kBasicOptimization`，类型 `kToggle`，名称「属性范围显示」（`plugin.cpp:75-79`）。其 `on_toggle` 回调为 no-op：运行时门禁由 Host 的 feature 布尔值 → `enable` 派生驱动生命周期（ARCHITECTURE.md §8.2），回调本身不需动作。

## 游戏内行为

两个声明式入口 hook（`plugin.cpp:17-46`），均声明为 `private / optional / fail_closed`，`resumable=true`，`step_budget=120`：

| 资源 id | 符号（`game/symbols/game_symbols.h:604-605`） | 职责 |
|---|---|---|
| `hook:UIDesc_AddOption` | `F_UIDESC_ADD_OPTION_VMA` = `0xb343c` | 详情选项行渲染；wrapper 在本行写入前后各取一次文本游标，再就本行 span 插入颜色 |
| `hook:UIDesc_MakeItem` | `F_UIDESC_MAKE_ITEM_VMA` = `0xb36a0` | 详情构造入口；wrapper 把 `item`（`x0`）存入 thread-local 上下文供 AddOption 使用 |

等级 → 内联颜色码映射（`feature.cpp:35-51`）：Grey→`'G'`、White→`'W'`、Green→`'C'`、Blue→`'L'`、Purple→`'V'`、Gold→`'Y'`。

## 使用的 Host SDK 与 Game 面

- `ctx.sdk->capabilities.require`：每次调用重新 require `attribute.quality`，不缓存接口指针（`feature.cpp:23-33`）。
- `ctx.sdk->item_handle.register_item`：把当前 item 注册为 `kQolItemHandleOwnerGame` 句柄（`feature.cpp:112-117`）。
- `ctx.sdk->ui`（`QolUiAdapter`）：
  - `ui_desc_text_cursor(builder, &offset)`：读取 builder 的**写游标**相对 UIDesc 文本缓冲基址的 offset（`contracts/ui.h:533`）。
  - `insert_inline_color_span(builder, before_offset, after_offset, code)`：在 `[before, after)` 本行 span 内插入颜色码并推进游标（`contracts/ui.h:534-535`）。
  - 仅当 `ui->minor >= kUiAdapterMinor`（当前 22）且两槽非空时使用（`feature.cpp:94-97`）。
- 不使用任何 Game Host 面：插件不保存 `ctx.game`。
- 说明：`QolUiAdapter::insert_inline_color`（`contracts/ui.h:437`）已标注 deprecated——它没有本行 span，无法用于行内着色；本插件不再调用。

## 消费者与职责边界

本插件不作为 provider，**无下游消费者**；级联影响面仅在依赖方向：

- required 依赖 `attribute.quality`：provider 缺失时本插件 `blocked`；provider 撤销时作为 required consumer 被级联停用（`cascade_stop`），provider 恢复后按 Host 生命周期重新激活。
- 本插件停用不影响任何其他插件（叶子）。

边界归其他 owner：属性范围与质量计算归 `attribute_quality` provider（见 `docs/reference/plugins/attribute_quality.md`）；本行 span 与内联颜色串的物理插入归 Game Adapter（`host/ui_adapter.cpp:93-160`、`game/access/attr_range_ui.cpp:128-166`）；item 句柄注册归 infra item-handle registry。

## 失败语义（fail-closed）

资源就绪 `on_resource_ready`（`feature.cpp:65-83`）：匹配 `kResourceIdAddOption` / `kResourceIdMakeItem` 任一，若 `backup == nullptr` → 清空对应 backup 指针、`set_active(false)`、返回非零（宿主据此按还原释放资源并置该插件 failed）；非空则保存并返回 0；未匹配 id 返回 0。语义与 `attribute_quality` provider 一致。

`uid_desc_add_option_wrapper`（`feature.cpp:86-134`）：

1. `backup` 为空直接返回；
2. 不 active 时只透传 backup；
3. active 时在调用 backup **之前**快照 `before_offset`（仅当 adapter 支持新槽）；
4. 调用 backup（游戏写入本行）；
5. 快照 `after_offset`；
6. 依次校验 `t_current_item`、`register_item`、`query_option_context`（`kind == OptionContext`、`status == Ok`、`0 <= level <= kAttributeQualityHighestLevel`）；
7. 任一不满足即返回，**不插色**；满足则调 `insert_inline_color_span`。

`uid_desc_make_item_wrapper`（`feature.cpp:136-144`）在 `active()` 时以 thread-local 保存/恢复 `t_current_item`；不 active 时直接透传。

## 生命周期

`install`/`on_enabled` → `set_active(true)`；`deactivate`/`on_disabled` → `set_active(false)`（`plugin.cpp:48-72`）。`fill_status` 输出 `active()`（`plugin.cpp:73-76`）。

## 文件地图

| 文件 | 事实位置 |
|---|---|
| `plugins/attribute_range_display/plugin.cpp:9-15,17-46,48-72,75-103` | 版本、required 依赖声明、资源声明、lifecycle、feature、descriptor。 |
| `plugins/attribute_range_display/feature.h:9-19` | 资源 id 常量与对外函数声明。 |
| `plugins/attribute_range_display/feature.cpp:16-19,21,23-33,35-51` | 运行状态、能力 require、颜色映射。 |
| `plugins/attribute_range_display/feature.cpp:65-83,86-134,136-144` | resource ready（含空 backup 拒绝）、AddOption wrapper（游标快照 + span 插入）、MakeItem wrapper。 |
| `contracts/ui.h:17,435-437,529-535` | adapter minor 22、deprecated `insert_inline_color`、游标/span 槽。 |
| `contracts/attribute_quality.h:14-19,31-104` | 质量能力 id/版本、结果结构与 API 尺寸校验。 |
| `game/access/attr_range_ui.h:44-46`、`attr_range_ui.cpp:128-136` | 文本缓冲基址只读访问器。 |
| `game/access/attr_range_ui.cpp:138-166`、`host/ui_adapter.cpp:93-160` | 本行 span 插入与 0x200 边界/游标一致性校验。 |
| `game/symbols/game_symbols.h:604-607` | 两个入口 hook 的 VMA、UIDesc 文本缓冲 VMA/容量。 |
| `tests/test_attribute_range_display.cpp`、`tests/test_ui_adapter.cpp` | host 侧 descriptor/wrapper/颜色/失败门禁与行尾游标 fixture。 |
