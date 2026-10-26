# `extension_bag` 插件设计

> **2026-10 数据面迁移（`inventory.bag_source` 1.1）**
>
> `extension_bag` 不再提供 `extension_bag` / `inventory.merge_projection` 能力、HTTP 路由、中立 provider 或全库存 move。
> 数据面只向 `core.item` 注册 `QolBagSource` 1.1（space=2），提供
> `count/info/item_at/for_each_item/export_item/import_item/remove/identify/use`。
> 原版/改版袋读写、跨空间移动、出售、入库、装备/镶嵌编排归 `core.item`；EB 只保留扩展袋数据原语与 UI 适配。
> `bagCount` 只由设置页修改。装备页、商店与合成投影均使用
> `ControlItem_SetItem/GetItem` 和 `INVEN_GetBagSize` 读取入口虚拟化，不写 `INVEN_pItem` 或容量字
> （见 [`docs/refactor/extension-bag/display-hook.md`](../../refactor/extension-bag/display-hook.md)）。
> 袋源契约与注册细节见 [`core.item`](core.item.md) 的「`QolBagSource` 契约」一节。

## 摘要

功能插件：提供可配置数量的扩展逻辑背包（默认 5，范围 1–8）、原生风格页签、投影显示与 sidecar 持久化；同时兼容 C18L 的原生追加袋布局。

- 插件 id `extension_bag`，版本 `0.1.0`（`plugin.cpp`）。
- 对外能力：无。数据面只向 `core.item` 注册 `QolBagSource` 1.1（space=2）；不再提供 `extension_bag` / `inventory.merge_projection`。
- HTTP：无。扩展袋数据查询与操作经 `core.item` 的库存面提供；EB 不拥有 HTTP 路由。
- 持久化：`extensionbags.items` **v5**（v4 自动迁移，默认 5 袋）。
- 设置：`bagCount` 只由设置页修改。
- 线程模型：所有 `QolBagSource` 回调与 UI 入口在游戏主线程；查询由 `core.item` 快照承担。

除特别说明，路径相对 `module/app/src/main/cpp/plugins/extension_bag/`。

## 1. 分层

| 层 | 目录 | 职责 |
|---|---|---|
| 数据模型 | `model/`、`runtime/` | 扩展袋数据、catalog、sidecar 状态与物化对象；不拥有全库存事务 |
| 数据源适配 | `api/bag_source.cpp` | 向 `core.item` 注册 `QolBagSource` 1.1（space=2），提供扩展袋数据原语 |
| 显示适配 | `core/item_display_hook.cpp` | 通过 item 控件与容量读取 hook 虚拟化投影显示，不写库存槽表或容量字 |
| UI 适配 | `ui/`、`runtime/` | 页签、布局、输入、装备页/商店/合成/详情页面；物品操作编排归 `core.item` |

`QolBagSource` 是 EB 唯一数据接口；provider、HTTP、旧全库存 move 与 EB 事务层均已删除。

### 核心组件

| 组件 | 文件 | 职责 |
|---|---|---|
| BagCatalog | `model/bag_catalog.h`、`bag_catalog.inc` | 扩展袋上限 8、活动数量、槽 16、类型容量 |
| `QolBagSource` | `api/bag_source.cpp` | 扩展袋数据访问、导入/导出、移除、定位与使用原语 |
| Item display hooks | `core/item_display_hook.cpp` | `ControlItem_SetItem/GetItem` 与 `INVEN_GetBagSize` 投影读取虚拟化 |
| BagCatalog | `model/bag_catalog.h`、`bag_catalog.inc` | 扩展袋上限 8、活动数量、槽 16、类型容量 |
| Bag state / persistence | `runtime/`、sidecar participant | 扩展袋数据状态、sidecar v5 与对象物化 |
| NativeTabLayout | `runtime/native_tab_layout.*` | C18 布局解析、reflow、命中、锚定 |
| UI adapters | `ui/`、`runtime/` | 页签、输入、投影宿主与页面事件适配 |
| Removed history | 旧 `api/provider`、`http/`、`interface command`、`transaction/`、`usecases/move` | 已删除；相应全库存/跨空间操作归 `core.item` |

## 2. 数据模型

### 2.1 编号域

- 扩展内部索引 `0..active-1`；逻辑 ID `6..6+active-1`。
- 原版袋 `0..4`；任务袋 `5` 不属于扩展袋数据源。
- C18 追加袋页签序号（运行时可达 18）是 **UI 命名空间**，只在投影层与扩展逻辑 ID 汇合。

### 2.2 袋定义

```cpp
kMaxExtensionBags = 8;        // 数组上限
kSlotCount = 16;              // 游戏槽上限，固定
active_extension_bags();      // 1..8，来自 State.bag_count
capacity_for_bag_type(type);  // {0,4,8,12,16}，生成表
extension_logical_bag_last(); // 6 + active - 1
```

`State.types/capacities/items` 固定按上限分配；`bag_count` 只控制可见/可用范围，不改变内存布局。

### 2.3 State

- `types[8]`、`capacities[8]`、`items[8][16]`、`bag_count`、`mode`、`original_selected`、`selected`、`inspected`、`info_bag`。
- `Item`：category、canonical count、payload 长度与 256B 固定缓冲。
- `mode`：Original / Module / ExitingModule；`ExitingModule` 过渡态用于原版回调收尾。
- 扩展物品对象与句柄由 EB 维护，供 `QolBagSource` 回调和 UI 投影使用；跨空间所有权转移由 `core.item` 负责。

### 2.4 两类袋的数据来源

| 袋 | 数据源 | 容量来源 | 操作落点 |
|---|---|---|---|
| 原版 0–4 | 游戏 | 游戏数据 | `core.item` 读写与操作 |
| 任务袋 5 | 游戏 | 游戏数据 | 由 `core.item` 按游戏语义处理 |
| C18 追加袋 6..N | 游戏 | 游戏数据 | `core.item` 原生袋路径；EB 只适配 UI 布局 |
| 扩展袋 6..6+active−1 | sidecar | catalog | EB `QolBagSource` 数据原语；操作编排归 `core.item` |

**物品属性与库存操作不由 EB 复制**：是否装备/宝石/强化卷/可用、售价、装备槽等事实及原版/改版袋读写、跨空间移动、出售、入库、装备/镶嵌编排归 `core.item`。

## 3. 数据驱动

- `bagCount` 是设置页设置项：随存档写入 sidecar 状态 JSON 顶层 `bagCount`；没有 HTTP 修改入口。
- 增加袋：新袋 `types=0`、`capacities=0`、items 空；减少袋：被移除袋必须为空，否则拒绝。
- 数量变更由设置页执行，并同步更新 `QolBagSource` 注册状态。
- 容量表由 `scripts/maintenance/build_bag_catalog.py` 从 `apk/static-data/json/tables/ITEMSTATICBASE.json` 生成；业务代码不得再写死 5/16/容量数组。
- 原版/C18 袋仍由游戏数据驱动；扩展袋由 sidecar 驱动，显示时通过读取入口投影。

## 4. 持久化

- section `extensionbags.items`，版本 **v5**；v4 读取时按默认 5 袋迁移并记录日志。
- `bagCount` 越界或非整数 → 拒绝加载，不静默截断。
- load：缺失清空并放行；版本不符、不可读、payload 损坏阻断会话。
- capture/complete：sidecar participant 对接 `core.save`，维护扩展袋状态与 dirty revision。
- dirty staging：每 revision 至多一次 `request_write`；失败清回 staged，安全点重试。
- 启动发布：load 完成后由 `core.item` 库存快照呈现数据源内容。

## 5. 投影与绘制

- 装备页、商店、合成投影均在原版刷新前建立显示上下文；`ControlItem_SetItem` 在第一次写入时替换为扩展物品，`INVEN_GetBagSize` 返回扩展袋容量；**不写 `INVEN_pItem` 或容量字**。
- 恢复：`restore_module_view_locked()` 在 `DisplaySubstitutionSuppressScope` 内调用原版刷新，控件一次性读回真实槽表。
- `ControlItem_GetItem` 作为读侧安全网：投影根内返回扩展物品，其他控件走原函数。
- 绘制只读：绘制 wrapper 读快照/mode，不改业务状态；临时 `current_bag` 的写入/恢复由投影控制器提供。
- 原版与 C18 原生袋物品不复制，仅投影显示；`core.item` 读原版空间看到真实槽表和原生容量。

## 6. C18L 兼容

- 能力位 bit3 `kQolTargetCapabilityC18NativeBagLayoutCandidate`：已知 C18L v507/v508/v512/v513/v514/v520 置位；未知 MD5 仅在运行期特征匹配时置位。
- 袋布局：`native_bag_layout()` 探测原生袋对象表（原版 6 袋 / C18 19 袋），`physical_item_at`、`item_ref_at`、`set_current_bag` 上界同源。
- 页签 reflow：模块列固定在原版列右侧；用完整 72×84 框判断冲突；冲突时只平移 C18 竖向组（保持 5px 间距），底部横排与其他控件不动；歧义 fail-closed。
- 几何按每个 equip root/generation 重新扫描（≤32 子控件，排除模块锚点），不跨 generation 缓存原生指针。
- 命中：C18 原生页签 child index 只作 UI 命中，不进入扩展逻辑 ID 域。
- 高亮：C18 module view 临时把 `current_bag` 写成页签总数（无对应页签→原生无高亮），绘制后恢复 raw；不使用 18/19 作为持久哨兵。

## 7. UI 与输入

- 页签宿主：背包装备页、商店、合成器；挂载在游戏袋容器上，所有权归游戏。
- 锚定：每帧重新解析活容器并证明身份（proc/type/user_type/data 快照）；失效只注销记录并重建；禁用/退出时 detach 后删除。
- 页签控件：user type 2、data 16 字节；点击经 proc 投递，主线程消费；同一帧多击做合并。
- 当前页签渲染上限 `kMaxRenderedExtensionBagTabs = 5`；`bagCount > 5` 时只渲染前 5 个，多列布局待实现。
- 输入：UI 事件由 EB 适配后交给 `core.item` 操作面；EB 不再执行跨空间移动事务。
- 详情/商店/合成页面保留 UI 适配；出售、入库、装备与镶嵌编排归 `core.item`。

## 8. 数据接口

### 8.1 `QolBagSource` 1.1

- EB 向 `core.item` 注册 `QolBagSource` 1.1，space=2；回调为 `count/info/item_at/for_each_item/export_item/import_item/remove/identify/use`。
- 接口索引与错误语义见 [`core.item`](core.item.md) 的「`QolBagSource` 契约」与「状态与失败语义」两节。

### 8.2 扩展 API（已删除）

- `ExtensionBagApi`、`extension_bag` 与 `inventory.merge_projection` 提供面已删除；旧中立 provider 也已删除。
- 库存查询与跨空间操作经 `core.item` 完成；显示投影由 EB UI hooks 适配，EB 不提供旧投影 capability。

### 8.3 HTTP（已删除）

- EB 不注册 HTTP 路由，也没有 typed command；扩展袋库存能力由 `core.item` 的库存接口提供。
- `bagCount` 只由设置页修改，不提供 HTTP 修改入口。

## 9. 线程、锁与重入

- `QolBagSource` 回调与 UI/显示 hook 在游戏主线程执行。
- 跨线程库存查询由 `core.item` 已发布的库存快照承担；EB 不提供 HTTP 查询、写命令或中立 provider。

## 10. 失败与恢复

- 跨空间移动、出售、入库、装备/镶嵌及其事务恢复由 `core.item` 负责；EB 不再维护全库存事务、move 或 transaction journal。
- `QolBagSource` 对扩展袋数据操作返回契约定义的结果；具体失败语义见 core.item 契约。
- 投影身份或布局无法确认时保持 fail-closed，不替换非投影控件的物品。

## 11. 已知限制

| 限制 | 影响 | 后续 |
|---|---|---|
| `bagCount > 5` 只渲染前 5 个页签 | 6–8 袋无法全部通过页签直接访问 | 多列页签布局 + reflow 区域加宽 |
| UI 物品操作依赖 `core.item` 操作能力 | 尚未接通的操作不能由 EB 自行执行 | 按 `core.item` 操作面接线并验收，不恢复 EB 事务 |
| C18 高亮抑制依赖“页签总数” | 依赖原生无该索引 | 新版本需复核页签计数 |

装备页、商店、合成投影 hook 及 `INVEN_GetBagSize` 容量虚拟化已实现，不写 `INVEN_pItem` 或容量字。

## 12. 关键不变量

1. 扩展逻辑 ID `6..6+active−1`；原版袋 `0..4`、任务袋 `5`，命名空间不混用。
2. 投影仅虚拟化控件与容量读取；不写 `INVEN_pItem` 或容量字。
3. EB 仅管理扩展袋数据；库存操作编排与跨空间所有权转移归 `core.item`。
4. 布局或投影来源无法确认时 fail-closed。
5. `QolBagSource` 回调与 UI 入口在主线程；跨线程查询由 `core.item` 快照承担。
6. 容量/上限/槽数只从 catalog 取，业务代码不写死。

## 13. 测试与验收

- Host 与静态检查按 `docs/refactor/verification.md` 对应验收卡执行；extension_bag 不再有 HTTP 路由或 HTTP 参数校验面。
- 投影验收覆盖装备页、商店、合成及 `INVEN_GetBagSize` 容量读取；具体证据以对应验收卡为准。
- 构建：`scripts/build-debug.sh`（唯一 Debug 入口）。
- 真机：v513/v514/v520；页签挂载、reflow、点击切袋、原生竖列退出及设置页 `bagCount` 调整。
- 详细验收口径见 `docs/refactor/verification.md`；重构状态见 `docs/refactor/extension-bag/`。
