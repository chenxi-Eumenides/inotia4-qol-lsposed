# extension_bag 实际架构（2026-10）

> 2026-10 数据源迁移已完成、操作策略迁移未完成：EB 向 `core.item` 注册 `QolBagSource` 1.1
>（space=2，`api/bag_source.cpp`），`core.item` 已拥有跨空间 journal/move 与部分原生动作。
> 但 EB 仍在 `transaction/ext_ext.cpp` 实现扩展袋间合并/交换及本地 pending/回滚，
> `api/bag_source.cpp` 仍实现 provider import/remove/use 与 use lease；商店接纳和扩展袋卸下
> 仍有目标/收纳编排。因此这不是用户目标架构已完成。`core.item` 跨空间 journal 与 EB
> provider 内部状态/pending、sidecar staging 是不同事务边界。当前契约见
> [`core.item`](../../reference/plugins/core.item.md) 的「`QolBagSource` 契约」。

本文描述重构后的实际结构、线程与数据流，以及与目标架构的差距。目标设计见
`target-architecture.md`，迁移基线见 `migration-baseline.md`。

## 1. 分层与文件

```
core.item
  库存查询、跨空间 journal/move、部分原生库存动作
       │                         ▲
        │ QolBagSource 1.1         │ 已迁移 UI 操作请求
       ▼                         │
extension_bag                    │
  api/bag_source.cpp ────────────┘
    扩展袋数据原语与 provider mutation：count/info/item_at/for_each_item/
    export_item/import_item/remove/identify/use（含 use lease）

  core/item_display_hook.cpp
    UI 显示读取虚拟化：ControlItem_SetItem/GetItem、INVEN_GetBagSize
    装备页/商店/合成投影；不写 INVEN_pItem 或容量字

  runtime/、model/、persistence/：扩展袋状态、catalog、对象物化与 sidecar
  transaction/：扩展袋间合并/交换及本地 pending/对象回滚
  ui/、runtime/：页签、布局、输入、详情/商店/合成适配与残留操作编排
```

旧 `api/provider`、`http/`、`interface command` 与旧式全库存 `move` 为历史删除项；当前
`transaction/` 仍存在，不能把它或全部 EB 操作事务称为已删除。

## 2. 线程模型

- `QolBagSource` 回调与 UI/显示 hook 在游戏主线程执行。
- `core.item` 的跨线程库存查询由其已发布快照承担；EB 不提供 HTTP 查询或写命令。
- 已迁移的 UI 事件由 EB 适配到 `core.item` 操作面；EB 仍管理部分 provider mutation 与 UI 触发的操作编排。

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
EB retained paths ──▶ transaction/ext_ext.cpp、商店接纳、扩展袋卸下
```

跨空间 move 由 `core.item` journal/use case 编排，部分原生动作也已迁移；EB 的 provider 回调仍负责
扩展袋数据 mutation/use lease，非空扩展袋间合并/交换仍走 EB 本地事务。商店接纳与扩展袋卸下
仍由 EB 选择目标/收纳位置并处理相关对象状态。`persist_state_locked()` 只代表内存 mutation
acceptance；sidecar 的 durable staging 由 `core.save` 的 `save.slot_data` capture/complete 完成。

### 3.3 原生事件

UI 适配包含事件翻译与显示生命周期，但并非全部只做 UI：

- 原版袋/任务袋与 C18 追加袋的数据及操作由 `core.item` 处理；EB 负责页签布局适配；
- C18 追加袋 child index 仅用于 UI 命中，不进入扩展逻辑 ID；
- 已迁移的扩展页签和拖放操作把交互转换为 `core.item` 请求；商店接纳与扩展袋卸下仍保留 EB 目标/收纳编排。

## 4. 数据驱动边界

| 数据 | 来源 | 驱动方式 |
|---|---|---|
| 变体（series/version/能力位） | `game_variant_table.inc` | 构建期扫描 APK 生成 |
| 扩展袋上限/活动数量 | `model/bag_catalog.inc` + `State.bag_count` | catalog 上限 + 设置页变更（默认 5） |
| 袋类型容量 | `ITEMSTATICBASE.json` → `bag_catalog.inc` | 生成表 |
| 槽数 16 | 游戏槽上限 | 常量 |
| 物品属性与跨空间库存动作 | `core.item` | EB 不复制物品事实；部分本地操作策略仍在 EB |
| 扩展袋状态 | sidecar JSON（v5，含 `bagCount`） | 数据 |
| UI 布局/显示适配 | EB UI 与 display hooks | 代码 |

不变量：

- `bagCount` 只由设置页修改；
- 扩展袋内容通过 UI 读取入口投影显示，不写 `INVEN_pItem` 或容量字；
- `core.item` 拥有跨空间 journal/move；EB provider mutation 与本地 pending/sidecar staging 是不同事务边界；
- C18 追加袋与原版袋始终以游戏数据为准，扩展袋以 sidecar 为准。

## 5. 关键组件

| 组件 | 职责 | 不负责 |
|---|---|---|
| `QolBagSource`（`api/bag_source.cpp`） | 注册扩展袋数据原语给 `core.item`；实现 import/remove/use 与 use lease | 原版袋访问、跨空间 journal/move 编排 |
| `item_display_hook`（`core/item_display_hook.cpp`） | 控件物品与容量读取虚拟化 | 库存槽表/容量字写入 |
| `bag_state_store` / persistence | 扩展袋状态、sidecar 与 revision；经 `save.slot_data` capture/complete staging | `core.item` 跨空间 journal |
| `projection_controller` | UI 投影状态与来源门禁 | 数据源注册、库存操作 |
| `native_tab_layout` | 布局解析、reflow、几何缓存、原生袋命中 | 页签交互 |
| UI adapters | 页签、输入、装备页/商店/合成/详情适配；商店接纳与扩展袋卸下仍有目标/收纳编排 | 已迁移至 `core.item` 的跨空间 journal/move |
| 历史删除项 | 旧 provider、HTTP、旧式全库存 move | 当前 EB 架构中均不存在；`transaction/` 目录仍含扩展袋间合并/交换 |

## 6. 迁移状态

| 项 | 状态 |
|---|---|
| `QolBagSource` 1.1（space=2） | 已注册；提供 count/info/item_at/for_each_item/export/import/remove/identify/use |
| 旧 provider、能力、HTTP、typed command、旧式全库存 move | 已删除 |
| 装备页/商店/合成显示虚拟化 | 已使用 `ControlItem_SetItem/GetItem` 与 `INVEN_GetBagSize`；不写槽表或容量字 |
| 扩展袋状态与 sidecar | 保留；bagCount 只由设置页修改 |
| 操作策略迁移 | 未完成：跨空间 journal/move 与部分原生动作走 `core.item`；EB 仍有扩展袋间合并/交换事务、provider import/remove/use lease，以及商店接纳和扩展袋卸下的目标/收纳编排 |

旧 provider、HTTP 与旧式全库存 move 仅用于追溯；`transaction/ext_ext.cpp` 等仍在用的路径属于当前实现事实，不代表用户最终目标已完成。

## 7. 迁移前验证记录

- Host：CTest 160/160（含 catalog、sidecar 迁移、布局、重入、命令契约）。
- 静态：`check_arch.py` 全绿（层依赖、裸地址、JNI 导出）。
- Android：Debug 构建；v520 真机 smoke：变体识别、页签安装、reflow、
  模块/原生页签切换、bagCount 1..8 边界。

以上计数是旧分层重构时的记录，不证明本次 `QolBagSource` 与显示 hook 的迁移验收。
当前验收口径见 `docs/refactor/verification.md` 与 extension-bag 对应验收卡；
未有对应证据时不得报告迁移行为验收通过。
