# ADR-0006：UI 组件分层审阅记录

- 状态：已采纳（**记录，实施待排期**；用户明确「目前只记录，不改造」）
- 关联：`ARCHITECTURE.md` §2.2（`core.ui` 自我约束）/§7.1（出口面内置插件）/§7.3（Kotlin 侧镜像）、
  **U11**（ui-kit adapter 接口冻结，§12.3）、ADR-0004（拍平 `plugins/` 单层）、ADR-0005（能力契约与行为所有权）

## 背景（事实）

D1 完成后当前 UI 事实（源码位置）：

- **底层原语**已在 `game/access/`：`ui_kit.{h,cpp}`、`ui_kit_controls.inc`、`ui_kit_render.inc`、
  `ui_components.{h,cpp}`（游戏样式绘制、控件读写、命中检测等）。
- **面板各自实现**：`plugins/core.ui/panels/`（设置页 `settings.*`、容器/导航 `stack.*`）；
  `plugins/save_backup/ui/`（备份面板）；`plugins/autosell/ui/`（自动出售面板）；
  文本作用域/绘制窗口归 `plugins/text_layer`；`contracts/panels.h`、`contracts/ui_choice.h` 为既有契约碎片。
- **面板间已有重复实现**：按钮、游戏样式方框、开关/选择、滚动容器与命中检测在多个面板各写一份，
  风格一致性与维护性依赖人工同步。

## 用户审阅原文（m0620，忠实记录）

> UI最终架构审阅回复，目前只记录，不改造：
> 建议这样分层
>     game/access/ui*：封装游戏资源和底层绘制，例如游戏样式方框、文字、贴图、点击检测。
>     contracts/ui.h：定义标准组件接口和数据结构，例如 Rect、ButtonDescriptor、PanelDescriptor。
>     plugins/core.ui：实现按钮、面板、文本框、选择器、滚动容器等通用组件。
>     普通插件：组合标准组件实现自己的界面，不直接处理游戏资源和底层绘制。
> 游戏样式方框应拆成两部分：
>     游戏贴图、资源 ID、实际绘制调用放 game/access/ui*。
>     Frame、Panel 等语义组件放 plugins/core.ui。
> 第一阶段只抽当前确实复用的组件：
>     Panel
>     Button
>     Label
>     Toggle/Select
>     ScrollView
> 某种自定义控件出现两到三个使用者后，再提升到 core.ui。这样既能统一游戏风格，也不会提前造一个过大的 UI 框架。

## 决策

1. **四层职责**（自上而下）：
   - `game/access/ui*`：封装游戏资源与底层绘制（游戏样式方框、文字、贴图、点击检测）。
   - `contracts/ui.h`：定义标准组件接口与数据结构（如 `Rect`、`ButtonDescriptor`、`PanelDescriptor`）。
   - `plugins/core.ui`：实现按钮、面板、文本框、选择器、滚动容器等**通用组件**。
   - 普通插件：组合标准组件实现自己的界面，不直接处理游戏资源与底层绘制。
2. **游戏样式方框两拆**：贴图/资源 ID/实际绘制调用留 `game/access/ui*`；`Frame`/`Panel` 等**语义组件**放 `plugins/core.ui`。
3. **第一阶段组件清单**（只抽当前确实复用者）：`Panel`、`Button`、`Label`、`Toggle/Select`、`ScrollView`。
4. **提升规则**：某种自定义控件出现**两到三个使用者**后，再提升到 `core.ui`；不提前构造过大的 UI 框架。
5. **实施节点**：待排期（不随 D1/D2 顺手做）；实施前不动现有面板行为。

## 与 U11 的关系

- 原 U11 为「`core.ui` 的 ui-kit adapter 接口冻结（`contracts/` 类型 + `host` 注入函数表；adapter 背后为 `game/access/ui*`）」。
- 本 ADR 的 `contracts/ui.h` **标准组件接口**是该待冻结对象的**收敛/替代**：组件接口（描述符与数据结构）落 `contracts/ui.h`；
  `core.ui` 与 `game/access/ui*` 的依赖姿态（adapter/直连与否）在实施时明确。
- 当前过渡依赖（`core.ui` 直接依赖 `game/access/ui*`、settings 运行时 ptr-slot 的临时豁免）
  在实施前**照旧登记、不变**（见 `ledger.md` D1 延后项、`arch_exemptions.json`）。

## 后果

- 正向：统一游戏风格；消除面板间重复实现（按钮/方框/开关/滚动/命中）；以「使用者数量」为提升门限，避免过度框架；
  普通插件只组合标准组件，界面与底层资源解耦。
- 约束：`contracts/ui.h` 描述符/数据结构一经实施即需冻结与版本协商；实施未排期，**不改现有面板行为**；
  现存面板在实施前保持各自实现与过渡依赖。
