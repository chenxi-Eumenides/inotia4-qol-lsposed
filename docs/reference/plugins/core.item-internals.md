# `core.item` 内部实现

本文记录 `core.item` 的目录边界、装配、快照、转移恢复与测试映射。对外能力、插件作者与 HTTP 客户端可依赖的契约见 [`core.item` 对外契约](core.item.md)。

## 目录分层与组件职责

| 层 | 职责 | 主要位置 |
|---|---|---|
| 装配与生命周期 | descriptor、端口绑定、资源 claim、启停、帧任务 | `plugins/core.item/plugin.cpp`、`runtime/lifecycle.cpp` |
| API | facts、state、袋源注册、投影回调注册表 | `api/facts.cpp`、`api/state.cpp`、`api/bag_source.cpp`、`api/projection.cpp` |
| Bags | 快照存储、原生袋扫描上限、transfer payload 结构 | `bags/snapshot_store.cpp`、`bags/native_bag_limit.h`、`bags/transfer.h` |
| Use cases | 库存读取、物品动作、出售、物品生命周期 | `usecases/read_inventory.cpp`、`item_action.cpp`、`sell_item.cpp`、`consume_item.cpp` |
| Hooks | 原生库存动作入口与快捷栏诊断 | `hooks/inventory_hooks.cpp`（装备页 5 条 UI hook 与拖拽合并 wrapper 已迁 `plugins/extension_bag/core/move_merge_hook.cpp`、`ui/equip_hooks.cpp`） |
| HTTP / shop | 查询分页、typed command、响应结果、shop 路由与快照 | `http/query_handler.cpp`、`http/write_handler.cpp`、`http/write_results.cpp`、`shop/` |

旧袋源编排、transport、transfer journal 与 model 校验层已删除，尚未重建。当前只保留原生同空间移动：由 Host `move_item` 端口交给原版代码执行；跨空间与注册源之间的移动请求返回 `item transfer unavailable`。

插件私有规则不向 Host 端口反向泄漏。原生内存解释由 game/access 与 adapter 负责；领域层通过契约和端口调用，不跨线程持有游戏对象地址。

## 插件装配

### Descriptor 与依赖

| 项目 | 当前声明 |
|---|---|
| descriptor | id `core.item`，version `0.1.0`，`deactivate_allowed=false`。 |
| required capabilities | 0 项。 |
| provides | 5 项：`item.facts`、`item.state`、`inventory.action`、`inventory.slot`、`inventory.projection`；能力名称、版本与用途见[对外契约](core.item.md)的“对外能力”。`inventory_refresh`、`inventory.bag_source`、`move_merge.control` 不再由本插件提供。 |
| Game ports | 5 项：`QOL_PORT_ITEM` 5.0（单点低层原语；空间化袋读经 `inventory.access` 1.1）、`QOL_PORT_ITEM_MONEY_WRITE` 1.0、`QOL_PORT_ITEM_DICE_WRITE` 1.1、`QOL_PORT_ITEM_INVENTORY_STATE_WRITE` 2.4、`QOL_PORT_CHARACTER_EXTRAS` 1.2。`QOL_PORT_MOVE_MERGE` 已迁 `extension_bag`。 |
| optional requires | 7 项：`character.state` 1.0、`http.registry` 1.1、`http.dispatch` 1.1、`world.facts` 2.0、`inventory.bag_source`（由 `core.inventory` 提供）、`inventory.access` 1.1（原生空间袋读）、`inventory_refresh`（由 `extension_bag` 提供）。 |

`world.facts` 缺失时出售以 `kSymbolsUnresolved` 失败，不继续扣款或移除物品。`inventory_refresh` 缺失时出售结算后的原版刷新退化为 no-op；`inventory.bag_source` 缺失时注册源查询为空；`inventory.access` 缺失时原生空间袋读返回 NotReady。其余 optional requires 不构成安装依赖。

### Features 与资源

| 项目 | 装配事实 |
|---|---|
| `core.item.provider` | `Hidden` toggle，默认开启；其 feature callback 不执行额外 toggle 动作。插件启停仍由生命周期处理。 |
| 库存 hooks | 11 个 hook claim：8 个 required、fail-closed 的库存动作入口；3 个 optional、fail-soft 的快捷栏诊断入口。装备页 5 条 UI hook（`UIEquip_ButtonEquipExe`/`ButtonUnequipExe`/`OKConfirmUseItem`/`RefreshItemArea`/`EquipControlEventProc`）与 `ptrslot:MOVE_MERGE_GOT` 已迁 `extension_bag`。 |
| 总资源 | 11 项；生命周期将库存 hooks 的 claim 合并进 descriptor。 |

hook 入口覆盖库存动作和诊断绑定；快捷栏三个 optional hook 仅提供诊断观察，不作为必需写入口。资源绑定由 `on_resource_ready` 分发到库存 backup 绑定。

### 生命周期门控

插件安装和重新启用都会尝试恢复转移记录。禁用时停止周期任务、销毁 shop snapshot 并清空库存快照存储。descriptor 不允许 deactivate；资源生命周期仍由 Host 约束，不能通过普通停用跳过 hook/backup 清理。

## 改版袋数探测（C18 布局）

| 项目 | 当前实现 |
|---|---|
| 布局标记 | `G_C18_BAG_TABLE_VMA = 0x735000` 只保留原版 6 个袋的镜像；首项非空即判定为 C18 布局。 |
| 真实袋对象表 | 由游戏 GOT `*(base + 0x2f3bc0)` 指向（v520 上为堆表，18 项）；`0x735000` 不是完整袋表。 |
| 槽表 | `G_C18_SLOT_TABLE_VMA = 0x735100`，每袋 `0x80` 字节。 |
| 探测 | 从 GOT 表扫描最多 32 项；非空候选必须可读、类别为 `1..4`、`+0x10` 容量位域属于 `{4,8,12,16}`，只保留最高有效索引。 |
| 禁止项 | 不得调用 `INVEN_GetBagSize(index)` 做校验：该函数接受索引 18，但索引 18 的表项不是袋对象，解引用会崩溃（v520 实测 tombstone 于 `INVEN_GetBagSize+24`）。 |
| count 计算 | `highest_valid_index + 1`，夹取到 `[6,32]`，再减去原版前缀 6 得到改版袋空间袋数。 |
| 布局提示 | `kQolTargetCapabilityC18NativeBagLayoutCandidate` 是 bit 3（`1<<3`），只表示布局候选；候选注释仍要求 `2×5` 拓扑准入。 |
| 门控 | 探测只在 `capabilities & kCapC18NativeBagLayoutCandidate` 的构建上执行；其它构建直接返回 `{g_inven, 6}`，不再读 `0x735000`（原版镜像边界正好在该地址）。 |
| 缓存 | `native_bag_layout()` 1 秒 TTL 缓存；`invalidate_native_bag_layout_cache()` 可显式失效（读档/布局变化）。插件侧空间枚举由 `inventory.access` 1.1 的 `native_for_each_space_slot` 组合 `item_ref_at` 逐槽读取。 |

探测位于 Host/game access，不由插件直接读取地址。v520 实测：GOT 表 18 项；`0x735000` 仅 6 项；索引 18 为非袋残留指针。
## 读取、快照与刷新

`read_inventory()` 在游戏线程读取原版/C18 物品，再读取注册袋源。优先使用空间枚举或来源枚举；不可用时按袋容量逐槽读取，只物化占用槽。money 与库存数据一起写入 `InventorySnapshot`。

| 机制 | 内部行为 |
|---|---|
| `SnapshotStore` | 持有不可变快照；完整读取成功后原子发布，HTTP 读取方只消费已发布对象。 |
| inventory epoch | 显式刷新先 bump epoch；读取成功后按新 epoch 发布。epoch 是快照版本，不是物品身份。 |
| `request_inventory_refresh()` | 立即在游戏线程请求一次库存重读与发布。 |
| 快捷栏刷新 | 原版 `PLAYER_UpdateShortcut(category)` 只刷新同类别槽；`InventoryRefreshApi.update_shortcut_category` 必须按注册源中出现的类别逐个调用，无参调用无效。 |
| 写后刷新 | 成功写动作调用库存刷新入口，使后续查询消费更新后的快照。 |
| 周期重建 | lifecycle scheduler 每 60 帧执行一次 `frame_tick()`；其中包含库存读取、item state 更新与 shop snapshot refresh。 |
| shop | 独立 snapshot 与独立 epoch；不与 inventory snapshot 共用版本。 |
| 注册源变化 | 注册表 generation 改变；后续库存快照反映来源集合变化。 |

当前 money 随 `InventorySnapshot` 发布，没有独立 wallet snapshot。HTTP GET 不按请求排队重建快照。

## 库存状态组装

raw state 保留原版袋 `0..5`，其后接 C18 展示段；注册来源段由 `count/info/item_at` 结构化生成。enriched JSON 由 item owner 构造，不拼接来源插件提供的 JSON 字符串片段。相关组装集中在 `api/state.cpp`，ABI 声明在 `contracts/item_state.h`。

## 袋源编排迁移

以下内容已从 core.item 移出，现由 core.inventory 的 `inventory.transfer` 承接：

- `usecases/move_item.cpp`：跨空间 move 编排，迁入 `core.inventory/transfer/move_item.*`。
- `bags/native_transport.*`、`bags/source_transport.*`：原生/注册源 payload transport，迁入 `core.inventory/transfer/`。
- `usecases/transfer_journal.*`：`item.transfer.journal` 持久化与启动 reconcile，已删除且不再重建。
- `model/`：袋映射、物品身份、变更集与移动规则校验，已删除。
- `bags/native_bag_source.*`、`bags/bag_source_registry.*`、`bags/item_handle_guard.*`：未编译的袋源抽象与句柄守卫，已删除。

当前移动路径：原生同空间由 `usecases/item_action.cpp` 调用 Host `move_item` 端口；跨空间与注册源移动通过 `inventory.transfer.move_item` 执行导出 → 导入 → 源移除，失败时尽力补偿。HTTP `POST /api/item/inventory/move_item` 已由 core.inventory 提供，结果体保持 `{"ok":true,"slots":[…]}`。`bags/transfer.h` 仅保留 `QolBagTransfer` payload 结构和哈希，用于出售回滚。

库存读取仍由 `usecases/read_inventory.cpp` 汇总原生空间与已注册袋源；袋源注册面由 core.inventory 的 `inventory.bag_source` 提供。

## 投影回调注册表

投影 callback registry 由 `api/projection.cpp` 管理，`extension_bag` 注册它自己的 drop callback；投影物品仍由注册袋源持有，不成为原生库存事实。

原拖拽合并 wrapper（`hooks/merge_hook.cpp` 包装 `ptrslot:MOVE_MERGE_GOT`：0x02 投影 drop 分流、0x04 原生合并 gate）已随 5 条装备页 UI hook 一并迁往 `extension_bag`（`core/move_merge_hook.cpp`、`ui/equip_hooks.cpp`）；`move_merge.drag_merge` feature 与 `QOL_PORT_MOVE_MERGE` 端口同归 `extension_bag`。core.item 侧只剩投影注册表；成功写入后的原版刷新经 `inventory_refresh`（owner `extension_bag`）的 trampoline 请求。

## 数据流摘要

| 流程 | 内部数据流 |
|---|---|
| 读 | 游戏线程读取 ITEM port 与注册袋源 → 构造 `InventorySnapshot` → `SnapshotStore` 原子发布；GET 从已发布快照组装响应。 |
| 写 | HTTP 参数校验 → typed command/ticket → 游戏线程 use case → Host port 或来源 callback → ticket result；成功后请求库存刷新。解析线程只做引用透传：注册源空间不做袋源回调/容量校验，统一留给主线程动作；需要袋数时读已发布快照。 |
| 跨空间转移 | 已删除；POST 校验与票据 ACK 保留，终态返回 `item transfer unavailable`，不触达袋源回调。 |
| 来源生命周期 | activate 主线程注册 `QolBagSource`、deactivate 主线程注销 → registry generation 变化 → 后续快照更新来源集合。 |

## HTTP 与 shop 内部适配

- `http/query_handler.cpp` 从快照存储取只读视图，完成库存查询、过滤、分页与 JSON 组装；它不调用库存原生读取接口。
- `http/write_handler.cpp` 负责校验并投递 typed command。
- `http/write_results.cpp` 将 ticket 完成结果编码为响应。

Shop 使用 `shop/` 内独立 snapshot 与 epoch；inventory 写入和读取不直接修改 shop snapshot。生命周期的周期任务分别推进库存/state 与 shop 刷新。

## 已知限制与风险

| 限制 | 影响 |
|---|---|
| 合并端口 bag 范围含 5（现由 `extension_bag` 持有） | `inventory.action` 与 transfer 排除任务袋 `(0,5)`，但 `extension_bag` 的 merge wrapper 检查范围为 `0..5`，未明确排除 5；不能由 action 层规则推断 hook 已排除任务袋。 |
| C18 缓存 key 不含所有袋表项 | 表项变化但 key 不变时需显式失效；当前失效入口没有调用点。 |
| 注册源原子性 | 来源 callback 若失败时部分修改 sidecar，core.item 无法可靠推断或修复；来源必须满足 export 只读及失败不变式。 |
| 分页与注册顺序 | 注册顺序或翻页期间库存变化可能导致重复/跳过；扁平袋号不是持久身份。 |
| 容量没有全局上限 | 大容量来源应实现枚举；逐槽 fallback 成本随容量增长。 |

## 测试与验证映射

| 范围 | 代码级验证入口 |
|---|---|
| descriptor、能力、端口、feature 与路由 metadata | `tests/test_item_facts_plugin.cpp`、`tests/test_item_facts_dispatch.cpp` |
| 查询分页、raw inventory、HTTP 写校验与结果 | `tests/test_item_facts_dispatch.cpp` |
| 动作出售、扣款与回滚 | `tests/test_inventory_action_sell.cpp`、`tests/test_inventory_trade_sell.cpp` |
| HTTP 移动 | `tests/test_item_facts_dispatch.cpp`；跨空间路径断言 `item transfer unavailable`。 |
| 袋源 mutation free/materialize rollback | `tests/test_extension_bag_bag_source_mutation.cpp`（`extension_bag_bag_source_mutation_tests`；Host-only，非真机验收；failure tests 覆盖 provider 可达的 free 拒绝与 materialize 失败回滚）。 |
| 物品句柄身份 | `tests/test_item_handle_registry.cpp`、`tests/test_item_handle_custody.cpp`、`tests/check_item_handle_contracts.cmake` |
| 库存 hook（core.item 侧） | `tests/test_inventory_hook_stage4.cpp` |
| 拖拽合并 wrapper 与装备页 UI hook（extension_bag 侧） | `tests/test_move_merge_plugin.cpp`、`tests/test_extension_bag_equip_hooks.cpp` |
| 静态资源与 HTTP route contract | `scripts/maintenance/check_arch.py`、`scripts/maintenance/check_http_routes.py` |

Host/static 测试不替代[验证规则与验收卡](../refactor/verification.md)要求的真机证据；构建、部署与设备取证流程见[构建与部署指南](../guides/build-and-deploy.md)。

## 已删除的旧结构

| 旧结构 | 当前状态 |
|---|---|
| `ExtensionBagApi` | 已从 core.item 当前实现移除；外部来源使用袋源注册契约。 |
| `QolInventoryExtensionProvider` | 不再是袋源读写接口或扩展袋识别入口。 |
| provider-first arbitration | 已删除；读取分别覆盖原生空间 0/1 与注册袋源空间。 |
| `extension_source_bridge` | 过渡适配器不在当前目录或运行路径；扩展袋直接注册袋源。 |
| 旧 fragment state | 已删除；注册源库存段由结构化袋源数据生成，不拼接来源方 JSON。 |
| `inventory.merge_projection` | 已删除；投影回调注册由新的 projection 能力承载。 |
| `usecases/move_item.*` | 已删除；跨空间移动编排迁入 core.inventory `inventory.transfer`。 |
| `usecases/transfer_journal.*` | 已删除；不再持久化转移记录或启动 reconcile。 |
| `bags/native_transport.*`、`bags/source_transport.*` | 已删除；transport 迁入 core.inventory `transfer/`。 |
| `model/` | 已删除；袋映射、移动规则与变更集校验待重建。 |
| `bags/native_bag_source.*`、`bags/bag_source_registry.*`、`bags/item_handle_guard.*` | 已删除；这些文件此前未编译且无引用。 |

旧 fragment state 与 provider 所有权不作为兼容 fallback；当前没有双读/双写路径。
