# `core.save` 插件

## 1. 作用与 owner 定位

`core.save` 是存档领域的 owner，注册 ID 为 `core.save`、版本 `0.1.0`；负责存档槽事实、slot-bound 数据参与者、原版保存协调与进档/建档转换。它是 `plugins/core.save/` 中可独立启停的普通领域插件，不等同于 `save_backup` 的备份管理功能（descriptor：`module/app/src/main/cpp/plugins/core.save/plugin.cpp:23-28,1058-1077`；领域边界：`ARCHITECTURE.md` §5.2）。

`core.save.provider` 是默认开启的 Hidden feature，服务 lifecycle 管理，不是玩家可见开关或页面入口（`module/app/src/main/cpp/plugins/core.save/plugin.cpp:1058-1063`）。

## 2. 功能面

### HTTP 写路由

descriptor 当前登记三条 POST 路由，均由 `core.save` 处理（`module/app/src/main/cpp/plugins/core.save/plugin.cpp:768-775,1070-1075`）：

| 方法 | 路径 | 用途 |
|---|---|---|
| POST | `/api/system/save` | 保存当前槽 |
| POST | `/api/system/enter_slot` | 进入指定槽 |
| POST | `/api/system/create_slot` | 创建角色存档并进入 |

参数、状态码、响应体及票据终态以 [API 参考 §7.3、§7.9–§7.10](../api-reference.md) 为准；这里不复制完整 HTTP schema。Kotlin `SaveController` 做路由入口校验，`SaveActions` 将请求交给 `NativeHttpCommand`（`module/app/src/main/java/com/inotia4/qol/api/controller/SaveController.kt:17-37`、`module/app/src/main/java/com/inotia4/qol/service/action/SaveActions.kt:18-50`）。`/api/ui/go_main_menu` 由 `core.ui_state` 负责，备份导出、列表、导入、删除由 `save_backup` 负责（API 参考 §7.3、§7.10；`docs/refactor/api-route-map.md:140-146`）。

### Capabilities 与 port 面

| 面 | 版本 | 当前作用 |
|---|---:|---|
| `save.facts` | 1.0 | 提供槽状态/存在性、英雄索引/等级/职业、load data、hub key、map ID、current slot 与 `is_ok` 读取函数（`module/app/src/main/cpp/contracts/save_facts.h:10-37`）。 |
| `save.slot_data` | 1.0 | 注册按槽隔离的 section participant，并提供 session、load/write request、查询和注销接口；策略及回调契约见 [`slot-data-contract.md`](../../refactor/slot-data-contract.md) 与 `module/app/src/main/cpp/contracts/slot_data.h:11-144`。 |

descriptor 不声明 required/optional capability；它发布以上两个 capability，并要求三个版本均为 1.0 的 Game Host ports（`module/app/src/main/cpp/plugins/core.save/plugin.cpp:82-95,1065-1077`；版本定义：`module/app/src/main/cpp/contracts/game_ports/ports.h:48,58,65`）：

| Port | 版本 | 用途 |
|---|---:|---|
| `QOL_PORT_SAVE` | 1.0 | 槽状态与存档事实读取（`module/app/src/main/cpp/contracts/game_ports/save_port.h:12-35`）。 |
| `QOL_PORT_SAVE_WRITE` | 1.0 | 主线程执行 save/enter/create 的 owner 写操作并生成迁移前结果（`module/app/src/main/cpp/contracts/game_ports/save_write_port.h:11-35`）。 |
| `QOL_PORT_SAVE_TRANSACTION` | 1.0 | Adapter 状态、受控原版保存 trampoline、save-file evidence 与 loading bypass 探针（`module/app/src/main/cpp/contracts/game_ports/save_transaction_port.h:18-66`）。 |

### 原版 hooks

`core.save` 登记三条 private、required、fail-closed entry hook 资源（`module/app/src/main/cpp/plugins/core.save/plugin.cpp:783-821`）：

| 资源 | 当前职责 |
|---|---|
| `hook:SAVE_Save` | 拦截普通/自动保存并交由联合保存协调器；读档窗口仅经受控 loading bypass 判定。 |
| `hook:SaveSlot_GoToNewGame` | 在原版新档入口前申请转换准入；准入失败时阻断原版新档流程。 |
| `hook:SelectCharacter_ButtonBackExe` | 取消未决选角转换并释放 token/lease；不伪造恢复已经删除的 dat。 |

该插件不注册 UI panel；存档管理器面板和备份操作属于 `save_backup`（`module/app/src/main/cpp/plugins/save_backup/plugin.cpp:56-64,123-234,338-362`）。

## 3. 使用的模块功能与消费者

- **Host SDK**：使用 owner token 验证与 owner store、slot-operation API、`save_route` bind/dispatch/unbind、typed command、typed ticket result handler、RenderPre/LogicPre scheduler 和日志（`module/app/src/main/cpp/plugins/core.save/plugin.cpp:433-439,567-584,712-733,824-978`）。assembly 将 `core.save` 以 `module-saves` 授权身份注册（`module/app/src/main/cpp/host/assembly.cpp:212-215`）；save write 与 save transaction ports 限定给 `core.save`（`module/app/src/main/cpp/host/plugin_registry.cpp:394-407`）。
- **当前 capability consumers**：`save_backup` required `save.facts`，读取原版存档事实以提供独立备份管理（`module/app/src/main/cpp/plugins/save_backup/plugin.cpp:31-48,347-362`）；`extension_bag` required `save.facts` 和 `save.slot_data`（`module/app/src/main/cpp/plugins/extension_bag/plugin.cpp:174-194,801-815`）；`autosell` required `save.slot_data` 用于按存档保存筛选配置（`module/app/src/main/cpp/plugins/autosell/plugin.cpp:38-49,244-262`）。
- `save_backup` 另使用自己的 `QOL_PORT_SAVE_CALL` 与 `ui_panels`；它拥有的备份面板及 `/api/system/backup/*` 不属于 `core.save`（`module/app/src/main/cpp/plugins/save_backup/plugin.cpp:31-52,56-64,227-235`）。

## 4. 职责边界

- `core.save` 拥有存档事实 capability、进档/建档转换、保存参与者顺序与三条 system save 写路由；低层原版读写通过 Game Host ports/Adapter 完成，不在 HTTP 线程直接写游戏内存（`module/app/src/main/cpp/plugins/core.save/plugin.cpp:153-228,354-397,441-447`）。
- `save.slot_data` 是消费者 section 的会话/保存参与机制，不是 `.qol_save` 备份管理 API；备份文件格式、外部目录、去重与恢复归 `save_backup`，契约见 API 参考 §7.3 的“存档备份”小节及 `module/app/src/main/cpp/plugins/save_backup/`。
- 回主菜单命令归 `core.ui_state`；Host 管理存储及 Adapter 受控原版 save/evidence 端口是协作边界，不把 `core.save` 描述为 socket、通用路由或配置存储 owner（`ARCHITECTURE.md` §5.2；API 参考 §7.10）。

## 5. 读写线程与生命周期

- **事实读取**：`SaveFactsApi` 函数包装 `QOL_PORT_SAVE` 的槽状态、英雄数据、地图与当前槽读取（`module/app/src/main/cpp/plugins/core.save/plugin.cpp:30-80`）；Game Adapter 的 port 实现负责具体槽解析及 NotReady/错误返回（`module/app/src/main/cpp/game/adapter_ports/save_port.cpp:23-122`）。该面不是新增 HTTP snapshot 路由。
- **slot data**：消费者声明 `read_policy`/`write_policy`；所有消费者回调由 `slot_data_pump` 在游戏主线程执行。SessionBound 在 session ready 后加载，SlotBound 写仅先标 staged/dirty、由原版保存协调器统一 capture/commit（`module/app/src/main/cpp/contracts/slot_data.h:23-31,80-95`；`module/app/src/main/cpp/plugins/core.save/slot_data_provider.cpp:314-379,396-479`）。
- **HTTP 写入**：Kotlin 只做传输/Agreement 前置并投递 native typed command；C++ handler 校验后立即返回 ticket，结果处理器在主线程调用 save write port。`save` 经 `save_coordinator_settle`；`enter_slot`/`create_slot` 通过 Game Host transition operation 执行，等世界态提交后才结算，等待期间 ticket 保持 pending（`module/app/src/main/java/com/inotia4/qol/service/action/SaveActions.kt:18-50`；`module/app/src/main/cpp/plugins/core.save/plugin.cpp:557-733`；`module/app/src/main/cpp/game/access/save.cpp:428-444`；`module/app/src/main/cpp/plugins/core.save/save_coordinator.h:43-54`；API 参考 §7.10）。
- **原版保存与转换**：`SAVE_Save` wrapper 限定游戏主线程；普通保存经 `save_route` 进入同一协调器，session 未就绪时仅经 Adapter bypass 探针准入。新档 hook 先 begin，选角取消释放转换凭据；确认序执行 reset/ensure、journal 检查和 slot-data session 激活（`module/app/src/main/cpp/plugins/core.save/plugin.cpp:153-228,253-347,354-426`；`module/app/src/main/cpp/plugins/core.save/save_transition.cpp:143-249,253-267`）。
- **生命周期**：install 校验三项 ports、owner-bound store 与 Adapter 接口，初始化 slot-data/transition/coordinator，再绑定 save route、typed result handler 与帧任务。RenderPre 任务推进 slot-data、转换观测和保存协调；LogicPre 时钟推进转换超时。`on_disabled` 关闭 slot-data 准入；deactivate 先关准入，再解绑路由、停协调器/转换、清 transition ticket contexts 并注销 typed handler/帧任务（`module/app/src/main/cpp/plugins/core.save/plugin.cpp:333-347,824-1021`）。

## 6. 文件地图

| 子职责 | 文件 | 关键位置/事实 |
|---|---|---|
| descriptor、capability、ports、feature | `module/app/src/main/cpp/plugins/core.save/plugin.cpp`、`module/app/src/main/cpp/contracts/game_ports/ports.h` | plugin ID/version、provides/ports 与 feature：plugin.cpp 26-95、1058-1077；port versions：ports.h 48、58、65 |
| 路由与 hooks | `module/app/src/main/cpp/plugins/core.save/plugin.cpp` | 写路由：768-775；required hooks：783-821；install/lifecycle：824-1021 |
| facts capability | `module/app/src/main/cpp/contracts/save_facts.h`、`module/app/src/main/cpp/contracts/game_ports/save_port.h`、`module/app/src/main/cpp/game/adapter_ports/save_port.cpp` | facts API：save_facts.h 10-37；port 类型：save_port.h 12-35；Adapter 读实现：save_port.cpp 23-127 |
| section participant ABI/provider | `module/app/src/main/cpp/contracts/slot_data.h`、`module/app/src/main/cpp/plugins/core.save/slot_data_provider.h`、`slot_data_provider.cpp` | ABI/policy/callback：slot_data.h 11-144；provider API：slot_data_provider.h 8-40；init/session/pump/save capture：slot_data_provider.cpp 272-479 |
| 保存协调器/journal | `module/app/src/main/cpp/plugins/core.save/save_coordinator.h`、`save_coordinator.cpp`、`save_journal.h`、`save_journal.cpp` | 事务顺序/接口：save_coordinator.h 11-54；settle/hook save/pump：save_coordinator.cpp 580-713；journal section/version：save_journal.h 8-10 |
| 进档/新档状态机 | `module/app/src/main/cpp/plugins/core.save/save_transition.h`、`save_transition.cpp` | 状态/流程契约：save_transition.h 9-63；begin/confirm/cancel：save_transition.cpp 143-267 |
| Host 装配/私有 port | `module/app/src/main/cpp/host/assembly.cpp`、`module/app/src/main/cpp/host/plugin_registry.cpp` | `module-saves` owner 装配：assembly.cpp 212-215；port owner 限定：plugin_registry.cpp 394-407 |
| Kotlin transport | `module/app/src/main/java/com/inotia4/qol/api/controller/SaveController.kt`、`module/app/src/main/java/com/inotia4/qol/service/action/SaveActions.kt`、`ActionApiServiceCore.kt` | endpoints/校验：SaveController.kt 17-37；command/Agreement gate：SaveActions.kt 18-50；service delegation：ActionApiServiceCore.kt 8-15 |
| 相关消费者 | `module/app/src/main/cpp/plugins/save_backup/plugin.cpp`、`module/app/src/main/cpp/plugins/extension_bag/plugin.cpp`、`module/app/src/main/cpp/plugins/autosell/plugin.cpp` | save facts/slot data requires：save_backup 31-48、extension_bag 174-194、autosell 38-49 |
