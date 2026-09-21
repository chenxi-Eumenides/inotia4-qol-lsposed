# 当前实现相对最终目标的差距、优化与改进

> 基线：2026-09-21 当前源码与脚本实测。本文只做“现状 → `ARCHITECTURE.md` 既定目标”的差距判断、优化顺序和检查条件，不定义新的目标架构。

## 1. 定位与权威边界

| 内容 | 权威与用途 |
|---|---|
| 目标结构、职责、依赖、生命周期和边界 | 根目录 `ARCHITECTURE.md`。本文不改写、不补充新的目标层或目标插件。 |
| 当前实现事实 | 源码、脚本实测输出和可复现真机证据。脚本“通过”只说明没有新增基线问题，不等于已经达到目标结构。 |
| API 路径与响应契约 | `docs/reference/api-reference.md`；本文只评估其 owner/实现迁移状态。 |
| 本文 | 当前差距的唯一审阅入口，以及唯一改进顺序；不替代架构书、API 契约或验收矩阵。 |

若当前实现与 `ARCHITECTURE.md` 冲突，先标为阻断差距；不得在本文通过改写目标来消解，必须回写 `ARCHITECTURE.md` 后再更新本文。

## 2. 用户目标的简洁判定模型

用户目标是：加载器只负责把可验证的能力装配起来；游戏适配只负责版本耦合；稳定的官方游戏语义由官方核心依赖 provider 持有；用户功能由可裁剪的功能插件持有。

| 角色 | 应负责 | 不应负责 |
|---|---|---|
| **Loader** | 目标进程识别、插件发现/注册、依赖解析、资源事务、生命周期、Host SDK/Game Host API 注入和故障状态。 | IAP、协议页、沉浸模式、水印等具体功能；角色、库存、存档等游戏语义。 |
| **Game Adapter** | 变体、符号、ABI、目标校验、主线程纪律，以及受控的低层 read/write/call。 | 角色/物品/世界/存档/任务等稳定业务语义，或某个用户功能的策略。 |
| **官方核心依赖 provider** | 一个稳定且被多个消费者复用的游戏语义，发布只读 capability；副作用通过 owner 命令入口完成。 | 聚合全部业务、持有 UI 呈现决策、成为万能 `core` 或被其他插件源码直接 include。 |
| **功能插件** | 一个可裁剪功能的规则、交互/UI、配置、资源声明、生命周期和 API owner；经 SDK、provider 或 Game Adapter 取事实。 | 直接安装 hook/patch、直接依赖 `game/`/`infra/` 私有实现、在 owner 命令之外写游戏。 |

判定规则很简单：看到“平台功能被 Loader 无条件执行”，是 Loader 越权；看到“高层业务塞进 Game Host”，是适配面越权；看到“多个功能重复读写角色/库存/世界”，是 provider 缺失；看到“HTTP/Kotlin 自己决定游戏业务”，是功能 owner 未迁移。

## 3. 当前结论

| 统计口径 | 当前审慎区间 | 判定 |
|---|---:|---|
| 只计进程内重构（Loader、适配、provider、功能插件、HTTP owner 和边界收口） | **约 50%–55%** | infra、hooks、patch、生命周期/资源事务和大部分功能插件已具备，但 Loader、provider、GameHost 收窄、HTTP owner 迁移及 legacy 清理仍是主阻断。 |
| 把外部生态（数据扩展、外部 native 插件、分发/恢复/托管保证）一并计入 | **约 35%–40%** | 这是更保守的整体区间；外部轨尚未进入实施，不应把内置插件静态编译完成等同于生态完成。 |

上述百分比是职责迁移的审慎区间，不是源码行数、插件数量或发布接受度。行为面仍以对应 VM-B/真机卡为准；缺真机证据时不能把静态检查或 Debug 构建记为通过。

## 4. 已完成与已形成的基础

- `infra/` 已有调度、命令、事件、快照、配置、存储、状态和 capability 基础。
- `hooks/` 已形成资源登记、冲突仲裁、分派和事务回滚；`patch/` 已形成声明、原字校验、安装和还原。
- 插件生命周期已覆盖启用、停用、deactivate、失败、回滚、在途和资源状态；capability 已有注册/撤销与级联的雏形。
- 大部分用户功能已进入 `plugins/<id>/`，包括扩展背包、库存 hook、存档、自动出售、合成、传送、推怪、任务、UI 及其他功能；这证明插件化纵向路径可用，但不证明所有依赖边界已清零。
- 当前插件均有独立的 `plugins/<id>/CMakeLists.txt` 片段/target，父级自动发现并链接；这解决了构建归属的第一步，尚未单独证明每个 target 不依赖宿主私有实现。

## 5. 阻断差距与路径证据

| 阻断差距 | 当前事实 | 路径证据 |
|---|---|---|
| Loader 仍持有功能行为 | `HookMain` 无条件安装 IAP、agreement、activity indicator、immersive、watermark 五类平台行为。 | `module/app/src/main/java/com/inotia4/qol/HookMain.kt:56-109`；加载流程由 `onPackageLoaded` 直接执行。 |
| bootstrap 仍初始化业务 | `ApiServer.bootstrap` 不只是 HTTP 传输启动；它初始化静态数据、配置、save store、save-backup native/UI、扩展背包 UI，并下发功能开关。 | `module/app/src/main/java/com/inotia4/qol/ApiServer.kt:41-90`；`HookMain.kt:136-138` 无条件调用。 |
| 宿主兼容补丁仍混合具体业务 | `host/core_patches` 同时处理 Hive 登录跳过和 monster 物品数量兼容扫描/patch。 | `module/app/src/main/cpp/host/core_patches.cpp:16-17,22-24,42-108,113-137`。 |
| Game Host API 过宽 | `QolGameHost` 目前聚合 character、item/inventory、world、craft、save、quest、choice 的高层读面/写面，并额外挂有扩展背包写面。 | `module/app/src/main/cpp/contracts/game_host.h:218-307,309-369,371-427,434-449`；实现集中在 `module/app/src/main/cpp/host/game_host.cpp:7-21` 及其后续适配函数。 |
| 官方核心 provider 不足 | 当前有功能插件，但没有按复用语义形成 character、world、inventory 等官方 provider；功能只能继续从宽大的 Game Host/legacy 面取事实。 | 插件装配清单 `module/app/src/main/cpp/host/assembly.cpp:57-76`；高层读面仍集中在 `contracts/game_host.h:418-427`。现有 `world_teleport`、`inventory_hooks` 等是功能/机制 owner，不等于通用 provider。 |
| extension_bag 写面仍在 Game Host | 库存、装备/经济、UI 刷新等写函数被集中暴露，而不是完全由 owner 命令和窄 capability 组织。 | `module/app/src/main/cpp/contracts/game_host.h:309-369,443-448`。 |
| Kotlin/API 业务仍集中 | Controller、Service、NativeBridge 调用链仍承载大量参数解析、状态拼装、业务路由和复合查询；Kotlin 还不是纯传输适配。 | `module/app/src/main/java/com/inotia4/qol/api/controller/*.kt`；`module/app/src/main/java/com/inotia4/qol/service/**/*.kt`；三源清单见 `module/app/src/main/cpp/plugins/core.api/routes.inc`、上述 Controller 集合及 `docs/reference/api-reference.md`。 |
| 内核装配面缺独立验收载体 | 当前插件装配面的验证只能借助真实业务插件，Loader 失败与业务失败混在一起；没有不进 release 的 probe 插件。 | `module/app/src/main/cpp/tests/` 现为 host fixture；`plugins/` 下无只进 Debug 的 probe target；插件清单见 `module/app/src/main/cpp/host/assembly.cpp:57-76`。 |
| legacy 边界仍有效 | `api/native`、`core/native`、根目录游戏聚合文件仍是迁移边界；脚本通过过渡映射保留现有债务，不能据此宣称七目录已收口。 | `module/app/src/main/cpp/api/native/**`、`core/native/**`、`game_system.{h,cpp}`、`game_dialog*`；`check_layers` 当前报告仍有 30 条基线违规。 |

## 6. 当前脚本实测基线

实测命令：`uv run python scripts/maintenance/check_layers.py`、`check_offsets.py`、`check_resources.py`、`check_jni.py`、`check_arch.py`。

| 检查 | 实测结果 | 正确解释 |
|---|---|---|
| 文件统计 | **393 个文件** | 当前检查口径的源码文件总数。 |
| `check_layers` | **30 条现存基线违规**，新增 0；脚本通过 | 仅表示没有新增层违规；30 条既有债务仍然存在。 |
| `check_offsets` | **4 条现存基线违规**，新增 0；脚本通过 | 仅表示没有新增裸地址/符号头越权。 |
| `check_resources` | **0 条新增/现存违规**；脚本通过 | 当前资源安装点与重复安装检查没有新增问题。 |
| `check_jni` | **0 条违规**，Kotlin external/native 导出为 144/144；脚本通过 | JNI 三向数量一致，不代表 JNI 业务归属已迁完。 |
| `check_arch` | 通过 | 汇总通过只表示相对基线无新增；不等于 legacy、provider、Loader 或 HTTP owner 已完成。 |

HTTP 目前是 **143 条路由**：native `routes.inc`、Kotlin Controller 清单、`api-reference.md` 三表的路径/方法清单一致；这只是清单一致，不代表 owner、成功响应、线程纪律、读写副作用和实现归属一致。三表一致不能替代 HTTP 语义迁移验收。

## 7. 防止两端万能化的目标数据流

目标数据流只采用已有架构边界，不新增一层：

```text
Loader
  └─ 注册/依赖解析/资源事务/生命周期 ──> Game Adapter + provider + 功能插件

HTTP client
  └─> Kotlin/AndServer 传输适配
       └─> core.api 注册/门控/route lease
            └─> owner 插件
                 ├─ query  ─> owner snapshot / provider capability ─> Game Adapter 只读适配
                 └─ command ─> owner 主线程命令 ─> Game Adapter 写入/调用

游戏 hook/帧/存档事件
  └─> 对应 owner provider 或功能插件
       └─ capability / snapshot / event ─> 其他消费者与 core.api
```

- Loader 不向下调用 IAP、save、背包或任意业务；它只装配和报告状态。
- Game Adapter 不向上聚合角色、库存、世界等业务 DTO；它只提供受控低层原语和版本事实。
- provider 不变成第二个 Loader 或万能 `core`：每个 provider 只拥有一个可复用语义，读面只读，写面走 owner 命令。
- HTTP/Kotlin 不变成第二个业务宿主：查询读 snapshot/capability，变更只发命令并返回票据；业务规则和副作用由 owner 插件执行。

## 8. 唯一改进顺序与阶段完成条件

改进顺序的原则：**先把“无插件的模块内核”定义清楚并验收，同时允许官方核心 provider 与功能插件并行推进**；Loader 机制按真实需求补齐，不先凭空冻结完整 SDK；外部插件加载/分发轨最后处理。

这里的内核（kernel）指 Loader、Game Adapter、`infra/hooks/patch`、Host SDK/Game Host 注入与状态上报，它必须能在**零业务插件**下启动、装配、上报与自恢复。内核的目标规则引用 `ARCHITECTURE.md`（尤其是宿主内核设施与「一切业务功能皆插件」的边界），本文不新增内核目标，也不把某个业务插件当作内核必需项。

| 阶段 | 唯一顺序 | 必须完成的工作 | 可检查的完成条件 |
|---|---|---|---|
| 1 | **定义并验收 kernel-complete（零插件内核）** | 明确零业务插件时的启动路径、装配面、状态上报和兜底；只冻结内核启动/装配/上报所需的最小接缝，不要求冻结完整 SDK。 | 零插件 Debug/Release 均可启动，`/api/health` ready，无业务插件注册，无 FATAL；内核启动/装配/失败路径有 host 证据与真机日志；没有把业务语义塞回 Loader/Game Adapter。 |
| 2 | **provider 与功能插件并行**（作为接口需求与回归载体） | provider 与功能插件同时推进；provider 以“真实消费者需要什么”为需求来源，功能插件作为接口需求方与回归载体，不等待完整 SDK 冻结。 | 每个 provider 都有真实消费者与回归用例；插件只经 SDK/provider/Game Adapter 取事实；并行期出现的新依赖能回流为内核/接口需求；受影响 VM-B 补齐。 |
| 3 | **Loader 机制按真实需求补齐** | 随 provider/插件暴露的真实缺口补 Loader 机制（发现、依赖解析、资源声明、状态、故障恢复等），按需补齐，不做提前设计。 | 每个新增机制都有真实使用方与验收；无使用方的机制不进入内核；机制收紧后，原先只能靠业务插件验证的装配面可由 probe 插件复现（见 §9）。 |
| 4 | **GameHost 收窄** | 把 `QolGameHost` 的高层领域聚合和 `extension_bag_write` 迁到 provider/owner 面；保留目标、变体、线程纪律、低层适配及既定稳定 facade。 | `contracts/game_host.h` 不再作为 character/world/inventory/save/quest/craft 总表；扩展背包写入只经 owner 命令；插件不需要 include `game/`；ABI/错误/线程规则有 host 负例与受影响 VM-B 证据。 |
| 5 | **HTTP A+ owner 迁移** | 将 143 条路由逐条绑定真实 owner；core.api 只做注册、传输、门控、lease 和基础诊断；Kotlin Controller/Service 收窄为参数与传输适配，业务查询/命令迁入 owner 插件。 | 三表仍保持 143 条路径、方法和冻结成功响应；每条路由 owner 无重复/遗漏；query 只读 snapshot/capability，command 只发 owner 命令/票据；API smoke 与受影响 VM-B 覆盖行为和失败语义。 |
| 6 | **边界硬化** | 清除 `api/native`、`core/native`、根目录聚合遗产和脚本过渡映射；收紧插件私有 include/link 规则；把基线债务逐条变成可失败的边界检查。 | 目标七目录外无未登记实现边界；基线违规归零或有明确、可过期的豁免；边界负例确实非零退出；`check_layers/offsets/resources/jni/arch` 与相关 host tests 通过，行为面仍补齐真机证据。 |
| 7 | **外部轨**（外部插件加载/分发最后） | 仅在前六阶段完成后，按既定外部轨处理数据扩展、外部 native 加载、用户显式启用、启动故障恢复、托管资源回收和更新回滚。 | schema/路径穿越/版本校验拒绝用例通过；native 插件的加载、禁用、托管资源回收和重启恢复有真机日志；状态明确区分宿主管理保证与进程内全权代码风险。 |

并行约束：阶段 1 先行且不得跳过；阶段 2 中官方核心 provider 与功能插件可并行开发，也可与阶段 1 部分并行，但阶段 1 完成前不得把它们当作内核必需项，也不得用业务失败掩盖内核失败。阶段 3 是“按需补机制”的持续动作，不预先冻结一套完整 SDK。

每阶段的“完成”同时要求：静态检查结果、相关 host 测试、对应 VM-B/受影响 VM 卡真机日志三者齐全。缺少行为证据时只能记为 `NOT_ACCEPTED`，不能用 `check_arch`、Debug 构建或 HTTP 200 替代。

## 9. 测试策略：host-only fixture 与 Debug/device probe 插件

当前建议采用两级验证。两级都**不进 release**，也**不属于产品插件**，避免把测试载体的行为与状态混入产品面。

| 层级 | 形态 | 进 release | 属产品插件 | 验证范围 |
|---|---|---|---|---|
| L1 host-only provider/consumer/failure fixture | `module/app/src/main/cpp/tests/` 内的 fake provider/consumer 与故障注入；无游戏内存、无 Android 依赖。 | 否 | 否 | 调用序、依赖解析、capability 注册/撤销/级联、资源事务与逆序回滚、缺依赖 blocked/degraded、重复 enable/disable/deactivate、ABI/版本不匹配、错误返回与 fail-closed。 |
| L2 Debug/device probe 插件 | 一个只进 Debug 构建、在真实设备上真实装配的 probe 插件，用于验证 Loader 装配面。 | 否 | 否 | 插件发现/注册/装配、Host SDK/Game Host 注入、provide/require 真实接线、状态上报、失败路径与恢复、资源进出、线程纪律。 |

为什么需要测试插件（probe）：

- 只有 L1 host fixture，只能证明机制单元测试通过，**不能证明插件装配面可用**（真实注册、依赖边、SDK 注入、状态上报）。
- 不能用现有业务插件作为唯一的 Loader 验收：业务失败会与 Loader 失败混在一起，无法定位是内核问题还是业务问题。
- probe 只暴露内核面（provide/require、失败注入、状态），不承载任何用户功能，从而让内核失败与业务失败可分离。

零插件 release/Debug 组合的验收条件：

| 组合 | 期望 |
|---|---|
| 零插件 Release | 可正常启动与退出；`/api/health` ready；无业务插件注册；无 FATAL；产物不包含任何 probe/fixture。 |
| 零插件 Debug（无 probe） | 与零插件 Release 行为一致，可观察内核启动/装配日志；无 FATAL。 |
| Debug + probe 插件 | Loader 装配面被真实验证：probe 的 provide/require、失败注入、状态与资源进出均可见；probe 不进 Release 产物；probe 失败可被独立定位为内核问题。 |

保障：probe 插件由独立 CMake target 控制，只在 Debug 打包；release 构建的插件清单不得包含 probe/fixture。“零插件内核”是内核验收基线，后续业务插件回填不得改变该基线的通过口径。

## 10. 当前结论的更新规则

本文只随当前源码、脚本输出和可复现验收证据更新。任何新目标、职责归属、ABI 约束或架构冲突必须先回写 `ARCHITECTURE.md`；本文随后只记录该目标下的差距、阶段位置和证据，不保留旧结论或历史时间线。
