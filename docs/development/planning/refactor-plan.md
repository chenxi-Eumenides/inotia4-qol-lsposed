# 代码、文档与工具重构计划

> 状态：代码拆分主体和文档目录迁移完成，进入维护阶段
> 更新：2026-09-04
> 适用基线：当前工作树 `v0.6.19`
>
> 本文是当前重构的执行计划，不是代码架构规范。当前有效的代码分层、依赖方向和 JNI 约束以 `docs/development/architecture.md` 为准；历史 P0-P4 实施记录见 `docs/history/refactor-plan-p0-p4.md`。

### 当前进度

- [x] P0：根目录及局部 `AGENTS.md`、正式脚本入口目录和历史文档边界已建立；文档目录已按四类用途迁移。
- [x] P1 第一阶段：扩展背包运行时文件已归入 `feature/extension_bag/`，纯模型/编码/所有权头文件已归入 `feature/extension_bag/model/`。
- [x] P2 第一阶段：UI 相关 native 文件已归入 `feature/ui/`，CMake 和跨域 include 已同步。
- [x] API 功能边界：原版信息/操作导出已建立独立 `api/` 边界，Kotlin Controller 已归入 `api/controller/`，不与模块增强 `feature/` 混合。
- [x] Native API 边界：纯原版域已迁入 `api/native/`，共享原语已归入 `core/native/` 与 `data/native/`；混合域已完成职责拆分。
- [x] P1：扩展背包运行时实现已按状态、持久化、事务、投影、绘制、输入、生命周期、装备和 API 拆分。
- [x] P2：世界域和 JNI 边界按职责拆分（UI 设置页、世界域、JNI bridge 及剩余 Native 混合域已完成）。
- [x] P3：API 内部 Kotlin Controller/Service 已按当前稳定职责组织；模块增强功能保持独立，不作为 API 业务实现。Kotlin 当前结构冻结，不再继续细拆。
- [x] Service 入口与实现解耦；Info 查询已拆出地图、佣兵、系统薄查询和商店查询文件，`InfoApiServiceCore.kt` 控制在 500 行以内。
- [x] Service 目录按稳定职责边界落地为 `service/info/`、`service/action/`、`service/enrichment/`，避免所有实现继续平铺在 `service/` 根目录。
- [x] Action 操作按 movement/inventory/character/party/quest/save/ui/extension bag 拆分，公共 attach 与保存后处理集中到 `ActionSupport.kt`。
- [x] 用户裁决：`InfoApiServiceCore.kt` 的 party/inventory/quest/ui/game 聚合和 `NameInjectorCore.kt` 暂不继续拆分；保持当前聚合边界，后续仅在出现独立变更需求或规模显著增长时再评估。
- [ ] P4（可选）：行数、依赖和脚本输出路径检查。仅作为后续维护建议，不属于本次重构必做项，不阻塞阶段完成。
- [x] 工具与脚本治理：旧 `analyze/`、`parse/` 已按用途迁移至 `analysis/`、`data/`、`verification/`；可复用 Frida patch 已归入 `analysis/frida/`，脚本缓存和分析日志已移出正式入口目录。

## 1. 重构目标

本次重构解决三个问题：

1. 按功能组织代码，一个功能拥有自己的目录，并在目录内按职责拆分。
2. 将面向用户、开发者和代码代理的文档分开，建立唯一权威来源。
3. 将脚本、第三方工具、生成产物和历史实验材料分开，降低误用和维护成本。

本次不以“所有文件都很短”为目标，而以以下结果为目标：

- 新增功能只需要进入一个明确的 feature 目录。
- Controller 不承载业务逻辑，JNI 文件不承载业务编排。
- 状态、持久化、事务、UI 绘制和 API 适配可以独立修改。
- README、AGENTS、架构文档和 API 文档各自只有一个职责。
- 每个重构阶段都能单独构建、测试、真机验证和回滚。

## 2. 当前基线与主要问题

### 2.1 代码热点

当前源码已经有 `game_character`、`game_inventory`、`game_world` 等功能文件，但扩展背包和 UI 仍然过于集中：

| 文件 | 规模 | 主要问题 |
|---|---:|---|
| `module/app/src/main/cpp/feature/extension_bag/game_ui_virtbag.cpp` | 351 行 | 编译单元装配、共享运行时状态和跨职责声明；具体实现已拆出 |
| `module/app/src/main/cpp/feature/extension_bag/model/virtual_bag_state.h` | 109 行 | 状态模型入口；载荷、拖拽、事务类型、状态操作和 JSON 已拆出 |
| `module/app/src/main/cpp/api/native/game_world.cpp` | 25 行 | 世界域职责已拆到 `.inc` 实现片段 |
| `module/app/src/main/cpp/bridge/native/gamebridge.cpp` | 40 行 | JNI 导出已按职责拆分，主文件仅保留生命周期入口 |
| `module/app/src/main/cpp/feature/ui/game_ui_settings.cpp` | 99 行 | UI 设置职责已拆到 `.inc` 实现片段 |
| `module/app/src/main/cpp/data/native/game_symbols.h` | 714 行 | 游戏版本常量集中，按设计暂不拆散，保持单一来源 |
| `module/app/src/main/java/com/inotia4/qol/service/info/InfoApiServiceCore.kt` | 500 行 | party/inventory/quest/ui/game 查询和公共快照组装仍集中，已拆出地图/佣兵/系统/商店查询 |
| `module/app/src/main/java/com/inotia4/qol/service/action/ActionApiServiceCore.kt` | 110 行 | 接口适配；操作实现已按域迁入同目录，公共 attach/保存后处理位于 `ActionSupport.kt` |

### 2.2 文档问题

- 根目录和源码边界已有 `AGENTS.md`，但文档入口和引用路径仍需统一。
- `README.md` 原同时包含项目介绍、架构规范、代理规则、环境规则和版本提交规则；**已重构**为面向用户的入口（功能、安装、使用、下载），代理规则与目录/环境/版本规范迁至根目录 `AGENTS.md`。
- `docs/development/architecture.md` 是有效规范，但仍混入较多历史迁移叙述。
- `docs/history/refactor-plan-p0-p4.md` 是旧 P0-P4 方案，不能继续作为当前目录结构的依据。
- `docs/` 中同时存在 API、环境、游戏指南、功能设计、交接记录和历史实验记录，入口层级不够清晰。

### 2.3 工具问题

- 原 `scripts/analysis/` 聚集静态分析、Frida 探针、UI 探索、地图验证、事件验证和一次性脚本。
- `scripts/data/` 已有数据导出职责，但与分析、验证脚本的入口风格不统一。
- `__handlers__/` 属于反编译/分析产物，不应与源码概念混在一起。
- `tools/` 的第三方工具、构建工具和运行输出需要明确区分。

## 3. 目标目录结构

当前只有一个 Android App，暂不拆成多个 Gradle module。代码目录和文档目录分别治理；文档目录重构不改变源码包结构，也不改变 API、JNI 和构建依赖。

```text
inotia4-qol-lsposed/
├── README.md                         # 项目总览、快速开始和顶层入口
├── AGENTS.md                         # 全仓库代理规则
├── .gradle/                          # 项目级 Gradle 缓存（可再生成）
├── .venv/                            # 项目级 Python 虚拟环境（可再生成）
├── pyproject.toml                    # Python 项目依赖和工具配置
├── uv.lock                           # Python 依赖锁定文件
├── docs/
│   ├── INDEX.md                      # 详细文档导航，不替代根 README
│   ├── guides/                       # 使用者、源码构建者和设备操作者指南
│   │   ├── game-guide.md
│   │   └── build-and-deploy.md
│   ├── development/                 # 开发维护、计划和模块功能设计
│   │   ├── architecture.md
│   │   ├── planning/
│   │   │   ├── backlog.md
│   │   │   └── refactor-plan.md
│   │   └── features/extension-bag/
│   │       ├── control-plane.md
│   │       ├── drag-protocol.md
│   │       └── module-save-store.md
│   ├── reference/                   # API 和原版游戏稳定事实
│   │   ├── api-reference.md
│   │   └── game/
│   │       ├── game-systems.md
│   │       ├── bag.md
│   │       ├── save.md
│   │       ├── ui.md
│   │       └── ui-kit.md
│   └── history/                     # 旧方案、交接和实验过程
│       ├── refactor-plan-p0-p4.md
│       ├── handoffs/
│       └── experiments/
├── module/
│   ├── AGENTS.md                     # Android 工程局部规则
│   └── app/src/main/cpp/
│       └── AGENTS.md                 # Native 局部规则
├── scripts/
│   ├── build-debug.sh                 # 自动发现项目 Gradle 缓存并构建 Debug APK
│   ├── AGENTS.md                     # 脚本局部规则
│   ├── analysis/                     # 静态分析和逆向探针
│   ├── verification/                 # smoke、回归和性能验证
│   ├── device/                       # adb 和设备操作
│   ├── data/                         # 数据解析和打包
│   └── maintenance/                  # 维护与一致性检查
├── tools/                            # 第三方工具本体，不放运行输出
├── apk/                              # 输入物与静态数据
├── output/                           # 最终 APK 交付物
├── archive/                          # 非当前实现依据的归档材料
└── .tmp/                             # 按任务隔离的一次性临时文件，完成后清理
```

根目录 `README.md` 始终保留。当前 `docs/development/architecture.md` 是代码规范唯一权威；`AGENTS.md` 按规则生效范围放在对应目录边界，不集中到 `docs/`。目录迁移完成后只保留一个正式路径，避免文档和脚本继续引用旧路径。

### 项目级环境与工具归属

以下目录和文件按“项目级环境声明、项目级缓存、工具本体、脚本源码”区分，不能仅为目录视觉集中而移动：

| 路径 | 归属 | 是否迁移 | 原因 |
|---|---|---|---|
| `.gradle/` | 项目级 Gradle User Home 和 wrapper 分发缓存 | 保留根目录 | Gradle 构建缓存服务整个项目；`GRADLE_USER_HOME=$PWD/.gradle` 与环境文档一致，移入 `tools/` 会把缓存误认为工具本体 |
| `.venv/` | 项目级 Python 虚拟环境 | 保留根目录 | `pyproject.toml`、`uv.lock` 和 `uv run` 以项目根为 Python 项目边界，所有脚本共享同一环境 |
| `pyproject.toml` | Python 依赖和工具配置 | 保留根目录 | 它描述整个项目的 Python 运行环境，不属于某一个脚本目录 |
| `uv.lock` | Python 依赖锁定结果 | 保留根目录 | 必须与 `pyproject.toml` 配对，保证项目级依赖可复现 |
| `tools/` | 第三方工具本体和工具发行包 | 保留 `tools/` | NDK、LSPatch 等是被脚本或构建流程调用的工具，不是构建缓存 |
| `scripts/` | 可复用脚本源码 | 保留 `scripts/` | 按分析、验证、设备、数据和维护职责组织，不承载运行环境目录 |

因此，`.gradle` 不迁入 `tools/`，`.venv`、`pyproject.toml`、`uv.lock` 也不迁入 `scripts/`。若保留 Gradle 的本地发行版压缩包，它属于 `tools/` 中的工具输入；解压后的 wrapper 缓存仍属于根目录 `.gradle/`。上述缓存、虚拟环境和工具本体均不纳入源码提交。

## 4. Kotlin 功能目录方案

将当前 `controller/`、`service/` 的平铺结构逐步调整为功能内聚结构：

```text
module/app/src/main/java/com/inotia4/qol/
├── bootstrap/
│   ├── HookMain.kt
│   ├── ApiServer.kt
│   └── NativeBridge.kt
├── core/
│   ├── config/
│   ├── error/
│   ├── json/
│   ├── logging/
│   └── staticdata/
├── integration/
│   └── ApiServices.kt
├── api/
│   ├── controller/
│   └── service/
│       ├── info/
│       ├── action/
│       └── enrichment/
├── patch/
└── store/
```

### Kotlin 拆分规则

- Controller 只负责路由、参数解析和调用 Service。
- Service 按功能拆分；不再让 `InfoApiServiceImpl` 负责所有查询。
- `ApiService.kt` 中的查询接口和操作接口按功能迁移到专用接口文件。
- `NameInjector.kt` 拆成按数据类型负责的富化器，例如 item、character、skill。
- 公共错误、JSON、配置、日志和静态数据放入 `core/`，不放进任意业务 feature。
- `NativeBridge` 保持为基础设施，既有 external 方法名、签名和 JNI 对应关系不变。
- API 路径、HTTP 方法和返回结构在本阶段保持不变。

## 5. Native 功能目录方案

```text
module/app/src/main/cpp/
├── bridge/
│   └── gamebridge.cpp
├── core/
│   ├── access/
│   ├── symbols/
│   ├── state/
│   ├── json/
│   ├── cache/
│   ├── frame/
│   └── operation/
├── feature/
│   ├── character/
│   ├── party/
│   ├── inventory/
│   ├── world/
│   ├── quest/
│   ├── dialog/
│   ├── shop/
│   ├── save/
│   ├── ui/
│   └── extension_bag/
├── patch/
└── tests/
```

`game_symbols.h`、`symbol_registry.h` 仍然是游戏版本常量和符号登记的唯一来源，不按功能复制。

### 5.1 扩展背包拆分

`game_ui_virtbag.cpp` 是第一优先级，建议按以下职责拆分：

| 目标文件 | 职责 |
|---|---|
| `extension_bag_state.cpp` | 当前背包、选中项和运行时状态 |
| `extension_bag_persistence.cpp` | sidecar 加载、保存和版本迁移 |
| `extension_bag_transaction.cpp` | 原版/扩展背包事务、回滚和恢复 |
| `extension_bag_projection.cpp` | 扩展背包到原版 UI 的投影 |
| `extension_bag_render.cpp` | 标签、格子和物品绘制 |
| `extension_bag_input.cpp` | 点击、拖拽、释放和命中检测 |
| `extension_bag_lifecycle.cpp` | 注入、退出、主菜单和存档切换 |
| `extension_bag_api.cpp` | `data_op_extension_bag_*` 和 JSON API |
| `extension_bag_internal.h` | 扩展背包内部跨文件接口 |

`virtual_bag_state.h` 再按模型、编码、所有权和恢复拆分。纯数据结构和纯函数优先移动到 host 测试可直接编译的文件。

### 5.2 世界域拆分

`game_world.cpp` 已拆为：

- `game_world_readers.inc`：地图、瓦片、单位、敌人、交互物和掉落读取。
- `game_world_navigation.inc`：BFS 和路径计算。
- `game_world_movement.inc`：逐帧移动任务。
- `game_world_operations.inc`：移动、行走、传送和交互操作。
- `game_world_story.inc`：剧情状态读取。

## 6. 文档职责方案

### README.md

只回答“项目是什么、如何快速使用”：

- 项目目标和功能概览。
- 支持的部署方式。
- 交付物说明。
- 快速构建和部署入口。
- 顶层目录概览。
- 文档索引。

详细代码规范、JNI 规则、环境踩坑和代理执行规则必须迁出。

### AGENTS.md

新增根目录 `AGENTS.md`，只维护代码代理必须遵守的规则：

- 修改前应阅读的文档。
- 源码、输入物、生成物和临时目录边界。
- JNI/API 不变约束。
- native 依赖方向。
- 构建、host 测试和真机验收要求。
- 不得向项目外写入产物。
- 版本、回滚和提交规则。

需要时增加局部规则文件：

```text
module/AGENTS.md
module/app/src/main/cpp/AGENTS.md
scripts/AGENTS.md
docs/AGENTS.md
```

代理文档不重复项目背景、API 手册和游戏玩法说明。

### architecture.md

当前有效规范位于 `docs/development/architecture.md`。该文档只维护：

- 分层定义。
- 依赖方向。
- 文件职责。
- JNI、缓存、线程和错误处理约束。
- 新增端点流程。

历史迁移原因、旧文件行号和已完成阶段放入 `docs/history/`。当前不单独建立 `decisions/`，跨功能决策放在对应开发文档中。

### 其他文档

| 文档 | 唯一职责 |
|---|---|
| `docs/guides/` | 使用模块、源码构建、部署和设备操作 |
| `docs/development/architecture.md` | 代码结构、依赖方向和工程规范 |
| `docs/development/planning/` | 当前待办和重构计划 |
| `docs/development/features/<feature>/` | 模块增强功能的设计、状态机和验收 |
| `docs/reference/api-reference.md` | 路由、参数、返回值和状态码 |
| `docs/reference/game/` | 原版游戏系统和稳定逆向事实 |
| `docs/history/` | 交接、实验、历史方案和过期材料 |

### 文档目录重构方案（已执行）

文档目录重构只调整文档归属和引用路径，不修改代码、API 契约、功能行为或验收结论。迁移完成后，每个主题只保留一个正式入口；旧路径不长期保留副本，历史内容只进入 `docs/history/`。

根目录 `README.md` 必须保留，仍是 GitHub/项目根目录的第一入口，负责回答“项目是什么、如何快速开始、重要文档在哪里”。`docs/INDEX.md` 是四类文档的导航页，不替代根目录 README。

#### 目标目录

```text
docs/
├── INDEX.md                        # 文档地图与阅读入口
├── guides/                         # 使用者、源码构建者和设备操作者指南
│   ├── game-guide.md
│   └── build-and-deploy.md
├── development/                    # 开发维护文档
│   ├── architecture.md
│   ├── planning/
│   │   ├── backlog.md
│   │   └── refactor-plan.md
│   └── features/extension-bag/
│       ├── control-plane.md
│       ├── drag-protocol.md
│       └── module-save-store.md
├── reference/                      # API 和原版游戏稳定事实
│   ├── api-reference.md
│   └── game/
│       ├── game-systems.md
│       ├── bag.md
│       ├── save.md
│       ├── ui.md
│       └── ui-kit.md
└── history/
    ├── refactor-plan-p0-p4.md      # 旧方案和阶段记录
    ├── handoffs/                   # 已结束阶段的交接记录
    └── experiments/                # 已结束实验、探针结论和旧报告
```

#### 文件迁移映射

| 当前路径 | 目标路径 | 处理原则 |
|---|---|---|
| `docs/game-guide.md` | `docs/guides/game-guide.md` | 使用流程，不承担实现规范 |
| `docs/environment.md` | `docs/guides/build-and-deploy.md` | 源码构建、设备和部署流程 |
| `docs/api-reference.md` | `docs/reference/api-reference.md` | 唯一 API 契约 |
| `docs/game-systems.md` | `docs/reference/game/game-systems.md` | 游戏机制参考 |
| `docs/system/*.md` | `docs/reference/game/*.md` | 稳定逆向结论；实验记录另行归档 |
| `docs/extension-bag-control-plane.md` | `docs/development/features/extension-bag/control-plane.md` | 功能控制面 |
| `docs/p5-*.md` | `docs/development/features/extension-bag/drag-protocol.md` | 扩展背包开发工作包 |
| `docs/module-save-store.md` | `docs/development/features/extension-bag/module-save-store.md` | sidecar 实现契约 |
| `docs/backlog.md` | `docs/development/planning/backlog.md` | 当前待办唯一来源 |
| `docs/refactor-plan.md` | `docs/development/planning/refactor-plan.md` | 当前重构方案 |
| `architecture.md` | `docs/development/architecture.md` | 当前代码规范 |
| `docs/handoff-*.md` | `docs/history/handoffs/` | 已结束阶段交接材料 |
| `docs/system/ui-experiments.md` | `docs/history/experiments/ui-experiments.md` | 实验过程和结果 |
| `docs/改版作者的一些探索文档/` | `docs/history/experiments/` | 历史探索资料 |

#### 文档职责边界

1. `README.md` 只介绍项目是什么、交付物是什么以及从哪里开始，不承载详细规范、环境踩坑和完整 API。
2. 根目录 `AGENTS.md` 只维护整个仓库的代理执行规则；`docs/INDEX.md` 只维护文档地图和阅读顺序。
3. `docs/development/architecture.md` 只维护当前代码结构、依赖方向和不可违反的工程约束。
4. `docs/reference/api-reference.md` 只维护 API 契约；`docs/guides/` 只维护使用、构建、部署和验收流程。
5. `docs/development/features/` 维护模块新增功能的设计和验收；`docs/reference/game/` 维护原版游戏机制参考；两者都不替代 backlog。
6. `docs/development/planning/` 只维护当前计划和待办；已完成事项只保留简短状态，不复制历史过程。
7. 当前不单独建立 `decisions/`；仍然有效的决策放在对应开发文档中，被新决策替代的内容移入 `history/`。
8. `docs/history/` 中的内容只用于追溯，代理不得依据其中的旧路径、旧版本或旧验收结论进行当前实现。

#### AGENTS.md 的目录层级规则

`AGENTS.md` 不是面向用户的项目说明文档，而是“所在目录及其子目录”的代理执行规则。它必须放在规则需要生效的目录边界上，不能统一收进 `docs/`：

| 文件 | 生效范围 | 作用 |
|---|---|---|
| 根目录 `AGENTS.md` | 全仓库 | 全局文档阅读顺序、目录边界、API/JNI 约束和通用验证要求 |
| `module/AGENTS.md` | Android 工程 | Gradle、APK、源码和构建产物约束 |
| `module/app/src/main/cpp/AGENTS.md` | Native 源码 | CMake、符号、锁边界、host test 和 native 依赖规则 |
| `scripts/AGENTS.md` | 脚本目录 | 脚本输入输出、项目内产物和运行方式 |
| `docs/AGENTS.md`（可选） | 文档目录 | 文档命名、唯一权威来源、链接和历史归档规则 |

因此，源码代理在修改 `module/` 或 `scripts/` 时可以自动获得对应局部约束；若所有规则都放在 `docs/AGENTS.md`，它们不会覆盖源码目录，也容易被执行代码任务的代理忽略。`docs/AGENTS.md` 只适合补充文档目录规则，不能替代根目录或源码目录的 `AGENTS.md`。

#### 已执行步骤

1. 已新增 `docs/INDEX.md`，登记四类文档、读者和权威关系。
2. 已创建 `guides/`、`development/`、`reference/` 和 `history/` 目录，并按迁移映射移动文档。
3. 已按职责归位文档：使用流程进入 `guides/`，实现方案进入 `development/`，稳定事实进入 `reference/`，实验过程进入 `history/`。
4. 已更新 `README.md` 和根目录 `AGENTS.md` 的正式路径引用。
5. 已更新主要 Markdown 文档引用；剩余历史正文中的旧路径仅作为迁移前记录保留，不作为当前入口。
6. 已通过 `git diff --check`；本方案不要求新增 P4 自动化检查脚本。

## 7. 工具与脚本治理

工具与脚本目录治理已完成，当前仅保留可重复使用的入口：

- `scripts/analysis/frida/save0-repair-patch.js`：已登记的可复用存档修复 patch；其他一次性逆向探针不保留在当前项目。
- `scripts/verification/`：smoke、性能、回归和 live session。
- `scripts/device/`：adb、设备启动、安装和触摸自动化。
- `scripts/data/`：静态数据导出、校验和 assets 打包。
- `scripts/maintenance/`：符号检查、依赖检查和工作区维护。

迁移规则：

1. 一次性实验脚本移入 `archive/experiments/`。
2. 可重复使用的验收脚本移入 `scripts/verification/`。
3. `__handlers__/` 移入 `archive/handlers/`，不再作为源码入口。
4. 第三方工具放在 `tools/third-party/`，工具输出不得放在工具目录。
5. 每个脚本顶部说明用途、输入、输出、是否写文件和是否需要真机。
6. 所有产物只能进入 `apk/`、`output/`、`archive/` 或 `.tmp/`。
7. `.tmp/` 必须按 `.tmp/<task-name>/` 隔离；完成后清理，不能作为源码、正式脚本、第三方工具或长期证据目录。

## 8. 分阶段执行顺序

### P0：建立边界

- 新增 `AGENTS.md`。
- 精简 README。
- 将旧重构方案改为历史记录。
- 建立新的 docs 索引和工具分类规则。

P0 不改业务逻辑、API 路径、JNI 签名和构建依赖。

### P1：扩展背包拆分

- 先拆 `game_ui_virtbag.cpp`。
- 再拆 `virtual_bag_state.h`。
- 每次只移动一个职责组。
- 每步执行 host test、Gradle 构建和扩展背包真机验收。
- 保持 `g_virtual_bag_mtx` 锁边界不变，禁止锁内调用会触发缓存刷新的 `op_ok()`。

### P2：UI 和世界域拆分

- `game_ui_settings.cpp` 已按几何、配置桥接、绘制、面板交互、注入和 API 拆分。
- 拆 `game_ui.cpp` 与 UI 辅助文件。
- `game_world.cpp` 已按剧情、读取、导航、移动任务和世界操作拆分。
- `bridge/native/gamebridge.cpp` 已按生命周期、数据读取、基础操作、UI、域操作、设置和扩展背包拆分。
- `game_inventory.cpp`、`game_dialog.cpp`、`game_access.cpp`、`game_patch.cpp` 和 UI 辅助文件已按职责拆分。
- `gamebridge.cpp` 只保留 JNI 转发、参数转换和结果转换。

### API 与增强功能的 native 归属

- API 原版导出域：角色、队伍、库存、世界、任务、对话、商店和系统聚合。
- 模块增强域：扩展背包、支付弹窗阻碍、堆叠上限、设置页增强、扩展存档支持。
- 共享技术域：符号解析、内存访问、JSON、缓存、寻路、帧任务和 JNI 桥接。
- `game_inventory`、`game_ui`、`game_save`、`game_patch` 当前含有跨边界调用，必须先提取增强适配层，再移动 API 原版部分。

### P3：Kotlin 服务内聚（已完成，结构冻结）

- 按功能拆分 `InfoApiServiceImpl.kt`。
- 按功能拆分 `ActionApiServiceImpl.kt`。
- 将查询、操作、快照 attach 和名称富化分离。
- Controller 只依赖功能 Service，不直接依赖 `NativeBridge`。
- `InfoApiServiceCore.kt`、`NameInjectorCore.kt` 保持当前聚合边界，不再因行数或形式继续拆分。

### P4：规则检查建议（可选，不阻塞重构）

以下内容只作为后续维护时可选采用的检查清单，不创建强制脚本，不纳入本次重构验收：

- 单个 `.cpp` 或 `.kt` 超过 500 行时人工审查。
- 人工检查 Controller 对 `NativeBridge` 的直接引用。
- 人工检查 `core/data` 对 `feature` 的反向依赖和 feature 循环依赖。
- 人工检查脚本输出路径是否位于项目目录内。
- 人工检查每个 feature 是否具有入口、实现和测试/验收说明。

## 9. 风险控制

| 风险 | 控制措施 |
|---|---|
| JNI 名称和签名耦合 | 既有 `NativeBridge` external 面冻结，JNI 只在 `gamebridge.cpp` 适配 |
| CMake 源文件迁移漏项 | 每次迁移同步更新 CMake，并执行全量构建 |
| 扩展背包锁边界变化 | 先记录锁范围，再做纯移动，不在拆分阶段改算法 |
| API 路由变化 | 路由 smoke 基线对比，路径、方法和成功响应保持不变 |
| 文档重复和冲突 | 每个主题指定唯一权威文档，其他文档只链接引用 |
| 历史脚本误用 | 一次性脚本归档，正式验证脚本使用统一入口 |
| 重构范围失控 | 每个阶段只处理一个边界，禁止顺手修复无关业务问题 |

## 10. 验收标准

每个实际执行的代码重构阶段必须满足：

1. Gradle debug 构建成功。
2. CMake host tests 全部通过。
3. JNI 既有方法名和签名未变化。
4. API 路由 smoke 通过，路径、方法和成功响应无非预期变化。
5. 扩展背包相关阶段完成真机验证，确认无崩溃、无死锁。
6. 文档链接有效，README、AGENTS、architecture 和 API 文档无重复权威描述。
7. Git diff 只包含当前阶段的目录迁移、职责拆分或对应测试。

P4 规则检查不属于上述强制验收条件；是否采用由后续维护需要决定。

每个阶段应形成独立提交，问题通过 `git revert` 回滚，不使用跨阶段的大型混合提交。
