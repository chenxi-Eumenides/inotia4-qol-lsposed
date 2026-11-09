# `extension_bag` 插件设计

> **2026-11 库存 hook/刷新面迁移**
>
> 自 `core.item` 迁入并由本插件持有：
> - 5 条装备页 UI hook：`hook:UIEquip_ButtonEquipExe`、`hook:UIEquip_ButtonUnequipExe`、`hook:UIEquip_OKConfirmUseItem`、`hook:UIEquip_RefreshItemArea`、`hook:UIEquip_EquipControlEventProc`（声明与 backup 交付在 `assembly/claims.cpp` + `ui/equip_hooks.cpp`）。
> - `ptrslot:MOVE_MERGE_GOT` 资源与 `QOL_PORT_MOVE_MERGE` 端口（`core/move_merge_hook.cpp`）。
> - `move_merge.drag_merge` feature（「拖动合并」，BasicOptimization、默认开启；`QOL_PORT_MOVE_MERGE` 白名单允许 `core.item` 或 `extension_bag`）。
> - `inventory_refresh` capability（原版库存区域刷新 trampoline）：本插件是 owner，`call_original` 直调 `RefreshItemArea` 的 backup 槽；`core.item` 等消费者经 capability 请求，缺失时其刷新退化为 no-op。`update_shortcut_category` 由 Host 装配期注入的原版 `PLAYER_UpdateShortcut` 承担。
>
> 原 `move_merge.control` 提供面随迁移删除；`inventory.bag_source` 注册面由 `core.inventory` 提供。

> **2026-10 数据面迁移（`inventory.bag_source` 1.1）**
>
> `extension_bag` 不再提供 `extension_bag` / `inventory.merge_projection` 能力、HTTP 路由、中立 provider 或全库存 move。
> 数据面只注册 `QolBagSource` 1.1（space=2），提供
> `count/info/item_at/for_each_item/export_item/import_item/remove/identify/use`。
> 原版/改版袋读写、跨空间移动、出售、入库、装备/镶嵌编排归 `core.item`；EB 只保留扩展袋数据原语与 UI 适配。
> `bagCount` 是模块级全局配置：设置页以 choice（5–20，默认「关闭」）写入，插件 config 键 `bagCount`（0–128，0=关闭）为运行时权威值。装备页、商店与合成投影均使用
> `ControlItem_SetItem/GetItem` 和 `INVEN_GetBagSize` 读取入口虚拟化，不写 `INVEN_pItem` 或容量字
> （见 [`docs/refactor/extension-bag/display-hook.md`](../../refactor/extension-bag/display-hook.md)）。
> 袋源契约与注册细节见 [`core.item`](core.item.md) 的「`QolBagSource` 契约」一节。

## 摘要

功能插件：提供可配置数量的扩展逻辑背包（设置页 choice 5–20，默认「关闭」；配置文件 `bagCount` 0–128）、原生风格页签、投影显示与 sidecar 持久化；同时兼容 C18L 的原生追加袋布局。

- 插件 id `extension_bag`，版本 `0.1.0`（`plugin.cpp`）。
- 对外能力：`inventory_refresh` 1.2（自 `core.item` 迁入）、`inventory.item_writeback` 1.0（provider 物品被游戏侧原地改写后的权威存储回写）。数据面向 `inventory.bag_source` 注册 `QolBagSource` 1.1（space=2；注册面由 `core.inventory` 提供）；不再提供 `extension_bag` / `inventory.merge_projection` / `move_merge.control`。
- HTTP：无。扩展袋数据查询与操作经 `core.item` 的库存面提供；EB 不拥有 HTTP 路由。
- 持久化：`extensionbags.items` **v6**（v5/v4 自动迁移；`storedBagCount` 记录数据范围，active 数量来自全局配置）。
- 设置：`extension_bag.bagCount` choice（5–20，默认「关闭」）取代原「额外背包」开关；插件 config 键 `bagCount`（0–128）是运行时权威值，设置页范围只是 UI 限制。
- 线程模型：所有 `QolBagSource` 回调与 UI 入口在游戏主线程；查询由 `core.item` 快照承担。

除特别说明，路径相对 `module/app/src/main/cpp/plugins/extension_bag/`。

## 1. 分层

| 层 | 目录 | 职责 |
|---|---|---|
| 数据模型 | `model/`、`runtime/` | 扩展袋数据、catalog、sidecar 状态与物化对象；不拥有全库存事务 |
| 数据源适配 | `api/bag_source.cpp` | 经 `inventory.bag_source`（owner `core.inventory`）注册 `QolBagSource` 1.1（space=2），提供扩展袋数据原语 |
| 显示适配 | `core/item_display_hook.cpp` | 通过 item 控件与容量读取 hook 虚拟化投影显示，不写库存槽表或容量字 |
| 装备页 hook / 拖拽合并 | `ui/equip_hooks.cpp`、`core/move_merge_hook.cpp`、`core/move_merge.cpp` | 5 条装备页 UI hook、`ptrslot:MOVE_MERGE_GOT` wrapper、`move_merge.drag_merge` gate 与 `inventory_refresh` trampoline（均自 `core.item` 迁入） |
| UI 适配 | `ui/`、`runtime/` | 页签、布局、输入、装备页/商店/合成/详情页面；物品操作编排归 `core.item` |

`QolBagSource` 是 EB 唯一数据接口；provider、HTTP、旧全库存 move 与 EB 事务层均已删除。

### 核心组件

| 组件 | 文件 | 职责 |
|---|---|---|
| BagCatalog | `model/bag_catalog.h`、`bag_catalog.inc` | 扩展袋上限 8、活动数量、槽 16、类型容量 |
| `QolBagSource` | `api/bag_source.cpp` | 扩展袋数据访问、导入/导出、移除、定位与使用原语 |
| Item display hooks | `core/item_display_hook.cpp` | `ControlItem_SetItem/GetItem` 与 `INVEN_GetBagSize` 投影读取虚拟化 |
| Equip UI hooks | `ui/equip_hooks.cpp` | `UIEquip_ButtonEquipExe`/`ButtonUnequipExe`/`OKConfirmUseItem`/`RefreshItemArea`/`EquipControlEventProc` 的三态分流与 backup 交付；`RefreshItemArea` backup 槽兼作 `inventory_refresh.call_original` trampoline |
| Move merge wrapper | `core/move_merge_hook.cpp`、`core/move_merge.cpp` | `ptrslot:MOVE_MERGE_GOT` 事件分流（0x02 投影 drop / 0x04 原生合并 gate），`move_merge.drag_merge` feature 门控 |
| inventory_refresh provider | `plugin.cpp`（capability 提供） | `inventory_refresh` 1.2：`call_original` 与 `update_shortcut_category`（owner=本插件，消费者经 capability 请求） |
| Item writeback provider | `api/item_writeback.cpp` | `inventory.item_writeback` 1.0：游戏侧原地改写物化对象后，把新内容同步回 descriptor（payload/count/hash + dirty） |
| Bag state / persistence | `runtime/`、sidecar participant | 扩展袋数据状态、sidecar v6 与对象物化 |
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
kMaxExtensionBags = 128;      // 数组上限
kSlotCount = 16;              // 游戏槽上限，固定
active_extension_bags();      // 1..128，来自全局配置（State.bag_count 为运行时副本）
capacity_for_bag_type(type);  // {0,4,8,12,16}，生成表
extension_logical_bag_last(); // 6 + active - 1
kMaxRenderedExtensionBagTabs = 20; // UI 页签上限（4 列 × 5 行）
```

`State.types/capacities/items` 固定按上限分配；`bag_count` 只控制可见/可用范围，不改变内存布局；`stored_bag_count` 是 sidecar 数据范围高水位。

### 2.3 State

- `types[128]`、`capacities[128]`、`items[128][16]`、`bag_count`、`stored_bag_count`、`mode`、`original_selected`、`selected`、`inspected`、`info_bag`。
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

- `bagCount` 是模块级全局配置：设置页 choice `extension_bag.bagCount`（5–20，默认「关闭」）负责 UI；插件 config 键 `bagCount`（0–128，0=关闭）是运行时权威值，启动/启用时先读 config，缺失才回退 choice。没有 HTTP 修改入口。
- 增加袋不清零新袋数据；减少袋不删除数据、不因非空拒绝；只清理离开 active 范围的运行时对象身份。
- 数量变更在主线程安全点 apply，并同步更新 `QolBagSource` 注册状态；`selected/inspected/info_bag` 越界时归一化。
- 容量表由 `scripts/maintenance/build_bag_catalog.py` 从 `apk/static-data/json/tables/ITEMSTATICBASE.json` 生成；业务代码不得再写死 5/16/容量数组。
- 原版/C18 袋仍由游戏数据驱动；扩展袋由 sidecar 数据 + 全局数量驱动，显示时通过读取入口投影。

## 4. 持久化

- section `extensionbags.items`，版本 **v6**：写 `storedBagCount`（数据范围高水位）与 `[0,storedBagCount)` 的 types/capacities/items；active 数量不在 sidecar 中作为权威值。v5 读取时把旧 `bagCount` 当作 `storedBagCount`；v4 读取时按默认 5 袋迁移。
- `storedBagCount` 越界或非整数 → 拒绝加载，不静默截断；写入时取 `max(旧高水位, active, 有数据最高袋+1)`，clamp 1..128。
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
- 几何按每个 equip root/generation 重新扫描（≤32 子控件，排除模块锚点）；子控件列表缓存按 `root + epoch` 保存一次链表遍历，控件创建/删除/root 变化立即失效，返回前仍校验 root/child，不把缓存指针当作所有权。
- 命中：C18 原生页签 child index 只作 UI 命中，不进入扩展逻辑 ID 域。
- 高亮：C18 module view 临时把 `current_bag` 写成页签总数（无对应页签→原生无高亮），绘制后恢复 raw；不使用 18/19 作为持久哨兵。

## 7. UI 与输入

- 页签宿主：背包装备页、商店、合成器；挂载在游戏袋容器上，所有权归游戏。
- 锚定：每次完整 ensure 重新解析活容器并证明身份（proc/type/user_type/data 快照）；失效只注销记录并重建；禁用/退出时 detach 后删除。
- 子控件缓存：Host UI adapter 的 `child_count`/`child_at` 按 `root + epoch` 缓存一次链表遍历；控件创建/删除/root 变化立即失效，返回前仍校验 root/child，保持 fail-closed。
- 页签 ensure 门控：root、子控件数、inventory generation（商店/合成为可见页签数）未变时每 10 帧完整 ensure 一次；其余帧只逐个校验已挂载按钮存活，任一失效立即完整重建。
- ControlItem 投影：`control→slot` 映射按 `root/bag/capacity` 缓存，命中仍用 `valid_child_locked` 复核；控件重建后自动重扫。
- C18 袋数：`native_bag_count_locked()` 按 EB revision 缓存；`native_bag_layout()` 1 秒 TTL，避免商店/合成页每帧扫描候选袋。
- 页签控件：user type 2、data 16 字节；点击经 proc 投递，主线程消费；同一帧多击做合并。
- 页签渲染上限 `kMaxRenderedExtensionBagTabs = 20`（每列 5 个，最多 4 列，向右排列）；`active > 20` 时只创建/绘制/命中前 20 个，其余袋仍 active、数据与袋源有效但无页签入口。
- 列/行映射：`column = index / 5`、`row = index % 5`；非 C18 的 x 步进 68、y 步进 70；C18 reflow 按完整 4 列组 bounds 计算 frame 与原生列 clearance，放不下时 fail-closed。
- 输入：UI 事件由 EB 适配后交给 `core.item` 操作面；EB 不再执行跨空间移动事务。
- 详情/商店/合成页面保留 UI 适配；出售、入库、装备与镶嵌编排归 `core.item`。

## 8. 数据接口

### 8.1 `QolBagSource` 1.1

- EB 经 `inventory.bag_source`（owner `core.inventory`）注册 `QolBagSource` 1.1，space=2；回调为 `count/info/item_at/for_each_item/export_item/import_item/remove/identify/use`。
- 接口索引与错误语义见 [`core.item`](core.item.md) 的「`QolBagSource` 契约」与「状态与失败语义」两节。

### 8.2 `inventory_refresh` 1.2

- 本插件是 `inventory_refresh` 的 owner（provider 自 `core.item` 迁入）：`call_original` 经 `hook:UIEquip_RefreshItemArea` 的 backup trampoline 直调原版刷新，绕过投影窗口；消费者（`core.item` 出售结算、`core.inventory` 写后刷新等）经 capability 请求，缺失时按 no-op 降级。
- `RefreshItemArea` 同时是 EB 装备页刷新路径的入口：5 条装备页 UI hook 的 id→backup 槽映射与 trampoline 记录由 `ui/equip_hooks.cpp` 单一维护。
- `update_shortcut_category(category)` 由 Host 装配期注入的原版 `PLAYER_UpdateShortcut` 承担；按注册源中出现的类别逐个调用。

### 8.3 拖拽合并（自 `move_merge`/`core.item` 迁入）

- 资源：`ptrslot:MOVE_MERGE_GOT`（optional、fail-closed），声明表在 `core/move_merge_hook.cpp`；端口 `QOL_PORT_MOVE_MERGE`（Host 白名单允许 `core.item` 或 `extension_bag`）。
- feature：`move_merge.drag_merge`（「拖动合并」，BasicOptimization、默认开启），callback 更新合并 gate；禁用或 wrapper 未就绪时不启用自动合并。
- 行为边界：只在整个源堆可放入目标剩余容量时合并，不拆成部分堆叠；投影显示槽同步与原版刷新由本插件完成。原 `move_merge.control` 提供面已删除，可用性以 gate/wrapper 就绪状态为准。

### 8.4 装备页 UI hook（自 `core.item` 迁入）

- 5 条 hook：`hook:UIEquip_ButtonEquipExe`、`hook:UIEquip_ButtonUnequipExe`、`hook:UIEquip_OKConfirmUseItem`、`hook:UIEquip_RefreshItemArea`、`hook:UIEquip_EquipControlEventProc`；声明与 backup 交付在 `assembly/claims.cpp` 与 `ui/equip_hooks.cpp`。
- 职责：装备页物品操作三态分流（原版路径 / 投影接管 / 阻断），卸下按钮、使用确认与刷新入口都经这里进入 EB 装备页逻辑；原版调用经 backup 透传，backup 缺失时 fail-closed。

### 8.5 扩展 API（已删除）

- `ExtensionBagApi`、`extension_bag` 与 `inventory.merge_projection` 提供面已删除；旧中立 provider 也已删除。
- 库存查询与跨空间操作经 `core.item` 完成；显示投影由 EB UI hooks 适配，EB 不提供旧投影 capability。

### 8.6 HTTP（已删除）

- EB 不注册 HTTP 路由，也没有 typed command；扩展袋库存能力由 `core.item` 的库存接口提供。
- `bagCount` 由设置页 choice 或配置文件 `bagCount` 修改，不提供 HTTP 修改入口。

### 8.7 `inventory.item_writeback` 1.0（provider 物品回写）

- 提供面：`contracts/inventory_item_writeback.h`；EB 在 `plugin.cpp` 声明 provide，实现在 `api/item_writeback.cpp`。
- 语义：游戏侧代码经句柄原地改写 EB 物品的物化对象后（例如 `custom_recipe` 升阶直接写 jewel word / 原生词条），
  调用 `commit_item(handle)`。EB 把物化对象重新序列化，更新对应槽的 `descriptor.payload`、canonical count 与
  `module_object_hashes`，并在内容确实变化时标记 dirty；句柄不属于 EB 时返回成功且无操作。
- 为什么需要：物化对象是显示/游戏逻辑载体，权威存储是 sidecar descriptor；只写对象会让后续
  `export_item`/`import_item`（移动）和存档 capture 读回旧内容。已有的镶嵌/强化 `apply_stuff` 路径在
  `public/projection.cpp` 内部做同样的同步；跨插件写入必须经本能力走同一语义。
- 失败：序列化失败或槽 descriptor 无效返回 `kQolErrInternal`；调用方已完成的原地写入不回滚，只应记录日志。

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
| `active > 20` 只渲染前 20 个页签 | 第 21 个及之后的袋无法通过页签直接访问，但数据/接口仍有效 | 单独设计更多背包显示的 UI 方案 |
| 设置页 choice 只提供 5–20 | 手改配置文件 `bagCount` 到 1–4 或 21–128 时设置页显示「关闭」或非法态，但运行时按配置生效 | 如需 UI 覆盖更大范围，扩展 choice 选项 |
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
7. 游戏侧原地改写 EB 物化对象后必须调用 `inventory.item_writeback.commit_item`；只改对象不回写 descriptor 会在移动/存档后回退。

## 13. 测试与验收

- Host 与静态检查按 `docs/refactor/verification.md` 对应验收卡执行；extension_bag 不再有 HTTP 路由或 HTTP 参数校验面。
- 投影验收覆盖装备页、商店、合成及 `INVEN_GetBagSize` 容量读取；具体证据以对应验收卡为准。
- 构建：`scripts/build-debug.sh`（唯一 Debug 入口）。
- 真机：v513/v514/v520；页签挂载、reflow、点击切袋、原生竖列退出及设置页 `bagCount` 调整。
- 详细验收口径见 `docs/refactor/verification.md`；重构状态见 `docs/refactor/extension-bag/`。
