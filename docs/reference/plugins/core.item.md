# `core.item` 插件

## 1. 作用与架构定位

`core.item` 是物品/库存领域的官方 provider/owner，负责物品事实、库存状态投影、物品查询及物品写命令。它依照 `ARCHITECTURE.md` 通过 typed capability、命令与 Game Host ports 实现，不依赖 Loader/Game Adapter 私有源码。

## 2. 功能面

### HTTP 路由

以下路由以 `plugins/core.api/routes.inc` owner 和 `core.item` descriptor handler 为准：

| 方法 | 路径 |
|---|---|
| GET | `/api/item/inventory` |
| GET | `/api/item/inventory/money` |
| GET | `/api/item/inventory/items` |
| GET | `/api/item/inventory/bag/{bag}/info` |
| GET | `/api/item/inventory/bag/{bag}/{slot}` |
| GET | `/api/item/shop/items` |
| POST | `/api/item/inventory/use_item` |
| POST | `/api/item/inventory/accept_dice` |
| POST | `/api/item/inventory/reject_dice` |
| POST | `/api/item/inventory/discard_item` |
| POST | `/api/item/inventory/sell_item` |
| POST | `/api/item/inventory/move_item` |
| POST | `/api/item/inventory/{role}/put_jewel` |
| POST | `/api/item/inventory/{role}/enchant` |
| POST | `/api/item/inventory/{role}/equip_item` |
| POST | `/api/item/inventory/{role}/unequip_item` |
| POST | `/api/item/shop/buy_item` |
| POST | `/api/op/inventory/money` |
| POST | `/api/op/inventory/add` |

`/api/op/inventory/set-slot` 与 `/api/op/inventory/set-equip` 的 owner 元数据为 `core.item`，当前是 Kotlin 501 占位，不是 native descriptor handler；`core.item` 实际 handler 不包含这两项。正式 API 定义与写结果细节见 [API 参考 §3、§7.9–§7.10](../api-reference.md)。

### Capabilities

- 提供 `item.facts`：物品分类、装备/宝石/禁售判断、价格、堆叠计数、inventory item ref/view、详情上下文及金钱查询等窄事实接口。v3.5 的 inventory view 追加 `native_options[]`/`jewel_options[]`/`enchant_options[]` 分词列表；`options[]` 仍是全量合并列表，现有 JSON 投影不变，附魔列表在属性未解码前恒为空。
- 提供 `item.state`：按 kind 输出 raw 或 enriched inventory JSON 投影。
- Descriptor 必需 `http.registry`、`http.dispatch`；可选依赖 `character.state`，用于角色状态投影相关写操作。

### 注册 feature

| feature id | 默认 | 分类 | 类型 |
|---|---:|---|---|
| `core.item.provider` | 1 | Hidden | toggle |

provider 为常驻需求，用 default true 的 hidden feature 表达；`enable`（Host 独占 lifecycle 期望）与 feature 由 Host 管理，插件不读写。

## 3. 依赖前置与消费者

- 必需 ports：`QOL_PORT_ITEM`、`QOL_PORT_ITEM_MONEY_WRITE`、`QOL_PORT_ITEM_DICE_WRITE`、`QOL_PORT_ITEM_INVENTORY_STATE_WRITE`。
- Host SDK 使用快照、帧调度、能力查找、typed command/ticket result handler 与日志。核心逻辑通过 Item Port 获取中立的 inventory-extension provider access；不会硬依赖某个扩展袋实现。
- `item.facts`/`item.state` 的消费者包括角色状态投影与多个物品/库存相关 feature；运行时真实消费者通过 `capabilities.require` 查找。Kotlin API Service/Controller 负责 HTTP 请求和 JSON 传输，不拥有物品语义。

## 4. 职责边界

- 拥有物品 facts、原版库存读取/序列化、查询投影、商品列表读取，以及物品操作命令及票据结果。
- **不拥有扩展背包语义**：扩展内容经中立 inventory-extension provider 接口贡献 fragment、item ref 等；袋归属和 provider 数据由扩展背包插件提供。`core.item` 不把扩展背包当作自身功能。
- 不拥有角色事实（`core.character`）、静态商品/文本数据（静态数据 provider）、低层内存读写（Game Adapter/Game Host），也不承接 HTTP 监听、传输或通用 JSON router（Kotlin host/`core.api`）。

## 5. 核心实现流程

- **读/快照**：每 15 帧在游戏线程检查 world 与静态数据准备状态，序列化原版库存、扩展 provider 可用时追加其 JSON fragment，并读取商店与 money；完整后发布 `Snapshot<ItemSnapshot>`。HTTP 线程从不可变快照取数据，再执行名称/物品字段富化、展开或袋/槽筛选。失败不发布半成品。
- **Capability 投影**：`item.state` 的 raw/enriched 输出复用同一 inventory serializer；enriched 通过现有字段富化逻辑生成。character state capability 是按需查询，不跨帧缓存接口指针。
- **写/命令/票据**：POST handler 解析输入并生成 typed 命令；HTTP 线程只提交队列并返回 ticket。主线程结果 handler 调用相应物品写 port；需要时采集操作前后库存/角色快照，形成与操作相关的结果。扩展项目动作通过中立 provider 通道转发，写入执行仍由实际 owner/provider 完成。
- **生命周期**：install 校验所有必需 ports、SDK 与快照初始化；enable 注册 15 帧任务并发布快照；disable/deactivate 移除任务、销毁快照、注销结果处理器并清除端口引用。optional capability 缺失时相关增强/投影按实现降级。

## 6. 文件地图

| 子职责 | 文件 | 职责 |
|---|---|---|
| 主 owner | `module/app/src/main/cpp/plugins/core.item/plugin.cpp` | descriptor、capabilities、快照、查询/写命令及结果处理 |
| JSON 处理 | `module/app/src/main/cpp/plugins/core.item/json_lite.h`、`json_lite.cpp` | 有界 JSON 解析与物品字段转换 |
| API 响应构造 | `module/app/src/main/cpp/plugins/core.item/response.h`、`response.cpp` | 查询输出富化、库存/商店投影 |
| 插件/快照声明 | `module/app/src/main/cpp/plugins/core.item/plugin.h`、`snapshot_sdk.h` | 模块入口及 Host 快照封装 |
| 公共契约 | `contracts/item_facts.h`、`contracts/item_state.h`、`contracts/inventory_extension.h`、`contracts/game_ports/item_port.h` | capability、扩展 provider 和 Game Host 端口类型 |
| Kotlin 请求入口 | `module/app/src/main/java/com/inotia4/qol/api/controller/InventoryController.kt`、`InventoryActionController.kt`、`ShopController.kt` | HTTP 参数/请求转交，不实现 native 领域语义 |

接口规格：[HTTP API 参考](../api-reference.md)。
