# 新架构实现地图

> 定位：**定位入口**。不是架构权威，不是执行台账。
>
> - 架构权威（结构与规则）：根目录 `ARCHITECTURE.md`
> - 阶段状态与功能域归属：`docs/refactor/ledger.md`
> - 机器事实快照（层分布、基线债务）：`docs/refactor/baseline.md`（脚本生成）
>
> **范围声明**：本文件只覆盖**已迁入新架构**的实现（`contracts/ game/ infra/ hooks/ patch/ plugins/ host/`）。
> **排除** `core/`、`feature/`、`api/`、`data/` 与其余 legacy bridge（`bridge/native/*` 除验证附录列出的 JNI 薄层外）。
> 目标结构与设计理由不在此重复，见 `ARCHITECTURE.md`。

<!-- BEGIN GENERATED:stats -->
## 生成统计

> 本节由 `scripts/maintenance/gen_map_doc.py` 生成，**禁止手工编辑**；
> 手工改动会在 `--check` 下被判定为不一致。

- 数据源：`module/app/src/main/cpp/{contracts,game,infra,hooks,patch,plugins,host}`、`module/app/src/main/cpp/plugins/*`、`module/app/src/main/cpp/tests/CMakeLists.txt`
- 口径：行数按 `wc -l`（换行符计数，含空行与注释）；文件限 `.cpp`/`.h`/`.inc`；不含 `bridge/`、`tests/` 与 legacy 路径
- CTest 分类规则：`gen_map_doc.py` 的显式白名单（测试源文件 → 机制/宿主或已迁移功能）确定新架构/已迁移 target，其余归 legacy；白名单条目须存在于 `tests/CMakeLists.txt`，两类互斥

### 新架构目录

| 目录 | 文件数 | 行数 |
| --- | --- | --- |
| `contracts/` | 23 | 1930 |
| `game/` | 39 | 3643 |
| `infra/` | 24 | 2154 |
| `hooks/` | 10 | 1565 |
| `patch/` | 6 | 484 |
| `plugins/` | 164 | 32694 |
| `host/` | 20 | 5658 |
| **合计** | **286** | **48128** |

### 已迁移插件（`plugins/*`）

- `plugins/attribute_quality/`：6 文件 / 411 行
- `plugins/attribute_range_display/`：4 文件 / 238 行
- `plugins/autosell/`：17 文件 / 2418 行
- `plugins/core.ui/`：17 文件 / 2188 行
- `plugins/craft_ui/`：4 文件 / 997 行
- `plugins/custom_recipe/`：10 文件 / 2110 行
- `plugins/extension_bag/`：41 文件 / 13919 行
- `plugins/inventory_hooks/`：11 文件 / 2305 行
- `plugins/native_choice/`：4 文件 / 402 行
- `plugins/push_all/`：6 文件 / 308 行
- `plugins/quest/`：4 文件 / 255 行
- `plugins/save_backup/`：13 文件 / 4605 行
- `plugins/simple_mode/`：6 文件 / 529 行
- `plugins/special_equip/`：6 文件 / 547 行
- `plugins/stack_limit/`：3 文件 / 428 行
- `plugins/text_layer/`：6 文件 / 494 行
- `plugins/world_teleport/`：6 文件 / 540 行

### CTest target 分类

- 全仓 target 总数：**50**
- 新架构/已迁移：**32**（机制/宿主 18 + 已迁移功能 14）
- legacy：**18**

新架构/已迁移（32）：
- 机制/宿主（18）：`capability_lifecycle_tests`、`extension_bag_contract_tests`、`hooks_dispatch_tests`、`hooks_popup_state_tests`、`hooks_registry_tests`、`hooks_transaction_tests`、`infra_capabilities_status_tests`、`infra_command_queue_tests`、`infra_config_commands_tests`、`infra_events_tests`、`infra_panels_tests`、`infra_scheduler_tests`、`infra_snapshot_tests`、`infra_store_tests`、`patch_layer_tests`、`plugin_lifecycle_tests`、`plugin_registry_tests`、`store_sell_rules_tests`
- 已迁移功能（14）：`attribute_quality_tests`、`attribute_range_display_tests`、`autosell_plugin_tests`、`autosell_rules_tests`、`autosell_store_tests`、`custom_recipe_tests`、`module_text_tests`、`native_choice_tests`、`push_all_rules_tests`、`quest_lifecycle_tests`、`save_warehouse_validate_tests`、`simple_mode_rules_tests`、`special_equip_rules_tests`、`text_layer_plugin_tests`

legacy（18）：`core_patches_shape_tests`、`core_ui_components_tests`、`core_ui_panels_tests`、`craft_ui_guard_tests`、`game_feature_tests`、`game_variant_tests`、`host_adapter_tests`、`host_tests`、`infra_data_tests`、`item_count_facade_tests`、`popup_enter_alias_tests`、`qol_log_tests`、`save_backup_rollback_tests`、`stack_limit_plugin_tests`、`stage4_hook_tests`、`transition_dispatch_tests`、`ui_adapter_tests`、`world_teleport_status_tests`
<!-- END GENERATED:stats -->

## 人工导航

### 1. 目录职责与关键入口

| 目录 | 职责 | 关键入口 |
| --- | --- | --- |
| `contracts/` | L0 中立契约：插件描述符、资源声明、命令/事件/文本层/能力接口、版本 | `contracts/plugin.h`、`contracts/commands.h`、`contracts/text_layer.h` |
| `game/` | 游戏版本耦合唯一处：符号解析、帧时钟、游戏访问 | `game/access/symbols.h`、`game/access/patches.h`、`game/clock.h` |
| `infra/` | 运行时骨架：命令队列/命令端口、事件、能力、配置、状态、快照、调度器 | `infra/command_queue.h`、`infra/commands.h`、`infra/scheduler.h`、`infra/runtime.h` |
| `hooks/` | 资源机制层：登记表、安装引擎、事务引擎、共享资源分派协议 | `hooks/registry.h`、`hooks/install.h`、`hooks/transaction.h`、`hooks/dispatch.h` |
| `patch/` | 指令 patch 机制：占用声明、原字校验、BL/word 安装与还原 | `patch/claims.h`、`patch/instr_patch.h` |
| `plugins/` | 内置静态插件（一插件一目录，清单见生成统计区） | 各 `plugins/<id>/plugin.h` 的 `plugin_descriptor()` |
| `host/` | 宿主装配与系统级 JSON：启动装配、插件注册表、帧源、状态/变体导出 | `host/assembly.h`、`host/plugin_registry.h`、`host/frame_source.h`、`host/status_json.h` |

### 2. 控制流

#### 2.1 启动装配（`host/assembly.cpp` → `host/plugin_registry.cpp`）

- `host::assembly_init()`（幂等，由 bridge 就绪后调用）依次：注入帧时钟 → 安装帧源常驻资源 → 注册命令排空帧任务 → 注入 hook/patch 原语与事务依赖 → 逐个 `register_plugin(...)` → `install_all(deps)`。
- 装配顺序与失败语义以 `ARCHITECTURE.md` §7.2 为准；这里只标入口：注册表实现在 `host/plugin_registry.cpp`，描述符由各插件 `plugin_descriptor()` 静态提供。

#### 2.2 帧派发（`host/frame_source.cpp` → `infra/scheduler.cpp`）

- `host/frame_source.cpp` 以两条声明式 `kCallPatch` 资源（`patch:GAMESTATE_DrawPlay+0x20` / `patch:MainProcess+0x40`）替换锚点；wrapper 在游戏主线程先 `infra::scheduler_dispatch(point)`、再派发 legacy `frame_task`、最后复刻原调用。
- `infra/scheduler.{h,cpp}` 是统一帧任务表（注册/注销/按帧点派发）；帧号经 `infra/runtime` 注入，未注入或帧号非法时跳过派发。

#### 2.3 命令队列（`infra/command_queue` / `infra/commands`）

- `infra/command_queue.{h,cpp}`：任意线程 `post`，游戏主线程每帧 `drain` 一次；票据 `pending/done/failed/cancelled`，可查询、可取消（仅限声明可取消且未执行）。
- `infra/commands.{h,cpp}`：`contracts::QolCommands` 的宿主实现与外部写入口；生命周期命令的执行体由宿主经 `commands_set_lifecycle_handler` 注入，`infra` 不依赖 `host`。

#### 2.4 资源事务（`hooks/transaction.cpp`）

- `hooks::tx_begin` / `tx_step`：安装按资源声明顺序、还原按逆序；每次 `tx_step` 最多推进一个资源步骤，禁止主线程阻塞等待；同一插件同时只允许一个未终态事务。
- 机制原语由宿主经 `TxDeps` 注入，`hooks/` 不依赖 `patch/`；非入口机制经 `TxDeps.install_other/release_other` 交由宿主分派。

#### 2.5 能力撤销（`host/plugin_registry.cpp` + 设计 §3.4.1/§3.4.4）

- `host::lifecycle_apply`（`deactivate` / `activate`）是主线程唯一执行器；前置拒绝条件与状态机见 `host/plugin_registry.h` 注释。
- 撤销级联与能力注册时序以 `ARCHITECTURE.md` §3.4.1 / §3.4.4 为准；消费者按能力（`infra/capabilities`）查找而非缓存指针，撤销期走降级路径（证据 VM-P4-04）。

#### 2.6 共享文本分派（`hooks/dispatch.cpp` + `plugins/text_layer`）

- `plugins/text_layer` 是共享资源 `hook:MEMORYTEXT_GetText` 的唯一 owner；wrapper 取得原函数文本后交 `hooks::dispatch_run_text`，按 `before → original → after` 分派。
- 消费者不再各自挂同址 hook，改经能力 `text_layer` 订阅（权限以 `ConsumerKind` 表达）；内置文本表是 after 阶段的 Modify 消费者，见 `plugins/text_layer/feature.h`。

#### 2.7 状态 JSON（`host/status_json.cpp`）

- `host_feature_status_json` / `host_resources_json` / `host_ticket_json` 把 `infra::status` 与 `hooks::registry` 导出为 `GET /api/system/features|resources|tickets/{id}` 的响应体，由 `bridge/native/gamebridge_status.cpp` 调用。

## 过渡依赖（逐条注明目标清除阶段）

| 位置 | 过渡内容 | 目标清除阶段 |
| --- | --- | --- |
| `host/assembly.cpp` | bootstrap 顺序收敛；hook/patch 原语注入仍取自 `plugins/inventory_hooks` 与 `core/native/call_patch` | P5 |
| `host/frame_source.cpp` | 帧源在 `assembly` 之前被调用，故自注入 patch 原语（与 `assembly` 重复） | P5 |
| `plugins/quest/plugin.cpp` | 进档回调仍借用 legacy `core/native/save_enter` 设施 | 随存档域迁移（C/D 阶段） |
| `plugins/inventory_hooks`（B2a-1） | **禁止 deactivate**：extension_bag / 自动出售 / 合成等 legacy 消费者依赖这 24 个 wrapper（扩展袋读取、扩展补扣、出售门禁）；物理释放在插件 `deactivate` 路径会执行，消费者将在 wrapper 缺失下运行。宿主无「不可 deactivate」标志，靠流程约束 | 消费者改经 capability requires 收口后（B2c/D2）解除 |
| `plugins/inventory_hooks/native_inventory_hook.h`（D2c-2b-1） | 过渡导出仅剩 `native_hook_func` / `native_unhook_func` 机制原语（→ P5）；`inventory_native_hook_call_refresh_item_area_original` 已经 `inventory_refresh` capability 收口，出售门禁已经 `hooks/dispatch` 布尔门收口 | 原语：P5 |
| `host/core_patches.cpp` + `game/access/patches.cpp`（B2a-2） | 常驻指令 patch 声明式化：19 条站点登记（位段缩窄 ×2 / clamp ×6 / 重定向 arg ×5 + call ×5 / hub ×1），`kBlRedirectToSymbol` 目标经 `fn_resolve` 解析。monster 兼容是**扫描式例外**：地址按运行时特征字节定位，无法静态登记，经 `patch::install_word_at` 占用同一张点表、不入 `hooks/registry` 资源表 | 扫描式例外长期保留（特征字节定位方式不变，无清除阶段） |
| `core/native/stack_limit_state.cpp`（B2a-2） | stack_limit 有效态实现放在 `core/native/`（`arch_common` 过渡映射为 `infra` 层）；只读端口 `stack_limit_port.h` 冻结 | P5 迁入 `infra/` 正式路径后转正 |
| `plugins/core.ui`（+ `autosell`/`save_backup` 面板）（D1） | 面板直接依赖基础层 `game/access/ui_kit` / `ui_components`，未走可替换的 ui-kit 接口（U11 adapter 未建立） | P5（U11） |
| `plugins/core.ui/panels/settings_config.inc`（D1） | 设置页读取/写入开关经 JNI 调 Kotlin `ModuleConfigUiBridge.getConfigJson`/`toggleConfig`（Kotlin JSON 配置源） | P5（决策 D：配置源迁 native config） |
| `plugins/extension_bag`（D2） | D2c-2b-2 后 move_merge/X1 与 extension source protection 收回本插件（`move_merge.inc`/`move_merge.h`），EB 的 3 个 stage4 谓词收回本插件（`extension_bag_equip_rules.h`），EB↔inventory_hooks 的 include 边清零；仍作为 `inventory_hooks` 24 wrapper 的消费者（不得在消费者收口前 deactivate）。refresh 经 `inventory_refresh` capability、出售门禁经 `hooks/dispatch` | D2d 收尾 |

## 验证附录

### 新架构/已迁移 CTest target

分类见上方生成统计区：新架构/已迁移 **17**（机制/宿主 **13** + 已迁移功能 **4**）。host 测试命令：

```
cd module/app/src/main/cpp/tests && cmake -B build && cmake --build build && ctest --test-dir build --output-on-failure
```

### 新栈 JNI 薄层（6）

`bridge/native/` 下仅以下 6 个属新栈薄层（其余 bridge 为 legacy，不在本图范围）：

- `gamebridge.cpp`（总入口 / `nativeInit`）
- `gamebridge_status.cpp`
- `gamebridge_variant.cpp`
- `gamebridge_simple_mode.cpp`
- `gamebridge_push_all.cpp`
- `gamebridge_lifecycle.cpp`

### 治理工具

已落地 7 项：

| 工具 | 作用 |
| --- | --- |
| `scripts/maintenance/check_layers.py` | 目录级依赖方向与无环（默认结构检查） |
| `scripts/maintenance/check_offsets.py` | 非 `game/` 层裸地址与符号头越权（默认结构检查） |
| `scripts/maintenance/check_jni.py` | Kotlin `external` ↔ native 导出 ↔ 描述符三向比对（默认结构检查） |
| `scripts/maintenance/check_resources.py` | 资源声明/注册完整性（默认结构检查） |
| `scripts/maintenance/check_arch.py` | 校验总入口（`--freeze` 冻结基线、`--only <name>` 单跑） |
| `scripts/maintenance/gen_baseline_doc.py` | 现状基线清单生成器（输出 `docs/refactor/baseline.md`） |
| `scripts/maintenance/check_symbols.py` | 独立运行：常量对 `.so` 校验 |

- 共享库 `scripts/maintenance/arch_common.py`：层映射、依赖矩阵、include 图、基线与豁免，被上列脚本复用。
- `scripts/maintenance/check_plugins.py`：**未落地**（规划）；注册表完整性与 `requires/provides` DAG 当前由 host 单测覆盖，脚本化随 P5 收紧落地。
