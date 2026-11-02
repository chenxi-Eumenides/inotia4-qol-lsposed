# `core.data` 插件

## 1. 作用与 owner 定位

`core.data` 是静态数据 owner 插件，注册 ID 为 `core.data`、版本 `0.1.0`；负责静态地图/数据表/文本/剧情资产的 native 查找、解析与 HTTP 输出，并提供窄 `static.data` capability。Kotlin 启动代码只传输打包资产原始字节，不承担这批资产的 native 查询逻辑（`module/app/src/main/cpp/plugins/core.data/plugin.cpp:21-25,554-560`；`module/app/src/main/java/com/inotia4/qol/CoreDataAssets.kt:6-7`）。

## 2. 功能面与 HTTP 路由

descriptor 注册 12 条 GET query routes（`module/app/src/main/cpp/plugins/core.data/plugin.cpp:431-474`；route owner metadata 见 `plugins/core.api/routes.inc:27-38`）：

| 数据面 | 路径 |
|---|---|
| 静态地图 | `/api/world/maps/list`、`/api/world/maps/{map_id}`、`/api/world/maps/{map_id}/tiles`、`/api/world/maps/{map_id}/exits` |
| 静态表 | `/api/system/tables`、`/api/system/tables/{table}`、`/api/system/tables/{table}/search`、`/api/system/tables/{table}/download` |
| 文本/剧情 | `/api/system/tables/text`、`/api/system/tables/story-events` |
| 帮助/下载 | `/api/system/help`、`/api/system/download` |

其中 `/api/system/tables/{table}/download`、`/api/system/help`、`/api/system/download` 当前固定返回 HTTP 501 `not implemented`（`plugin.cpp:402-429`；API 契约与占位说明见 [API 参考 §7.4–§7.5](../api-reference.md)）。其余 HTTP 参数、状态码和成功响应只以 [API 参考](../api-reference.md) 为准；本说明不复制完整 schema。地图路由的 API 契约见 API 参考 §3.3。

## 3. 使用的模块功能与消费者

- **Optional requires**：`http.registry` 1.1、`http.dispatch` 1.1（`module/app/src/main/cpp/plugins/core.data/plugin.cpp:29-34`；公共版本见 `contracts/http.h:96-99`）。缺失或撤销 HTTP provider 只影响本插件的 HTTP endpoints，不级联停用 `static.data` capability 或其他非 HTTP 功能；provider 恢复后按可选依赖生命周期重新接线。
- **Provides**：`static.data` 1.1；`QolStaticDataApi` 暴露 `map_name(map_id, out, capacity, out_size)`、`map_exits(map_id, out, capacity, out_count)`、`map_info(map_id, out)` 与 `map_tiles(map_id, out_tiles, capacity, out_size)`（`plugin.cpp` 的 static API 实现与 `kProvides`；`contracts/static_data.h`）。`map_info` 返回逻辑宽高，`map_tiles` 返回 `kStaticMapTileSize`（4096）字节的 64×64 通行矩阵（`kStaticMapTileDim=64`，bit3=不可通行）。
- **数据分源**：地图逻辑宽高与瓦片矩阵来自 `maps/tiles.json`（每 map 一项，`width`/`height` 缺失按 64、瓦片 base64 解码后不足 4096 字节视为不可用）；传送点/出口来自 `maps/exits.json`，不在瓦片矩阵内，`map_exits` 的语义与来源保持不变。
- **Ports**：descriptor 没有 Game Host port requirement（`plugin.cpp:582-600`）。
- **HTTP 接线**：descriptor 声明自己的 12 个 typed GET endpoint handlers，并声明可选 `http.registry`/`http.dispatch` dependencies；Host binder 按 plugin lifecycle owner 接线/撤销 handler。`routes.inc` 与 `core.api/routes.inc` 是同步/校验使用的 route facts，不由 `plugin_install` 动态注册 handler（`plugin.cpp` 的 `kHttpEndpoints`/`kDescriptor`）。
- **Capability consumer**：当前源码已确认 `static.data` 的提供端及四个函数面；没有确认到生产插件通过 capability registry 要求该接口的 consumer，因此不指定消费者。HTTP 路由调用者与 `static.data` capability consumer 是不同关系，不据此推断消费者。

## 4. 职责范围与边界

- `core.data` 拥有上述 12 条静态查询路由与静态 map name/exits 能力；实时地图事实及移动归 `core.world`，`core.data` 不读取 live 游戏状态（路线区分见 `routes.inc:20-30`；目标归属见 `ARCHITECTURE.md` §5.2）。
- Kotlin `ApiServer` 负责 AndServer/socket 与 HTTP transport；`DataController` 将 HTTP controller 调用转发至 native query，`CoreDataAssets` 只导入 asset bytes（`module/app/src/main/java/com/inotia4/qol/ApiServer.kt:172-210`、`api/controller/DataController.kt:10-60`、`CoreDataAssets.kt:6-35`）。
- 本插件不拥有静态数据资产的原始 APK 来源，也不把配置或 Kotlin JSON parser 作为第二份静态数据实现；API response schema 仍归 [API 参考](../api-reference.md)。

## 5. 对外提供与依赖影响

对外提供 `static.data` 1.1 的地图名、出口、地图宽高与瓦片矩阵读取函数（出口来自 `maps/exits.json`，宽高/瓦片来自 `maps/tiles.json`），以及在 HTTP provider 可用时接线的静态 GET routes。Host binder 根据 descriptor 注册/撤销 query handlers；缺少可选 HTTP provider 不改变 `static.data` capability 和其他非 HTTP 功能（`plugin.cpp` 的 endpoint/descriptor 声明与生命周期回调）。静态资产导入是独立 JNI/启动期路径，不在 plugin install 中解析，也不由 plugin deactivate 清空（`ApiServer.kt`；`gamebridge_core_data.cpp`；`plugin.cpp` 的 asset import API）。

## 6. 原始资产读写、线程与生命周期机制

- **资产输入**：`ApiServer.initFeatures` 调用 `CoreDataAssets.load`；Kotlin 递归列举 `static-data` 下 JSON 文件、核算原始字节并经 JNI 分批提交，不在该入口解析文件内容（`module/app/src/main/java/com/inotia4/qol/ApiServer.kt:65-80`；`CoreDataAssets.kt:6-35,37-65`）。每文件上限 16 MiB、总量上限 32 MiB、文件数上限 128；单文件/总量检查也由 Kotlin ingress 实施，文件数上限由 native begin/import 实施（`CoreDataAssets.kt:8-10,15-30`；`plugins/core.data/plugin.cpp:21-25,602-628`）。
- **native staging/提交**：`begin_asset_import` 清空 staging 并记录预期文件数/总字节；`import_asset` 校验相对路径与容量后拷贝 raw bytes；`finish_asset_import` 校验实际文件数和字节总量后用 map swap 发布新资产集。上述状态由 `g_catalog_mutex` 保护；cancel/校验失败会清 staging 并使 catalog 不 ready（`plugin.cpp:34-55,130-139,602-660`）。JNI 入口只把参数传给该 import API（`bridge/native/gamebridge_core_data.cpp:26-61`）。
- **读取路径**：HTTP handler/静态 API 先在 mutex 下将 raw `std::string` 从 `g_assets` 复制出来，然后在锁外解析成局部 `JsonValue` 并组装结果。HTTP 路由不保留解析后的 JSON 对象 cache：地图列表每次复制 `MAPINFOBASE.json` 并调用 `parse_json`，地图详情/出口/tiles 路由也各自读取并解析原始资产（`plugin.cpp` 的 map routes 与 static API 实现）。
- **瓦片缓存**：`map_info`/`map_tiles` 首次调用时在 catalog 锁下解析 `maps/tiles.json`，把每个 `m<map_id>` 条目的逻辑宽高与 4096 字节瓦片解码进 `unordered_map`（`g_map_cache_mutex` 保护），之后只读该缓存。资产 import 成功、失败或 cancel 时清空缓存，避免继续服务上一版资产。解析失败、资产未导入或 `maps/tiles.json` 缺失时返回 `kQolErrNotReady`；空指针或 `capacity < kStaticMapTileSize` 返回 `kQolErrInvalidArgument`；map 未命中返回 `kQolErrNotFound`。
- **线程边界**：资产 import 在 Kotlin bootstrap/initFeatures 路径同步发起；route handler 读取的是已发布的 raw bytes，并由 mutex 保护复制；解析在锁外进行。源码未把这些静态数据函数定义为 live 游戏读取或游戏主线程写操作（`ApiServer.kt:70-80`；`plugin.cpp:130-139,245-347,476-552,602-660`）。
- **生命周期**：`plugin_install` 初始化 `static.data` API 并注册 query handlers，任一注册失败时撤销 owner handlers；`plugin_deactivate` 注销 query handlers。asset import/cancel 有各自 JNI 入口并与 route install/deactivate 分离（`plugin.cpp:554-575,602-660`；`gamebridge_core_data.cpp:26-61`）。

## 7. 文件地图

| 子职责 | 文件 | 关键事实 |
|---|---|---|
| descriptor、HTTP routes、static.data 实现、资产缓存/import | `module/app/src/main/cpp/plugins/core.data/plugin.cpp` | limits/requires/cache 状态：21-55；raw asset copy：130-139；瓦片惰性缓存：193-244；12 routes：431-474；map APIs/provides：476-562；install/deactivate/descriptor：554-600；staging/atomic swap：602-660 |
| JSON parse/stringify/query 解码 | `module/app/src/main/cpp/plugins/core.data/json.cpp`、`json.h` | JSON 字段/解析辅助；query decode：`json.cpp:300-360` |
| 静态数据 capability contract | `module/app/src/main/cpp/contracts/static_data.h` | capability/version、瓦片常量与 map_name/map_exits/map_info/map_tiles API |
| HTTP route/query contract | `module/app/src/main/cpp/contracts/http.h` | route metadata 与 query dispatch interface：17-23、62-78、80-99 |
| Kotlin 静态资产 ingress | `module/app/src/main/java/com/inotia4/qol/CoreDataAssets.kt` | raw byte listing/limits/import transaction：6-35；递归列举与字节统计：37-65 |
| 启动调用 | `module/app/src/main/java/com/inotia4/qol/ApiServer.kt` | `initFeatures` 调用资产 load：65-80 |
| JNI raw-byte bridge | `module/app/src/main/cpp/bridge/native/gamebridge_core_data.cpp` | begin/asset/finish/cancel：26-61 |
| Kotlin route forwarding | `module/app/src/main/java/com/inotia4/qol/api/controller/DataController.kt` | endpoint annotations 与 NativeHttpQuery delegation：10-60 |
| API schema 权威 | `docs/reference/api-reference.md` | 静态地图 §3.3、静态表与三条 placeholder §7.4–§7.5 |
