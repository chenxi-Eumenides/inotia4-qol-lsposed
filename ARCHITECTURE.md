# 架构设计书：Loader、Game Adapter 与插件生态

> **目标架构唯一权威：本文。版本：v0.4.2。**
>
> 本文定义系统目标架构、职责边界、依赖方向及兼容红线，是目标架构的唯一权威。HTTP 契约见 `docs/reference/api-reference.md`；当前源码结构统计见 `docs/refactor/map.md`；验证矩阵与活跃验收状态见 `docs/refactor/verification.md`。

## 1. 目标与生态模型

### 1.1 目标

系统必须同时满足：

1. **机制与业务分离**：Loader 可以独立启动、记录日志、读取配置、调度帧任务和仲裁资源，但不产生用户可见游戏功能。
2. **版本耦合单点收口**：游戏版本、符号、ABI、线程事实和低层受控适配只进入 Game Adapter/Game Host。
3. **语义按领域归属**：当前地图、角色/队伍、物品/背包、存档/任务等高频语义由窄领域的官方核心依赖 provider 插件提供，不由 Loader、Game Host 或一个万能核心插件集中提供。
4. **功能可裁剪**：数量读写、扩展背包、额外物品、新合成、自动出售、传送等特殊修改属于功能插件或其依赖插件；启停、失败和依赖关系可观测。
5. **插件可组合**：功能插件可消费 provider，也可直接声明 Loader 的 hooks/patches 实现自身功能；使用 provider 不是获得 hook/patch 的前置条件。
6. **可迁移、可验证、可外置**：插件只依赖公共契约、自身实现和注册表能力；内置静态插件按未来可外置的边界开发，但不提前冻结外部 SDK。

### 1.2 生态角色

```text
Loader（机制运行时）
  ├─ Game Adapter / Game Host（版本与低层受控适配）
  ├─ core.api（HTTP 共享出口）
  ├─ core.custom_ui（UI 共享工具：组件、面板框架、游戏原生控件、文本机制）
  ├─ core.settings（普通成品页插件：模块设置页，仅渲染 Host feature catalog）
  ├─ core.<domain>（领域 owner：读写一体，如 core.world / core.character / core.item / core.save / core.quest / core.craft / core.system / core.ui_state）
  └─ 功能插件 / 依赖插件（特殊修改与组合）
```

这是一种 loader + core 插件（共享出口/工具/成品页 + 领域 owner）+ 依赖模组 + 功能模组的生态。`core.*` 是**官方核心插件的命名空间**，分三类：共享出口（`core.api`）、共享 UI/文本工具与成品页面（`core.custom_ui`、`core.settings`）、领域 owner（`core.<domain>`，一个领域一个 owner、读写一体、内部再分读面/写面）。它不是高权限层——core 插件与其它插件走同一套契约、依赖、启停和错误规则；也不得用单个 `core.game` 之类的聚合插件承载全部领域语义，领域必须按真实消费者拆分，不受名称或固定插件总数限制。

### 1.3 零插件组合

“零插件组合”是正式验收场景：仅加载 Loader 与 Game Adapter/Game Host，不注册任何 provider、功能插件或 `core.*` 插件，游戏仍可启动并保持原版行为；Loader 只能完成自身初始化、日志、配置、帧任务和机制待命，不显示模块 UI、不安装业务 hook/patch、不提供业务 HTTP 数据，也不改变游戏规则。

因此，Loader/Game Host 的启动成功不能以“某个功能已可用”作为条件；插件全部缺失是合法状态，依赖插件缺失只影响依赖它的插件。

### 1.4 并行开发与内核先行

官方 provider 与功能插件可以并行开发。Loader 接口不是一次性为所有未来插件设计，而是由真实插件需求驱动：只有在存在当前消费者且该需求属于通用机制时，才为 Loader/Host SDK 增加窄接口；插件专属需求留在插件自身，不因“以后可能需要”扩展 Host SDK 或 Game Host。

除外部插件加载与分发外，Loader/模块内核可以先完成。此时只能称为**内核能力完整**，不表示所有 provider、功能插件已经实现，也不表示所有插件 API 已提前冻结。内核优先完成范围为：

- 零插件启动和原版行为保持；
- 自举日志、配置和初始化；
- 插件描述符、注册、依赖解析、生命周期和状态诊断；
- capability、命令、事件、帧调度和 store 机制；
- hook/patch/slot 资源声明、占用仲裁、安装、回滚和还原；
- Game Adapter 的低层机制、fake 后端和对应测试。

内核优先完成不包含任何用户功能、领域 provider、功能插件或外部插件加载/分发。provider 与功能插件可在内核契约稳定到其真实需求的范围内并行接入；新需求继续按上一段的“当前消费者 + 通用机制理由”准入。

## 2. 权威、范围与兼容红线

### 2.1 文档权威层级

| 内容 | 唯一依据 |
|---|---|
| 最终目标架构、职责和依赖方向 | 根目录 `ARCHITECTURE.md`（本文） |
| HTTP 路径、方法、参数、成功响应和状态码 | `docs/reference/api-reference.md` |
| 验证矩阵、验收卡和活跃债务 | `docs/refactor/verification.md` |
| 当前生成的结构地图、基线与路由映射 | `docs/refactor/map.md`、`docs/refactor/baseline.md`、`docs/refactor/api-route-map.md` |

上述材料不能反向放宽本文目标；脚本通过只表示当前检查结果，不表示目标边界已经满足。普通历史和归档材料不参与目标解释。

### 2.2 不可破坏的项目红线

- 既有 API 路径、HTTP 方法保持不变；query 路由的成功响应保持冻结。带游戏内存写入的写路由按 §7.1 的票据契约执行：POST 立即返回 `{"ok":true,"ticket":N}`，原操作结果在票据终态尽力保留、可回取；受影响路由以 `docs/reference/api-reference.md` 的显式枚举为准，属刻意的调用方契约变更，不得静默替换。
- 既有 `NativeBridge` external 方法名与签名保持冻结；新增接口必须另行设计兼容面，不借迁移删除或改写既有签名。
- 配置唯一持久化来源为 native Host 常驻管理设施持有的外部 `config.json`。每个已注册 `plugin_id` 持久化为一个对象 `{"enable":bool,"features":{feature_id:bool},"config":{自定义key:任意JSON}}` 三层模型；schema、Store、读写与管理 API 全部由 Host 常驻模块设施持有，与任何插件（含设置页）生命周期分离。Kotlin 只传输 raw HTTP body，不读取、解析、缓存或写入配置。
- `enable` 是 Host 独占的 lifecycle 期望状态，普通插件不可读写；`features` 只含当前注册 feature 的布尔值（注册元数据 default 缺失时补齐，已有值优先）；`config` 是插件业务自由键值。插件可读自己的完整 namespace（不含 `enable`），settings 获 Host 管理授权。单 key 更新在同一 Store 事务内完成 durable write 与内存提交；feature 更新还在同一事务中把 `enable` 重算为更新后全部已注册 feature 布尔值的逻辑或。持久化失败时 feature、派生 `enable`、revision 与运行时 effects 均不提交；旧整 namespace replace/clear 槽返回 Unsupported。
- `game_symbols.h` 是内置适配所用游戏版本常量、VMA、偏移和 ABI 签名的唯一来源；域插件不得持有裸地址或 include 该头。插件可为 `EntryHook` 声明纯导出符号名（`ResourceClaim.symbol = "elf-export:<ELF导出名>"`）作为目标身份；它不是版本耦合裸地址，也不取代 `game_symbols.h` 对内置 VMA/ABI 常量的权威性。
- 游戏写入和有副作用调用只能在游戏主线程的 owner 命令处理器中执行；HTTP、初始化和后台线程不直接调用游戏函数，也不阻塞等待游戏结果。
- 同进程 native 代码无法获得崩溃隔离。受控 API 只提供目标校验、线程纪律和错误返回，不构成安全沙箱。
- 任何资源必须有目标校验、占用仲裁、失败回滚、开关和状态上报；未登记的 raw hook/patch 不获得 Loader 的回收保证。

## 3. 顶层结构与依赖

### 3.1 七个逻辑目录

```text
module/app/src/main/cpp/
  contracts/   跨边界 POD、函数表、版本和命令类型
  game/        Game Adapter 的版本耦合实现
  infra/       Loader 的调度、快照、事件、配置、日志、存储和 Result
  hooks/       hook 资源登记、仲裁、安装、分派和还原
  patch/       指令 patch、指针槽、原字校验和回滚
  plugins/     core 出口、官方 provider、功能插件和依赖插件
  host/        入口、JNI、装配、注入和平台桥接
```

这是物理职责划分，不是插件分类的数量清单。`contracts/` 是无实现、无状态的中立层；`plugins/` 是所有业务和共享出口的唯一业务目录；`host/` 负责装配，不承载业务规则。

### 3.2 依赖矩阵

| 来源 \ 目标 | contracts | game | infra | hooks | patch | plugins | host |
|---|---:|---:|---:|---:|---:|---:|---:|
| `contracts/` | — | — | — | — | — | — | — |
| `game/` | ✓ | — | — | — | — | — | — |
| `infra/` | ✓ | — | — | — | — | — | — |
| `hooks/` | ✓ | ✓ | ✓ | — | — | — | — |
| `patch/` | ✓ | ✓ | ✓ | — | — | — | — |
| `plugins/<id>/` | ✓ | — | — | — | — | — | — |
| `host/` | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | — |

规则：

- 插件源码只能 include `contracts/` 与自身目录；不得 include 其他插件源码或 `game/`、`infra/`、`hooks/`、`patch/`、`host/` 私有实现。
- 插件之间不产生编译期依赖；运行时通过 capability、快照、事件、命令端口和 Host SDK 函数表协作。
- `hooks/`、`patch/` 是 Loader 机制，可使用 Game Adapter 的内部适配；插件只声明资源和提供 handler/消费者回调，不直接调用机制实现。
- `host/` 可以依赖全部目录来装配和注入，但装配代码不决定领域业务顺序、不实现业务算法。
- 结构校验应把插件越层 include/link 作为目标违规；基线不能将其变成永久许可。

### 3.3 层职责

| 层 | 负责 | 不负责 |
|---|---|---|
| `contracts/` | 固定宽度 C 类型、POD、函数表、版本、Result、命令和事件载荷 | 实现、全局业务函数、STL 容器、游戏地址、业务状态 |
| `game/` | 变体识别、符号解析、ABI、目标校验、线程标注、低层 read/write/call 和 UI 原语适配 | 领域 DTO、角色/背包/存档语义、功能开关、插件生命周期 |
| `infra/` | Loader 的帧调度、命令队列、快照容器、事件、能力注册、日志、配置、存储和错误结果 | 游戏版本知识、业务规则、领域状态 |
| `hooks/` / `patch/` | hook、指令 patch、指针槽的登记、安装、冲突、分派、还原 | 业务语义和用户可见功能 |
| `plugins/` | provider、功能、依赖和共享出口的规则、状态、UI、能力、命令及资源声明 | 直接进入宿主私有实现、越过 owner 命令写游戏 |
| `host/` | 入口、JNI、平台桥、Game Host 注入、插件注册和拓扑装配 | 把 Loader 或 Game Host 变成业务聚合层 |

## 4. Loader、Host SDK 与 Game Adapter 边界

### 4.1 Loader：只提供通用机制

Loader 是常驻机制运行时，不是业务插件。它可以提供：

- 模块入口、初始化顺序、插件发现/注册和生命周期执行器；
- 日志、外部 `config.json` 自举读取、配置命名空间和持久化；
- 主线程判断、命令投递、帧任务和有界帧步进；
- hooks、patch、ptr slot、所有权槽的声明、冲突仲裁、安装、禁用、回滚、物理还原和状态；
- 通用 capability/事件注册表、typed snapshot 容器、命令票据、存储事务和错误结果；
- Kotlin 与 native 之间的 HTTP 传输桥，以及插件通用的 Host SDK 函数表。

Loader 不得：

- 读取或解释当前地图、角色、队伍、物品、背包、存档、任务等游戏语义；
- 内置自动出售、传送、扩展背包、数量策略、合成等用户功能；
- 为业务插件预装或默认启用业务 hook/patch；
- 生成业务 DTO、业务 HTTP 路由、业务 JSON 或业务 UI；
- 以“方便插件”为由新增跨领域万能接口。

即使 Loader 能调用 Game Adapter 的低层入口，也只能执行插件声明并由资源事务管理的机制动作。Loader 独立运行时没有用户可见游戏功能。

### 4.2 Host SDK：中立机制面

Host SDK 是 Loader 注入给插件的中立函数表，内容限于日志、配置、调度、事件、能力查找/发布、快照、命令端口、存储、面板登记和 hook/patch/resource 声明。SDK 不返回万能游戏对象，不持有裸游戏指针，不允许插件绕过 owner 命令执行副作用。

公共契约只保留 POD、固定宽度类型和函数指针；错误通过 `QolResult` 或等价明确结果表达。不得把实现函数直接作为 `contracts/` 的全局链接入口。

### 4.3 Game Adapter 与 Game Host

Game Adapter 是 `game/` 的实现，Game Host 是 Loader 注入给插件的**窄低层受控适配面**。它们只覆盖：

- 当前游戏 target、变体、能力位和符号可用性；
- 主线程判定、主线程投递和帧事实；
- 经目标校验的低层 read/write/call、指针/句柄检查、ABI 错误返回；
- 游戏 UI 的底层资源、绘制、控件和命中适配；
- 为某个低层机制所必需的稳定受控 dispatch slot。

“受控”表示校验、线程和错误语义统一，不表示安全隔离。Game Adapter/Game Host 不提供角色、队伍、物品、库存、地图、存档、任务、合成或传送等高阶业务 API，不承载功能开关，不管理普通插件生命周期，也不提供一张跨域聚合的 `QolGameHost`/`Extra` 万能表。

高阶语义的实现路径必须是：

```text
插件 → provider capability / owner command
provider → Game Host 低层适配
Game Host → Game Adapter → 游戏
```

Game Host 不提供跨域聚合的 `QolGameHost` 或 `Extra` 面；高阶领域语义由对应 provider 或功能插件拥有，并通过窄领域接口使用低层受控适配。

## 5. 插件分类与领域归属

### 5.1 三种来源判定

每个新接口先按以下顺序判断：

1. 只表达 target、变体、线程、帧、低层受控 read/write/call 或 Loader 机制 → Host SDK/Game Host。
2. 表达稳定领域语义，存在多个真实插件消费者或需要领域唯一所有权 → 独立官方 provider 插件。
3. 只有一个消费者、纯算法或私有状态 → 留在该功能插件内部。

“实现方便”不是归属理由。单消费者语义不得为了方便塞进 Loader、Game Host 或 provider；出现稳定的第二个真实消费者后，才评估提升为 provider。

### 5.2 官方核心依赖 provider

官方核心插件是 `plugins/` 中可启停、可依赖、可测试的普通插件，命名为 `core.<domain>`，与共享出口 `core.api`、UI 工具 `core.custom_ui`、成品页 `core.settings` 同族。它们**按领域分类，一个领域一个 owner、读写一体**（内部再分读面/写面），而不是由一个 `core.game` 或 `core.api` 聚合全部语义。可存在的领域包括但不限于：

- 当前地图、世界状态和地图事实；
- 角色、队伍、装备和角色状态；
- 物品、库存、数量、原版/扩展背包视图；
- 存档槽、存档读写参与和任务事实；
- 合成结果，或其他确有多个消费者的稳定语义。

界面与文本不另立“万能 UI 域”：**界面状态**（当前画面、游戏自身面板、对话选择框）是 `core.ui_state` 这一个领域的读写一体 owner；**可复用的 UI 组件、面板框架、游戏原生控件与文本机制**是共享工具 `core.custom_ui`；**开箱即用的整页**（模块设置页为首个）是 `core.settings`。功能插件拥有自己的页面与文案，只消费上述工具。

每个 provider 只发布自己拥有的窄 capability、typed snapshot 和 owner command：

- 只读事实通过 capability 或快照提供，接口不暴露游戏内存指针；
- 有副作用的动作由 provider/实际 owner 通过命令端口在主线程执行；
- provider 不把呈现决策、HTTP JSON 形状或全部领域聚合塞进语义接口；owner 自组装、固定 kind、版本化 buffer 的只读状态 JSON 投影属于所有者契约面；跨域消费者仅可筛选与重组，不得按他域字段重拼装；
- provider 停用只级联真实依赖者，不能让整个模块因一个领域失效。

provider 的数量和命名随领域边界演进，不受固定插件数量或固定 `core.*` 数量约束。

### 5.3 功能插件与依赖插件

功能插件拥有一个可裁剪的用户功能及其规则、配置、UI、状态和验收。例如：

- `stack_limit` 拥有 99/999 数量策略、S2 编解码和数量读写行为；
- `extension_bag` 拥有扩展背包、额外物品和背包持久化；
- 自定义合成插件拥有新配方与合成行为；
- `autosell` 拥有自动出售策略；
- `world_teleport` 拥有传送功能。

它们可以消费官方 provider，也可以直接通过 Host SDK 声明自己需要的 hooks、patches、ptr slots 或所有权资源。功能插件不必先经过核心 provider 才能使用 Loader 机制，但不得直接 include Loader/Game Adapter 私有实现。

功能插件之间用 capability、事件和命令端口协作；能力 id、版本、接口类型和发布模式必须静态声明。高频上下文（进档、面板、商店打开、当前角色变化）用 `availability/probe`、快照或领域事件表示，不用反复注册/注销 capability。

### 5.4 共享出口、共享工具与成品页插件

`core.*` 只表示官方核心插件，并不授予额外权限。未来是否存在更多共享出口/工具/成品页由实际消费者和稳定契约决定，不用总数约束。

| 插件类别 | 允许职责 | 明确禁止 |
|---|---|---|
| `core.api` | HTTP 共享出口、路由/能力登记协作、通用门控和状态出口 | 游戏语义、领域 live 读取、业务规则、万能 native HTTP router |
| `core.custom_ui` | 可复用 UI 组件、面板注册/导航框架、**游戏原生控件**（如原生选择框）与**文本机制**（独占文本钩子 + 注册接口） | 具体功能页面、游戏业务语义、替其他插件承载文案内容、底层资源细节 |
| `core.settings` | 模块设置页这一**成品页**的内容/行为/资源；只渲染 Host feature catalog 并调用 Host 常驻管理 API | 通用组件框架、通用文本机制、游戏语义、schema/Store/配置与 lifecycle 管理 API 所有权 |
| 领域 owner `core.<domain>` | 一个窄领域的事实与 owner 行为（读写一体） | 聚合全部游戏对象、替代 Loader/Game Host、承担所有 API/UI |
| `core.feature` | 迁移期临时接管已证实未实现 HTTP 端点的兼容占位响应 | 最终发布插件、已实现功能、任意新业务或领域职责 |

`core.api`、`core.custom_ui`、`core.settings` 都是普通插件：默认可启用但可停用，停用只使对应出口/工具/页面降级，不使 Loader 日志、配置、生命周期、Game Adapter 或其他业务插件失效。`core` 前缀不得成为迁移垃圾桶。**机制与内容分离**：可复用的机制（组件、控件、文本钩子与注册接口）归 `core.custom_ui`；具体内容（页面、文案、数据）归各自功能或领域插件。

`core.feature` 是仅用于开发迁移期的临时插件，不属于最终发布插件。它只暂存已逐条证实尚未实现的 HTTP 端点，并保持原有占位响应；端点实现后必须迁入对应领域 owner。本插件清空后删除，不得扩展为一般功能、领域 owner 或新的占位收容处。

## 6. 资源、生命周期与线程

### 6.1 声明式资源与直接机制使用

插件可以声明：入口 hook、调用点 patch、指针槽、行为所有权槽和其他 Loader 支持的资源。资源描述至少包含目标、机制、owner、必需/可选、失败策略、可还原性、门控和处理入口。

- 一个资源一个 owner；共享资源由 owner 提供 wrapper/backup，消费者只注册回调或数据。
- Loader 负责冲突检测、目标校验、安装、空转/禁用、回滚、在途计数和物理归还。
- `entry hook`/`ptr slot` 可在禁用时保留空转 wrapper；调用点 patch 禁用按物理还原处理。
- 失败按资源声明降级或 fail-closed；物理还原失败统一保留 `tombstone`、标记 `failed` 并隔离，不能谎报空闲。
- 未通过资源登记而自行安装的 raw hook/patch 属插件自有副作用，Loader 不承诺冲突检测、回滚或停用回收。

因此，“插件可直接声明 hook/patch”与“Loader 不做业务”同时成立：插件拥有业务意图和 handler，Loader 只执行通用机制。

### 6.2 生命周期

插件至少经过 `registered → resolving → installed_disabled/enabled`，并支持 `blocked`、`unsupported`、`invalid`、`deactivating`、`inactive`、`failed` 状态。`degraded` 是独立健康标记，不替代生命周期状态。

- `requires` 缺失进入 `blocked`；变体或必需符号不满足进入 `unsupported`；清单或 ABI 非法进入 `invalid`。
- enable/disable/deactivate 幂等并由唯一执行器串行化；必需能力撤销按依赖图级联，可选能力进入 degraded。
- 资源安装、等待在途归零、回滚和还原通过帧任务推进；主线程不得睡眠或阻塞等待。
- 资源安装失败按逆序回滚；回滚失败进入 `tombstone + failed + isolated`。
- 插件停用清除消费者、能力暴露、订阅和托管资源；不要求 `dlclose`。

### 6.3 线程与数据

| 调用方 | 允许 | 禁止 |
|---|---|---|
| HTTP/Kotlin 线程 | 读取 typed snapshot、经 owner API 读取配置投影，提交命令并查询票据；Kotlin 仅传输配置 raw body | live 游戏读取、直接读内存、同步等待、Kotlin 解析/缓存/写配置 |
| 游戏主线程 | live 读取、命令执行、资源步进、事件派发、快照发布 | 睡眠、自旋、阻塞等待 |
| 初始化线程 | 读取配置、注册插件、投递任务 | 访问游戏状态、执行游戏函数 |
| 插件主线程回调 | 读 capability、更新私有状态、投递命令、登记消费者 | 申请/释放资源、持锁跨插件调用 |
| 后台线程 | 读取快照和原子标量 | 任何游戏函数调用 |

快照不得包含函数指针、迭代器或指向游戏内存的裸指针；发布使用自洽的版本/帧号。跨插件接口不得跨撤销事务缓存裸函数表指针。

状态通知优先使用 Host-owned revisioned observer events：订阅时取得当前状态，后续由 Host 在状态变化时推送；runtime effects、module health 与内部 config receipt observer 不依赖 native 定时轮询。公开 `GET /api/config/receipt/{id}` 和 `/api/system/events` 仍是可由外部调用方轮询的 HTTP 契约。Kotlin 的 MainActivity `onCreate`/focus 启动事件 coalesce 到一个 worker，不创建启动 timer retry；API listener config 变化后的 500ms reconcile 是可取消的单次 debounce，不是轮询。条件变量 `wait_for` 用于一次有界等待通知，不构成重复 polling loop。

## 7. HTTP、UI 与 Kotlin/native 边界

### 7.1 `core.api` 与 HTTP

`core.api` 只提供 HTTP 出口，不实现游戏语义。业务 API 的 owner 可以是官方 provider 或功能插件；owner 负责 typed snapshot/result/command 的内容，`core.api` 只负责共享出口的门控、注册生命周期、错误映射和通用状态协作。

HTTP 传输边界固定为：

- Kotlin `host/` 保留 AndServer/socket 监听、HTTP 解析、请求生命周期、响应写回和兼容 JSON 处理；
- native 不建立通用 HTTP router，不接管 socket，不把任意 JSON 请求当作万能调用入口；
- native owner 提供按领域定义的 typed snapshot、typed result 和 typed command；Kotlin bridge 调用对应的既有或新增窄入口，再保持既有 JSON 响应兼容；
- 配置与 lifecycle 控制由 Host 常驻管理设施提供（`host/features/http_control.cpp`，装配期绑定为 resident route，先于 registry 接受态与 op 门控）：`GET /api/config/list` 投影当前已注册 plugin 对象，`POST /api/config/set` 提交 typed 单 key feature/config/desired 请求，`POST /api/op/features/lifecycle` 提交 desired activate/deactivate，`GET /api/config/receipt/{id}` 回取异步 receipt。三条 `/api/config/*` 路由在 `core.api/routes.inc` 中的 owner metadata 字面量为 `settings`；这是路由表 owner 标签，不改变 Host 按路径优先处理 resident route 的行为，也不依赖 `settings` 插件处于 active。配置 namespace 仍为 `core.settings`。这些端点不经任何普通插件（含设置页）的生命周期，写入最终都走 Host 单一 Store transaction，不形成第二套 writer。
- query 只读取快照、不可变配置和登记数据；command 只解析为 owner 命令并立即返回票据（`{"ok":true,"ticket":N}`），操作在主线程执行、结果写回票据；HTTP 线程不等待主线程。

既有 URL、HTTP 方法、`NativeBridge` external 方法/签名冻结。query 路由与同步响应写路由的成功响应保持冻结；使用票据契约的带游戏写入路由，其响应行为必须在 `docs/reference/api-reference.md` 逐条公布（含票据终态的结果回取方式、保留窗口与超限语义）。路由 owner 由领域职责决定。诊断端点也不能加入游戏语义聚合。**调试端点例外**：`/api/debug/*` 属开发调试用途，不构成对外调用方契约，其响应可随内部机制调整（如票据化），但须在 api-reference 中标注为调试端点；release 隔离与 smoke 可按既有口径对其单独处理。

### 7.2 `core.custom_ui`、`core.ui_state` 与 `core.settings`

底层游戏资源、贴图、绘制调用、原版控件和命中检测由 Game Adapter 的 UI 适配面负责。

- **`core.custom_ui`（共享工具）**：通过稳定契约提供通用组件、容器、面板注册与导航，以及**游戏原生控件**（如原生选择框：弹框、等待选择、取回结果）与**文本机制**（独占文本钩子 + 文本注册/添加接口）。普通插件的 `ui/` 组合这些组件实现自己的页面；功能页面不归 `core.custom_ui`。文本**内容**也不归它——各插件经注册接口贡献自己的文案。
- **`core.ui_state`（UI 领域 owner）**：拥有游戏界面状态的读写——当前画面、游戏自身面板、对话与选择框状态，以及对应操作（打开/关闭游戏面板、选择对话项、开始交互）。它不提供可复用组件，也不承载功能插件的页面。
- `QOL_PORT_UI_WRITE` v1.1 为 panel/main-menu 写入提供 `step`/`cancel_pending`；主线程遇到 `NotReady` 时将票据结果推迟到下一逻辑帧，最多 60 次 attempt 后显式失败。命令仍按原票据 JSON 契约返回结果；帧步进不会改变 Host SDK 版本。
- **`core.settings`（普通成品页插件，无配置所有权）**：模块设置页独立成插件（id=`settings`，page entry 默认 true），只渲染 Host feature catalog 并调用 Host 常驻管理 API；消费 `core.custom_ui` 的组件与文本注册接口。它不拥有 schema、Store 或配置 HTTP adapter：配置与 lifecycle 控制由 Host 常驻设施提供，不随该页插件生命周期停止。settings 停用时整个设置页/控件/回调撤销，重新 activate 后从注册元数据与当前持久状态重建，不复用旧控件或 token；settings 自停后不通过自身页面恢复。

组件只在确有多个真实消费者且契约稳定时提升为通用组件；不要预先构造覆盖全部业务的 UI 框架。

### 7.3 Kotlin 与 native 的职责表

| 能力 | Kotlin | native/插件 owner |
|---|---|---|
| HTTP | 监听、传输、解析、响应写回 | 不实现通用 router |
| JSON | 非配置 API 的传输职责；配置 HTTP 仅传 raw body/response bytes | 不读取、解析、缓存或写配置；配置 JSON validation、plugin 对象 projection 与 durable commit 由 native Host 常驻管理设施承担 |
| 游戏读取 | 调用窄 bridge、消费结果 | provider 发布 typed snapshot/Result |
| 游戏写入 | 发起请求、展示票据/结果 | owner command 在主线程执行 |
| 业务语义 | 不聚合游戏 live 状态 | provider/功能插件拥有 |
| 配置与日志 | 启动早期日志、必要镜像 | Loader native 自举配置和主日志 |

## 8. 契约、能力与状态

### 8.1 capability 与命令

provider 在描述符中静态声明 `provides`，消费者声明 `requires`/`optional_requires`；版本按 major 相等、minor 满足进行协商。能力撤销先拒绝新 lease，再通知可选消费者摘除、按必需依赖级联、等待在途归零，最后注销。

只读 capability 不能直接修改游戏；副作用统一进入 owner command。命令使用固定宽度 POD payload、票据和明确错误；空指针表示不可用，不能用默认值伪装未就绪。

票据除执行状态外，在终态可回取 owner 命令产生的原操作结果 JSON。结果保留是**明确尽力语义**：单结果上限与总字节预算有界、超出按最旧优先淘汰（保留状态、淘汰结果），超限以显式字段标记省略；票据状态区分 `unknown`（不存在）、`expired`（已淘汰）与 `failed`（命令执行失败）。具体窗口、上限与回取端点（`GET /api/system/tickets/{id}`）在 `docs/reference/api-reference.md` 公布。

结果处理器当帧无法确定结果时（如转场类写需等游戏世界态提交）可请求**有界延迟结算**：票据保持 `pending`，帧排空于后续帧重新执行该命令，直到自行结算或达到宿主上限；超限以显式失败结算，不得以默认值或提前 `done` 伪装成功。延迟窗口内的票据仍保持单飞与可取消语义。

### 8.2 Loader 自举

- 最后观测手段是独立日志：native 日志就绪前允许 Kotlin 启动早期写者，交接后由 native 单写者接管。
- 最后恢复手段是外部 `config.json` 加重启；该文件由 native Host 常驻管理设施早期读取并写入，Kotlin 不持有配置副本。Store 在 Host lifetime 内管理所有 plugin 对象；设置页插件未安装或停用不影响配置读取、持久化和 lifecycle 控制。
- 配置文件顶层为 `plugin_id → JSON object`，每个对象含 `enable`/`features`/`config` 三层。旧平铺字段不属于当前 schema；Host Store 仅按 owner-specific 规则迁移已支持的 API、OP 与 listener 配置遗留值，不把历史名称一概视为迁移输入。未知 root 与未注册 plugin 原样保留。`GET /api/config/list` 投影按 Host 当前实际注册 plugin 集合过滤，不会暴露或删除未注册数据。
- 注册元数据（`feature_id`、`default`、`category`（General/BasicOptimization/Gameplay/Tools/Hidden）、`type`(toggle/entry)、`name`、`description`、`action_label`、`display_priority`、`callback`）由插件 descriptor 静态提供、不持久化；`display_priority` 缺省为 0，合法范围为 -10..10，越界注册明确拒绝。分类 ID、名称和优先级由 Host 单一静态注册表声明，管理 SDK v1.26 通过尾部 `read_categories` 返回 Host 生命周期内不可变的 POD catalog；SDK minor 26 是带 priority 的 `QolFeatureCatalogItem` stride 的必需消费门禁。priority 只决定呈现顺序，不改变 feature 值或生命周期。禁止零 feature 插件；`entry` 同样有布尔状态并参与运行判定。每次成功持久化的 feature 值提交（包括提交与现值相同的布尔值）后，Host 以该插件更新后的全部已注册 feature 布尔值重算 `enable`：任一为 true 则 `enable=true`，全为 false 则 `enable=false`；entry 值同样计入。feature 与派生 `enable` 处于同一 Store 事务，持久化失败时两者、revision 与运行时 effects 均不提交。entry invocation 只提交调用，不修改 feature/enable。启动 seed 与显式 desired lifecycle 写入不做此派生归一化；普通插件运行判定仍为 `want_run = enable && any(当前注册 feature=true)`，实际状态另受依赖、版本和安装结果约束。依赖级联/激活失败不改 desired；常驻需求用 default true 的 hidden feature 表达。`core.api` 的 `api.enabled` 是 General、默认开启的 API 服务 feature；Kotlin listener adapter 根据 runtime effect 的 actual、desired 与有效配置接线。`module.platform` 只拥有 General `debug.logging`（名称为「debug日志」）；`module.security` 拥有 General、默认关闭的 `op.enabled`。OP 授权只作用于 endpoint 明确声明 OP 的操作，与 API listener 独立；关闭 API 不改 OP 状态，开启 API 也不推导或开启 OP。`custom_recipe.recipes`（唯一可见开关，default false）与 `craft_ui.custom_crafting`（hidden default true，`craft_result` 升为 requires）级联启停，级联不改 desired。
- Host player feature catalog 包含所有已注册 descriptor 的 feature 定义，唯一按身份排除的是注册 ID 为 `settings` 的设置页插件；catalog 不按 `core.*` namespace 或其他 descriptor 可见性标记筛选。分类与 feature 均按 `display_priority` 升序呈现，同值保持注册顺序。设置页模型在 Debug 显示全部分类（包括 Hidden），Release 使用相同排序，仅不显示 Hidden 分类；General 分类中的 API 服务、OP 权限和 `debug.logging` 在 Release 仍显示。分类顺序元数据为 Hidden -10、General -9、BasicOptimization -8、Gameplay -5、Tools -3。除 General `core.api.api.enabled` 外，所有 `core.*` feature 均为 Hidden 且没有玩家描述；`core.custom_ui.open_debug_components` 仅在 `QOL_DEBUG_COMPONENTS` Debug 构建中注册为带 callback 和 `action_label="打开"`、priority -5 的 Hidden entry，Release descriptor 不注册该 entry。`module.security.op.enabled` 注册为 General、priority -4，因此 Debug 与 Release 设置页均显示；Hidden 分类只在 Debug 显示。`module.platform.debug.logging` 的 priority 为 -3；指定 feature 的 `core.api.api.enabled` 为 -5、`extension_bag.extended_bag` 和 `save_backup.open_manager` 为 -2、`autosell.auto_sell` 与 `custom_recipe.recipes` 为 -1。非隐藏的常规开关集合保持 11 个：七个既有非 core 功能开关、`module.platform.debug.logging` 和三个新增基础优化开关；`move_merge.drag_merge` 是既有开关，只调整为 BasicOptimization 分类。`save_backup.open_manager` 是原有 Tools entry。具体当前分类、默认值、公开集合与历史标识映射见 `docs/reference/plugins/settings.md` §7。
- `item_count` 的数量解释归 Host 常驻 Adapter；`stack_limit` 只拥有 999 数量策略（`stack_limit.limit_999`，default false）及对应资源 lifecycle，不在本文件展开其扩展读取接口。
- durable commit 后，Host 按 revision 串行更新 runtime projection/effects；需要游戏资源 gate 的效果仍经主线程命令，HTTP listener/watermark 等由 revision 通知处理。`core.api` 的 `api.enabled=false` 不阻断游戏内 settings UI 的配置写入与恢复路径。
- Store 的 hard-I/O degraded 采用 Host Store 定义的默认读写语义并通过状态/错误字段诊断；插件不得回退到 infra、Kotlin 镜像或旁路写文件。`ReadyUnpersisted` 表示当前进程可用但 durable persistence 尚未确认，必须与 hard failure 区分，不能报告为已 durable commit。
- 日志、配置自举、Loader、资源仲裁和 Game Adapter 属宿主设施，不参与普通插件生命周期，也不以 `core.*` 名义暴露。

## 9. 内置静态轨与未来外部轨

当前只要求内置静态插件：编译进模块主库，但每个插件仍按独立目录、独立 descriptor、独立契约和可独立测试边界实现。编译形态不改变依赖规则。

未来可以另行设计数据扩展或 native 外部插件轨，但现在只保留方向：外部插件必须消费版本化公共契约，并接受同进程代码无法隔离的风险。容器格式、签名/信任、动态加载、权限模型、外部 C ABI、更新和回滚都不在本文冻结，也不能作为当前实现前提。不得为了未来外部轨提前建设万能 Host API、通用 native HTTP router 或全域 provider。

## 10. 版本适配与可验证性

### 10.1 Game Adapter 适配规则

- 只有 `game/` 保存 `game_symbols.h`、内置 VMA、偏移和 ABI 签名；其他目录禁止 include 符号头和写裸地址。插件自声明的纯 ELF 导出名仅是 `EntryHook` 的目标身份，不是裸地址或 ABI 声明；Game Adapter 负责将其严格解析为当前 `libgame.so` 中已定义的函数导出，并验证可执行范围与 hook 对齐要求，不得回退到 VMA。既有内部目标 ID 保持原样。
- 变体识别、符号解析、原字校验和目标能力在资源安装前完成；所有 required 且 fail-closed 的入口 hook 必须在插件 `install()`/hook 写入前完成目标预检，目标不匹配的插件进入 `unsupported`，不得半安装。Loader 负责预检校验、资源仲裁及失败回滚，且在机制运行时重新预检；机制运行失败进入 `failed`。预检仅证明目标存在且满足低层地址约束，不证明 ABI 或行为兼容。
- Game Adapter 只返回低层受控结果；业务层把它转换为自己的 provider snapshot/result/command，不把游戏结构指针传播到 HTTP 或跨线程。

该符号声明仅定义内置插件资源目标边界，不冻结第三方外部轨；外部轨的容器、信任、ABI 与加载契约仍按 §9 另行设计。

### 10.2 测试插件与 fixture

测试插件是必须的验证载体，但不是产品插件，不进入 release，也不能成为生产依赖。测试代码必须与产品注册表和 release 构建有明确隔离。

测试分为两级：

1. **Host-only fixture**：最小 provider、consumer 和 failure fixture，可创建多个实例，用于验证 capability DAG、生命周期、能力发布/撤销、资源回滚、线程纪律和命令执行；它们只依赖 fake Game Adapter/后端，不承载业务规则。
2. **Debug/测试构建 device probe plugin**：仅用于设备探针，验证真实 LSPosed hook/patch、帧调度、主线程纪律、deactivate/物理还原和零插件组合；它不提供用户功能，不用于生产依赖图。

release 验收必须证明测试插件未被链接、未被注册、未被打入插件清单；零插件组合验收不得偷偷包含 device probe。禁止用测试插件承载业务、替代正式 provider/功能插件或成为生产路径的隐式依赖。

### 10.3 验收分层

1. **无插件启动**：Loader + Game Adapter 可启动；日志、配置、帧任务和机制待命可用；没有用户可见模块功能。
2. **内核机制验证**：Host-only fixture/fake 后端覆盖资源冲突、失败回滚、重入、能力撤销、线程纪律、快照一致性和命令票据。
3. **插件验证**：每个 provider/功能插件验证依赖缺失、启停、降级、资源归还和自身行为。
4. **机制真机验证**：Debug/测试构建的 device probe 覆盖真实 hook/patch、帧调度、主线程、deactivate/还原；该 probe 不替代正式插件验收。
5. **插件行为验证**：行为面必须使用受影响 VM 卡的真机日志；Host 测试、静态审查或 Debug 构建不能替代真机证据。
6. **release 隔离验证**：确认测试 fixture/device probe 未链接、未注册、未进入 release 清单。
7. **架构验证**：运行依赖方向、符号越权、资源登记、JNI 与生成材料校验；校验输出只说明检查结果，不改变本文规则。

## 11. 最终边界速查

| 问题 | 归属 |
|---|---|
| hook/patch 如何安装、冲突、回滚、还原 | Loader 的 `hooks/` / `patch/` |
| 日志、帧任务、资源仲裁 | Loader/Host SDK |
| 配置 schema/Store/持久化与管理 API（`enable`/`features`/`config` 三层） | Host 常驻模块管理设施 |
| 插件 lifecycle 期望（`enable`）与实际运行状态 | Host 常驻 lifecycle 控制设施（desired/actual 分离） |
| 游戏版本、符号、线程、低层受控读写调用 | Game Adapter/Game Host |
| 当前地图、角色/队伍、物品/背包、存档/任务 | 按领域拆分的官方 provider 插件 |
| 数量解释与读写（`item_count`） | Host 常驻 Adapter |
| 999 数量策略及其资源 lifecycle | `stack_limit` 插件 |
| 扩展背包、新物品、新合成、自动出售、传送 | 功能插件或其依赖插件 |
| HTTP 传输、解析、兼容 JSON | Kotlin host 与 `core.api` 出口协作 |
| typed snapshot/result/command | 对应 provider/功能插件 owner |
| 通用 UI 组件、面板框架与文本机制 | `core.custom_ui` |
| 游戏界面状态与操作 | `core.ui_state` |
| 模块设置页（成品页） | `core.settings` |
| 功能面板和业务呈现 | 对应功能插件 |

任何新职责都必须能在本表和前文找到唯一归属；若只能放入“万能 Loader”“万能 Game Host”或“万能 core provider”，说明边界仍未完成。
