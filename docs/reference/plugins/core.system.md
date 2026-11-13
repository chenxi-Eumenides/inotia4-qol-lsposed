# `core.system` 插件

## 1. 作用与架构定位

`core.system` 是系统信息 query provider，仅聚合系统/运行状态查询，不承担通用系统管理或业务领域逻辑。它从 Game Host system port 取得定型 JSON，通过 Host SDK 快照与 HTTP capability 协作，保持 Kotlin transport、native owner 和 JNI bridge 的职责边界。

## 2. 功能面

### HTTP 路由

routes.inc 与 descriptor 均将以下五个 GET handler 归于 `core.system`：

| 路径 | 内容 |
|---|---|
| `/api/system/snapshot` | 系统聚合快照 |
| `/api/system/game` | 游戏状态聚合 |
| `/api/system/info` | 版本、身份及存档相关系统信息 |
| `/api/system/game_frame` | 游戏帧信息 |
| `/api/system/events` | 系统事件 JSON |

正式响应定义见 [API 参考 §6、§7.9](../api-reference.md)。诸如 `/api/system/tables/*`、`/api/system/help` 等静态资料路由由 `core.data` 拥有；`/api/system/features`、tickets、routes 等由 `core.api` 持有，不属于本插件。

### Capability 面

- 不提供跨插件业务 capability。
- 必需 capability：`http.registry`、`http.dispatch`。
- 必需 `QOL_PORT_SYSTEM`（descriptor 要求 ABI 1.1）。

### 注册 feature

| feature id | 默认 | 分类 | 类型 |
|---|---:|---|---|
| `core.system.snapshots` | 1 | Hidden | toggle |

快照服务为常驻需求，用 default true 的 hidden feature 表达；`enable`（Host 独占 lifecycle 期望）与 feature 由 Host 管理，插件不读写。

### 系统补丁（无开关，常驻）

除查询面外，`core.system` 还声明 8 条 `kCallPatch` + `kWordReplace` 资源，恒开（`gate=nullptr`、无 feature 开关），随插件启用安装、停用物理还原：

| 资源 id | 站点 | 语义 |
|---|---|---|
| `patch:MAPITEMSYSTEM_AllocateSlot+0x18` | `P_MAPITEMSYSTEM_ALLOCATE_SLOT_CAP` | 地面掉落槽位上限 32→127 |
| `patch:MAPITEMSYSTEM_Create+0x4` | `P_MAPITEMSYSTEM_CREATE_POOL_SIZE` | 地面掉落池分配 0x400→0xfe0 |
| `patch:MAPITEMSYSTEM_Initialize+0x34` | `P_MAPITEMSYSTEM_INITIALIZE_POOL_LIMIT` | 切图初始化循环界 0x400→0xfe0 |
| `patch:MAPITEMSYSTEM_Unload+0x28` | `P_MAPITEMSYSTEM_UNLOAD_POOL_LIMIT` | 切图卸载循环界 0x400→0xfe0 |
| `patch:ITEMPOOL_Create+0x4` | `P_ITEMPOOL_CREATE_SIZE` | 物品对象池分配 0x2ee0→0xfff0（300→1638 槽） |
| `patch:ITEMPOOL_Initialize+0x18` | `P_ITEMPOOL_INITIALIZE_LIMIT` | 物品对象池初始化循环界 0x2ee0→0xfff0 |
| `patch:ITEMPOOL_Allocate+0x18` | `P_ITEMPOOL_ALLOCATE_BOUND_HI` | 池扫描上界高 12 位 `#0x2,lsl#12`→`#0xf,lsl#12` |
| `patch:ITEMPOOL_Allocate+0x1c` | `P_ITEMPOOL_ALLOCATE_BOUND_LO` | 池扫描上界低 12 位 `#0xee0`→`#0xff0` |

- **地面掉落池**：`MAPITEMSYSTEM_AllocateSlot` 在计数 `>0x1f` 时销毁最旧槽，计数为 `int8`，理论上限 127 槽；四条必须同组，缺一条会写穿池或越过池尾。安装时宿主先执行 `game::drop_pool_ensure_capacity(127)`：池已存在则 `MEM_Malloc(0xfe0)` + 拷贝 + 释放旧池并改写 `MAPITEMSYSTEM_pPool`，池未创建则交给补丁后的 `Create`；失败时四条同败。
- **物品对象池**：`ITEMPOOL` 是堆上连续数组（每对象 `0x28` B），`Create` 分配、`Initialize` 逐对象初始化、`Allocate` 线性扫到池尾。三条必须同组：只改分配大小会在初始化时越过池尾，只改初始化会把新增槽留成未初始化脏 flag，只改一半扫描界会扫出池外。1638 槽 = `0xfff0` B 是两个 `MOVZ #imm16` 单字补丁能编码的最大 0x28 整数倍。
- **变体一致性**：原版 v1.3.2、C18L v603、monster v29、盗版大修 v0810、幻想 v1.2 五个 APK 的 arm64 `libgame.so` 在相同偏移处指令字逐字一致，无需按变体登记备用字；布局不同的变体按 expected 字校验失败后 fail-soft 跳过。
- **停用语义**：字补丁还原，已扩大的池保留到下一次 `Create`；停用时若地面仍有 >32 件掉落，原版 `Unload` 只清前 32 槽，其余物品对象残留在 `ITEMPOOL`。

## 3. 依赖前置与消费者


- Game Host 注入 system port，其 `body_json` 读取 snapshot/game/info/frame 类型的只读投影，`events_json` 提供事件查询体。
- Host SDK 使用 snapshot 存储、帧调度与日志接口；该插件没有写命令、票据 handler 或业务写 port。
- HTTP 客户端经 Kotlin `GameController`/`EventsController` 和 service/query bridge 消费。Kotlin 侧负责 socket、请求生命周期、响应写回；native 插件只处理 owner query 数据。

## 4. 职责边界

- 拥有五条系统信息只读 HTTP query handler 和其快照读取/响应路由。
- 拥有对象池容量补丁（`MAPITEMSYSTEM` 地面掉落池 127 槽、`ITEMPOOL` 物品对象池 1638 槽，共 8 条 claim）；不拥有掉落倍率、重抽与池满兜底语义，那些归 `world_tuning`。
- 不拥有静态数据查询（`core.data`）、HTTP registry/全局 feature 状态与 tickets（`core.api`）、角色/库存/任务事实或写操作。
- Kotlin host 维持 AndServer、解析及传输；JNI/NativeBridge 仅作为既有窄桥接入口；native `core.system` 以 `QOL_PORT_SYSTEM` 获取数据并序列化响应，不建立 socket、不承担通用 HTTP router。

## 5. 核心实现流程

- **读/快照**：每 15 帧通过 system port 读取 snapshot、game、info、frame 四类 JSON，校验缓冲容量后发布 `Snapshot<SystemSnapshot>`。snapshot/frame 是核心可用数据；若 game/info 身份尚未下发，可对应暂不可用而不阻止其余数据发布。核心读取失败保留上一份完整快照并告警。
- **事件查询**：`/api/system/events` 经 port 的 `events_json` 读取并直接填充 HTTP response buffer，不经过四类快照缓存。
- **写/命令/票据**：无 native 写路由、owner command 或 ticket result handler。
- **生命周期**：install 校验 SDK、system port ABI 与 `body_json/events_json`；enable 初始化快照并启动 15 帧任务；disable/deactivate 停任务、销毁快照、清除端口引用。路由随 descriptor/宿主 owner 生命周期注册和撤销。

## 6. 文件地图

| 子职责 | 文件 | 职责 |
|---|---|---|
| system query owner | `module/app/src/main/cpp/plugins/core.system/plugin.cpp` | descriptor、system 快照读取、五条 query handler 与生命周期 |
| 插件声明 | `module/app/src/main/cpp/plugins/core.system/plugin.h` | 插件入口声明与 `kResourceCount` |
| 补丁登记 | `module/app/src/main/cpp/game/access/patches.cpp` | `P_MAPITEMSYSTEM_*` / `P_ITEMPOOL_*` 站点（名称 → 地址/原字/替换字） |
| 补丁常量 | `module/app/src/main/cpp/game/symbols/game_symbols.h` | 两组站点的 VMA、指令偏移、原字/新字与 `ITEMPOOL_CAPACITY` |
| 快照封装 | `module/app/src/main/cpp/plugins/core.system/snapshot_sdk.h` | Host SDK 快照接口包装 |
| Game Host 契约 | `module/app/src/main/cpp/contracts/game_ports/system_port.h` | system JSON 与 events 读取端口 |
| Kotlin query 层 | `module/app/src/main/java/com/inotia4/qol/api/controller/GameController.kt`、`EventsController.kt` | Kotlin 请求解析与 query service 调用 |
| JNI bridge | `module/app/src/main/java/com/inotia4/qol/NativeBridge.kt` | 既有 native query 桥接签名；不承载业务聚合 |

接口规格：[HTTP API 参考](../api-reference.md)。
