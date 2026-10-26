# `core.item` 插件

`core.item` 是物品与库存领域 owner：提供物品事实、库存状态与快照、袋源注册、库存动作、库存 HTTP 查询/写入，以及原生库存入口和拖拽合并 wrapper。descriptor、能力、端口与资源声明位于 `module/app/src/main/cpp/plugins/core.item/plugin.cpp`、`runtime/lifecycle.cpp`；领域代码位于 `module/app/src/main/cpp/plugins/core.item/`。

## 责任边界

| `core.item` 拥有 | 不拥有 |
|---|---|
| 原版/C18 原生库存读写编排、袋源注册表、物品动作、快照、库存入口 hook、拖拽合并 wrapper。 | 扩展袋 sidecar、袋数据、页签和 UI；`extension_bag` 实现并注册 `QolBagSource`，也注册投影回调。 |
| `item.facts`、`item.state`、`inventory.action`、`inventory.bag_source`、`inventory.projection` 等对外能力，以及库存/shop 路由描述。 | HTTP listener、传输、OP 权限门禁；由 Host/Kotlin 承担。shop 目前仍属于 `core.item`，尚未独立拆出。 |
| 通过 Host/Game Adapter 端口使用原生物品事实和写入原语。 | 原生游戏内存适配和地址解释；由 game/access 与 Host 端口实现。角色、世界事实由各自 owner 提供。 |

## 设计决策与非目标

| 决策 | 当前约束 / 实现事实 |
|---|---|
| 三个编号空间彼此独立 | 原版 `space=0`、C18 `space=1`、注册袋源 `space>=2`；袋索引只在自身空间有效。稳定位置是 `{space_id, bag, slot}`。 |
| 原版袋数固定 | 6 袋，索引 `0..5`；`(0,5)` 是只读任务袋。常量见 `contracts/game_ports/item_port.h`。 |
| C18 袋数动态探测并缓存 | Host 从 C18 袋对象表探测最高非空项，缓存在原生访问层；范围夹取和失效规则见下节 `C18 探测`。 |
| 容量由袋接口报告 | `QolBagInfo.capacity` 是来源给出的容量；`core.item` 不为注册袋设一个全局容量上限。Native adapter 仍遵守原生槽表布局。 |
| 依赖反转 | `extension_bag` 依赖 `core.item` 的 `item.facts` / `inventory.bag_source`，由 core.item 调用来源回调；core.item 不调用扩展背包能力。 |
| 脏纪元、显式刷新与周期重建 | 快照有 epoch；成功写入请求刷新。显式刷新立即重读；生命周期另每 60 帧调用一次读取，作为周期兜底，见 `runtime/lifecycle.cpp`、`usecases/read_inventory.cpp`。 |
| 快照相互隔离 | 库存快照有自己的 epoch，shop snapshot 独立维护 epoch。当前 money 随 `InventorySnapshot` 发布，并没有独立 wallet snapshot。 |
| 分页使用位置游标 | 游标表达 `{space,bag,slot}`；扁平袋号仅用于便利寻址，不作持久键或跨请求身份。 |
| 不保留旧接口/兼容层 | 袋源注册式契约是当前边界；不恢复旧 provider、fragment JSON 或 `ExtensionBagApi` 兼容实现。 |

**非目标**：`core.item` 不拥有扩展袋 UI、sidecar schema 或显示实现；不把 shop 拆成独立插件作为本契约的一部分；不在插件层复制 C18 内存探测；不引入跨插件锁或分布式事务。当前 60 帧周期是实际兜底，不表示 HTTP GET 会排队按需重建：GET 只消费已发布快照，写后显式刷新在游戏线程执行。

## 空间编号、袋数与容量

### 空间编号

| 空间 | `space_id` | 空间内袋索引 | 袋数来源 | 原生袋号映射 |
|---|---:|---|---|---|
| 原版 | 0 | `0..5` | 常量 6 | 与索引相同 |
| C18 追加袋 | 1 | `0..c-1` | Native port 动态探测 | `6 + bag` |
| 扩展袋及其他注册源 | `>=2` | `0..count()-1` | 每个注册源的 `count()` | 无 |

`space_id` 和 `bag` 共同标识袋；相同的 bag 数字不表示同一袋。扩展袋当前使用 `space=2`，但注册顺序不是稳定身份。常量与 ref 定义见 `contracts/game_ports/item_port.h`。

### C18 探测

| 项目 | 当前值 / 语义 | 代码依据 |
|---|---|---|
| 袋对象表 VMA | `G_C18_BAG_TABLE_VMA = 0x735000` | `game/access/inventory.cpp` |
| 槽表 VMA | `G_C18_SLOT_TABLE_VMA = 0x735100` | `game/access/inventory.cpp` |
| 每袋槽跨度 | `0x80` 字节 | `game/access/inventory.cpp` |
| 首项探测 | 检查袋对象表首指针非空，并逐项取最高非空原生袋索引 | `game/access/inventory.cpp:46-79` |
| 真实原生袋索引 | `0..17`；getter 可接受的索引 18 不是实际袋 | `contracts/game_ports/item_port.h`、`game/access/inventory.cpp` |
| 探测计数 | `highest_non_null_index + 1`，夹取到 `[6,18]`；C18 空间 count 再减原版前缀 6 | `game/access/inventory.cpp:69-79,380-392` |
| 能力提示 | `kQolTargetCapabilityC18NativeBagLayoutCandidate` 是 bit 3（值 `1<<3`），表示布局候选；候选位注释仍要求 `2×5` 拓扑准入 | `contracts/game_host.h`、`game/variants/game_variant.h` |
| 缓存失效 | 表地址、首袋指针或表可读状态变化时重新初始化；另提供 `invalidate_native_bag_layout_cache()` 显式失效入口 | `game/access/inventory.cpp:56-62,375-378`、`game/access/inventory.h` |

该探测位于 Host/game access，不是插件自行读地址的许可。当前缓存自动比较的 key 不含所有袋表项；若表内后续变化但地址、首指针和可读状态均不变，应由适用的 Host 生命周期调用显式失效入口。仓库当前只有该入口的声明与定义，没有调用点（`game/access/inventory.h`、`game/access/inventory.cpp`）。不得把 C18 空间索引与注册源索引混用。

### 容量与扁平袋号

- `QolBagInfo` 包含 `capacity` 与 `slot_count`。注册源按自身契约提供它们；`core.item` 不把容量裁成固定 16。Native 游戏表自身的物理槽访问另由 Host adapter 限定。字段见 `contracts/game_ports/item_port.h`、`contracts/bag.h`。
- 枚举优先使用 `for_each_item` / Native `for_each_space_slot`；否则按袋容量逐槽读取。快照只保留占用槽记录。读取实现见 `plugins/core.item/usecases/read_inventory.cpp`。
- 扁平袋号按原版 → C18 → 注册源连接。原版/C18 部分可推导，注册源顺序取决于注册表，不得持久化，也不用于分页游标；映射实现见 `plugins/core.item/model/bag_catalog.cpp`。
- 任务袋 `(space=0, bag=5)` 可以读取，但不能作为任何物品动作的源或目标，也不能写入 `current_bag`。

## 分层与目录

| 组件 | 职责 | 代码位置 |
|---|---|---|
| 装配 / 生命周期 | descriptor、端口绑定、安装/启停、帧任务、journal reconcile | `plugin.cpp`、`runtime/lifecycle.cpp` |
| API | facts、state、袋源注册、projection callback registry | `api/facts.cpp`、`api/state.cpp`、`api/bag_source.cpp`、`api/projection.cpp` |
| 袋与快照 | Native/注册源 transport、source registry、不可变快照、句柄校验 | `bags/native_transport.cpp`、`bags/source_transport.cpp`、`bags/bag_source_registry.cpp`、`bags/snapshot_store.cpp`、`bags/item_handle_guard.cpp` |
| 模型 | 袋空间/扁平映射、物品身份、变更集与移动规则 | `model/bag_catalog.cpp`、`model/item_identity.h`、`model/change_set.cpp`、`model/move_rules.cpp` |
| 用例 | 读库存、动作、移动/转移、出售和恢复记录 | `usecases/read_inventory.cpp`、`item_action.cpp`、`move_item.cpp`、`sell_item.cpp`、`transfer_journal.cpp` |
| Hooks | 原生库存入口、backup 绑定、拖拽合并 wrapper | `hooks/inventory_hooks.cpp`、`hooks/merge_hook.cpp` |
| HTTP / shop | 查询分页、typed command、响应及当前 shop 路由/快照 | `http/query_handler.cpp`、`http/write_handler.cpp`、`http/write_results.cpp`、`shop/` |

插件外消费者通过 `contracts/` ABI 调用；不得依赖插件私有实现或跨线程持有游戏对象地址。Host 原语和领域规则保持分离。

## 插件声明、能力与资源

插件 id/version 为 `core.item` / `0.1.0`，`deactivate_allowed=false`；声明见 `plugin.cpp`。required capability 为 0 项，optional requires 共 4 项。

### Provides：8 项

| 能力 | 版本 | 责任 |
|---|---:|---|
| `item.facts` | 3.6 | 物品类别、库存 view/count/编码、装备/宝石/禁售和报价等事实。 |
| `item.state` | 1.1 | raw/enriched 库存状态 JSON 与尺寸查询。 |
| `inventory.bag_source` | 1.1 | 注册/注销外部袋源；读面及跨空间 transfer 原语实现者。 |
| `inventory_refresh` | 1.1 | 原版库存区域刷新与快捷栏数量更新。契约 id 在 `contracts/inventory_refresh.h` 使用下划线。 |
| `inventory.action` | 3.0 | move、use、discard、sell、equip、unequip、put_jewel。 |
| `inventory.slot` | 1.0 | resolve/view/remove 槽位查询面。 |
| `move_merge.control` | 1.0 | 报告 feature 与 wrapper 是否都允许拖拽合并。 |
| `inventory.projection` | 1.0 | 投影回调注册/注销；扩展袋投影回调由注册方实现。 |

能力 id/version 在 `contracts/` 头文件定义，提供表在 `plugins/core.item/plugin.cpp`。

### Ports：6 项

| Port | 版本 | 用途 |
|---|---:|---|
| `QOL_PORT_ITEM` | 3.8 | 原生物品事实与库存读取。 |
| `QOL_PORT_ITEM_MONEY_WRITE` | 1.0 | 金钱写入。 |
| `QOL_PORT_ITEM_DICE_WRITE` | 1.1 | 骰子接受/拒绝写入。 |
| `QOL_PORT_ITEM_INVENTORY_STATE_WRITE` | 2.2 | 原生库存动作、物品、装备与商店写入。 |
| `QOL_PORT_MOVE_MERGE` | 1.2 | 原生移动、当前袋及句柄校验。 |
| `QOL_PORT_CHARACTER_EXTRAS` | 1.2 | 合并时材料应用判定。 |

Port 顺序来自 `plugin.cpp`；版本常量见 `contracts/game_ports/ports.h` 及各 port 头文件。

### Optional requires：4 项

| 能力 | 版本 | 使用面 |
|---|---:|---|
| `character.state` | 1.0 | 角色状态消费者。 |
| `http.registry` | 1.1 | HTTP 路由注册。 |
| `http.dispatch` | 1.1 | HTTP 请求分发。 |
| `world.facts` | 2.0 | 出售时检查世界状态；缺失时以 `kSymbolsUnresolved` 失败，不继续扣款或移除。 |

optional requires 不构成安装依赖；声明与调用分别见 `plugin.cpp`、`usecases/sell_item.cpp`、`http/routes.cpp`。

### Features 与资源

| 项目 | 当前声明 |
|---|---|
| `core.item.provider` | 默认开启；Hidden toggle，不执行额外 toggle 回调。 |
| `move_merge.drag_merge` | 默认开启；Basic optimization toggle，只切换 wrapper 内 gate。 |
| 库存 hooks | 27 项：24 个 required fail-closed 入口 hook；3 个快捷栏诊断 optional fail-soft hook。见 `hooks/inventory_hooks.cpp`。 |
| 合并 wrapper | 1 个 optional fail-closed `ptrslot:MOVE_MERGE_GOT`；wrapper 未就绪时 control reports disabled。见 `hooks/merge_hook.cpp`。 |
| 总资源 | 28 项；`runtime/lifecycle.cpp` 合并两组 claim。 |

## `QolBagSource` v1.1 契约

`inventory.bag_source` 是 `core.item` 提供的注册面。每个 `QolBagSource` 占用独立 `space_id>=2`，所有回调由 `core.item` 在游戏主线程调用。ABI 定义见 `contracts/bag.h`。

### 字段与注册 API

| 字段 | 类型 / 要求 | 语义 |
|---|---|---|
| `version` | `QolVersion`；major 为 1，minor 至少 1 | 袋源协议版本。 |
| `space_id` | `int32_t`，至少 2 | 本来源空间编号；不能与已有注册冲突。 |
| `count` | 必需 `int32_t (*)()` | 返回本空间当前袋数；袋索引从 0 起。 |
| `info` | 必需 `QolResult (*)(bag, QolBagInfo*)` | 返回容量和已占用槽数。 |
| `item_at` | 必需 `QolResult (*)(bag, slot, QolInventoryItemRef*)` | 读指定槽；空槽用 `kQolErrNotFound` 表示。 |
| `for_each_item` | 必需 `QolResult (*)(QolInventoryBagSlotFn, void*)` | 枚举来源的占用物品；ref 仅在回调时使用，不跨线程保存原生指针。 |
| `export_item` | 必需 `QolResult (*)(handle, count, QolBagTransfer*)` | 只读导出供转移的 payload。 |
| `import_item` | 必需 `QolResult (*)(transfer, preferred_bag, preferred_slot, result*)` | 将 transfer 导入来源并返回实际位置、句柄、数量。 |
| `remove` | 必需 `QolResult (*)(expected_handle, count, result*)` | 按预期句柄扣减或移除物品。 |
| `identify` | 可选 `QolResult (*)(handle, ref*, slot*)` | 将本来源句柄定位到空间、袋和槽。 |
| `use` | 可选 `QolResult (*)(ref, slot)` | 执行来源自己的使用结算。 |

| 注册面 | 契约 |
|---|---|
| API | `QolBagSourceRegistryApi { struct_size, register_source, unregister_source }`，随 `inventory.bag_source` 1.1 提供。 |
| 主线程 | 注册/注销只在游戏主线程执行；来源激活时注册，停用时注销。 |
| 校验 | `count/info/item_at/for_each_item/export_item/import_item/remove` 均必需；`identify/use` 可选。 |
| 唯一性 | `space_id<2`、重复注册、错误版本或必需回调缺失均拒绝；不覆盖已注册来源。 |
| 生命周期 | 注册期间来源结构与函数表必须有效；注销后不得再调用来源回调。 |
| 快照 | 注册/注销改变注册表 generation，后续库存快照必须体现来源集合变化。 |

来源的数量、句柄及事务原子性由来源 callback 实现；core.item 不接管其 sidecar 数据。注册/查找实现见 `api/bag_source.cpp`、`bags/bag_source_registry.cpp`。

### 回调语义与失败结果

| 回调 | 成功语义 | 主要失败语义 |
|---|---|---|
| `count()` | 返回非负袋数。 | 来源未就绪时应返回 0 或通过上层 readiness 报告不可用；不得伪造袋数。 |
| `info(bag,out)` | `capacity` 非负；`slot_count` 为占用槽数；容量 0 合法。 | 越界/无效参数用 `kQolErrInvalidArgument`；来源不可用用 `kQolErrNotReady`。 |
| `item_at(bag,slot,out)` | 填 `space_id`、空间内 bag、slot、有效 `slot+generation` 句柄及物品事实。 | 空槽或不存在为 `kQolErrNotFound`；非法 ref 为 `kQolErrInvalidArgument`。 |
| `for_each_item(fn,ctx)` | 对占用物品调用回调；回调非 0 可停止枚举。 | 单项物化失败不得伪装成成功物品；来源按自身策略记录错误。 |
| `export_item(handle,count,out)` | 不得修改来源；`out.count` 是请求转移量，payload 由来源定义并保持不透明。 | 过期/非本源 handle 为 `kQolErrNotFound`；count 非正或超过堆量为 `kQolErrInvalidArgument`；不可用为 `kQolErrNotReady`。 |
| `import_item(transfer,bag,slot,out)` | 规范化 payload 中数量为 `transfer.count`，成功返回实际 `{ref,item,count}`。 | 空参数、非法类别/数量/长度为 `kQolErrInvalidArgument`；指定槽被占用为 `kQolErrConflict`；失败不得留下部分对象、句柄或持久化改动。 |
| `remove(handle,count,out)` | 部分移除只减数量且保留有效句柄；整堆移除使句柄失效。 | 过期/非本源 handle 为 `kQolErrNotFound`；非法数量为 `kQolErrInvalidArgument`；失败不得部分扣减。 |
| `identify` / `use` | identify 返回 ref/slot；use 完成来源自身结算。 | 可选回调缺失时对应操作为 `kQolErrUnsupported`。 |

所有回调都在主线程调用；不得将句柄以外的游戏裸指针跨帧或跨线程保存。错误码还包括 `kQolErrBusy`（处理中）、`kQolErrBlocked`（业务拒绝/不可执行）及 `kQolErrInternal`（内部或回滚不一致），定义见 `contracts/result.h`。

`import_item` 的目标选择参数：`preferred_bag=-1, preferred_slot=-1` 表示来源自行选袋/槽；指定 bag、slot 为 `-1` 表示在该袋选槽；bag 与 slot 都非负时必须使用该槽，占用返回 `kQolErrConflict`。该 ABI 不要求来源在明确槽请求时自动合并。

### `QolBagTransfer`

```cpp
struct QolBagTransfer {
    int32_t category;
    int32_t count;
    uint32_t payload_size;
    uint8_t payload[256];
};
```

该结构和常量定义于 `contracts/bag.h`：固定缓冲容量 256 字节，有效 payload 长度为 `1..255`（最大 255）；不得动态扩容。payload 是来源定义的序列化状态，core.item 只透传，不解释、不改写；部分堆叠时 `count` 是本次数量，目标必须将 payload 内数量规范化到该值。接收端应在回调内复制需要保留的数据。

## 库存动作与转移矩阵

`inventory.action` 3.0 是跨插件物品副作用唯一入口，参数使用显式 `QolActionRef {space_id,bag,slot}`；执行前按位置重读并复核句柄。主线程调用契约见 `contracts/inventory_action.h`。

| 方法 | 输入 | 结果 |
|---|---|---|
| `move(from,count,to)` | 明确源/目标和正数量 | `{status,moved_count,merged,target,target_item}` |
| `use(ref)` | 明确物品位置 | `{status,consumed_count}` |
| `discard(ref,count)` | 明确物品位置和数量 | `{status,consumed_count}` |
| `sell(ref,expected_item,count,source)` | 预期句柄、数量、来源标签 | `InventorySellResult`；依赖 `world.facts`。 |
| `equip(role,ref,equip_slot)` | 角色、物品 ref、装备槽 | `{status,consumed_count}`；当前仅原生物品支持。 |
| `unequip(role,equip_slot)` | 角色和装备槽 | `{status,consumed_count}` |
| `put_jewel(role,jewel_ref,equip_slot)` | 宝石 ref 和装备槽 | `{status,consumed_count}`；当前注册袋宝石不支持。 |

### move 路径

| 源 / 目标 | 当前执行路径 |
|---|---|
| 同一原生空间（原版到原版或 C18 到 C18） | Host `move_item` 快路径；不走注册源 callback。 |
| 原版与 C18 不同空间 | `move_item` transfer 编排：Native serialize → Native load/place → Native remove；写前先建 journal。 |
| 原生空间 ↔ 注册源 | transfer 编排：原生 serialize/load/place 与注册源 export/import/remove 按来源/目标方向组合；先完整写目标再移除来源。 |
| 同一注册空间 | source `export_item` → source `import_item` → source `remove`；来源移除失败时移除目标补偿。 |
| 不同注册空间 | 与原生/注册源跨空间一致，走 journal 和两侧来源原语；目标写入成功前不删源。 |

跨空间统一编排见 `usecases/move_item.cpp`、`bags/native_transport.cpp`、`bags/source_transport.cpp`；同注册空间路径在 `usecases/item_action.cpp`。同空间原生的游戏内部移动与跨空间 payload transfer 是不同路径。

### 合并规则

- 原生拖拽 wrapper 只处理原生物品：扩展来源或非原生句柄会被保护，不让原版合并路径接管。事件 `0x02` 可转给 projection drop callback；事件 `0x04` 才检查原生合并，见 `hooks/merge_hook.cpp`。
- 自动合并判定要求源和目标都是 `count-encoded` 物品、类别相同、双方 count 有效、目标 count limit 可读且合并后不超过上限；`can_apply_stuff` 必须成功返回“不接管”。若材料操作接管、事实未知或门禁未就绪，不执行该合并路径。
- wrapper 仅在源堆可完整并入目标剩余容量时执行；超过剩余容量不在该 gate 中拆为部分堆叠。通过 action/transfer 指定部分数量时，`QolBagTransfer.count` 与目标导入数量仍须一致。Native `move_handle` 和各注册源 `import_item` 的落槽/堆叠由对应原语完成。
- `inventory.action` 与 transfer 明确排除任务袋 `(0,5)`。UI 投影中的扩展物品不能作为原生合并输入。原生拖拽 wrapper 直接调用独立 `QolMoveMergePort`；其当前 bag 参数检查范围是 `0..5`，未在该 adapter 明确排除索引 5（`hooks/merge_hook.cpp`、`game/adapter_ports/move_merge_port.cpp`），因此不能把 action 层排除自动视为 hook 路径已证明。

use/discard 由 Host 原生写接口或来源 `use/remove` 执行；sell 按报价、移除、加钱的次序处理，加钱失败时回滚物品；equip/unequip/put_jewel 的当前注册源限制见上表。成功写入后刷新库存快照。

## 跨空间 journal 与恢复

### 存储格式

| 项目 | 当前实现 |
|---|---|
| owner store section | `item.transfer.journal`，section version 1；实现见 `usecases/transfer_journal.cpp`。 |
| header | 6 字节：`uint16_t magic`、`uint16_t format_version`、`uint16_t count`。 |
| record | 72 字节；最多 8 条。结构由静态断言锁定。 |
| record 字段 | 16 字节 id、state 与 reserved、source/target 的 space/bag/slot、category、count、对齐字段、`uint64_t payload_hash`、时间戳。 |
| state | `0=intent`；`1=target_written`。intent 在导出前写入，目标导入后写 target_written。 |
| payload hash | 存储 payload 的 64 位 FNV-1a hash；计算见 `bags/transfer.h`。 |
| 满/不可建 | 达到 8 条，或当前存档槽可用但 intent 持久化失败时，新的跨空间 move 返回 blocked，不继续改物品。 |

Journal 是 intent log，不保存 payload 本体。执行次序：写 intent → source export → target import → 写 target_written/hash → source remove → 成功清 journal 并刷新库存。export/import 失败时清 journal；source remove 失败时尝试删除目标补偿；补偿成功清 journal 并返回源错误，补偿失败保留记录并返回 internal error。相关路径见 `usecases/move_item.cpp`。

### 启动 reconcile

`plugin_install` 与 `plugin_on_enabled` 都调用 `reconcile()`。当前恢复按 source/target 槽存在性处理；虽记录 `intent` / `target_written` 两态，reconcile 不按 state 分支：

| source | target | 恢复动作 |
|---|---|---|
| 存在 | 存在 | 删除 target、保留 source；成功后清记录。 |
| 不存在 | 存在 | 视为目标已存在，保留 target 并清记录。 |
| 存在 | 不存在 | 视为源仍保留，清记录。 |
| 不存在 | 不存在 | 记 WARN 后清记录。 |

查找/删除遇到不可判定错误会中止并返回 false；删除 target 失败则保留记录。当前生命周期忽略 `reconcile()` 返回值，且 `transfer_journal::count()` 未接入保存拦截；因此，未恢复 journal **不阻止存档**。若无法取得当前存档槽（`save.facts` 不可用或查询失败），记录仅保存在内存并告警；该降级允许 transfer 继续，但重启后不具备落盘恢复保障。代码依据：`usecases/transfer_journal.cpp`、`runtime/lifecycle.cpp`。

## 读取、快照与分页

`read_inventory()` 在游戏线程读取 ITEM port 的原版/C18 空间，再按已注册来源读取 `space>=2`。优先走空间枚举或来源 `for_each_item`，不可用时按袋容量读取；只物化占用槽。读取 money 后发布不可变 `InventorySnapshot`，HTTP GET 从共享快照构造 JSON，不直接遍历游戏对象。实现见 `usecases/read_inventory.cpp`、`bags/snapshot_store.cpp`、`http/query_handler.cpp`。

| 时机 | epoch / 行为 |
|---|---|
| 显式 `request_inventory_refresh()` | bump inventory epoch，立即调用 `read_inventory()`；成功发布时再以新 epoch 发布快照。 |
| 写动作成功 | 调用库存刷新入口，使后续查询读到重建快照。 |
| 周期兜底 | lifecycle scheduler 每 60 帧调用 `frame_tick()`；其中执行库存读取、item state 更新和 shop snapshot refresh。 |
| shop | shop snapshot 单独维护 epoch；读写刷新由 `shop/` 管理。 |
| HTTP GET | 只读当前不可变快照；不在请求线程访问原生库存，也不因游标自动触发 refresh。 |

`item.state` 的 raw 外形保留原版袋 0..5、C18 后接展示段；注册源段由 `count/info/item_at` 结构化生成，enriched JSON 由 item owner 构造，不拼接来源方 JSON 片段。见 `contracts/item_state.h`、`api/state.cpp`。

### 分页

| 项目 | 当前规则 |
|---|---|
| 路由 | `GET /api/item/inventory` 返回袋元数据及物品；`GET /api/item/inventory/items` 只返回物品。 |
| 参数 | `cursor`、`limit`、可选 `space` / `bag`；`space` 指定时 bag 是空间内索引，否则 bag 按扁平袋号解析。 |
| 游标 | 文本形式 `space:bag:slot`，按三元组做 cursor 比较；空 cursor 等同未提供，非法格式 400。 |
| limit | 默认 100；允许范围 1..500，非法值 400。 |
| 响应 | `next_cursor`、`has_more`；不输出总量。 |
| 顺序边界 | 比较键是 `(space,bag,slot)`；快照组装按空间/注册表枚举。注册源顺序可能变化，且翻页期间库存可变；不把扁平袋号或 cursor 当持久身份。 |

查询常见结果：无可读快照 503；非法参数/cursor/limit 400；袋或槽不存在 404；空槽 body 为 `null`；富化或响应拼装失败 500。具体 HTTP 参数与响应字段以[HTTP API 参考](../api-reference.md)为准。

## 投影与原生拖拽 wrapper

`inventory.projection` 的注册表归 `core.item`；`extension_bag` 注册自己的 projection callbacks。显示层由 extension_bag 负责，core.item 不把显示控件当作原生库存事实源。扩展袋投影不转移袋数据所有权，也不使扩展物品成为 native item。

`core.item` 自己持有 `ptrslot:MOVE_MERGE_GOT` wrapper。wrapper 的事件 `0x02` 允许 projection drop callback 处理；事件 `0x04` 保护注册源/非原生物品并检查 native 输入门禁，符合前节规则才走原生合并。成功后按需同步投影槽、刷新原版库存 UI、清 touch cursor。feature 关闭或 wrapper 未就绪时不启用合并。代码见 `api/projection.cpp`、`hooks/merge_hook.cpp`、`plugins/extension_bag/assembly/projection_registration.cpp`。

## HTTP surface

当前 inventory/shop 常规路由共 17 条：15 条 inventory + 2 条 shop；另有 4 条 OP-gated inventory route。声明与门控见 `plugins/core.item/routes.inc`、`plugins/core.item/shop/routes.inc`。普通路由 `lease_required=1`、`op_gated=0`；OP 路由 `lease_required=1`、`op_gated=1`。

### 常规路由：15 条 inventory + 2 条 shop

| 方法 | 路径 | 目的 |
|---|---|---|
| GET | `/api/item/inventory` | 袋元数据与分页物品列表。 |
| GET | `/api/item/inventory/money` | 当前金钱。 |
| GET | `/api/item/inventory/items` | 仅分页物品列表。 |
| GET | `/api/item/inventory/bag/{bag}/info` | 单袋容量与占用槽数。 |
| GET | `/api/item/inventory/bag/{bag}/{slot}` | 单槽；空槽返回 `null`。 |
| POST | `/api/item/inventory/use_item` | 使用物品。 |
| POST | `/api/item/inventory/accept_dice` | 接受待结算骰子结果。 |
| POST | `/api/item/inventory/reject_dice` | 拒绝待结算骰子结果。 |
| POST | `/api/item/inventory/discard_item` | 丢弃物品/数量。 |
| POST | `/api/item/inventory/sell_item` | 出售物品。 |
| POST | `/api/item/inventory/move_item` | 移动物品到袋/槽。 |
| POST | `/api/item/inventory/{role}/put_jewel` | 宝石放入角色装备槽。 |
| POST | `/api/item/inventory/{role}/enchant` | 角色装备附魔写命令。 |
| POST | `/api/item/inventory/{role}/equip_item` | 装备物品或按类别执行装备写命令。 |
| POST | `/api/item/inventory/{role}/unequip_item` | 从角色装备槽卸下物品。 |
| GET | `/api/item/shop/items` | 查询当前商店货架。 |
| POST | `/api/item/shop/buy_item` | 购买商店物品。 |

### OP-gated 路由：4 条

| 方法 | 路径 | 当前实现 |
|---|---|---|
| POST | `/api/op/inventory/money` | OP 设置金钱；Native typed command。 |
| POST | `/api/op/inventory/add` | OP 添加物品；Native typed command。 |
| POST | `/api/op/inventory/set-slot` | 仅 owner metadata；Kotlin 返回 501 `not implemented`。 |
| POST | `/api/op/inventory/set-equip` | 仅 owner metadata；Kotlin 返回 501 `not implemented`。 |

HTTP POST 在请求线程做参数校验并投递 typed command，先返回 ticket；写端口在游戏主线程执行，最终业务结果由 ticket result handler 给出。`/api/item/inventory/{role}/enchant` 是独立写命令，不属于 `inventory.action` 3.0 方法表。handler 位于 `http/write_handler.cpp`、`http/write_results.cpp`。

## 状态、失败与安全规则

### 状态枚举

`InventoryActionStatus`：`kOk`、`kNotInWorld`、`kInvalidRef`、`kStale`、`kEmpty`、`kTaskBagExcluded`、`kConflict`、`kFull`、`kUnsupported`、`kInternal`、`kCreditFailed`、`kRollbackFailed`、`kExtensionSaleUnavailable`。

`InventorySellStatus`：`kOk`、`kNotInWorld`、`kTaskBagExcluded`、`kEmptySlot`、`kNoItemHandle`、`kStale`、`kSymbolsUnresolved`、`kNoSell`、`kPriceInvalid`、`kCreditFailed`、`kRemoveFailed`、`kRollbackFailed`、`kExtensionSaleUnavailable`。完整定义及顺序见 `contracts/inventory_action.h`。

| 层 | 失败表达 |
|---|---|
| Action API | 用 action/sell status 表达业务失败；过期身份 `kStale`，任务袋 `kTaskBagExcluded`，注册袋不支持的动作 `kUnsupported`。 |
| Bag source / transfer | `kQolErrInvalidArgument`、`kQolErrNotFound`、`kQolErrNotReady`、`kQolErrConflict`、`kQolErrBlocked`、`kQolErrUnsupported`、`kQolErrInternal`。 |
| HTTP GET | 400 参数错误、404 不存在、500 内部/响应失败、503 快照不可读。 |
| HTTP POST | 参数错误 400；命令 port/队列不可用 503；已接收时返回 ticket，业务结果异步读取。 |

### 不变量与线程

1. 所有 action 在游戏主线程执行；调用方显式传入 `{space,bag,slot}`，不延迟解析“当前页签”。
2. 写前按 ref 重新读取并复核 `{slot,generation}`；句柄任一字段为 0 无效，过期后不得重试旧句柄。定义见 `contracts/item_handle.h`。
3. `(0,5)` 不可作为 action 源/目标；注册源空间号不得写入原生 `current_bag`。
4. HTTP GET 仅读取不可变 snapshot；HTTP POST 不在请求线程直接改物品。袋源注册与 callback 仅在主线程。
5. 目标完整写入前不移除 transfer source；源移除失败必须尝试目标补偿；补偿失败不得报告成功并保留 journal。
6. 缺少袋数、容量、来源或原生写原语时 fail-closed；不得按相同 bag 数字猜测物品归属或伪造新对象替代 payload。
7. 写操作成功后请求 inventory refresh；epoch 用于快照版本，不是裸指针/袋位置的替代身份。
8. 插件 descriptor 声明 `deactivate_allowed=false`；停用不得绕过 Host 对库存 wrapper 资源的生命周期约束。

## 数据流摘要

| 操作 | 数据流 |
|---|---|
| 读 | HTTP GET → 已发布快照 → 当前页 JSON；游戏线程 `read_inventory` → ITEM port 与注册源 → `InventorySnapshot` 原子发布。 |
| 写 | HTTP 校验 → typed command/ticket → 游戏主线程 action/usecase → Host port 或 source callback → 结果回写 → 成功后刷新快照。 |
| 跨空间转移 | action → journal intent → source export → target import → target_written → source remove → 完成清记录；失败按补偿规则收敛。 |
| 来源生命周期 | activate 主线程注册 `QolBagSource` → generation 变化；deactivate 主线程注销 → 后续快照反映来源变化。 |

## 验收、真机证据与风险

| 范围 | 当前代码级验证入口 |
|---|---|
| descriptor、能力、端口、feature 与路由 metadata | `tests/test_item_facts_plugin.cpp`、`tests/test_item_facts_dispatch.cpp` |
| 查询分页、raw inventory、HTTP 写校验与结果 | `tests/test_item_facts_dispatch.cpp` |
| 动作出售、扣款/回滚 | `tests/test_inventory_action_sell.cpp`、`tests/test_inventory_trade_sell.cpp` |
| HTTP 移动/注册源路径 | `tests/test_item_facts_dispatch.cpp`；当前无独立 transfer journal 测试文件。 |
| 袋映射、句柄身份 | `tests/test_bag_catalog.cpp`、`tests/test_item_handle_registry.cpp`、`tests/test_item_handle_custody.cpp`、`tests/check_item_handle_contracts.cmake` |
| hook、拖拽合并 wrapper | `tests/test_inventory_hook_stage4.cpp`、`tests/test_move_merge_plugin.cpp` |
| 静态资源/HTTP route contract | `scripts/maintenance/check_arch.py`、`scripts/maintenance/check_http_routes.py` |

设备行为按[验证规则与验收卡](../../refactor/verification.md)执行；Host/static 测试不替代验收卡要求的真机证据。构建、部署和设备取证流程见[构建与部署指南](../../guides/build-and-deploy.md)。当前文档只说明验收边界，不把代码测试通过等同真机验收通过。

| 风险 | 影响与控制 |
|---|---|
| Native hooks 与 backup | 多个入口改变库存行为；任一 required hook/backup 未就绪必须 fail-closed，行为改动需按验收卡做设备冒烟。 |
| 任务袋拖拽 gate | action/transfer 明确拒绝 `(0,5)`；独立原生 merge port 接受 `0..5` 的 bag 参数，任务袋 hook 行为需单独以代码/设备证据核对。 |
| 注册源原子性 | 来源 callback 若在失败时部分修改 sidecar，core.item 无法可靠推断/修复；必须遵守 export 只读、import/remove 失败不变式。 |
| C18 缓存 | 探测只在 cache key 变化或显式失效时重算；布局/袋表变化时需确认失效入口被 Host 生命周期调用。 |
| Journal 保存边界 | 当前 reconcile 失败不阻止存档，且无 save slot 时仅内存降级；不可将 journal 描述成强制保存锁。 |
| Journal 写入阶段 | `mark_target_written()` 的 store write 结果不向上返回，reconcile 也不区分两种 state；不能把两阶段记录描述为强一致事务。 |
| 分页与注册顺序 | 注册源枚举顺序变化或翻页期间库存变更会影响游标连续性；客户端不得持久化扁平袋号或假定跨快照无重复/跳过。 |
| 无容量总上限 | 大容量来源应优先实现枚举；逐槽 fallback 的成本随容量增长。 |

## 已删除的旧结构

- `ExtensionBagApi`：已从当前 core.item 实现移除；外部来源改用 `inventory.bag_source` 注册契约。
- `QolInventoryExtensionProvider`：不再是 core.item 袋源读写接口或扩展袋识别入口。
- provider-first arbitration：当前读路径分别读取 ITEM port 的空间 0/1 与注册 `QolBagSource` 空间，不再由 provider 优先认领并仲裁袋号。
- `extension_source_bridge`：过渡适配器不属于当前目录或运行路径；扩展袋直接注册 `QolBagSource`。
- `inventory.merge_projection`：旧提供面由 `inventory.projection` 回调注册表替代。
- 旧 fragment state：扩展库存段不再拼接来源方 JSON 字符片段，当前由袋源 `count/info/item_at` 结构化生成。
- 旧 fragment state 与 provider 所有权均不是兼容 fallback；core.item 不保留双读/双写路径。
