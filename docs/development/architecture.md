# 模块架构与代码规范

> 日期：2026-08-16 ｜ 状态：✅ 现行（v0.6.0 四层架构） ｜ **本文件是代码结构与规范的唯一权威来源**
> 其他文档（README/data-sources）中的结构描述应引用本文件，不再重复维护。
> 重构方案与分阶段计划见 `docs/development/planning/refactor-plan.md`；本项目将 API 导出能力与模块增强功能视为两个不同的产品边界。

## 0. 产品功能边界

`api/` 是独立、完整的原版能力导出模块：负责导出原版游戏信息、提供原版游戏操作接口，并包含其 Controller、Service、JNI/native 适配和数据构造链。它不是 `feature/` 下增强功能的业务层，不能把扩展背包、设置增强或其他模块改动视为 API 的实现。

`feature/` 只包含模块新增或修改游戏行为的功能，目前包括 `extension_bag`、`iap_blocker`、`stack_limit`、`settings`、`save` 和 `save_backup`。这些功能可以被 API 触发或与 API 共享底层原语，但产品归属和验收边界独立于 API。

推荐顶层关系：

```text
api/                 原版信息与功能导出（独立完整功能）
feature/             模块增强功能（与 api 并列）
core/                共享技术原语
bridge/              JNI 边界
platform/            注入、进程、日志和配置运行时
```

native 侧按同一产品边界理解：`api/native/` 承载原版信息和原版操作导出的实现；`feature/*` 只承载模块新增或修改行为；JSON、符号解析、内存访问、缓存和帧任务等不属于产品功能，归入 `core/` 或 `data/`。当前 `game_inventory`、`game_ui`、`game_save` 与扩展功能存在交叉依赖，必须先拆出交叉部分，再迁移，禁止按文件名整体归类。

## 1. 模块架构总览（四层）

```
┌─ api 层（Kotlin controller/）    路由绑定 + 参数解析 + 错误包装（HTTP 状态码）；不碰 NativeBridge
├─ service 层（Kotlin service/）   语义修正、名称注入、跨域聚合、快照、OP 编排（含 OP 门禁）、配置下发
├─ parse 层（native 域文件+引擎）   原始字段→JSON 构造；写操作直改内存；输出缓存/帧驱动
└─ data 层（native）               符号/访问/查询原语：符号解析、偏移常量、读原始字段、静态元数据缓存、跨域遍历原语
```

```
┌──────────────────────────────────────────────────────────────┐
│  游戏进程（LSPosed 注入，libxposed 101）                       │
│                                                              │
│  HookMain.kt       模块入口：onModuleLoaded → 轮询初始化      │
│    │                                                         │
│    ├─ NativeBridge.kt   JNI 桥（114 个 external + loadLibrary）│
│    │     │                                                    │
│    │     ▼                                                    │
│    │  libgamebridge.so（native 层，22 cpp）                    │
│    │  ├─ bridge/native/gamebridge*.cpp  JNI 薄层（导出 Java_* 函数）│
│    │  ├─ data 层：game_symbols.h / symbol_registry.h /        │
│    │  │   symbol_resolver.* / game_access.* / game_state.* /  │
│    │  │   game_tiles.*                                        │
│    │  ├─ parse 引擎：game_json.* / game_nav.* / game_cache.* /│
│    │  │   game_motion.* / game_ops_common.*                   │
│    │  ├─ parse 域：game_character/party/inventory/world/      │
│    │  │   quest/ui/dialog/shop/save/system                    │
│    │  └─ patch 域：feature/patch/game_patch.* / game_ptr_hook.h│
│    │                                                          │
│    └─ ApiServer.kt      AndServer 嵌入式 HTTP（config.json 配置地址/端口，默认 0.0.0.0:8088） │
│          ├─ service/ApiServices.kt  服务注册中心（v0.4.0 重构）│
│          ├─ service/ApiService.kt   单文件双接口（InfoApiService + ActionApiService）│
│          ├─ service/info/InfoApiServiceImpl + InfoApiServiceCore │
│          ├─ service/action/ActionApiServiceImpl + domain action files │
│          ├─ service/OpApiService.kt  OP 唯一入口（含 opEnabled 门禁）│
│          ├─ service/ConfigApiService.kt  配置下发 native 收口 │
│          ├─ service/enrichment/NameInjector.kt + NameInjectorCore.kt │
│          ├─ store/ModuleSaveStore.kt  按槽 sidecar 容器       │
│          ├─ util/JsonUtil / ControllerGuard / ApiException /  │
│          │   GlobalExceptionResolver / CorsInterceptor        │
│          ├─ controller/（23 个，见 §3）                        │
│          ├─ patch/IapBlocker.kt / ImmersiveMode.kt           │
│          └─ StaticData.kt   assets 静态数据读取               │
│                                                              │
│  调用链：HTTP controller（路由+参数解析）→ ApiServices 接口 →  │
│          ServiceImpl（业务编排）→ NativeBridge → 对应域文件    │
│  多通道预留：ApiServices 接口不绑定 HTTP，未来 Binder/         │
│          LocalSocket 调用方复用同一 Service 层               │
└──────────────────────────────────────────────────────────────┘
```

### 1.1 各层职责

| 层 | 文件 | 职责 | 禁止 |
|---|---|---|---|
| data | `game_symbols.h` / `symbol_registry.h` / `symbol_resolver.*` / `game_access.*` / `game_state.*` / `game_tiles.*` | 符号解析（VMA/ELF .dynsym）、偏移常量、`resolve_global`、跨域实体查询原语（member_or_null / lead_member / find_inventory_item / inventory_count / inventory_item_at / find_char_by_merc_slot）、**跨域遍历原语（for_each_bag_slot / pool_obj_valid）**、瓦片静态缓存、状态判定（game_in_world / ui_blocked / tutorial_*） | 构造业务 JSON；依赖 parse 层任何头 |
| core | `core/native/game_json.*` / `core/native/game_nav.*` / `core/native/game_cache.*` / `core/native/frame_task.* / core/native/frame_host.*` / `core/native/game_ops_common.*` | json_escape、BFS 寻路、帧缓存（12 槽表驱动）、统一帧任务管理器（§2.1）、op_ok/op_err 响应信封 | 注入游戏语义名称 |
| parse 域 | `game_character/party/inventory/world/quest/ui/dialog/shop/save/system` | build_*/data_* 读构造、data_op_* 写操作 | 域间水平互调（仅 system 聚合方向向下 + 分发器特例，见 §1.2） |
| patch | `feature/patch/game_patch.*` / `game_ptr_hook.h` + Kotlin `patch/` | 注入/修改补丁域：IAP 屏蔽、沉浸模式、堆叠上限、craft 注入、recover、migrate_stack（见 §2.5） | — |
| service | Kotlin `service/` | inject* 名称注入、快照 attach、OP 编排（LogFile.op + **OP 门禁**）、配置下发 native | 解析 HTTP 参数；直接读内存 |
| api | Kotlin `controller/` | 路由、参数校验、错误包装（含 HTTP 状态码） | 调 NativeBridge |

### 1.2 依赖方向强制规则（可用 include 检查脚本验证）

```
gamebridge（JNI 顶）→ 所有层
parse 域文件 → data 层 + parse 引擎 + system 聚合方向的域头
parse 引擎 → data 层（cache 额外引用域文件的 build_* 函数指针，同层无环，合法）
data 层 → 仅 STL
禁止：data → parse、域文件之间非聚合方向互调、任何 include 环
```

**分发器特例（P3-50 记录）**：`game_dialog` 的 `data_op_dialog_select` 分发器**单向调用** ui/save/world/json 域（include game_ui.h / game_save.h / game_world.h / game_json.h）——分发器按 action 分发到 dialog_ok/dialog_cancel/npc_dialog_select，并读取界面/存档/地图状态构造响应。**仅限该分发器**，其余域文件仍遵守「域间仅 system 聚合方向」规则。

### 1.3 缓存落位

- data 层：符号地址、结构偏移、瓦片矩阵（初始化一次常驻）
- service 层：StaticData 物品名/词缀/任务表（懒加载常驻）
- parse 层：`g_cache_slots` 输出缓存（按帧号失效，表不变）
- 游戏状态任何层不缓存

### 1.4 帧机制独立（不合并）

`wait_frame_boundary`（请求驱动等帧）、`cache_prefetch_thread_fn`（后台预取）与统一帧任务管理器（`frame_task` + `frame_host`，§2.1）**保持独立**：分别服务惰性请求、预取线程、逐帧任务三个不同场景，语义与锁边界各异。op_ok 内 `frame_cache_force_refresh` 属「写后刷新」第四种模式，同样保留。

## 2. native 层文件职责（22 cpp）

| 文件 | 层 | 职责 | 依赖 |
|---|---|---|---|
| `bridge/native/gamebridge*.cpp` | bridge | **JNI 薄层**：114 个 `Java_*` 导出，按生命周期、数据读取、操作、UI 和 feature 分组，仅参数传递 + 字符串转换，无业务逻辑 | 各层头 |
| `game_symbols.h` | data | **常量单一来源**：结构体偏移、VMA、函数签名。含逆向来源注释 | 无 |
| `symbol_registry.h` | data | 符号登记表（SYM 宏名 → VMA/解析来源），check_symbols.py 校验清单 | 无 |
| `symbol_resolver.*` | data | ELF `.dynsym` 符号名动态解析 + `.rela.dyn` RELATIVE 反查（GOT 槽），VMA 仅兜底 | game_symbols.h |
| `game_access.*` | data | `/proc/self/maps` 基址定位 + `resolve_global()` 符号解析 + `fn_*` 函数指针 + `bridge_init()` | game_symbols.h |
| `game_state.*` | data | **状态判定 + 跨域查询原语 + 遍历原语**：game_in_world / ui_blocked / tutorial_*、member_or_null / lead_member / find_char_by_merc_slot / find_inventory_item / inventory_count / inventory_item_at、**for_each_bag_slot / pool_obj_valid** | 仅 STL |
| `game_tiles.*` | data | 静态瓦片矩阵（64×64 通行矩阵，assets maps/tiles.json 经 JNI 传入） | 仅 STL |
| `core/native/game_json.*` | core | 纯 JSON 工具：json_escape | 仅 STL |
| `core/native/game_nav.*` | core | BFS 寻路（nav_bfs / nav_bfs_multi，基于瓦片矩阵） | game_state.h（lead_member） |
| `core/native/game_cache.*` | core | **帧缓存层**：12 槽表驱动（惰性/预取双模式），对外 data_*_json 接口 | 域文件 build_* 函数指针 |
| `core/native/frame_task.* / core/native/frame_host.*` | core | **统一帧任务管理器**（多点位 + 句柄 API，§2.1）+ 锚点安装（GAMESTATE_DrawPlay+0x20 bl MAP_DrawBase） | game_state.h / game_system.h / call_patch |
| `core/native/save_enter.*` | core | 进入存档回调：读档/新档发起点 call_patch 标记 + 帧检测 world 就绪触发一次（§2.1） | frame_task.h / call_patch / game_state.h |
| `core/native/game_ops_common.*` | core | op_ok / op_err 响应信封（含 frame_cache_force_refresh）+ 写操作跨域共享 helper | game_cache.h |
| `core/native/module_save.*` | core | 统一原版完整保存入口：participant prepare、调用原版 `SAVE_Save`、成功 commit、失败 abort；线程局部防重入 | game_access / game_state |
| `api/native/game_character.*` | API native 域 | 角色：member_json / build_player_json / build_skills_json + 战斗/成长写操作（cast/attack/stop_combat/set_experience/set_level/add_experience/set_status_point/add_stat/set_auto_attack/set_skill_usage/learn_action/set_hp/set_mp/set_attr/stat_reset/skill_reset） | data + 引擎 |
| `api/native/game_party.*` | API native 域 | 队伍：build_party_json / build_mercenaries_json + include/exclude/discharge/withdraw/switch_player/party_swap | data + 引擎 |
| `api/native/game_inventory.*` | API native 域 | 背包：build_inventory_json / append_item_attrs / item_is_equip + set_money/add_money/minus_money/add_item/remove_item/use_item/discard/sell/move/jewel/enchant/dice_accept/dice_reject/equip/unequip；通过 core port 组合扩展背包投影和堆叠上限 | data + core + 引擎 |
| `api/native/game_world.*` | API native 域 | 地图：build_map_json / build_tiles_json / build_units_json / build_enemies_json / build_interactives_json / build_drops_json / append_position + move/walk/walk_stop/interact/teleport + **nav_task_tick / walk_task_tick** | data + 引擎 |
| `api/native/game_quest.*` | API native 域 | 任务：data_active_quest / quest_list / quest_completed / quest_active + quest_quit | data + 引擎 |
| `api/native/game_ui.*` | API UI 域 | 界面：debug_ui / ui_screen / popup_top_vma / top_panel_name + main_menu / panel_open / panel_close | data + 引擎 + core ports |
| `game_dialog.*` | parse 域 | 对话：npc_dialog_options / dialog_content + npc_interact / **dialog_select 分发器** / dialog_ok / dialog_cancel / npc_dialog_select / story_next / story_skip | data + 引擎 + 分发器特例（ui/save/world/json） |
| `api/native/game_shop.*` | API native 域 | 商店：shop_items + shop_buy | data + 引擎 |
| `api/native/game_save.*` | API 存档域 | 存档：save_slots / current_save_slot + save / enter_slot（内部完整性门禁）/ create_slot | data + 引擎 + core ports |
| `api/native/game_save_preflight.*` | API native 域（纯逻辑） | 存档预检判决：槽结构状态/失败码 → valid/corrupt/missing/incompatible/unknown + 阶段名 + 结构化 JSON；零游戏依赖，编入 host 单测 | 无（纯 STL） |
| `game_system.*` | parse 域 | **系统聚合域（唯一允许 include 其他域头的聚合域）**：build_gamestate_json / build_snapshot_json + frame_count / init_report / events / emit / take_snapshot | data + 引擎 + 各域头 |
| `feature/patch/game_patch.*` | patch | **注入/修改补丁域**：IAP 屏蔽 / 沉浸模式 / 堆叠上限（47 patch 点）/ craft 三函数 / recover_after_hive_block / migrate_stack（§2.5） | data + game_ptr_hook.h |
| `feature/extension_bag/game_ui_virtbag.*` | feature | 扩展背包运行时：状态、投影、拖拽、绘制、生命周期与扩展背包操作 | data + core + patch |
| `feature/extension_bag/extension_bag_port.cpp` | feature adapter | 将扩展背包内部实现适配为 `core/native/extension_bag_port.h` 稳定端口 | extension_bag runtime |
| `feature/extension_bag/extension_bag_context.h` | feature internal | 持久化拆分使用的内部上下文访问点，不对外形成 API/core 契约 | extension_bag runtime |
| `feature/extension_bag/extension_bag_persistence.cpp` | feature persistence | 通过内部上下文执行状态 JSON 的加载、保存和 pending 事务隔离恢复 | feature context + model |
| `feature/extension_bag/extension_bag_geometry.*` | feature geometry | 扩展背包布局门禁及控件命中适配；纯网格计算委托 `core/native/grid_geometry.*` | core geometry + game control data |
| `feature/extension_bag/extension_bag_api.cpp` | feature API facade | 对外扩展背包 JSON/操作入口的稳定包装，不承载锁内业务实现 | extension bag internal API |
| `feature/save_backup/save_backup_bundle.*` | feature 纯逻辑 | 存档管理器备份纯逻辑：`.qol_save` bundle 组装/解析（大端+CRC32）、SHA-256/MD5/CRC32/base64 原语、极简扁平 metaJson 提取、MSAV sidecar 容器最小校验/重定槽；纯 STL，编入 host 单测 | 无（纯 STL） |
| `feature/save_backup/save_backup.*` | feature | **存档管理器备份**：`.qol_save` 导出（checksum 去重）/导入（两段事务 + `.rollback/` 回滚，sidecar 直改第 6 字节 slot + 重算尾部 CRC32）/列表/删除；返回体即 HTTP 响应 JSON | game_access + core（op_err）+ save_backup_bundle |
| `feature/ui/game_ui_savebackup.*` | feature UI | **存档管理器面板**：设置页入口按钮 + 三栏备份面板（左槽/中操作/右列表每页 4 行 + 翻页 + 提示区 + 返回）；复用 IAP 死条目 `F_PANEL_UNK2_ENTER`，仅主菜单注入、离开还原；导入/删除面板内两步确认 | game_ui_kit + game_ui_settings + feature/save_backup + data |

**已解散文件**：`game_data.h`（已删除）、`game_read.cpp`（→ character/party/inventory/world/system + state）、`game_misc.cpp`（→ world/quest/ui/dialog/shop/save/system）、`game_ops_value.cpp`（→ character/inventory + patch）、`game_ops_action.cpp`（→ 九域 + patch）。

### 关键约定
- **核心实现原则（最高优先级）**：**以读写内存、调用游戏函数为主，hook 只在必要时使用**。
  - 读内存：读符号 VMA 直读（`g_base + VMA` 解引用），覆盖玩家/背包/地图/面板/弹窗文本等存量数据
  - 调游戏函数：函数指针调用（`fn_search_path`/`fn_move_as_path`/`fn_get_member` 等，符号解析后直接执行，不修改游戏代码），覆盖 move/equip/use-item 等操作端点（v0.3.1）
  - 写内存：直接修改游戏状态（如 `*ctrl = 0` 清零控制态），覆盖需要改状态的操作
  - hook（拦截函数/改参数）**默认不用**：修改执行流有崩溃面，且与"数据导出"定位不符；仅在确有拦截需求且读内存/调函数无法实现时才引入，引入前须书面说明理由
- **禁止在域文件 / gamebridge.cpp 中出现裸偏移或裸地址**——一律通过 `game_symbols.h` 常量（`C_*`/`I_*`/`O_*`/`S_*`/`M_*`/`G_*_VMA`/`F_*_VMA`）
- **结构体访问一律用偏移常量 + 注释**，禁止 magic number
- **JNI 函数名 = `Java_<包>_<类>_<方法名>`**，Kotlin `external fun` 方法名须与导出名精确对应（曾因缺 `native` 前缀导致 UnsatisfiedLinkError，见 `docs/guides/build-and-deploy.md` §5a 踩坑）
- native 层不抛异常给 Java：失败返回 `-1`/空值，由 Kotlin 层容错
- **带参 JNI**：`nativeGetPathJson(tx, ty)` 等参数经 JNI `jint` 传递（v0.2.33 起）

### 2.1 统一帧任务管理器（frame_task + frame_host，2026-09-13）

**位置**：`core/native/frame_task.{h,cpp}`（管理器）+ `core/native/frame_host.{h,cpp}`（锚点安装）。**动机**：逐帧操作（移动/寻路/自动出售扫描）需要「任意线程可注册、回调只在游戏主线程帧周期执行、可多任务、可查询」。旧 `FrameTaskManager`（`game_motion.{h,cpp}`，后台线程轮询 + 单任务 clear）与 `frame_tick`（单注册表、无句柄）已删除，由本管理器取代。

**多触发点位**：`enum : FramePointId { kFramePointRenderPre = 0, kFramePointCount };` 每点位独立按帧去重；当前只安装 `kFramePointRenderPre`（渲染开始前）。

**锚点（指令 patch，非 Native Hook）**：`GAMESTATE_DrawPlay+0x20` 的 `bl MAP_DrawBase`（原字 `0x9401d18a`，常量 `F_GAMESTATE_DRAWPLAY_DRAWBASE_CALL_OFF`）。wrapper 先 `frame_task_dispatch(kFramePointRenderPre)` 再 `fn_map_drawbase()`。选点依据（真机探针）：`GAMESTATE_Draw+0x3c` 的 `bl GAMESTATE_DrawPlay` 在 world 态不执行（DrawPlay 由函数指针调用），而 DrawPlay 内两条路径都汇聚到 `+0x20`，故每帧必执行。

**API**：

| 函数 | 语义 |
|---|---|
| `frame_task_add(point, fn, ctx, interval, count)` | 返回句柄 `FrameTaskId`（0=失败）；`interval` 0=每帧；`count` 0=一直、1=一次；首个派发周期必触发 |
| `frame_task_remove(id)` | 注销（幂等）；未知 id 返回 false |
| `frame_task_query(id, out)` | `FrameTaskStatus{active, remaining_runs（无限=-1）, frames_to_next}` |
| `frame_task_dispatch(point)` | 锚点 wrapper 在游戏主线程调用；同一 (point, frame) 只派发一次；`thread_local` 重入门禁 |

回调签名 `bool (*)(int64_t frame, void* ctx)`，返回 false 自动注销。

**线程模型**：注册/删除/查询任意线程可调；与派发共用一把锁；**持锁快照、锁外回调**，调用前再次持锁确认 id 存在；回调只在游戏主线程执行。帧号来源 `data_frame_count()`。

**移动互斥槽**（`game_world_movement.inc`）：`g_motion_task` + `motion_task_start/stop`，nav/walk 共用，注册即替换；`stop_all_tasks()` 已删除，`walk_stop` 改调 `motion_task_stop()`，不再影响其它帧任务（自动出售）。

**现有消费者**：nav（`nav_task_tick`）、walk（`walk_task_tick`）、自动出售扫描（`autosell_tick`，`feature/autosell/autosell_scan.cpp`）、进入存档回调（`save_enter_tick`，`core/native/save_enter.cpp`）。

**真机验证（2026-09-13）**：锚点安装成功；world 态 move_to / walk_dir / stop_move 逐帧驱动生效；无崩溃。

### 2.2 帧驱动方案演进（为什么不用 hook）

| 方案 | 结果 | 原因 |
|---|---|---|
| **统一帧任务管理器（frame_task + frame_host）** | ✅ 采用（2026-09-13） | 指令 patch 锚点到游戏主线程帧周期（GAMESTATE_DrawPlay+0x20 bl MAP_DrawBase），`frame_task_dispatch` 按帧去重；回调主线程执行、frame 参数直传；旧 FrameTaskManager 后台线程并发读写内存已废弃 |
| ShadowHook 1.0.10 | ❌ | LSPosed 环境 stub→new_addr 映射表在错误 linker 命名空间查找，桥跳野地址（0x79299114e4 访问违例） |
| 手写 arm64 inline hook | ❌ | 已修 5 bug（adrp 掩码 0x9F000000、imm21 重组 immhi<<2\|immlo、stp 编码 0xa9b0、blr 数据槽、.S 符号冲突）仍 SIGBUS/SIGILL 崩溃（trampoline lr 污染、Draw 后续指令寄存器依赖） |
| 填 PATHLIST 游戏自驱动 | ❌ | 玩家控制态（0x2e2=7）下游戏每帧重置玩家动作，驱动条件复杂（0x2fa/0xc40/0x2e0 耦合） |

**arm64 inline hook 技术教训**（后续若再尝试）：
- 所有函数入口第 1 条几乎都是 adrp（PC 相对）→ 重放必须重定位
- trampoline 必须保存/恢复原始 lr（blr 污染 lr → 重放区 stp x29,x30 存错 lr → 原函数 ret 跳错）
- AGP 对 .S 汇编不支持 -fPIC 符号重定位（ldr literal/adr 均报错）→ 需纯 C++ mmap 生成指令
- LSPosed 环境下 .S 全局符号跨 TU 引用解析到 base.apk 错误地址

### 2.2.1 C++ Hook 技术选型结论（2026-09-04）

本项目将 native 侧的行为改造能力拆为五种机制。前四种已经属于游戏当前实现，LSPosed Native API 是新增的通用入口 Hook 能力。它们不是互相替代的库，而是针对不同控制点的工具。

| 机制 | 作用 | 能力边界 | 推荐用处 | 当前决策 |
|---|---|---|---|---|---|
| **直接读写游戏内存** | 通过 `g_base +` 符号/偏移读取或修改全局变量、对象字段和状态位 | 只能改变已知数据；不会自动执行校验、刷新 UI、派发事件或触发存档逻辑；必须掌握对象生命周期和并发关系 | 数据导出、开关/状态位修改、简单数值操作，以及读写游戏函数无法覆盖的存量状态 | **首选，已在真机验证** |
| **调用游戏函数指针** | 通过 `symbol_resolver` 得到游戏函数地址，以声明的函数签名直接调用原版逻辑 | 只能调用已定位且签名确认的函数；参数、返回值、结构体/浮点 ABI 错误会导致崩溃；调用线程和游戏状态必须满足原函数前置条件 | 移动、装备、使用物品、保存、UI 操作等需要让游戏自身执行完整逻辑的功能 | **优先于任何入口 Hook** |
| **`PtrHook` 函数指针槽覆盖** | 修改游戏对象、GOT 槽或回调表中的函数指针，将调用目标替换为模块 wrapper；wrapper 可选择调用原函数 | 只对经过间接调用的函数指针有效；找不到稳定槽位时不能使用；wrapper 必须完全匹配调用约定；对象销毁或槽位重建后需要重新安装 | 控件 `ExecuteProc`、`ControlProc`、事件处理器和回调表拦截；适合需要 before/after 或条件抑制的 UI/输入逻辑 | **入口 Hook 前的首选拦截方式，已在真机验证** |
| **指令 `patch`** | 在已确认的函数偏移处替换 ARM/ARM64 机器指令，改变分支、立即数或门禁条件 | 只能处理确定的少量指令点；必须同步处理页权限、指令缓存、版本漂移、并发执行和恢复；不适合复杂业务逻辑或大段函数改写 | 固定常量/上限、条件分支、IAP 屏蔽和沉浸模式等少量稳定 patch 点 | **现有机制继续使用，必须可校验、可回滚** |
| **LSPosed 官方 Native Hook API** | 由 LSPosed 在 so 加载事件中提供函数替换入口；模块通过 `native_init` 注册，并在回调中对目标函数执行 Native Hook | 本质是 Inline Hook，不是 GOT/PLT Hook；只解决入口重定向，不解决函数签名、线程并发、递归、生命周期和卸载安全 | 仅用于前四种机制无法覆盖、且确实必须拦截普通函数入口的场景；这是本项目唯一允许的 Native Hook 机制 | **允许使用，仍须最后接入并完成验证** |

关键事实与约束：

- 直接读写内存和调用游戏函数指针是本项目的数据访问与操作主路径；不要为了“统一”而把它们改造成入口 Hook。
- `PtrHook` 只改数据段中的函数指针，不改函数机器码，因此不需要 inline trampoline、`mprotect` 或指令缓存刷新；但它依赖稳定的间接调用槽位，不能拦截直接 `bl` 调用。
- 指令 `patch` 与 `PtrHook` 是互补关系：前者改执行指令，后者改间接调用目标。所有 patch 地址必须来自 `game_symbols.h`/`symbol_resolver`，并保留原指令校验和失败回滚。
- LSPosed 官方 Native Hook API 的 `hookFunc` 由框架内部 HookFunction/LSPlant 路径提供，官方文档的目标是函数替换，不应描述为 GOT/PLT Hook；它仍然继承 Inline Hook 的 trampoline、ABI、并发和卸载风险。
- 本项目禁止引入或使用 Dobby、ShadowHook 和手写 ARM64 trampoline；Native Hook 入口统一使用 LSPosed 官方 API 提供的 hook/unhook 函数指针。官方当前结构字段名为 `hookFunc`/`unhookFunc`，本项目 `NativeAPIEntries` 使用同 ABI 的本地适配字段 `hook_func`/`unhook_func`。
- 当前模块已通过 `module/app/src/main/resources/META-INF/xposed/native_init.list` 注册 `libgamebridge.so`；注册只证明加载入口存在，不证明目标函数 Hook 或业务链路已经验收。
- 使用 LSPosed API 时，`handle + dlsym()` 只适合从实际已加载目标库解析动态符号。项目现有 `game_access` 明确禁止自行 `dlopen/dlsym` 来访问 `libgame.so`，因为 Android linker namespace 可能加载独立副本；对非导出函数仍应使用项目的 `symbol_resolver` 和 `game_symbols.h`。
- 任一入口 Hook 都必须先验证：目标地址来源、完整函数签名、浮点/结构体返回约定、递归路径、主循环并发、重复安装、恢复时机，以及目标函数正在执行时的卸载行为。没有这些验证，不得进入正式功能。

推荐顺序固定为：**直接读写内存 → 调用游戏函数指针 → `PtrHook` → 指令 `patch` → LSPosed Native Hook API**。这不是绝对的技术强弱排序，而是从低执行流风险到高执行流风险的决策顺序：先选择能满足需求且不改函数入口的机制，只有前四种都无法实现时，才引入 LSPosed Native Hook API。参考：[LSPosed Native Hook](https://github.com/LSPosed/LSPosed/wiki/Native-Hook)、[LSPosed native API 实现](https://github.com/LSPosed/LSPosed/blob/master/core/src/main/jni/src/native_api.cpp)。

### 2.3 帧同步采集缓存层（v0.4.57）

**位置**：`game_cache.cpp`（重构前在 game_data.cpp，P3 迁入）。**动机**：高频请求时每次实时读游戏内存 + 构造 JSON（units 含 BFS）→ 响应慢/线程爆炸；且请求线程碰游戏内存与主循环竞争。

**设计**（表驱动，改频率只动一行，**12 槽冻结**）：

```cpp
CacheSlot g_cache_slots[] = {
    {"player",      1, 0, "", build_player_json,       true},   // interval=帧数，1=每帧
    {"party",       1, 0, "", build_party_json,        true},
    {"map",         0, 0, "", build_map_json,          true},   // 瓦片矩阵静态数据（同图不变），惰性
    {"units",       1, 0, "", build_units_json,        true},
    {"gamestate",   1, 0, "", build_gamestate_json,    false},
    {"snapshot",    1, 0, "", build_snapshot_json,     false},
    {"inventory",   0, 0, "", build_inventory_json,    true},   // 惰性：偶发查看，请求驱动
    {"skills",      0, 0, "", build_skills_json,       true},   // 惰性
    {"mercenaries", 0, 0, "", build_mercenaries_json,  true},   // 惰性
    {"drops",       1, 0, "", build_drops_json,        true},
    {"enemies",     0, 0, "", build_enemies_json,      true},   // 惰性：units 子集（type==1）
    {"interactives",0, 0, "", build_interactives_json, true},   // 惰性：units 子集（type==2 且可交互）
};
```

**核心机制（v0.4.59 惰性/预取双模式）**：
- **表驱动 interval 语义**：`interval>0` = 每 n 帧预取（预取线程主动构造）；`interval=0` = 惰性（请求驱动）——**改一行即切换模式**
- **预取槽（interval>0，如 player/party/units/gamestate/snapshot/drops=1）**：预取线程帧计数驱动每 n 帧构造，请求直接读缓存（µs 级命中，无等帧）
- **惰性槽（interval=0，如 map/inventory/skills/mercenaries/enemies/interactives）**：请求驱动——同帧复用（本帧已构造 → 返回缓存），跨帧 → 单飞等帧边界构造（帧号变化 = Draw 完成，数据稳定窗口 28-55ms）
- **refreshing 单飞**：`std::atomic<bool> refreshing` CAS 保证同一槽同时只有一个线程构造；构造期间并发请求直接返回旧缓存（不等帧）——避免请求率 > 帧率时全部排队等帧
- **等帧锁外**：等帧边界在 `g_cache_mtx` 外执行，不阻塞其他请求的锁竞争
- **预取线程启动条件**：`frame_cache_start()` 仅在存在 interval>0 槽时启动线程（全惰性配置 = 零线程零空闲负担）
- **写操作强制刷新**：`op_ok()` 内调 `frame_cache_force_refresh()`——同步刷新全部槽（不等帧边界），操作后 attach 立即读最新
- **events**：`data_events_json` 直接 `take_snapshot()` + `g_events_mtx` 锁保护 diff（审计 H4）
- **帧暂停兜底**：`wait_frame_boundary` 100ms 超时后直接构造（读内存与帧无关，帧暂停仍可获取数据）

**JNI 接口不变**：gamebridge.cpp 仍调 `data_*_json()`，Kotlin 层仅 Service 组装（v0.4.58 currentMap 的 unitsJson 去重）。

**性能实测**：
- v0.4.59（真机，2026-08-12）：预取槽锁外构造修复后 party 单端点 500 并发 **337.5 req/s**（与 v0.4.57 持平）；纯预取 5 端点 267.8 req/s（units BFS 重构造成本）、纯惰性 2 端点 365.4 req/s（轻量端点）；混合 7 端点 182.4 req/s（端点构成差异，非机制问题）。**吞吐差异主因是端点构造成本（units BFS）而非缓存机制**。
- **P2/P3 复测（重构后）**：v0.5.48（P2）party 端点 keep-alive **712 req/s**（≥300 基线达成）；v0.5.49（P3）复测 **260-391 req/s**（端点构成差异，非机制回归）。

### 2.4 跨域遍历原语规范（P2 新增）

data 层 `game_state.*` 提供两个跨域遍历原语，**收编全部同构遍历**（P2 重复消解，替换 10 处拷贝）：

| 原语 | 签名 | 语义 | 收编位置 |
|---|---|---|---|
| `for_each_bag_slot` | `void for_each_bag_slot(BagSlotFn fn, void* ctx)`，`BagSlotFn = bool(*)(void* item, int bag, int slot, void* ctx)` | 6×16 背包槽遍历（哨兵判断 + 槽遍历公共骨架） | 7 处背包遍历（原 state/ops_action/ops_value/patch） |
| `pool_obj_valid` | `bool pool_obj_valid(const uint8_t* obj)` | 角色池对象哨兵判定 | 3 份角色池过滤（原 read/misc/ops_action） |

> 遍历/过滤的**域特定差异**（mode 过滤、hero 排除、situation 变体）通过回调参数/谓词注入保留，原语只收编「哨兵判断 + 槽遍历」公共骨架。build_inventory 的 stride 风格保留原样（结构不同，强行统一得不偿失）。

### 2.5 patch 域（注入/修改补丁）

**native**：`feature/patch/game_patch.cpp` + `game_ptr_hook.h`（v0.5.18 起，P3 扩充为「注入/修改补丁域」）。

| 能力 | 机制 | 说明 |
|---|---|---|
| IAP 屏蔽 | 指令 patch（`patch_apply`/`patch_revert`，可逆改写 libgame.so 指令） | 屏蔽内购弹窗 |
| 沉浸模式 | 指令 patch | 全屏沉浸 |
| 堆叠数量格式 | 读侧 getter hook + 写侧调用方级 `s2_*` 编码 | 数量位固定 S2 布局（`count=128a+b`，bits22-31）；无版本标识、无迁移路径，不回滚布局；装备/损坏 marker 判定不纳入数量写点 |
| 堆叠上限 999 | 可逆指令 patch（**10 个 clamp 点**：保存/空槽检查） | `set_stack_limit_enabled` 只切换新操作的 99/999 上限，已有 canonical 数量不改写 |
| 背包拖拽合并 | 可逆 GOT hook | `set_move_merge_enabled` 覆盖背包格事件处理器；配置独立控制同类可堆叠物品的拖拽合并 |
| 蜂巢阻塞恢复 | `data_recover_after_hive_block()` | IAP 恢复语义（v0.5.18 hive 屏蔽恢复） |

**game_ptr_hook.h**（v0.5.18）：函数指针包装——覆盖游戏内存中的函数指针字段（按钮 ExecuteProc、控件 Proc/ControlProc、回调表），wrapper 内可回调原函数。与指令 patch 互补：只改数据段指针，无需 mprotect/指令缓存刷新，无 inline hook 的 trampoline lr 污染问题。**调用约定约束**：wrapper 签名必须与被覆盖函数完全一致（参数寄存器 x0-x7、返回值、被调用者保存寄存器 x19-x28、16 字节栈对齐）。

**Kotlin**：`patch/IapBlocker.kt` / `patch/ImmersiveMode.kt`（模块启动期经 ConfigApiService 下发 native 生效）。

## 3. Kotlin 层文件职责

| 文件 | 职责 |
|---|---|
| `HookMain.kt` | 模块入口；核心原则=读内存+调游戏函数为主，hook 仅必要时使用：轮询 `bridge_init()` 直至成功 → 反射拿 context → 启动 ApiServer |
| `NativeBridge.kt` | JNI 声明（`System.loadLibrary("gamebridge")` + **114 个 external**，JNI 面冻结见 §9.5） |
| `ApiServer.kt` | AndServer 启动（监听地址/端口读 ModuleConfig（外部 config.json）、模块 assets 注入、StaticData 挂接） |
| `ModuleConfig.kt` | **配置组件（v0.5.17，v0.5.21 改外部源）**：外部存储 config.json 为唯一配置来源（缺失用默认值并立即写入），提供监听地址/端口/堆叠上限增加/拖拽合并/**opEnabled** 等配置的获取与修改（每次修改立即持久化） |
| `store/ModuleSaveStore.kt` | **模块 sidecar 存储**：保存扩展背包等模块数据；不覆盖原版 `save*.dat`。原版存档备份能力已移除，后续设计见 `docs/development/planning/backlog.md` P1。 |
| `service/ApiServices.kt` | **服务注册中心（v0.4.0，P0-3 重构）**：controller/调用层从这里取 Service 实例；多调用通道预留（Binder/LocalSocket 复用同一 Service 层） |
| `service/ApiService.kt` | **单文件双接口**：`InfoApiService`（信息查询服务接口，GET /api/info/* 契约）+ `ActionApiService`（合法操作服务接口，POST /api/action/* 契约），均不绑定 HTTP 语义 |
| `service/info/*` | **信息查询服务**：入口保持接口契约；地图、佣兵、系统薄查询和商店查询按职责独立，party/inventory/quest/ui/game 仍由 `InfoApiServiceCore` 聚合协调 |
| `service/action/*` | **合法操作服务**：入口保持接口契约；`ActionApiServiceCore` 负责接口适配，`Movement/Inventory/Character/Party/Quest/Save/SaveBackup/Ui/ExtensionBagActions` 按操作域实现，`ActionSupport` 统一快照 attach、保存后处理和槽位查找 |
| `service/OpApiService.kt` | **OP 唯一入口（v0.5.46 新建，v0.5.47 门禁）**：接口+实现，opSetAttr 批量循环逻辑在 impl；**所有方法入口统一 OP 门禁**（ModuleConfig.opEnabled 未开启 → 403 `{"ok":false,"error":"op disabled"}`） |
| `service/ConfigApiService.kt` | **配置下发收口（v0.5.46）**：nativeSetStackLimitEnabled/nativeSetMoveMergeEnabled/nativeSetTilesData 直调收口（applyToNative）；ConfigController 与 ApiServer 启动期统一调用 |
| `service/enrichment/*` | **名称/结构注入**：入口保持稳定调用名，Core 负责 inject* 名称注入 + restructure* 字段重排（Role 呈现顺序/main_stats 结构化/物品 base-bonus-gem-chaos-enchant 结构）+ StaticData 查询；item/role/quest 富化器仍待拆分 |
| `util/JsonUtil.kt` | 通用 JSON 工具（解析容错 + **错误响应格式 A 构造**：NOT_FOUND/NOT_READY/BAD_REQUEST 常量 + `err(msg, code)` 工厂 + `parseBody(body)` 入口） |
| `util/ControllerGuard.kt` | controller 公共守卫：native 未就绪返回 503 语义串（architecture §9.3-9）；异常分支返回 `err("internal error", 500)` |
| `util/ApiException.kt` | 业务异常（携带 HTTP 状态码 + 消息），controller 抛出让 GlobalExceptionResolver 统一转响应 |
| `util/GlobalExceptionResolver.kt` | **AndServer 全局异常处理（@Resolver 注册，v0.5.45）**：ApiException/HttpException → 其携带状态码；ParamValidateException → 400；其余 → 500 |
| `util/CorsInterceptor.kt` | **CORS 跨域拦截器（@Interceptor 注解，编译期自动注册）**：浏览器直连调试用 |
| `api/controller/HealthController.kt` | **GET /api/health**（服务健康，v0.5.0 由 /api/health 迁移后保持顶层） |
| `api/controller/CurrentMapController.kt` | **/api/world/map/***（id/exits/units/enemies/interactives/drops/distance 独立端点，v0.5.0 由 /api/info/map 迁移，v0.5.35 移除复合端点） |
| `api/controller/PartyController.kt` | **/api/character/party**（复合 + count/leader/{1..3} + 槽内子端点，v0.5.0 由 /api/info/party 迁移） |
| `api/controller/MercenaryController.kt` | **/api/character/mercenary**（复合 + list/{1..18}，v0.5.0 由 /api/info/mercenary 迁移） |
| `api/controller/InventoryController.kt` | **/api/item/inventory**（复合 + money/items/bag/*，v0.5.0 由 /api/info/inventory 迁移） |
| `api/controller/QuestController.kt` | **/api/quest**（复合 + active/list/list/{id}/completed，v0.5.0 由 /api/info/quest 迁移） |
| `api/controller/UiController.kt` | **/api/ui**（复合 + screen/panel/dialog/*，v0.5.0 由 /api/info/ui 迁移） |
| `api/controller/GameController.kt` | **/api/system/game + /api/system/snapshot**（复合 + snapshot/info/frame；v0.5.0 由 /api/info/game 迁移，v0.5.18 修正 snapshot 独立路径） |
| `api/controller/EventsController.kt` | **/api/system/events**（事件流，since 参数预留，v0.5.0 由 /api/info/events 迁移） |
| `api/controller/DataController.kt` | 静态数据端点（/api/world/maps/list、maps/{mapId}；/api/system/tables、tables/{table}、tables/{table}/search、text、story-events，v0.5.0 由 /api/data/* 迁移） |
| `api/controller/MovementController.kt` | **移动操作（POST /api/world/movement/*，v0.5.0 迁移）**：move/walk/path/stop/interact |
| `api/controller/CombatController.kt` | **战斗操作（POST /api/character/combat/*，v0.5.0 迁移）**：{role}/config/auto-attack、{role}/switch 等 |
| `api/controller/InventoryActionController.kt` | **背包操作（POST /api/item/inventory/*，v0.5.0 迁移）**：use-item、discard、{role}/equip（含 category）、{role}/unequip 等 |
| `api/controller/CharacterController.kt` | **角色成长（POST /api/character/grow/*，v0.5.0 迁移）**：skill、{role}/stat 等（OP 端点已迁 OpController，v0.5.46） |
| `api/controller/PartyActionController.kt` | **队伍操作（POST /api/character/party/*，v0.5.0 迁移）**：include、exclude、discharge、withdraw |
| `api/controller/UiActionController.kt` | **UI 操作（POST /api/ui/*，v0.5.0 迁移；v0.5.46 并入原 NpcController 端点）**：dialog/select、dialog/interact、main-menu、panel/* |
| `api/controller/OpController.kt` | **OP 唯一入口（POST /api/op/*，v0.5.46 收口）**：10 个已实现端点（hp/mp/experience/level/set_attr/inventory-add/status-point/party-swap/money/teleport）+ 11 个占位（NOT_IMPL 501）；全部经 OpApiService |
| `api/controller/ShopController.kt` | **商店（/api/item/shop/*，v0.5.0 归入 item 域）**：GET items + POST buy |
| `api/controller/QuestActionController.kt` | **任务操作（POST /api/quest/quit，v0.5.0 归入 quest 域）** |
| `api/controller/SaveController.kt`       | **存档操作（/api/system/save/*，v0.5.0 由 info/action 迁移归并）**：slots 读 + save/enter-slot/create 写；进入存档时由 native 内部门禁执行完整性检查 |
| `api/controller/ConfigController.kt`     | **模块配置（GET /api/config/list + POST /api/config/set，v0.5.21）**：读当前配置 + 设置配置（每次修改立即持久化外部 config.json；监听地址/端口变化时延迟重启 ApiServer 生效；stackLimitIncrease 变化时通知 native 生效；纯 Kotlin 层，不走 ControllerGuard） |
| `api/controller/DebugController.kt` | 调试端点（/api/debug/ui、/api/debug/path，开发期；v0.5.46 补 ControllerGuard.guard + InfoApiService 方法） |
| `patch/IapBlocker.kt` | IAP 屏蔽（模块启动期经 ConfigApiService 下发 native） |
| `patch/ImmersiveMode.kt` | 沉浸模式（模块启动期经 ConfigApiService 下发 native） |
| `StaticData.kt` | assets 静态数据读取（内存缓存） |
| `LogFile.kt` | 文件日志（/sdcard/Android/data/<游戏包>/files/inotia4-export.log） |

### 约定
- **controller 只做路由 + 参数解析 + 调用 Service**，业务逻辑在 Service 层（InfoApiServiceImpl/ActionApiServiceImpl/OpApiService/ConfigApiService）或 native 或 StaticData
- **调用层（controller）不直接调 NativeBridge**——统一经 ApiServices 接口（多调用通道预留，v0.4.0）
- **简单端点字段提取统一走 InfoApiService**（v0.3.13：从 native 复合 JSON 提取，controller 不直接解析）
- **静态数据读取统一走 `StaticData`**，controller 不得直接操作 assets
- **API 按实体领域分组（v0.5.0）**：7 域顶层路径 = `character`（角色/佣兵/战斗/成长）、`world`（地图/移动）、`item`（背包/商店）、`quest`（任务）、`ui`（界面/对话）、`system`（健康/游戏/事件/存档/静态表）、`op`（越权操作，独立权限）；**每组内 GET（读）与 POST（写）混合，读写同域，HTTP 方法区分**；废弃 info/data/action 前缀
- 新增端点归组：按实体归入对应域 controller；OP 类端点一律 `/api/op/*` 前缀（安全边界，见 §9.1）
- 新增端点遵循「controller 路由 → ApiServices 接口 → ServiceImpl 实现 → NativeBridge external → native JNI」五段式
- **路由一律全路径写法（v0.5.18 强制）**：controller 类上**禁用 `@RequestMapping`**，方法级 `@GetMapping/@PostMapping` 直接写与 api-reference.md 完全一致的完整路径（如 `/api/character/party/{slot}`、`/api/system/snapshot`）。原因：AndServer 注解处理器对「类级 + 方法级」路径是**纯字符串拼接**（`pPath + cPath`，无「方法级以 `/` 开头即覆盖类级」的语义），类级前缀 + 方法级全路径会拼出重复路径——v0.5.18 前 snapshot 曾注册为 `/api/system/game/api/system/snapshot` 导致文档路径 404。另注意 `/{slot}` 纯模糊首段校验失败

## 4. 常量与符号管理（换版本核心）

**`game_symbols.h` 是全部游戏版本相关常量的唯一位置**。校验工具：

```bash
uv run python scripts/maintenance/check_symbols.py [libgame.so 路径]
```

- 解析 `game_symbols.h` 的 `G_*_VMA`/`F_*_VMA` 常量（`SYMBOL_TO_MACRO` 映射表）
- 与新 libgame.so 符号表对比，输出 `✅ 一致 / ⚠️ 需更新`
- **改动 game_symbols.h 后必须运行此校验**

### 换游戏版本迁移流程
1. 新 APK 解包 → 提取 `lib/arm64-v8a/libgame.so`
2. `uv run python scripts/maintenance/check_symbols.py <新so>` → 列出变化的 VMA
3. 更新 `game_symbols.h` 的 VMA；结构体偏移（`C_*`/`I_*`）按 check 输出人工判断是否变化
4. 重新提取静态数据（M3 流程，`scripts/data/` 脚本）
5. 构建 v0.3.x 并真机验证

## 5. 命名约定

- native 文件：`game_*` 前缀（symbols/access/state/tiles/json/nav/cache/motion/ops_common + 域文件）+ `gamebridge`（JNI 边界，库名绑定）
- Kotlin：类名明确职责（HookMain/NativeBridge/ApiServer/StaticData/LogFile）
- **避免语义重叠命名**（曾出现 `game_bridge` 与 `gamebridge` 混淆，已改名 `game_access`）

## 6. 新增端点标准流程

1. native 对应域文件加 JSON 构造函数（用 symbols.h 常量）
2. `gamebridge.cpp` 加 JNI 导出（名与 Kotlin external 精确一致）
3. `NativeBridge.kt` 加 external 声明
4. 按实体归组选 controller 加路由（新增端点按「分组总览」表归入对应域 controller；OP 端点 → `/api/op/*` 前缀；简单端点可走 `service/InfoService` 提取）
5. `docs/reference/api-reference.md` 更新端点表（含版本号）
6. 构建 → 真机验证（`scripts/verification/live_session.py` 采样）

> **写操作端点（v0.3.0）**：额外步骤——先 objdump 逆向函数签名（原 `docs/research/control-capability.md` §5 方法，
> 2026-08-16 文档清理后以 `docs/development/planning/refactor-plan.md` 域映射为准），
> game_symbols.h 加 F_*_VMA + typedef，game_access 解析函数指针，对应域文件实现 `data_op_*`（内部检查
> `STATE_nState==5` 并返回 `{"ok":..}` JSON），再走三段式。

### 写操作端点通用规范（v0.3.2-0.3.6 真机验证沉淀）

0. **`STATE_nState==5`（world）前置检查 = 简化假设，非硬性要求**（2026-08-08 修正）：统一要求操作时游戏处于 world 状态，
   是为**减少开发难度与测试广度**——避免在主菜单/存档界面调用游戏函数因数据结构未就绪崩溃。**未经逐操作实证**；
   如需支持非 world 状态操作，可去除该检查并在多种界面状态下实测验证（原 docs/research/control-capability.md §0 论证，2026-08-16 清理）。

1. **调游戏函数前先查判定函数**（`*_IsUse` / `*_IsRealEquip` / `*_CanUse` / `*_CanEquip`）——游戏对「能否做某事」通常有现成判定函数，先搜符号表，别自己猜。判定不过返回明确错误（如 `item not usable`），避免触发游戏内部非预期 UI/状态（乱码弹窗）。
2. **返回值不等于成功标志**：ARM64 tail-call（`b` 非 `bl`）会覆盖返回值语义；返回 -1 的函数走 truthy 判断会误判为成功。用**状态观察**（操作后槽位空/坐标变/装备变化）判定生效，或单独处理 -1。
3. **目标槽/位置被占用时先清理**：如 `CHAR_EquipItem` 目标槽被占用返回 0——先 `fn_unequip` 再穿，实现自动替换。
4. **前置校验拦截非法操作**：游戏内「合法操作」对主控/特殊 NPC 会走 UI 弹窗（乱码）——API 层前置校验拦截（`cannot exclude leader` / `cannot exclude quest npc`），绝不直接调游戏函数。
5. **Kotlin 方法名避开 Java 保留字**（switch/object/class 等）：kapt/ksp 注解处理器按 Java 标识符生成代码，保留字方法被**静默跳过**（无报错、无路由）。排查「代码有注解但运行 404」：解包 APK 查 dex 路由字符串。

## 7. 构建与校验命令

> 构建/部署/真机循环命令见 `docs/guides/build-and-deploy.md` §3.1，本节仅列代码规范相关的校验命令。

```bash
# 符号校验（改 game_symbols.h 后必跑）
uv run python scripts/maintenance/check_symbols.py

# host 单测（native 纯函数，不依赖设备；v0.5.51 起）
cd module/app/src/main/cpp/tests && cmake -B build && cmake --build build && ctest --test-dir build

# 全量路由 smoke（真机，对比 v0.5.43 基线）
uv run python scripts/verification/smoke_all.py
```

## 8. 文档地图（避免重复）

> 完整文档地图与三级分级见 `docs/INDEX.md` 与根目录 `AGENTS.md`，本节仅列与代码结构直接相关的文档。

| 文档 | 职责 | 与本文档关系 |
|---|---|---|
| `README.md` | 面向用户的项目总览（功能、安装、使用、下载） | 结构概览指向本文档 |
| `docs/development/planning/refactor-plan.md` | 重构实施方案和阶段计划 | 结构迁移目标与分阶段计划 |
| `docs/player-operations.md` | 操作分级（合法 vs OP） | 新增操作端点时引用 |

## 9. API 安全与并发基线（强制，2026-08-08 代码审计新增）

> 来源：2026-08-08 全量代码审计。以下为**后续所有编码必须遵守的持续性规范**；
> 审计发现的一次性修复项在 `docs/development/planning/backlog.md`「代码审计修复项」跟踪，不在本节重复。
> 本节是 §2/§4/§6 的补充维度（鉴权/并发/错误语义/符号登记），不替代既有约定。

### 9.1 暴露面与鉴权

1. **HTTP 端点必须经过鉴权**（token / IP 白名单中间件），**写操作（POST）强制**；鉴权机制未就绪前，禁止新增对外端点。
2. **OP 能力（改数据类 native 函数：money/exp/statuspoint/teleport/sell 等）必须带全局开关且默认关闭**。`/api/op/*` 权限机制就绪前：禁止新增 OP 路由、禁止在 controller 中直接调用 OP 函数——仅靠「不挂路由」隔离不构成安全边界。
   - **OP 门禁达成（v0.5.47）**：`ModuleConfig.opEnabled` 全局开关（默认 false，config.json 持久化）；`OpApiService` 所有方法入口统一门禁，未开启抛 `403 {"ok":false,"error":"op disabled"}`。门禁在 service 层统一入口，controller 不持有 NativeBridge 引用无法绕过——`/api/op/*` 已接线端点（§9.1-2 范围）满足「全局开关默认关闭」基线，接线不违反本节。
   - 门禁覆盖范围：OpApiService 全部已实现方法（v0.5.47 为 10 个）；未接线端点（占位 NOT_IMPL 501）不经过 service，维持原样。
3. **调试端点（/api/debug/*）必须登记文档**（architecture + api-reference），release 构建排除或加鉴权。

### 9.2 并发

4. **模块自身共享状态必须线程安全**：缓存用 `ConcurrentHashMap`（禁止裸 `HashMap` 跨请求读写）；跨请求状态（如 events 基线快照）用 `std::mutex`/`atomic` 保护，禁止函数内 `static` 可变状态无锁访问。
5. **写操作（op_*）必须全局互斥串行化**；操作与结果快照读取必须在同一临界区完成（避免 read-modify-write 竞态）。
6. native 全局状态即便「初始化后只读」，初始化窗口的读写仍须同步（读加锁或原子门控）。

### 9.3 错误响应与防御

7. **错误响应统一 JSON 包装 + HTTP 状态码语义**（400 参数错 / 404 未找到 / 500 内部错 / 503 未就绪），**禁止透传 native 失败原串**（"-1"/空值）给客户端。
   - **错误信封格式 A（v0.5.45 统一）**：`{"ok":false,"error":"<原因>"}`；HTTP 状态码表：

     | 状态码 | 语义 | 触发 |
     |---|---|---|
     | 400 | 参数错误 | 路由参数解析异常（NumberFormatException 等）、body 解析失败、参数校验不过 |
     | 403 | 权限不足 | OP 门禁未开启（`op disabled`） |
     | 404 | 未找到 | 资源不存在（not found） |
     | 500 | 内部错误 | 未捕获异常（internal error） |
     | 501 | 未实现 | OP 占位端点（not implemented） |
     | 503 | 未就绪 | native 未就绪（not ready） |

   - **实现机制**：controller 抛 `ApiException(code, msg)`（或 JsonUtil.err 工厂 + 显式状态码）→ `GlobalExceptionResolver`（@Resolver 注册）统一转 HTTP 响应；路由参数解析异常统一捕获转 400 + JSON 错误体（不再泄漏原始 Java 异常串）。
8. **native 函数指针调用一律判空后再调**（与 `fn_get_next_exp` 事故对齐）；`NewStringUTF` 返回值须判 null/异常。
9. 所有 HTTP 调用 native 前检查 `NativeBridge.ready`，未就绪返回 503（防止 `UnsatisfiedLinkError` 冒泡成 500）。

### 9.4 符号与常量（补充 §4）

10. **新逆向出的 VMA/偏移一律入 game_symbols.h 并登记 `check_symbols.py` 校验清单**；禁止在域文件/gamebridge 出现裸地址（含面板识别地址、调试字段地址）。
11. **调用游戏函数一律走 game_access 解析层（fn_*）**，禁止 `g_base + VMA` 直接强转调用（dialog_ok/cancel 的反例须收敛）。

### 9.5 JNI 面冻结约定（重构期确立）

**NativeBridge 114 个 external 冻结**。native bridge 已按职责拆为多个 `gamebridge_*.cpp`，JNI 导出名与分发逻辑不随域拆分变化。新增端点按 §6 五段式扩展，**禁止改名/改签名既有 external**。

## 10. 滞后修正清单（P4 文档同步，全部实测确认）

重构期间文档与代码的滞后点，P4 统一修正（以代码为准）：

| # | 文档原描述 | 实际（代码实测） | 修正 |
|---|---|---|---|
| 1 | HealthController 路由 `/api/system/health` | `GET /api/health`（HealthController.kt:15） | 已改 §3 表 |
| 2 | 接口文件 InfoApiService.kt / ActionApiService.kt 两文件 | `ApiService.kt` 单文件双接口（InfoApiService + ActionApiService） | 已改 §3 表 |
| 3 | FrameTaskManager 位置 game_data.cpp | 已整体删除（game_motion.{h,cpp} 随统一帧任务管理器移除） | 已改 §2.1 |
| 4 | move 回调名 move_task_tick | `nav_task_tick`（game_world.cpp:312） | 已改 §2.1 |
| 5 | 不存在的路由 `/api/ui/dialog/ok`、`/api/ui/dialog/cancel`、`GET /api/ui/dialog/content` | 实际能力由 `/api/ui/dialog/select`（action=ok/cancel/index）+ `GET /api/ui/dialog` 承担（D2 裁决：修文档不补实现） | 已改 §3 表（UiActionController/UiController） |
| 6 | ModuleConfig 注释「实际生效逻辑未实现（预留）」 | 已实现且 ConfigController/ApiServer 在调 | 已改 §3 表（ModuleConfig） |
