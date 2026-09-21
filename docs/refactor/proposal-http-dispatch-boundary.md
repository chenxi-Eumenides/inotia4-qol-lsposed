# 提案：HTTP 分派边界与 `api/native` 吸收（待审阅）

> 状态：**待审阅提案（尚未实施）**。本文不修改 `ARCHITECTURE.md`，也不表示下列处理已经执行。
> 基线：分支 `refactor/arch-v2`，2026-09-21。相关：`docs/refactor/proposal-three-source-ownership.md`（三档来源）、`docs/refactor/architecture-review-2026-09-19.md` §4.2/§11。

## 1. 结论摘要

`core.api` 目前只是**路由元数据登记表 + 租约**，**不具备 native 分派能力**；HTTP 请求仍完全由 Kotlin（AndServer + Controller + Service）处理，
且 Kotlin 服务层承担大量**响应组装**（`StaticData`、`NameInjector`、`UiActivityTracker`、写操作后的 `state` 拼装）。
因此"把 `api/native` 整体迁移到 provider 插件"这个目标**缺少一个前置决策**：HTTP 分派究竟留 Kotlin 还是迁 native。

同时确认：`api/native` **不是单一 API 层**，而是混合了「游戏事实读取 / 游戏写操作 / JSON 物化 / 缓存构造 / JNI 兼容入口 / UI 与存档聚合」，
不能整体迁为某一个 provider。

## 2. 实测事实

| # | 事实 | 证据 |
|---|---|---|
| 1 | 路由三处一致：Controller 143、`api-reference` 143、`plugins/core.api/routes.inc` 143；缺失/多余/重复均为 0 | `scripts/maintenance/check_http_routes.py` |
| 2 | owner 分布：`api.character` 42、`api.item` 21、`api.world` 16、`api.system` 13，其余见 `routes.inc` | 同上 |
| 3 | **HTTP 传输在 Kotlin**：AndServer 启动与请求处理 | `ApiServer.kt:137-165`、`api/controller/*.kt` |
| 4 | **native 无 HTTP 分派**：`core.api` 只注册元数据与 lease | `plugins/core.api/plugin.cpp:17-47` |
| 5 | **`HttpRoute` 无 handler 字段**，`HttpReq` 无 query/路径参数 | `contracts/http.h:17-31` |
| 6 | **路由匹配为精确字符串比较** → `/api/character/party/{slot}` 无法命中 `/api/character/party/0` | `host/http_registry.cpp:39-47` |
| 7 | Kotlin 服务层做响应组装而非纯转发 | `service/info/InfoApiServiceCore.kt:35-120,210-345`、`service/action/ActionApiServiceCore.kt:20-118` |
| 8 | `NativeBridge` 只有各领域方法，**无通用请求/响应 JNI** | `NativeBridge.kt:30-209` |
| 9 | 遗留 `api/native/*.cpp` 仍**直接编入主库** | `module/app/src/main/cpp/CMakeLists.txt:62-95` |
| 10 | `bridge/native/gamebridge_internal.h` 集中 include 全部域头（JNI 聚合入口，非跨插件 provider 消费者） | `bridge/native/gamebridge_internal.h:8-25` |
| 11 | 宿主契约**已提供**角色/物品/世界/任务/存档/库存的读写面；`autosell`、`custom_recipe` 已在消费 | `contracts/game_host.h`、`host/game_host.cpp:1398-1539` |
| 12 | `game_save_preflight.{h,cpp}` 是**纯逻辑、零游戏依赖** | 文件头与实现 |

## 3. 两个选项

### 选项 A（中间态，可先行）：保留 Kotlin/AndServer 传输，只迁移**业务 owner**
- native 侧继续只做路由登记（现状），**不给 `HttpRoute` 加 handler**；
- 把 `api/native` 的**游戏事实读取**改为宿主契约消费（`contracts/game_host.h` 已具备），使其可被吸收；
- Kotlin Controller/Service 逐步把"业务计算"下沉到插件（经 capability/命令端口），本身退化为传输 + 信封；
- 优点：不触碰传输 ABI，风险小、可分批；缺点：`core.api` 仍非"唯一真源分派"，与审阅 §4.2 的目标尚有差距。

### 选项 B（目标态）：HTTP 分派迁 native，Kotlin 只留 socket/解析/写回
必须**另行设计并冻结**以下 ABI 与语义（当前均缺失）：
1. **模板路由匹配与参数提取**（替换精确比较；`{slot}` 等占位符）；
2. **query 与 body 解析**（`HttpReq` 增加 query/路径参数视图）；
3. **响应缓冲与 headers/content-type**（POD、容量、错误码）；
4. **同步操作与主线程命令的返回语义**（哪些路由同步返回、哪些投递命令 + 票据 + 轮询）；
5. **`state` 兼容协议**（写操作后的状态拼装，现状在 Kotlin）；
6. `op_gated`、lease 与门控检查点的位置。

## 4. 建议

1. **先执行选项 A 中与决策无关的部分**（不改传输 ABI、不改 URL/方法/响应）：
   - `api/native` 读侧改走宿主契约；`game_save_preflight` 等纯逻辑件先归位；
   - 随 `api/native` 读侧收缩，**被它阻塞的 `core/native` 三组**（`game_cache`、`module_save`、`inventory_trade`）即可吸收；
   - 每条路由建立映射表：`route → Controller → Service → NativeBridge → native 实现 → 加工 → 响应`。
2. **选项 B 另开设计任务**，产出上述 6 项 ABI 冻结后再实施；在此之前**不要**给 `HttpRoute` 加 handler。
3. 迁移期保持一致：URL、HTTP 方法、成功响应**不变**；`check_http_routes.py` 与 `/api/system/routes` 作为一致性证据。

## 5. 待审阅者回答的问题

1. 是否确认"先 A 后 B"的路线？或直接进入 B（则需先接受传输 ABI 的设计周期）？
2. 选项 B 的 6 项 ABI 中，是否接受"同步路由直接返回、命令类路由走票据 + 轮询"的二分？
3. `state` 兼容协议应归 `core.api`、对应 provider，还是保留在 Kotlin 作为传输层职责？
4. `api.*` 的 provider 划分是否同意按 owner 分布（character/item/world/system/…），且以"≥2 真实跨插件消费者"为准绳？
