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

两个声明式入口 hook（`plugin.cpp:17-46`），均已声明为 `private / optional / fail_closed`，`resumable=true`，`step_budget=120`：

| 资源 id | 符号（`game/symbols/game_symbols.h:604-605`） | 职责 |
|---|---|---|
| `hook:UIDesc_AddOption` | `F_UIDESC_ADD_OPTION_VMA` = `0xb343c` | 详情选项行渲染；wrapper 非空 backup 时先透传，再尝试为当前 item 的该 option 插入内联颜色 |
| `hook:UIDesc_MakeItem` | `F_UIDESC_MAKE_ITEM_VMA` = `0xb36a0` | 详情构造入口；wrapper 把 `item`（`x0`）存入 thread-local 上下文供 AddOption 使用 |

等级 → 内联颜色码映射（`feature.cpp:35-51`）：Grey→`'G'`、White→`'W'`、Green→`'C'`、Blue→`'L'`、Purple→`'V'`、Gold→`'Y'`。

## 使用的 Host SDK 与 Game 面

- `ctx.sdk->capabilities.require`：每次调用重新 require `attribute.quality`，不缓存接口指针（`feature.cpp:23-33`）。
- `ctx.sdk->item_handle.register_item`：把当前 item 注册为 `kQolItemHandleOwnerGame` 句柄（`feature.cpp:98-103`）。
- `ctx.sdk->ui->insert_inline_color`：申请插入内联颜色（`feature.cpp:116-119`）。
- 不使用任何 Game Host 面：插件不保存 `ctx.game`。

## 消费者与职责边界

本插件不作为 provider，**无下游消费者**；级联影响面仅在依赖方向：

- required 依赖 `attribute.quality`：provider 缺失时本插件 `blocked`；provider 撤销时作为 required consumer 被级联停用（`cascade_stop`），provider 恢复后按 Host 生命周期重新激活。
- 本插件停用不影响任何其他插件（叶子）。

边界归其他 owner：属性范围与质量计算归 `attribute_quality` provider（见 `docs/reference/plugins/attribute_quality.md`）；内联颜色串的物理插入与文本缓冲右移归 Game Adapter（`host/ui_adapter.cpp:74-91`、`game/access/attr_range_ui.cpp:128-`）；item 句柄注册归 infra item-handle registry。

## 失败语义（fail-closed）

资源就绪 `on_resource_ready`（`feature.cpp:65-83`）：匹配 `kResourceIdAddOption` / `kResourceIdMakeItem` 任一，若 `backup == nullptr` → 清空对应 backup 指针、`set_active(false)`、返回非零（宿主据此按还原释放资源并置该插件 failed）；非空则保存并返回 0；未匹配 id 返回 0。语义与 `attribute_quality` provider 一致。

`uid_desc_add_option_wrapper`（`feature.cpp:86-120`）在任一条件命中时直接返回、不插入颜色：`!active`、`builder==nullptr`、`t_current_item==nullptr`、SDK/ui/api 未就绪、`register_item` 非 `Ok` 或 `slot·generation` 为 0、`query_option_context` 非 `Ok`、`kind != OptionContext`、`status != Ok`、`level < 0` 或 `level > kAttributeQualityHighestLevel`。backup 非空时先调用，保证原渲染行为透传。

`uid_desc_make_item_wrapper`（`feature.cpp:122-130`）在 `active()` 时以 thread-local 保存/恢复 `t_current_item`；不 active 时直接透传。

## 生命周期

`install`/`on_enabled` → `set_active(true)`；`deactivate`/`on_disabled` → `set_active(false)`（`plugin.cpp:48-72`）。`fill_status` 输出 `active()`（`plugin.cpp:73-76`）。

## 文件地图

| 文件 | 事实位置 |
|---|---|
| `plugins/attribute_range_display/plugin.cpp:9-15,17-46,48-72,75-103` | 版本、required 依赖声明、资源声明、lifecycle、feature、descriptor。 |
| `plugins/attribute_range_display/feature.h:9-19` | 资源 id 常量与对外函数声明。 |
| `plugins/attribute_range_display/feature.cpp:16-19,21,23-33,35-51` | 运行状态、能力 require、颜色映射。 |
| `plugins/attribute_range_display/feature.cpp:65-83,86-130` | resource ready（含空 backup 拒绝）、AddOption/MakeItem wrapper 与上下文传递。 |
| `contracts/attribute_quality.h:14-19,31-104` | 质量能力 id/版本、结果结构与 API 尺寸校验。 |
| `contracts/ui.h:435` | `QolUiAdapter::insert_inline_color` 槽。 |
| `game/access/attr_range_ui.cpp:128-`、`host/ui_adapter.cpp:74-91` | 内联颜色插入实现与广播。 |
| `game/symbols/game_symbols.h:604-605` | 两个入口 hook 的 VMA 与签名。 |
| `tests/test_attribute_range_display.cpp` | host 侧 descriptor/wrapper 透传/颜色/失败关闭用例。 |
