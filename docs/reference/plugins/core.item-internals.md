# `core.item` 内部实现

本文记录 `core.item` 的目录边界、装配、快照、转移恢复与测试映射。对外能力、插件作者与 HTTP 客户端可依赖的契约见 [`core.item` 对外契约](core.item.md)。

## 目录分层与组件职责

| 层 | 职责 | 主要位置 |
|---|---|---|
| 装配与生命周期 | descriptor、端口绑定、资源 claim、启停、帧任务、启动恢复 | `plugins/core.item/plugin.cpp`、`runtime/lifecycle.cpp` |
| API | facts、state、袋源注册、投影回调注册表 | `api/facts.cpp`、`api/state.cpp`、`api/bag_source.cpp`、`api/projection.cpp` |
| Bags | 原生/注册源 transport、注册表、快照存储、句柄检查 | `bags/native_transport.cpp`、`bags/source_transport.cpp`、`bags/bag_source_registry.cpp`、`bags/snapshot_store.cpp`、`bags/item_handle_guard.cpp` |
| Model | 袋空间和扁平映射、物品身份、变更集、移动规则 | `model/bag_catalog.cpp`、`model/item_identity.h`、`model/change_set.cpp`、`model/move_rules.cpp` |
| Use cases | 库存读取、物品动作、移动、出售、转移日志 | `usecases/read_inventory.cpp`、`item_action.cpp`、`move_item.cpp`、`sell_item.cpp`、`transfer_journal.cpp` |
| Hooks | 原生库存入口与拖拽合并 wrapper | `hooks/inventory_hooks.cpp`、`hooks/merge_hook.cpp` |
| HTTP / shop | 查询分页、typed command、响应结果、shop 路由与快照 | `http/query_handler.cpp`、`http/write_handler.cpp`、`http/write_results.cpp`、`shop/` |

插件私有规则不向 Host 端口反向泄漏。原生内存解释由 game/access 与 adapter 负责；领域层通过契约和端口调用，不跨线程持有游戏对象地址。

## 插件装配

### Descriptor 与依赖

| 项目 | 当前声明 |
|---|---|
| descriptor | id `core.item`，version `0.1.0`，`deactivate_allowed=false`。 |
| required capabilities | 0 项。 |
| provides | 8 项；能力名称、版本与用途见[对外契约](core.item.md)的“对外能力”。 |
| Game ports | 6 项：`QOL_PORT_ITEM` 3.8、`QOL_PORT_ITEM_MONEY_WRITE` 1.0、`QOL_PORT_ITEM_DICE_WRITE` 1.1、`QOL_PORT_ITEM_INVENTORY_STATE_WRITE` 2.2、`QOL_PORT_MOVE_MERGE` 1.2、`QOL_PORT_CHARACTER_EXTRAS` 1.2。 |
| optional requires | 4 项：`character.state` 1.0、`http.registry` 1.1、`http.dispatch` 1.1、`world.facts` 2.0。 |

`world.facts` 缺失时出售以 `kSymbolsUnresolved` 失败，不继续扣款或移除物品。其余 optional requires 不构成安装依赖。

### Features 与资源

| 项目 | 装配事实 |
|---|---|
| `core.item.provider` | `Hidden` toggle，默认开启；其 feature callback 不执行额外 toggle 动作。插件启停仍由生命周期处理。 |
| `move_merge.drag_merge` | `Basic optimization` toggle，默认开启；callback 更新拖拽合并 gate。 |
| 库存 hooks | 27 个 hook claim：24 个 required、fail-closed 的库存入口；3 个 optional、fail-soft 的快捷栏诊断入口。 |
| 合并 wrapper | 1 个 optional、fail-closed 的 `ptrslot:MOVE_MERGE_GOT` claim；资源未就绪时 `move_merge.control` 报告 disabled。 |
| 总资源 | 28 项；生命周期将库存 hooks 与合并 wrapper 的 claim 合并进 descriptor。 |

hook 入口覆盖库存查询/写入和备用入口绑定；快捷栏三个 optional hook 仅提供诊断观察，不作为必需写入口。资源绑定由 `on_resource_ready` 分发到库存 backup 绑定或 wrapper 就绪状态。

### 生命周期门控

插件安装和重新启用都会尝试恢复转移记录。禁用时关闭拖拽合并 gate、停止周期任务、销毁 shop snapshot 并清空库存快照存储。descriptor 不允许 deactivate；资源生命周期仍由 Host 约束，不能通过普通停用跳过 hook/backup 清理。

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
| 缓存 | 无袋数缓存；每次探测重新读 GOT 表，避免袋表在进入世界/打开界面后变化导致旧值。`invalidate_native_bag_layout_cache()` 保留为空实现。 |

探测位于 Host/game access，不由插件直接读取地址。v520 实测：GOT 表 18 项；`0x735000` 仅 6 项；索引 18 为非袋残留指针。
## 读取、快照与刷新

`read_inventory()` 在游戏线程读取原版/C18 物品，再读取注册袋源。优先使用空间枚举或来源枚举；不可用时按袋容量逐槽读取，只物化占用槽。money 与库存数据一起写入 `InventorySnapshot`。

| 机制 | 内部行为 |
|---|---|
| `SnapshotStore` | 持有不可变快照；完整读取成功后原子发布，HTTP 读取方只消费已发布对象。 |
| inventory epoch | 显式刷新先 bump epoch；读取成功后按新 epoch 发布。epoch 是快照版本，不是物品身份。 |
| `request_inventory_refresh()` | 立即在游戏线程请求一次库存重读与发布。 |
| 写后刷新 | 成功写动作调用库存刷新入口，使后续查询消费更新后的快照。 |
| 周期重建 | lifecycle scheduler 每 60 帧执行一次 `frame_tick()`；其中包含库存读取、item state 更新与 shop snapshot refresh。 |
| shop | 独立 snapshot 与独立 epoch；不与 inventory snapshot 共用版本。 |
| 注册源变化 | 注册表 generation 改变；后续库存快照反映来源集合变化。 |

当前 money 随 `InventorySnapshot` 发布，没有独立 wallet snapshot。HTTP GET 不按请求排队重建快照。

## 库存状态组装

raw state 保留原版袋 `0..5`，其后接 C18 展示段；注册来源段由 `count/info/item_at` 结构化生成。enriched JSON 由 item owner 构造，不拼接来源插件提供的 JSON 字符串片段。相关组装集中在 `api/state.cpp`，ABI 声明在 `contracts/item_state.h`。

## 原生与注册来源 transport

原生空间内移动使用 Host 的原生移动入口，不调用注册源 transfer callback。原版与 C18 之间，以及 Native 与注册源之间，转移会组合原生 serialize/load/place 与袋源 export/import/remove。相同注册空间的搬动也经来源 transfer 原语处理；该路径在 `usecases/item_action.cpp`。跨空间编排由 `usecases/move_item.cpp` 负责，原生/来源 transport 分别维护自己的 payload 转换。

## 跨空间转移实现与恢复

### 执行次序与补偿

跨空间 move 由 `usecases/move_item.cpp` 编排；原生 payload 由 `bags/native_transport.cpp` 处理，注册源 payload 由 `bags/source_transport.cpp` 处理。流程为：

1. 持久化 intent 记录。
2. source 导出 payload；export 只读。
3. target 导入 payload，并记录目标位置与 payload hash。
4. source 按预期句柄移除请求数量。
5. 成功后清除 journal 并刷新库存。

export 或 import 失败会清 journal；source remove 失败时尝试删除目标补偿。补偿成功则清记录并返回源错误；补偿失败保留记录并返回 internal error。成功移除 source 前，目标已完成写入。

### `item.transfer.journal` 存储

| 项目 | 二进制/持久化事实 |
|---|---|
| owner store section | `item.transfer.journal`，section version 1。 |
| header | 固定 6 字节：`uint16_t magic`、`uint16_t format_version`、`uint16_t count`。 |
| record | 固定 72 字节，最多 8 条；布局由静态断言锁定。 |
| 字段 | 16 字节 id、state 与 reserved、source/target 的 space/bag/slot、category、count、对齐字段、`uint64_t payload_hash`、时间戳。 |
| state | `0=intent`、`1=target_written`；intent 在导出前写，目标导入后写 target_written。 |
| payload hash | 对 payload 计算 64 位 FNV-1a；journal 不存 payload 本体。 |
| 满或 intent 无法持久化 | 已有 8 条，或存档槽可用但 intent 写入失败时，新的跨空间 move blocked，不执行物品修改。 |

`mark_target_written()` 的 store write 结果不会向上返回，因此记录状态未必反映目标写入结果；journal 不是强一致事务。

### 启动 reconcile 与降级

`plugin_install` 与 `plugin_on_enabled` 都调用 `reconcile()`。当前恢复仅依据 source/target 槽是否存在，不按 journal 的两种 state 分支：

| Source | Target | Reconcile 处理 |
|---|---|---|
| 存在 | 存在 | 删除 target、保留 source；成功后清记录。 |
| 不存在 | 存在 | 保留 target 并清记录。 |
| 存在 | 不存在 | 保留 source 并清记录。 |
| 不存在 | 不存在 | 记 WARN 后清记录。 |

查找或删除遇到不可判定错误会中止；删除 target 失败保留记录。lifecycle 当前忽略 `reconcile()` 返回值，且 `transfer_journal::count()` 未连接保存拦截，因此未恢复 journal **不阻止存档**。无法取得当前存档槽（`save.facts` 不可用或查询失败）时，记录仅在内存中并告警；转移仍可继续，但重启后没有落盘恢复保障。

## 投影与拖拽合并 wrapper

投影 callback registry 由 `api/projection.cpp` 管理，`extension_bag` 注册它自己的 drop callback；投影物品仍由注册袋源持有，不成为原生库存事实。

`hooks/merge_hook.cpp` 包装 `MOVE_MERGE_GOT`：

| 事件 | 内部路径 |
|---|---|
| `0x02` | 尝试交给 projection drop callback；已处理则结束该投影路径。 |
| `0x04` | 保护注册源/非原生物品，检查原生合并输入事实与 feature gate，再委托原生合并端口。 |

原生自动合并要求源、目标均为 count-encoded 同类别物品，双方 count 有效，目标 count limit 可读，合并后不超过上限；`can_apply_stuff` 必须成功并返回“不接管”。材料操作接管、事实未知或 gate 未就绪时不执行该自动合并。wrapper 只在整个源堆可放入目标剩余容量时合并，不在 gate 中拆成部分堆叠。

成功后按需同步 projection 显示槽、刷新原版库存 UI 并清除 touch cursor。feature 关闭或 wrapper 未就绪时不启用该合并路径。投影显示层由扩展袋负责。

## 数据流摘要

| 流程 | 内部数据流 |
|---|---|
| 读 | 游戏线程读取 ITEM port 与注册袋源 → 构造 `InventorySnapshot` → `SnapshotStore` 原子发布；GET 从已发布快照组装响应。 |
| 写 | HTTP 参数校验 → typed command/ticket → 游戏线程 use case → Host port 或来源 callback → ticket result；成功后请求库存刷新。 |
| 跨空间转移 | use case → journal intent → source export → target import → target_written/hash → source remove → 清记录；失败按补偿/reconcile 规则收敛。 |
| 来源生命周期 | activate 主线程注册 `QolBagSource`、deactivate 主线程注销 → registry generation 变化 → 后续快照更新来源集合。 |

## HTTP 与 shop 内部适配

- `http/query_handler.cpp` 从快照存储取只读视图，完成库存查询、过滤、分页与 JSON 组装；它不调用库存原生读取接口。
- `http/write_handler.cpp` 负责校验并投递 typed command。
- `http/write_results.cpp` 将 ticket 完成结果编码为响应。

Shop 使用 `shop/` 内独立 snapshot 与 epoch；inventory 写入和读取不直接修改 shop snapshot。生命周期的周期任务分别推进库存/state 与 shop 刷新。

## 已知限制与风险

| 限制 | 影响 |
|---|---|
| 合并端口 bag 范围含 5 | `inventory.action` 与 transfer 排除任务袋 `(0,5)`，但独立 merge port 当前检查范围为 `0..5`，未明确排除 5；不能由 action 层规则推断 hook 已排除任务袋。 |
| C18 缓存 key 不含所有袋表项 | 表项变化但 key 不变时需显式失效；当前失效入口没有调用点。 |
| Journal 保存边界 | reconcile 失败不阻止存档；无 save slot 时只有内存记录。不得将 journal 描述为强制保存锁。 |
| Journal 状态写入 | `mark_target_written()` 写失败结果未上报，reconcile 也不区分状态，恢复并非强一致事务。 |
| 注册源原子性 | 来源 callback 若失败时部分修改 sidecar，core.item 无法可靠推断或修复；来源必须满足 export 只读及失败不变式。 |
| 分页与注册顺序 | 注册顺序或翻页期间库存变化可能导致重复/跳过；扁平袋号不是持久身份。 |
| 容量没有全局上限 | 大容量来源应实现枚举；逐槽 fallback 成本随容量增长。 |

## 测试与验证映射

| 范围 | 代码级验证入口 |
|---|---|
| descriptor、能力、端口、feature 与路由 metadata | `tests/test_item_facts_plugin.cpp`、`tests/test_item_facts_dispatch.cpp` |
| 查询分页、raw inventory、HTTP 写校验与结果 | `tests/test_item_facts_dispatch.cpp` |
| 动作出售、扣款与回滚 | `tests/test_inventory_action_sell.cpp`、`tests/test_inventory_trade_sell.cpp` |
| HTTP 移动与注册源路径 | `tests/test_item_facts_dispatch.cpp`；当前无独立 transfer journal 测试文件。 |
| 袋映射、句柄身份 | `tests/test_bag_catalog.cpp`、`tests/test_item_handle_registry.cpp`、`tests/test_item_handle_custody.cpp`、`tests/check_item_handle_contracts.cmake` |
| hook 与拖拽合并 wrapper | `tests/test_inventory_hook_stage4.cpp`、`tests/test_move_merge_plugin.cpp` |
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

旧 fragment state 与 provider 所有权不作为兼容 fallback；当前没有双读/双写路径。
