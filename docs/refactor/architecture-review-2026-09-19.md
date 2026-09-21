# 架构审阅意见：七目录、出口插件与 S2 归属

> 审阅日期：2026-09-19
>
> 基线 HEAD：`6fc6098270e220f3f8c4aa4b32180b35885d4500`
>
> 总体状态：**NOT_ACCEPTED**
> 本文是独立的审阅记录，不是目标架构规范，不取代根目录 `ARCHITECTURE.md`，也不表示本文提出的目标已经实施。本文只记录当前仓库事实、最终裁决和签署前必须完成的迁移条件。

## 1. 执行摘要

七个顶层目录 `contracts / game / infra / hooks / patch / plugins / host` 的目标划分合理：
契约、游戏版本耦合、运行时基础设施、资源机制、指令机制、业务插件和宿主装配可以形成清晰的
能力边界。`ARCHITECTURE.md` §2.1（第 77–91 行）对该目标的职责描述是可执行方向。

但是，当前实现和文档尚未达到签署条件。主要原因不是七目录本身错误，而是目标规则、迁移期
兼容映射、源码实际依赖和校验脚本输出尚未收口到同一事实模型。当前存在真实的越层 include、
过宽的 Game Host API、混合职责的 S2 机制、单一构建 target，以及能够把既有违规视为通过的
基线棘轮。

本审阅的插件数量裁决如下：
| 项目 | 结论 |
|---|---|
| 当前插件数 | 17 个内置插件 |
| 当前 `core.*` 数量 | 1 个：`core.ui` |
| 最佳 `core.*` 数量 | 2 个：`core.api`、`core.ui` |
| `core.api` 落地后的目标插件数 | 18 个 |
| 是否应增加 `core.game` 等核心插件 | 否 |
| 是否应在本阶段改公开 plugin id | 否 |

`core.api` 是待落地的 HTTP 出口与路由插件，`core.ui` 是通用 UI 组件和面板容器出口。二者都是
普通的、可声明依赖和可执行生命周期的插件，不是高权限宿主内核。Game Host API、Host SDK、
日志、配置自举、资源登记和插件加载器仍属于不可停用的宿主内核设施。

因此，本审阅不建议把现有问题包装成“七目录已经完成”或“架构已经签署”。正确结论是：
目标方向可接受，当前实现与文档未达到签署；应按本文 P0–P5 条件继续迁移，并以真机验收卡
闭合行为面。

## 2. 审阅范围与证据口径

### 2.1 权威关系
- 目标结构和依赖原则以 `ARCHITECTURE.md` 为准；本文不修改其权威地位。
- `docs/refactor/layers.md`、`map.md`、`baseline.md` 是现状和迁移定位材料，不能覆盖源码事实。
- `docs/refactor/ledger.md` 记录阶段和债务；`verification.md` 定义 `NOT_ACCEPTED` 与真机证据门槛。
- `docs/refactor/adr/0006-ui-component-layering.md` 提供 UI 组件拆分依据。
- `docs/refactor/adr/0007-extension-bag-contract-and-mechanisms.md` 提供扩展背包与 S2 相关迁移背景。
- `docs/refactor/adr/0008-extractable-plugins.md` 提供插件可外置和 Game Host API 不插件化的目标约束。

### 2.2 当前事实的判定方式
本文把“当前事实”和“推荐目标”严格分开：

| 标记 | 含义 |
|---|---|
| 当前事实 | 可由仓库源码、脚本输出或已提交文档直接定位 |
| 迁移债 | 当前存在，但已有目标归属或清除阶段 |
| 目标设计问题 | 目标边界本身仍过宽、混合或缺少硬约束 |
| 推荐目标 | 本审阅提出的收口方式，不能写成已实施事实 |

当前 `uv run python scripts/maintenance/check_arch.py` 的输出为：层方向违规 32 条、符号/裸地址
违规 9 条、资源违规 3 条；三项均因基线棘轮显示“通过”。这只能说明没有新增基线条目，
不能说明当前代码符合七目录目标。

## 3. 五类分层原则
目标体系应明确区分以下五类，不用 `core` 前缀掩盖类别差异。
| 类别 | 职责 | 明确禁止 | 生命周期 |
|---|---|---|---|
| Host SDK | 向插件提供日志、配置、调度、命令、事件、能力注册表、存储、面板注册、资源声明等中立宿主能力 | 游戏业务语义、裸游戏内存、替插件拥有资源、绕过 owner 命令路径执行副作用 | 宿主常驻、不可停用；插件通过注入函数表使用 |
| Game Host API | 提供 target/变体事实、主线程纪律、受控低层 read/write/call、稳定 item_count 等宿主适配面 | 角色/物品/世界等高阶业务规则、功能开关策略、普通插件生命周期 | 宿主常驻、不可停用；由 `host` 实现并注入 |
| `core.*` 出口插件 | 只承载共享出口面：HTTP 注册/分派，或通用 UI 组件/面板壳 | 变成万能业务层、拥有全部游戏语义、取得高权限、替代 Host SDK/Game Host API | 普通插件生命周期；默认启用；停用只造成对应出口降级 |
| 领域 provider | 持有跨多个消费者复用的稳定业务语义、读模型、资源或行为所有权，发布 capability 或 owner 命令 | 直接被其他插件 include；把低层游戏适配和所有业务聚成一个 provider；把呈现决策塞进语义接口 | 普通插件生命周期；依赖者按 capability DAG 级联或降级 |
| 普通功能插件 | 持有一个可裁剪功能的规则、交互、资源声明和生命周期；消费 Host SDK、Game Host API 或 provider | 直接 include `game/infra/hooks/patch/host`；自己安装 hook/patch；在命令路径外写游戏 | 普通插件生命周期；可启用、停用、deactivate、回滚 |

五类不是五个顶层目录。Host SDK 和 Game Host API 的实现位于宿主内核，provider 与功能插件
位于 `plugins/`，`core.*` 也位于 `plugins/`。目录体现物理职责，类别体现运行时和契约职责。

## 4. `core` 前缀的严格定义
### 4.1 前缀规则
`core.<name>` 只能表示一个对多个普通插件提供“共享出口面”的普通插件，并且必须同时满足：

1. 能力面可以用稳定的 `contracts/` POD/函数表表达。
2. 有明确的消费者边界，不能只是把宿主内部实现换一个名字。
3. 可以走普通插件 descriptor、requires/provides、资源声明和生命周期状态机。
4. 停用后只影响该出口，不使宿主失去加载、调度、日志、配置自救或游戏适配能力。
5. 不拥有比普通插件更高的内存、线程、hook、patch 或 ABI 权限。
6. 其功能能够独立测试，并能给出“出口降级但业务主体仍可运行”的失效语义。

因此，“core”不是“关键所以高权限”的同义词，也不是“不可停用设施”的别名。后者应归
`host`、`infra`、`game`、`hooks` 或 `patch`，而不是创建新的 `core.*`。

### 4.2 `core.api`
**目标职责：** `core.api` 是 HTTP 出口与 `http.registry` 注册口唯一真源。它负责路由
登记和分派、JSON 信封、错误映射、`/api/op` 门控、查询/命令线程纪律、route lease 和
基础诊断端点（如 `/api/health`、`/api/system/routes`）。Kotlin AndServer 仍负责 socket、
HTTP 解析和响应写回；`core.api` 不实现监听器。

**提供能力：** `http.registry`。路由使用 POD 的 `HttpRoute`、`HttpReq`、`HttpRes`；查询
只能读取快照、不可变配置和登记数据，命令只解析语义并投递票据。现有 URL、HTTP 方法和
成功响应不因 owner 迁移改变。

**依赖：** `contracts`、Host SDK，以及宿主注入的 native↔Kotlin 传输桥。路由 handler 的
游戏语义、DTO、owner 规则由 API provider 或对应功能插件提供，不由 `core.api` 计算。

**失效影响：** 禁用时先拒绝新请求、冻结路由表、等待 route lease 归零，再撤销
`http.registry`；HTTP 出口降级，游戏内功能和宿主日志/配置/生命周期仍可运行。route 注册失败
只影响对应插件的 HTTP 出口，并将其标为 degraded。

**明确禁止：** 直接读游戏内存；实现 character/item/world 业务；持有领域 live provider；
改写既有 URL；让 controller 代替 owner 执行业务；实现 socket/HTTP 解析；因“诊断重要”而
绕过普通插件生命周期。

当前事实是 `core.api` 已完成首步落地：已提供 `http.registry`、143 条冻结路由 owner 表和
`/api/system/routes` 诊断出口；142 条业务路由仍由 Kotlin controllers 处理，完整 API provider
拆分、分派接线和 P5 其余工作仍待完成。本文未完成部分仍应按目标处理，不能宣称 P5 已整体完成。

### 4.3 `core.ui`
**目标职责：** `core.ui` 提供设置页容器、面板栈、分页和导航，以及通用组件
`Panel/Button/Label/Toggle/Select/ScrollView`。它通过 `contracts/ui.h` 的
`QolUiComponents` 发布组件能力，通过面板注册契约提供面板壳和导航。

**依赖：** `contracts`、Host SDK；底层游戏 UI 原语由 host 注入的 adapter 提供，adapter
背后才访问 `game/access/ui*`。`core.ui` 不应把 `game/access` 的资源细节重新暴露给普通插件。

**失效影响：** 设置页、通用组件和面板导航不可用或降级；普通插件的规则、资源和命令不应
因此获得或失去高权限，宿主和非 UI 功能仍应可启动。依赖 `ui_components` 或 `ui_panels`
的插件可以 blocked/degraded，不能把 UI 故障扩散成全模块失效。

**明确禁止：** 实现自动出售、存档备份、扩展背包等功能面板；实现游戏语义；直接依赖宿主
私有实现目录；把每个使用者的业务状态塞入通用组件；建立一个提前过大的 UI 框架。

当前 `core.ui` 已在 `module/app/src/main/cpp/plugins/core.ui/`，但其完整消费者迁移和
UI adapter/动态绑定收口仍是迁移项。ADR-0006 的“两到三个使用者后再提升”规则应保留。

### 4.4 `core.*` 的共同生命周期
`core.api` 和 `core.ui` 都是普通 `PluginDescriptor`，不使用特殊的“永远成功”状态：

- 默认启用，但默认启用不等于不可停用。
- `disable` 关闭能力发布、清空消费者和出口；`deactivate` 按普通资源事务等待、还原和注销。
- 停用只使 HTTP 或 UI 出口降级；宿主内核继续工作。
- 其 capability 必须静态声明，运行时发布/撤销由生命周期执行器处理。
- 不能通过 `core` 前缀取得 Game Host API 的额外成员或资源优先级。

## 5. 明确拒绝的 `core.*` 名称与正确归属
以下名称均不应创建。拒绝的是类别和职责归属，不是拒绝对应功能本身。
| 拒绝名称 | 原因 | 正确归属 |
|---|---|---|
| `core.game` | 会把不可停用宿主内核伪装成普通插件 | Game Host API：`host` 实现，`contracts/game_host.h` 定义；版本耦合由 `game/` adapter 承担 |
| `core.inventory` | 库存语义同时包含扩展袋、库存 hook、合并和写回，不应形成万能 provider | `extension_bag` 持有扩展袋；`inventory_hooks` 持有库存入口和刷新/分派；共同语义经明确 capability |
| `core.save` | 存档业务、sidecar 容器和游戏低层存档调用是不同所有权 | `save_backup` 持有备份业务；`infra/store` 持有容器存储；低层 save read/call 属 Game Host API |
| `core.diagnostics` | 诊断不是高权限业务插件，最后观测必须不依赖插件 | `host/status_json`、`infra` 状态/日志；HTTP 基础端点由 `core.api` 出口承载 |
| `core.compat` | 变体兼容不是独立业务出口 | `game/variants`、`game/access`、host adapter；具体功能差异由对应资源声明和 provider 处理 |
| `core.config` | 配置自举必须独立于插件系统 | `infra/config` 早期自读，Host SDK 提供命名空间访问；Kotlin 只作过渡镜像 |
| `core.log` | 日志是 R8 自举兜底，不能随插件停用 | `infra/log` native 单写者和 Kotlin 启动早期写者 |
| `core.registry` | 注册表是宿主机制，不应经过普通插件生命周期 | `infra/capabilities`、`hooks/registry`、`patch/claims`、host/plugin registry；`core.api` 仅拥有 HTTP registry |

上述拒绝也意味着不能用 `core.game` 或 `core.registry` 作为过渡垃圾桶。迁移债应登记在原路径、
目标归属和清除阶段中，不能用新 plugin id 把边界问题隐藏起来。

## 6. 当前 17 个插件的完整分类
当前源码目录和 `docs/refactor/map.md` 生成统计均指向以下 17 个插件。该表是当前分类审阅，
不是要求改名；本阶段不建议改公开 plugin id。

| 插件 | 分类 | 当前职责/归属判断 |
|---|---|---|
| `core.ui` | `core.*` 出口 | 通用 UI 组件、设置页容器、面板栈和面板注册 |
| `attribute_quality` | provider/resource owner | 发布 `attribute.quality`，持有属性范围、百分位和档位语义 |
| `native_choice` | provider/resource owner | 原生选择框能力及其 ptr slot/入口资源 |
| `custom_recipe` | provider/resource owner | 自定义配方、配方表所有权，发布 `craft_result` |
| `extension_bag` | provider/resource owner | 扩展背包整域、投影、持久化、背包资源和相关 owner 行为 |
| `inventory_hooks` | provider/resource owner | 库存入口 hook、库存刷新能力、库存侧分派和资源 owner |
| `text_layer` | provider/resource owner | `MEMORYTEXT_GetText` 共享资源 owner、文本层能力和作用域 |
| `attribute_range_display` | 普通功能插件 | 消费属性质量语义，负责颜色映射、文本改写和显示 |
| `autosell` | 普通功能插件 | 自动出售规则、扫描、配置、持久化和面板；消费 provider |
| `craft_ui` | 普通功能插件 | 合成器交互和资源声明；消费 `craft_result` |
| `push_all` | 普通功能插件 | 推怪行为与 call patch |
| `quest` | 普通功能插件 | 任务状态补正与进档调度 |
| `save_backup` | 普通功能插件 | 存档备份导入/导出、回滚和面板 |
| `simple_mode` | 普通功能插件 | 半血、伤害倍率和入口 hook |
| `special_equip` | 普通功能插件 | 特殊装备穿脱放行及出售门禁消费者 |
| `stack_limit` | 普通功能插件；可发布领域策略能力 | 99/999 上限策略、S2 相关策略资源；**不叫 `core`** |
| `world_teleport` | 普通功能插件 | 世界传送和原生选择框消费者 |

因此，当前是 1 个 `core.*`、6 个 provider/resource owner、10 个普通功能插件；新增
`core.api` 后为 2 个 `core.*`、6 个 provider/resource owner、10 个普通功能插件，总数 18。

`stack_limit` 可以在后续设计中发布稳定的领域策略 capability，但它仍是一个普通的可裁剪
插件：策略能力和 999 资源可以停用或降级，不能因为它涉及底层 patch 就取得 core 身份。

## 7. Game Host API 与 provider 的判断规则
### 7.1 判断顺序
每个新接口按以下顺序判断，不按“实现起来方便”决定归属：
1. 只描述 target、变体、符号可用性、线程判定、帧号、受控低层 read/write/call 或安全错误返回：归 Game Host API。
2. 描述稳定的角色、物品、库存、属性、配方、扩展袋等业务语义，且有多个消费者：归独立 provider。
3. 只有一个消费者的规则：留在该普通功能插件，不为复用假设提前抽 provider。
4. 有副作用的业务动作：由实际 owner 插件通过命令端口在主线程执行；provider 可以发布 owner 命令，但不让消费者直接拿宿主低层写指针。
5. hook、patch、ptr slot 的安装、回滚、冲突和物理归还：归宿主机制；业务 owner 只声明资源并提供 handler/消费者注册。
6. HTTP 路径、参数、DTO 和语义：归对应 API provider/功能 owner；封装、门控、lease 和路由分派归 `core.api`。

Game Host API 不应提供“物品业务对象”这种不可收敛的万能 DTO。它可以提供安全、稳定、低层
的 item_count facade，让插件不直接读取 S2 位段；canonical/effective 的业务策略仍由
`stack_limit` provider 和 owner 语义决定。

### 7.2 UI 的三段拆分
UI 必须区分三个面，避免把 `core.ui` 变成 `core.game`：
| 面 | 归属 | 内容 |
|---|---|---|
| Host Game UI 原语 | `game/access/ui*` + host adapter + Game Host API 受控面 | 游戏控件资源、贴图/resource id、底层绘制、命中检测、原版控件实例和 ABI 适配 |
| `core.ui` 组件/面板壳 | `plugins/core.ui` | `Panel/Button/Label/Toggle/Select/ScrollView`，设置页容器、面板栈、分页、导航和注册口 |
| 中立 registry | Host SDK / `infra` | 面板登记、owner、生命周期、占用、事件和调度；不包含游戏绘制语义 |

普通插件的 `ui/` 只组合标准组件并实现自己的面板状态和交互。`save_backup`、`autosell` 等
功能面板不能因复用绘制原语而归入 `core.ui`。`contracts/ui.h` 只描述跨边界所需的 POD、
函数表和 adapter 类型，不应成为游戏 UI 实现的容器。

## 8. S2 最新最终裁决
本节覆盖并取代旧的“宿主 core patch 统一持有 S2”建议。以下是本次审阅采用的最终边界。

### 8.1 数量语义
- 原生数量上限是 **99**。
- S2 只因 `stack_limit` 的 999 能力引入；没有 `stack_limit`，游戏仍使用原生 99 视图和策略。
- S2 位段布局、codec、游戏 item_count getter、S2 写路径 patch，以及 99/999 业务策略均归 `stack_limit`。
- 这不意味着插件直接操作裸位段；插件通过稳定的 Game Host API `item_count` facade 取得 canonical 或 effective 计数。
- 其他插件只看 canonical/effective 结果，不 include S2 codec，不复制位段解释，也不直接改游戏函数表。

### 8.2 Host 的最小职责
Host 只拥有：
1. 一张不可变的 Game Host API 函数表；
2. 一个用于安全 dispatch 的宿主 slot/入口；
3. 目标校验、线程纪律、错误返回和资源仲裁。

Host **绝不**在 `stack_limit` 启用或停用时直接修改函数表。`stack_limit` 注册其 provider，
通过声明式资源和安全 dispatch slot 发布有效策略；函数表的地址和成员在装配后保持不可变。
这避免了“普通插件可以重写宿主 ABI 表”的隐藏高权限。

### 8.3 provider residency 与 feature enabled 分离
`stack_limit` 的“代码/provider 常驻”和“999 功能启用”是两个维度：

| 维度 | 含义 |
|---|---|
| provider residency | `stack_limit` 的 codec/getter/provider 和 dispatch 实现是否已经驻留并可被宿主调用 |
| feature enabled | 当前是否开放 999 视图，以及 999 专属 clamp 等可撤销策略资源是否生效 |

如果已经产生持久化 S2 数据，provider residency 在没有数据迁移方案前不可卸载；解释已有 S2
数据所需的 getter、写路径与兼容资源也必须随 provider 保持驻留。之后可以关闭 999 功能，使
effective 视图回到 99，并撤销仅服务 999 的 clamp，但不能把仍负责解释已有 S2 数据的 provider
当作普通代码随意 unload。真正的卸载必须先有完整数据迁移、旧数据识别和真机回归证据。

这与普通插件的“可停用”不矛盾：可停用的是 feature enabled 和出口行为，不是已经承担持久
编码解释职责的驻留 provider。状态 API 必须分别报告两者。

### 8.4 相关所有权
- native bag marker 归 `extension_bag`，因为它描述扩展背包对象，不是普通数量编码。
- 库存合并、库存回写和跨原版/扩展背包的编排归 `inventory_hooks` / `extension_bag`，按具体 owner 分割；不能塞入 Host 或 `core.inventory`。
- `stack_limit` 负责 S2 数量语义和 99/999 策略；它不拥有扩展袋的 marker，也不拥有库存事务编排。

### 8.5 `contracts/stack_codec.h` 的收口
当前 `module/app/src/main/cpp/contracts/stack_codec.h` 同时包含：S2 位段编解码、99/999 策略、
native bag marker、effective 视图、写回门控和扩展背包相关判断（第 23–31、86–131 行）。
这是混合职责，不符合公共 contracts 只描述稳定跨边界 ABI 的目标。

目标是删除它作为公共实现头，并把 S2 codec 变为 `stack_limit` 的实现头。公共 `contracts/`
只保留 Game Host API 函数表、provider 函数表和必要的 POD。`item_count` facade 应是稳定
函数表成员；不得把 codec constexpr、marker 位段和业务开关泄露到公共契约。

## 9. 当前阻断事实
### 9.1 文档与源码矛盾
`docs/refactor/layers.md` 第 136 行声称“17/17 插件对 game/infra/hooks/patch/host/及跨
插件 include 计数为 0”，但当前源码存在可直接定位的越层 include：

| 代表路径 | 当前事实 |
|---|---|
| `module/app/src/main/cpp/plugins/save_backup/feature.cpp:31-33` | include `core/native/game_feature.h`、`game_ops_common.h`、`qol_log.h` |
| `module/app/src/main/cpp/plugins/autosell/view.cpp:5,8` | include `game/read/item_class.h` 和根部 `game_access.h` |
| `module/app/src/main/cpp/plugins/autosell/scan.cpp:12-16` | include `core/native/inventory_trade.h`、日志和 save-enter/exit |
| `module/app/src/main/cpp/plugins/inventory_hooks/inventory_hook_stage4.cpp:3-4` | include `core/native/sell_price.h`、`game/symbols/game_symbols.h` |
| `module/app/src/main/cpp/plugins/save_backup/ui/panel.cpp:5-6` | include `core/native/game_ops_common.h`、`qol_log.h` |
| `module/app/src/main/cpp/plugins/core.ui/panels/settings.cpp:4-10` | include `core/native` 与根部 `game_*` 私有实现 |
| `module/app/src/main/cpp/plugins/extension_bag/game_ui_virtbag.cpp:3-22` | include `game_*`、`game_symbols.h` 与多个 `core/native` 端口 |

这不是对 `layers.md` 的措辞挑剔，而是插件可外置约束的实质失败。该文件的生成时点还写为
HEAD `7e77af0`（第 7 行），与本审阅基线 HEAD 不同，不能作为当前源码证明。

### 9.2 校验脚本把迁移债当作允许项
`scripts/maintenance/arch_common.py` 当前仍有以下过渡规则：

- `BASE_LAYERS` 第 35–37 行包含 `domains`；
- `allows()` 第 67–69 行允许 `plugins` 依赖 `game/infra/hooks/patch/domains`；
- `_ROOT_OVERRIDES`、`_CORE_OVERRIDES` 和 `feature/`/`core/native/` 映射把遗留路径映射到目标层。

因此 `check_layers` 实际报告 32 条层方向违规，却在基线中判定通过；`check_offsets` 报告 9 条
符号/裸地址违规，也在基线中判定通过；`check_resources` 报告 3 条资源违规，仍在基线中
判定通过。`check_arch.py` 全部通过的含义只是“没有新增违规”，不是“七目录合规”。

这是两类问题：过渡映射和基线是**迁移债**；让 `plugins` 永久允许越层、让校验成功掩盖硬边界是
**目标设计问题**，P5 必须把后者改为硬失败。

### 9.3 构建和 ABI 边界仍未独立
`module/app/src/main/cpp/CMakeLists.txt:2-54` 只有一个 `gamebridge` shared target，且同时
编译 bridge、legacy `core/native`、`api/native`、`game/`、插件和新目录。单一 target
无法证明插件可独立编译，也不能证明插件没有链接宿主私有实现。

这属于迁移债；但“插件可外置”的目标要求必须有独立 target 或等价的强链接矩阵，否则
17/17 include 统计即使归零，也不足以证明边界成立。

### 9.4 Game Host API 的 `extra` 过宽
`contracts/game_host.h:233-260` 的 `QolGameHostExtraApi` 一次暴露 character、item、
inventory、craft、world、save、quest、choice 的读写和调用面；`host/game_host.cpp:872-922`
把这些成员全部注入。它当前像一张按迁移方便拼出的万能业务表，而不是最小低层原语表。

这是**目标设计问题**，不是单纯的路径债：Game Host API 需要保持安全、稳定、低层，但现表已经
把库存写入、存档加解密、合成 UI、世界传送和选择框等高阶动作放在同一 `extra` 中。收口时应
按 facade、owner 命令和 provider capability 拆分，保留必要原语，删除跨域业务聚合。

### 9.5 `core_patches` 混合 S2 与 hub

`host/core_patches.cpp:26-34` 的 reason 将 S2 数量布局、位读重定向、上限 clamp 和 hub 登录
跳过写在同一个 `core` 说明中；第 39–70 行的实际 claims 则是 12 条 S2/库存相关 patch 与
`P_HUB_LOGIN_SKIP`。六条 999 clamp 已由 `plugins/stack_limit/plugin.cpp` 单独声明，reason 仍把
两组职责混写。第 209–215 行又对 hub 做 fail-soft。

S2 既然最终归 `stack_limit`，就不应继续由 host `core_patches` 持有；hub 跳过是独立的兼容/功能
行为，不能和 S2 共用 owner 语义。这是**目标设计问题叠加迁移债**：资源机制可复用，但 owner、
生命周期、失效影响和验收卡必须分开。

### 9.6 contracts 中存在 capability 绕过

`contracts/stack_limit.h:5-9` 暴露全局 `stack_limit_enabled()` 和
`stack_limit_set_effective(bool)`；`contracts/autosell.h:42-46` 暴露全局的
`autosell_get_runtime_config`、`autosell_apply_config`、`autosell_store_persist`。

这些声明不是纯函数表/POD，而是跨边界的全局实现函数。它们绕开 capability lookup、provider
residency 和 lease，也让 contracts 直接成为链接入口。`stack_limit` 应经 provider 函数表或
Game Host `item_count` facade 交互；autosell 配置应经 Host SDK/其明确 provider port 交互。

这是**目标设计问题**，当前全局函数是**迁移债**。收口后公共 contracts 不留这类全局业务函数。

## 10. 推荐目标拓扑与注册原则

### 10.1 Capability DAG

推荐的目标拓扑如下；箭头表示“依赖并通过函数表/命令/capability 消费”，不是 C++ include：

```text
contracts (POD / function tables)
        ↑
host kernel: game adapter + infra + hooks + patch + lifecycle executor
        ├── Host SDK / Game Host API
        │       ├── stable item_count facade
        │       ├── UI primitive adapter
        │       └── thread / target / safe dispatch
        │
        ├── provider/resource owners
        │       ├── stack_limit ──(S2 policy/item_count provider)
        │       ├── attribute_quality ──> attribute_range_display/autosell
        │       ├── native_choice ──> world_teleport
        │       ├── custom_recipe ──> craft_ui
        │       ├── extension_bag <──> inventory_hooks (explicit owner ports)
        │       └── text_layer ──> text consumers
        │
        ├── core.ui ──> UI panel consumers
        ├── core.api ──> API route consumers/providers
        └── ordinary feature plugins
```

`contracts` 是零状态公共类型层；host kernel 是实现和装配根；插件之间只经 capability、
命令端口、事件和声明式资源协作。任何 provider 只发布自己拥有的事实或命令，不发布一个
“所有游戏对象都能读写”的总表。

### 10.2 注册原则

1. 每个插件 descriptor 显式列出 `requires`、`optional_requires`、`provides` 和 resources；
   capability id、版本、接口类型和 publish mode 必须静态可审计。
2. 宿主由 descriptor 构建 capability DAG，按拓扑处理 install、publish、revoke 和级联；
   不依赖 `host/assembly.cpp` 手写 provider 先于 consumer 的顺序。
3. `assembly.cpp` 只注册描述符和注入机制原语，不承载业务顺序和 provider 特例。
4. provider 发布 capability 后，消费者通过 lease/lookup 使用；撤销先拒绝新 lease、摘除
   optional 消费者、按 required 级联，再注销能力。
5. 不可变函数表在装配后不得被插件改写；运行时策略只经安全 dispatch slot 和 capability 状态变化表达。
6. 资源 owner、业务 owner、API route owner 和 capability provider 必须分别可查询；同一插件
   可以同时拥有多个角色，但不能用一个模糊 `core` owner 代替记录。

## 11. P0–P5 迁移优先级与验收标准

以下是推荐迁移顺序，不是当前已完成状态。每个阶段的行为面仍须补受影响 VM 卡；缺少真机证据
时只能为 `NOT_ACCEPTED`。

### P0：冻结事实和边界词汇

- 固定七目录、两种 `core.*`、17→18 插件数量口径和五类职责。
- 将 `layers.md` 的“17/17 include=0”改为与源码一致的现状事实，或在迁移完成后重新生成。
- 将 32/9/3 基线违规标成迁移债，不把 `check_arch.py` 通过当签署证据。
- 验收：文档能区分当前事实、迁移债和目标；基线 HEAD、脚本输出和源码代表路径可复核。

### P1：公共契约纯度和 S2 seam

- 删除公共 `contracts/stack_codec.h` 的混合实现职责；S2 codec、S2 getter/写路径和 99/999
  策略归 `stack_limit` 实现。
- 设计并冻结稳定的 Game Host `item_count` facade；公共 contracts 只保留函数表和 POD。
- 消除 `stack_limit.h`、`autosell.h` 这类全局实现函数，改为 capability/provider port。
- 明确 native bag marker 属 `extension_bag`，库存合并/回写属 `inventory_hooks`/`extension_bag`。
- 验收：contracts 无 STL、无全局业务函数、无 S2/业务实现；host 测试覆盖 canonical/effective、
  99/999、marker 隔离、写回门控和错误语义。

### P2：插件边界和独立构建

- 逐项清除插件对 `core/native`、`game/`、根部 legacy header、`game/infra/hooks/patch/host`
  的 include；Game Host API/Host SDK 是唯一宿主访问面。
- `check_layers` 对插件越层 include/link 改为硬失败；删除允许 `plugins → game/domains` 的过渡规则。
- 为插件建立独立 CMake target 或等价的可证明链接边界，不能继续只依赖单一 `gamebridge` target。
- 验收：插件 include 和链接违规为 0；独立 target 可编译、可运行 host tests；失败不会被既有基线吞掉。

### P3：provider 声明和 capability DAG

- 给 `stack_limit`、`attribute_quality`、`native_choice`、`custom_recipe`、`extension_bag`、
  `inventory_hooks`、`text_layer` 补齐清晰的 provides/requires；运行时消费必须与 descriptor 一致。
- 将 `assembly.cpp` 的手写 provider 顺序改为 DAG 执行；补足 missing provider、版本不兼容、
  动态发布/撤销、required cascade 和 optional degraded 测试。
- 拆 `QolGameHostExtraApi`，按低层原语、稳定 facade 和 owner 命令重新定义边界。
- 验收：没有靠注册顺序才能启动的必需消费者；capability registry 能解释每条边和 generation；
  host/fake 覆盖撤销与重入，真机覆盖启动和降级。

### P4：`core.api` / `core.ui` 出口收口

- 落地 `core.api`，把现有路由按冻结 URL 建立唯一 owner 表；Kotlin 只留 AndServer 传输适配。
- 保持 `core.ui` 为通用组件和面板壳；功能面板留在 `autosell`、`save_backup` 等普通插件。
- 两个 core 插件使用普通生命周期；默认启用，停用只造成 HTTP/UI 出口降级，不影响日志、配置、
  加载器、Game Host API 和其他业务插件。
- 验收：路由无重复/遗漏，query/command 线程纪律和 route lease 正确；`core.api` 与 `core.ui`
  disable/deactivate/activate 有真机往返，状态、资源和降级影响一致。

### P5：S2 residency、资源 owner 和签署门禁

- 把 S2 所有 claims 从 `host/core_patches` 迁至 `stack_limit`；其中表示兼容资源随 provider
  residency 驻留，999 专属 clamp 随 feature enabled 撤销；hub 兼容 patch 单独归属。
- 实现 provider residency 与 feature enabled 双态；持久 S2 产生后无迁移方案不得卸载 provider。
- 使 immutable function table 和 safe dispatch slot 成为唯一宿主写入口；`stack_limit` 只能注册
  provider，绝不直接改函数表。
- 清除 `domains`/legacy 映射、过渡全局函数和 remaining resource exceptions；更新脚本和生成文档。
- 验收：contracts 纯度、插件 include/link 硬失败、独立 target、provider 声明、core 停用降级、
  S2 双态/不可卸载语义全部有 host 测试和受影响真机卡；`check_arch.py` 的违规不是靠基线通过；
  所有 P5 卡有可复现命令和实际输出。

## 12. 结论

七目录目标合理，最佳 `core.*` 数量是 2 个：`core.api` 与 `core.ui`。当前 17 个插件的
分类可以沿用现有公开 id；`core.api` 落地后目标为 18 个插件。`stack_limit` 可以承载可发布
的领域策略能力，但不应改名为 `core.*`。

当前仓库存在源码与 `layers.md` 矛盾、校验器允许越层、单一 `gamebridge` target、过宽
`GameHostExtra`、`core_patches` 混合职责和 contracts 全局函数绕 capability 等阻断事实。
其中部分是迁移债，部分是目标设计问题；两者都不能被“基线通过”或已有 smoke 结果替代。

本审阅建议在 P0–P5 条件全部满足、对应行为面取得真机日志证据后再签署。本文没有修改
`ARCHITECTURE.md`，也不声称上述推荐拓扑、契约收口或 S2 归属已经实施。当前审阅结论保持：
**NOT_ACCEPTED**。
