# 新架构实现地图

> 定位：**定位入口**。不是架构权威，不是执行台账。
>
> - 架构权威（结构与规则）：根目录 `ARCHITECTURE.md`
> - 阶段状态与功能域归属：`docs/refactor/ledger.md`
> - 机器事实快照（层分布、基线债务）：`docs/refactor/baseline.md`（脚本生成）
>
> **范围声明**：本文件只覆盖**已迁入新架构**的实现（`contracts/ game/ infra/ hooks/ patch/ domains/ features/ host/`）。
> **排除** `core/`、`feature/`、`api/`、`data/` 与其余 legacy bridge（`bridge/native/*` 除验证附录列出的 JNI 薄层外）。
> 目标结构与设计理由不在此重复，见 `ARCHITECTURE.md`。

<!-- BEGIN GENERATED:stats -->
## 生成统计

> 本节由 `scripts/maintenance/gen_map_doc.py` 生成，**禁止手工编辑**；
> 手工改动会在 `--check` 下被判定为不一致。

- 数据源：`module/app/src/main/cpp/{contracts,game,infra,hooks,patch,domains,features,host}`、`module/app/src/main/cpp/features/*`、`module/app/src/main/cpp/tests/CMakeLists.txt`
- 口径：行数按 `wc -l`（换行符计数，含空行与注释）；文件限 `.cpp`/`.h`/`.inc`；不含 `bridge/`、`tests/` 与 legacy 路径
- CTest 分类规则：`gen_map_doc.py` 的显式白名单（测试源文件 → 机制/宿主或已迁移功能）确定新架构/已迁移 target，其余归 legacy；白名单条目须存在于 `tests/CMakeLists.txt`，两类互斥

### 新架构目录

| 目录 | 文件数 | 行数 |
| --- | --- | --- |
| `contracts/` | 8 | 372 |
| `game/` | 14 | 935 |
| `infra/` | 22 | 2007 |
| `hooks/` | 8 | 1280 |
| `patch/` | 4 | 285 |
| `domains/` | 2 | 111 |
| `features/` | 46 | 3236 |
| `host/` | 12 | 1781 |
| **合计** | **116** | **10007** |

### 已迁移插件（`features/*`）

- `features/attribute_range/`：4 文件 / 303 行
- `features/autosell/`：12 文件 / 1068 行
- `features/inventory_hooks/`：2 文件 / 98 行
- `features/push_all/`：6 文件 / 267 行
- `features/quest/`：4 文件 / 162 行
- `features/simple_mode/`：6 文件 / 462 行
- `features/special_equip/`：6 文件 / 423 行
- `features/stack_limit/`：2 文件 / 103 行
- `features/text_layer/`：4 文件 / 350 行

### CTest target 分类

- 全仓 target 总数：**33**
- 新架构/已迁移：**22**（机制/宿主 15 + 已迁移功能 7）
- legacy：**11**

新架构/已迁移（22）：
- 机制/宿主（15）：`capability_lifecycle_tests`、`hooks_dispatch_tests`、`hooks_registry_tests`、`hooks_transaction_tests`、`infra_capabilities_status_tests`、`infra_command_queue_tests`、`infra_config_commands_tests`、`infra_events_tests`、`infra_scheduler_tests`、`infra_snapshot_tests`、`infra_store_tests`、`patch_layer_tests`、`plugin_lifecycle_tests`、`plugin_registry_tests`、`store_sell_rules_tests`
- 已迁移功能（7）：`attribute_range_tests`、`autosell_rules_tests`、`autosell_store_tests`、`module_text_tests`、`push_all_rules_tests`、`simple_mode_rules_tests`、`special_equip_rules_tests`

legacy（11）：`custom_recipe_tests`、`game_feature_tests`、`game_variant_tests`、`host_tests`、`infra_data_tests`、`native_choice_tests`、`popup_enter_alias_tests`、`qol_log_tests`、`save_warehouse_validate_tests`、`stage4_hook_tests`、`transition_dispatch_tests`
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
| `domains/` | 中立领域逻辑（可被多个插件消费），当前仅 `attribute_range` | `domains/attribute_range/rules.h` |
| `features/` | 内置静态插件（一插件一目录，清单见生成统计区） | 各 `features/<id>/plugin.h` 的 `plugin_descriptor()` |
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

#### 2.6 共享文本分派（`hooks/dispatch.cpp` + `features/text_layer`）

- `features/text_layer` 是共享资源 `hook:MEMORYTEXT_GetText` 的唯一 owner；wrapper 取得原函数文本后交 `hooks::dispatch_run_text`，按 `before → original → after` 分派。
- 消费者不再各自挂同址 hook，改经能力 `text_layer` 订阅（权限以 `ConsumerKind` 表达）；内置文本表是 after 阶段的 Modify 消费者，见 `features/text_layer/feature.h`。

#### 2.7 状态 JSON（`host/status_json.cpp`）

- `host_feature_status_json` / `host_resources_json` / `host_ticket_json` 把 `infra::status` 与 `hooks::registry` 导出为 `GET /api/system/features|resources|tickets/{id}` 的响应体，由 `bridge/native/gamebridge_status.cpp` 调用。

## 过渡依赖（逐条注明目标清除阶段）

| 位置 | 过渡内容 | 目标清除阶段 |
| --- | --- | --- |
| `host/assembly.cpp` | bootstrap 顺序收敛；hook/patch 原语注入仍取自 `feature/patch` 与 `core/native/call_patch` | P5 |
| `host/frame_source.cpp` | 帧源在 `assembly` 之前被调用，故自注入 patch 原语（与 `assembly` 重复） | P5 |
| `features/quest/plugin.cpp` | 进档回调仍借用 legacy `core/native/save_enter` 设施 | 随存档域迁移（C/D 阶段） |
| `features/special_equip/plugin.cpp` | 出售门禁谓词注册调用 legacy `inventory_native_hook_set_sell_gate`（前置声明，避免 feature→feature include） | B2c 库存业务搬迁后改为共享分派消费者 |
| `features/inventory_hooks/plugin.cpp` + `feature/patch/native_inventory_hook.cpp` | claims 表与 backup 目标仍留 `feature/patch`（handler 只能在该 TU 取址），经 `inventory_hooks_claims` / `inventory_hooks_bind_backup` 两个自由函数桥接（双向不 include，避免 feature→feature 依赖）；24 条资源由宿主声明式安装 | claims/backup 随 B2c 业务搬迁迁入 `features/inventory_hooks` |
| `features/inventory_hooks`（B2a-1） | **禁止 deactivate**：extension_bag / 自动出售 / 合成等 legacy 消费者依赖这 24 个 wrapper（扩展袋读取、扩展补扣、出售门禁）；物理释放在插件 `deactivate` 路径会执行，消费者将在 wrapper 缺失下运行。宿主无「不可 deactivate」标志，靠流程约束 | 消费者改经 capability requires 收口后（B2c/D2）解除 |
| `feature/patch/native_inventory_hook.h` | 过渡导出：`inventory_native_hook_call_refresh_item_area_original`（refresh → B2c）、`inventory_native_hook_set_sell_gate`（sell-gate → B2c）、`native_hook_func` / `native_unhook_func` 机制原语（→ P5） | refresh/sell-gate：B2c；原语：P5 |
| `host/core_patches.cpp` + `game/access/patches.cpp`（B2a-2） | 常驻指令 patch 声明式化：19 条站点登记（位段缩窄 ×2 / clamp ×6 / 重定向 arg ×5 + call ×5 / hub ×1），`kBlRedirectToSymbol` 目标经 `fn_resolve` 解析。monster 兼容是**扫描式例外**：地址按运行时特征字节定位，无法静态登记，经 `patch::install_word_at` 占用同一张点表、不入 `hooks/registry` 资源表 | 扫描式例外长期保留（特征字节定位方式不变，无清除阶段） |
| `feature/patch/game_patch.{h,cpp}`（B2a-2） | 常驻 patch 原语与 stack_limit 状态已迁出，仅保留 move_merge 开关（GOT 槽指针 hook）与 IAP 恢复；`game_ui_virtbag.cpp` 仍因 move_merge 开关 include 本头（feature→feature 过渡依赖） | move_merge 相关随 B2a-3 迁移后删除 |
| `core/native/stack_limit_state.cpp`（B2a-2） | stack_limit 有效态实现放在 `core/native/`（`arch_common` 过渡映射为 `infra` 层）；只读端口 `stack_limit_port.h` 冻结 | P5 迁入 `infra/` 正式路径后转正 |

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
