# extension_bag 实际架构（2026-10）

> 2026-10 数据面迁移已完成：EB 只向 `core.item` 注册 `QolBagSource` 1.1
>（space=2，`api/bag_source.cpp`），并保留 UI 适配。旧 provider、HTTP、全库存 move
> 与 EB 事务层均已删除；原版/改版袋操作及跨空间编排归 `core.item`。下文中的相应旧结构
> 均为历史记录，不代表当前实现。当前契约见
> [`core.item`](../../reference/plugins/core.item.md) 的「`QolBagSource` 契约」。

本文描述重构后的实际结构、线程与数据流，以及与目标架构的差距。目标设计见
`target-architecture.md`，迁移基线见 `migration-baseline.md`。

## 1. 分层与文件

```
core.item
  库存查询、原生袋读写、跨空间操作、出售/入库、装备/镶嵌编排
       │                         ▲
       │ QolBagSource 1.1         │ UI 操作请求
       ▼                         │
extension_bag                    │
  api/bag_source.cpp ────────────┘
    扩展袋数据原语：count/info/item_at/for_each_item/
    export_item/import_item/remove/identify/use

  core/item_display_hook.cpp
    UI 显示读取虚拟化：ControlItem_SetItem/GetItem、INVEN_GetBagSize
    装备页/商店/合成投影；不写 INVEN_pItem 或容量字

  runtime/、model/、persistence/：扩展袋状态、catalog、对象物化与 sidecar
  ui/、runtime/：页签、布局、输入、详情/商店/合成页面适配
```

旧 `api/provider`、`http/`、`interface command`、全库存 `move` 与 `transaction/` 为历史删除项，
不属于当前 extension_bag 分层。

## 2. 线程模型

- `QolBagSource` 回调与 UI/显示 hook 在游戏主线程执行。
- `core.item` 的跨线程库存查询由其已发布快照承担；EB 不提供 HTTP 查询或写命令。
- UI 事件由 EB 适配到 `core.item` 操作面；库存业务状态与事务恢复不由 EB 管理。

## 3. 数据流

### 3.1 扩展袋数据与显示读取

```
core.item inventory query ──▶ QolBagSource 1.1 ──▶ 扩展袋 sidecar/state
UI refresh ──▶ ControlItem_SetItem/GetItem + INVEN_GetBagSize ──▶ 显示投影
```

读取 hook 只对当前投影控件/容量入口虚拟化；物品槽表和容量字保持原生真实值。

### 3.2 写与操作编排

```
UI 控件 ──▶ extension_bag UI adapter ──▶ core.item operation
core.item ──▶ QolBagSource import_item/remove/use ──▶ 扩展袋数据
```

跨空间 move、出售、入库、装备/镶嵌及其校验、提交/恢复由 `core.item` 负责；EB 不保留旧事务流程。

### 3.3 原生事件

UI 适配只做事件翻译与显示生命周期：

- 原版袋/任务袋与 C18 追加袋的数据及操作由 `core.item` 处理；EB 负责页签布局适配；
- C18 追加袋 child index 仅用于 UI 命中，不进入扩展逻辑 ID；
- 扩展页签和拖放控件只把交互转换为 `core.item` 操作请求。

## 4. 数据驱动边界

| 数据 | 来源 | 驱动方式 |
|---|---|---|
| 变体（series/version/能力位） | `game_variant_table.inc` | 构建期扫描 APK 生成 |
| 扩展袋上限/活动数量 | `model/bag_catalog.inc` + `State.bag_count` | catalog 上限 + 设置页变更（默认 5） |
| 袋类型容量 | `ITEMSTATICBASE.json` → `bag_catalog.inc` | 生成表 |
| 槽数 16 | 游戏槽上限 | 常量 |
| 物品属性与库存动作 | `core.item` | EB 不复制物品事实或库存操作编排 |
| 扩展袋状态 | sidecar JSON（v5，含 `bagCount`） | 数据 |
| UI 布局/显示适配 | EB UI 与 display hooks | 代码 |

不变量：

- `bagCount` 只由设置页修改；
- 扩展袋内容通过 UI 读取入口投影显示，不写 `INVEN_pItem` 或容量字；
- `core.item` 拥有库存写入和操作事务；
- C18 追加袋与原版袋始终以游戏数据为准，扩展袋以 sidecar 为准。

## 5. 关键组件

| 组件 | 职责 | 不负责 |
|---|---|---|
| `QolBagSource`（`api/bag_source.cpp`） | 注册扩展袋数据原语给 `core.item` | 原版袋访问、跨空间事务 |
| `item_display_hook`（`core/item_display_hook.cpp`） | 控件物品与容量读取虚拟化 | 库存槽表/容量字写入 |
| `bag_state_store` / persistence | 扩展袋状态、sidecar 与 revision | 库存事务、HTTP/API |
| `projection_controller` | UI 投影状态与来源门禁 | 数据源注册、库存操作 |
| `native_tab_layout` | 布局解析、reflow、几何缓存、原生袋命中 | 页签交互 |
| UI adapters | 页签、输入、装备页/商店/合成/详情适配 | 物品域业务编排 |
| 历史删除项 | provider、HTTP、transaction coordinator、全库存 move | 当前 EB 架构中均不存在 |

## 6. 迁移状态

| 项 | 状态 |
|---|---|
| `QolBagSource` 1.1（space=2） | 已注册；提供 count/info/item_at/for_each_item/export/import/remove/identify/use |
| 旧 provider、能力、HTTP、typed command、全库存 move | 已删除 |
| 装备页/商店/合成显示虚拟化 | 已使用 `ControlItem_SetItem/GetItem` 与 `INVEN_GetBagSize`；不写槽表或容量字 |
| 扩展袋状态与 sidecar | 保留；bagCount 只由设置页修改 |
| UI 物品操作迁移 | 按 `core.item` 操作能力接线与验收；EB 不恢复自有事务 |

provider、HTTP 与全库存 move 的旧结构仅用于追溯；不得作为当前架构或实现计划依据。

## 7. 迁移前验证记录

- Host：CTest 160/160（含 catalog、sidecar 迁移、布局、重入、命令契约）。
- 静态：`check_arch.py` 全绿（层依赖、裸地址、JNI 导出）。
- Android：Debug 构建；v520 真机 smoke：变体识别、页签安装、reflow、
  模块/原生页签切换、bagCount 1..8 边界。

以上计数是旧分层重构时的记录，不证明本次 `QolBagSource` 与显示 hook 的迁移验收。
当前验收口径见 `docs/refactor/verification.md` 与 extension-bag 对应验收卡；
未有对应证据时不得报告迁移行为验收通过。
