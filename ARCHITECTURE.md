# 架构设计书：分层收口与插件化

> 状态：**v0.3.13（P5 开工基线候选，待外部复审后签署）**｜基线日期：2026-09-18
> 签署定义：签署 = 外部复审无阻断项 且 版本记录与未决项表逐项有结论。P0–P3 的「完成」仅指各阶段出口条件达成，**不表示**本设计书已签署或未决项已闭合（见 §12.3）。
> 适用范围：**§1–§9、§11、§12 对内置插件轨（含全部核心架构）为权威规范**；**§3.7/§3.8 外部插件轨为未冻结设计草案**（P6 前允许大改，不构成实施承诺）。
> 权威层级：架构规则以本文为准；校验脚本输出只代表「当前实现事实」。二者不一致时构建失败，必须显式走「改设计」或「登记临时豁免（带文档锚点）」之一。
> 本文只描述目标结构；结构类内容由脚本生成或校验，人工只维护「为什么」。

**版本记录**

| 版本 | 变更 |
|---|---|
| v0.1 | 初稿：分层、资源仲裁、版本收口、线程约束、插件化 |
| v0.2 | 闭合第一轮审阅：依赖矩阵、生命周期、能力契约、资源所有权、分发模型、脚本边界、纵向迁移 |
| v0.3.13 | UI 组件分层审阅记录（ADR-0006）：`game/access/ui*`（底层资源与绘制）→ `contracts/ui.h`（标准组件接口）→ `plugins/core.ui`（通用组件）→ 普通插件组合；方框两拆与首期组件清单（Panel/Button/Label/Toggle/Select/ScrollView）与 2–3 使用者提升规则；记录，实施待排期 |
| v0.3.12 | B/D 决策落地与复盘校准：HTTP 传输保持 Kotlin（`core.api` 持有路由表/注册口/信封/门控/线程，Kotlin 承担 AndServer 传输，跨栈边界见 §7.3）；外部 `config.json` 改由 native `infra/config` 启动早期自读（R8 重写）；`check_*` 能力边界按「已落地/规划」重述；过渡映射清除条件、`core.ui` adapter 自我约束、`provides[]` 字段标注 P5 目标、动态能力章节号对齐（可用性事件 §3.4.5、动态能力 §3.4.6） |
| v0.3.11 | P5 前出口面与动态能力校准：`core.api` 收缩为 HTTP 传输/注册内核，只保留基础端点；既有 API 路径与插件 id 解耦并保持冻结，内置路由按 owner 表归属、外部路由限定 `/api/ext/<id>/...`；capability 采用“静态声明 + 低频动态发布/撤销 + 接口内高频就绪状态”模型，补撤销 lease、失败与 `degraded` 清除语义（可用性事件 §3.4.5；动态能力 §3.4.6） |
| v0.3.10 | P5 前架构拍平（ADR-0004）：删除 `products/` 层，`features/` 更名 `plugins/`；出口面成为内置插件 `core.api`/`core.ui`（`http.registry` 注册口：封装/信封/门控/线程归 api，语义解析归各插件）；新增能力注册/注销事件（§3.4.5）支撑 optional 依赖延迟注册；新增 R8 自举兜底红线（log + 外部 config）；分层收敛为五层八目录；P5 执行 |
| v0.3.9 | P6 外部插件方向校准：外部形态收敛为数据扩展与**未签名、全权 native 插件**；删除证书白名单、`read-only/trusted-write` 权限分级及同进程 `.so` 可受限的假设；Host API 改为托管协作面 + raw 便利面，补 TargetRegistry、跨插件能力、托管保证边界、用户显式启用与启动故障恢复 |
| v0.3.8 | P4-A4 实施校准：分派权限以注册期 `ConsumerKind` 表达（类型化句柄为别名）；**新增阶段参数**（before/after）与「每个回调每次分派只调用一次」规则；Intercept 短路后 after 仅 Observe 生效 |
| v0.3.7 | P4-A1 实施校准：`patch/` 以**注入原语**实现（`core/native/call_patch` 的物理迁移推迟 P5，避免过渡期 include 越层）；非入口机制经 host 注入分派（`TxDeps.install_other/release_other`），保持 `hooks → patch` 禁止；`kCallPatch` 禁用即物理还原 + 单指令门禁翻转同步执行写入 §4.2 规则 6 |
| v0.3.6 | 闭合第四轮审阅：`on_resource_ready` 失败改为**全量逆序回滚**（与 §3.2 语义 5 及实现一致；回滚失败 → tombstone+隔离）；删除写入口的「生命周期回调」例外（`game/write\|ops` 仅主线程命令处理器）；内置插件变体约束改由**必需资源的符号/能力位条件**表达（删除 §8 的 `supportedVariants` 行）；§3.2 补 `resolving → failed` 迁移边、`invalid` 移除「契约 major 不兼容」 |
| v0.3.5 | 闭合第三轮审阅：`degraded` 定为正交健康标记、`cascade_stopped` 降为原因码（生命周期收敛为 10 态）；共享资源同样由 owner 提供 `handler`；`on_resource_ready` 定为受限生命周期回调并带失败回滚语义；物理还原失败统一 `failed + isolated`；删除 HTTP 同步等待例外；ticket 查询随首个返回票据的端点落地；feature 对 `game/write\|ops` 的调用限定主线程命令路径；补 capability 注册时序；明确 feature UI 与 products/qol 边界；首页版本/签署状态与未决项归属校准 |
| v0.3.4 | P3 实施校准：状态 API 补 `GET /api/system/resources` 与 `POST /api/op/features/lifecycle`（生命周期控制经命令端口，`op` 门控）；票据查询端点明确推迟到 P4 |
| v0.3.3 | P2 实施校准：`PluginDescriptor` 增加 `on_resource_ready`——宿主声明式安装后须把 backup（原函数入口）交还所有者，否则 wrapper 无法调用原函数 |
| v0.3.2 | P2 实施校准：资源声明补充 `handler`（所有者提供的替换入口）——否则「宿主声明式安装」在入口 hook 机制下无可执行体 |
| v0.3.1 | 实施校准（P1）：诊断接口按实测收敛——`features` 为 `{"ok":true,"features":[...]}` 信封并含 `version/resource_count`；`/api/health` 以加法扩展承载 `ready/variant`；`safeMode` 移至 P6 |
| v0.3 | 闭合第二轮审阅：contracts 定为 L0 中立契约层、跨线程访问矩阵、能力撤销级联事务、hook 物理归还与 tombstone、外部 Host API 只读默认档、资源异步安装状态机、分派三段式与短路唯一、required/optional × failure 状态映射、外部更新仅重启生效、迁移阶段依赖修正 |

---

## 0. 摘要

| 项 | 结论 |
|---|---|
| 分层 | **五个逻辑层**（含 L0 契约层）、**八个顶层目录**：`contracts / game / infra / hooks / patch / domains / plugins / host` |
| 依赖 | 完整依赖矩阵（§2.2），环由结构与脚本双重排除 |
| 功能形态 | **一切功能皆插件**：内置插件（含出口面 `core.api`/`core.ui`）；外部数据扩展与全权 native 插件（P6 草案） |
| hook 资源 | 资源集中在 `hooks/`：一资源一安装者 + 消费者注册；插件只声明与注册，机制由宿主执行 |
| 生命周期 | 十态状态机（§3.2），异步跨帧迁移，幂等、可回滚 |
| 线程 | **跨线程访问矩阵**（§6.1）：HTTP 只读快照与票据；游戏状态访问仅主线程 |
| 版本耦合 | 只有 `game/` 允许出现 VMA/偏移/ABI 签名；其余层双重强制（禁止 include symbols 头 + 文本扫描） |
| 可观测 | 插件/资源/能力/Variant 状态可查询（HTTP），P1 起提供最小诊断接口 |
| 文档 | 结构由脚本生成/校验；脚本不承诺运行时保证（§9.2） |

---

## 1. 设计目标与红线

### 1.1 目标

1. **可裁剪**：一个功能可以被独立启用、停用、删除，且停用后零业务副作用。
2. **可验证**：每个功能有独立状态上报与验收卡；安装结果（含失败原因）可查询。
3. **可迁移**：换游戏版本只改 `game/`；功能层不感知变体差异。
4. **可并行**：功能之间零编译期依赖，可多人/多代理并行开发与真机验证。
5. **可开放**：结构上支持将来加载外部插件（native `.so`），开放前不付 ABI 冻结成本。
6. **防复发**：同地址二次挂载、非主线程调用游戏函数这两类失败必须由结构阻止。

### 1.2 物理事实红线

| # | 事实 | 依据 |
|---|---|---|
| R1 | LSPosed NativeHook 对同一地址只能挂载一次，无链式能力，二次挂载返回 rc=-1；提供成对的 `unhook` API，但「卸载后重新挂载」的安全性需真机验证（U6） | 真机验证 + API 事实 |
| R2 | 代码运行在游戏进程内、单一 `.so`；hook 回调在游戏主线程，HTTP 在任意线程 | 运行时事实 |
| R3 | 游戏有多个变体（原版 / 大修 / monster），符号、跳板形态、能力位不同；游戏 targetSdk=29 | 包体实测 |
| R4 | 平台加载规则：app 私有目录 `.so` 可 `dlopen`；`/sdcard` 为 noexec；宿主符号对插件不保证可见，能力必须用函数指针表传递；`android_dlopen_ext` 无 flag 可绕过限制；Android 17（target 37）起 `System.load` 要求只读 | 官方文档 + AOSP bionic/sepolicy |
| R5 | 行为改动手段有五种：直读写内存、调游戏函数、指针槽覆盖、指令 patch、入口 hook。**机制选择按需求决定，不设固定风险序**：风险来自缺少目标校验、失败回滚、开关与状态上报 | 真机验证 |
| R6 | 同进程内**无法隔离崩溃**：非法内存访问、ABI 破坏、栈溢出会导致游戏进程终止。任何「失败隔离」承诺仅限「消费者返回可恢复错误」 | 运行时事实 |
| R7 | 外部 native `.so` 一经加载即拥有游戏进程内的完整代码执行能力；符号隐藏、linker namespace、Host API 裁剪和清单声明均不能限制其读取/修改内存、调用系统接口或绕过资源登记 | Linux/Android 进程模型 |
| R8 | 最后观测手段是日志、最后恢复手段是外部 `config.json` + 重启；二者必须独立于插件系统运行，不得依赖任何插件（自举兜底）。**日志双段**：启动早期（native `qol_log` 未就绪）由 Kotlin 侧写者负责，交接点 = native `qol_log` 首次初始化成功并落盘首条日志，此后日志由 native 单写者接管、Kotlin 早期段不再追加。**配置**：外部 `config.json` 由 native `infra/config` 在启动早期自读，不依赖插件、不依赖 Kotlin 初始化顺序 | 设计决策（ADR-0004、v0.3.12）+ 运行时事实 |

---

## 2. 结构与依赖矩阵

### 2.1 分层

```
L0  contracts/  中立契约层：POD 接口类型、版本常量、命令类型（零依赖、零实现、零状态）
L1  game/       游戏耦合唯一收口：变体、符号、ABI 签名、强类型读/写/调原语
L2  infra/      运行时：调度/命令队列/快照/事件/能力注册表/命令端口/日志/配置/存储/Result
L2  hooks/      入口 hook 资源层：登记、仲裁、分派、生命周期（唯一调 LSPosed API 处）
L2  patch/      指令 patch 与指针槽机制（占用声明、原字校验、回滚）
L3  domains/    语义域：游戏事实 → 传输无关 DTO 与领域操作
L3  plugins/<id>/   功能与出口面（插件）：含内置出口插件 core.api / core.ui
L4  host/       模块入口、JNI 边界、平台原语注入、装配与拓扑、插件注册表
```

`contracts` 是 **L0 中立横切层**：不计入业务分层，不依赖任何层，被所有层允许依赖（见矩阵）。

### 2.2 依赖矩阵（机器可读，脚本以此为准）

| 目录 | contracts | game | infra | hooks | patch | domains | plugins | host |
|---|---|---|---|---|---|---|---|---|
| `contracts/` | 自身 | — | — | — | — | — | — | — |
| `game/` | ✓ | 自身 | — | — | — | — | — | — |
| `infra/` | ✓ | —（经注入接口） | 自身 | — | — | — | — | — |
| `hooks/` | ✓ | ✓ | ✓ | 自身 | — | — | — | — |
| `patch/` | ✓ | ✓ | ✓ | — | 自身 | — | — | — |
| `domains/` | ✓ | ✓ | ✓ | — | — | 自身（聚合域例外） | — | — |
| `plugins/<id>/` | ✓ | ✓ | ✓ | ✓（仅声明资源） | ✓（仅声明资源） | ✓ | —（零 include） | — |
| `host/` | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | 自身 |

规则：
- `domains` **不依赖 hooks/patch**：域层不感知机制；资源声明归 `plugins` 或独立资源提供者。
- `plugins` 之间**零编译期依赖**：彼此只经 `infra/capabilities` 注册表取接口（接口类型在 `contracts/`）；出口面插件（`core.api`/`core.ui`）与功能插件同规则，不存在编译期互依赖。
- `core.ui` 的**自我约束**：矩阵允许 `plugins → game`，但 `core.ui` 不直接依赖 `game/`；UI 原语经 `contracts/` 的 ui-kit 接口 + `host` 注入的 adapter（adapter 背后为 `game/access/ui*`），使容器与控件装配可在无 `game/` 的环境下测试。该 adapter 接口冻结登记为 U11（§12.3）。
- `core.api` 不做游戏语义计算：除 `/api/health`、`/api/system/routes` 等传输级基础端点外不自带业务 API；路由 handler 由内置 API 提供插件或功能插件注册，只读请求读快照/登记数据，变更请求经命令票据；游戏读取由 owner 插件经 `domains`/快照完成。
- `domains/system` 是唯一允许 include 其他 domain 头的聚合域；其余域禁止水平互调；跨域复用原语下沉 `game/`（读）或 `contracts/`（类型）。
- `infra → game` 禁止：帧时钟、主线程派发、变体信息由 `host` 以注入接口提供。
- `contracts` 只使用固定宽度 C 类型（§3.4），不使用 STL 容器、虚函数、异常。

### 2.3 各层职责与禁止

| 层 | 职责 | 禁止 |
|---|---|---|
| `host/` | 模块入口、JNI 薄层（按插件分组）、平台原语注入（帧时钟/主线程派发/hook/patch/HTTP 传输桥）、装配顺序、插件注册表 | 业务逻辑；直接读写游戏内存 |
| `plugins/<id>/` | 一个插件：生命周期、规则、自有界面（`ui/`）、对外能力与命令；出口面插件（`core.api`/`core.ui`）同规则，仅额外拥有注册口能力（§7.1） | include 其他插件；执行 hook/patch 机制；出现裸地址；在命令路径之外调用 `game/write/**`、`game/ops/**` |
| `domains/` | 语义域：`LiveApi`（主线程 live model）与 `ReadModel`（快照）两套接口 | 依赖 plugins；输出 JSON；依赖 hooks/patch；水平互调（聚合域除外） |
| `hooks/` | 资源登记与仲裁、安装/还原引擎、consumer 分派 | 业务语义 |
| `patch/` | 指令 patch、指针槽：占用声明、原字校验、回滚 | 业务语义 |
| `infra/` | 调度、命令队列、快照、事件、能力注册表、命令端口、日志、配置、存储、Result | 依赖 game；持有游戏语义 |
| `game/` | 变体、符号、偏移、ABI 签名、强类型读写/调用原语 | 依赖任何上层；构造 DTO；注入业务名 |
| `contracts/` | POD 接口、版本常量、命令类型、JNI 绑定元数据 | 任何实现、状态、STL 容器 |

### 2.4 目录布局（native）

```
module/app/src/main/cpp/
  host/        gateway.cpp / assembly.cpp / transport_bridge.{h,cpp}（向 core.api 注入 native↔Kotlin 传输桥）/ plugin_registry.{h,cpp}
  plugins/
    core.api/   server/（http.registry 注册口 + §9.1 既有诊断端点）
    core.ui/    panels/（容器、通用组件；底层 ui_kit 在 game/access/ui*，见 ADR-0006）
    <id>/       plugin.cpp  rules.{h,cpp}  ui/*.cpp  api.cpp  ports.h
  domains/        character/ party/ inventory/ world/ quest/ ui/ shop/ save/ dialog/ system/
  contracts/      <capability_id>.h   <resource_id>.h   result.h
  hooks/          registry.{h,cpp} install.{h,cpp} dispatch.{h,cpp} resources/*.h
  patch/          instr_patch.{h,cpp} ptr_slot.{h,cpp} claims.{h,cpp}
  infra/          scheduler/ events/ capabilities.{h,cpp} commands.{h,cpp}
                  log/ config/ store/ snapshot.h
  game/           variants/ symbols/ access/ read/ write/ ops/
  tests/
```

---

## 3. 插件体系

### 3.1 内置轨与外部形态

| 轨道 | 形态 | 何时用 | 成本 |
|---|---|---|---|
| **内置静态插件**（默认） | 编译进主 `.so`，以 `PluginDescriptor` 注册；CMake target 独立；含出口面 `core.api`（HTTP）/ `core.ui`（游戏内 UI 容器） | 全部内置功能 | 零 ABI 成本，编译期类型安全 |
| **外部数据扩展**（P6 草案） | 未签名 ZIP：JSON、表、资源；不含可执行代码 | 静态规则、配方、文本、资源 | 宿主可完整校验和撤销；不能表达任意逻辑 |
| **外部 native 插件**（P6 草案） | 未签名 ZIP + `.so` + C ABI | 需要逻辑、hook、patch 或跨插件协作 | 进程内全权代码；无安全隔离保证 |

### 3.2 生命周期状态机（正式定义）

```
registered → resolving → { installed_disabled ⇄ enabled } → deactivating → inactive
                ↓    ↘           ↓
             blocked   failed（可显式重试）
                ↓
          unsupported / invalid（不可重试，除非条件改变/人工修正）
```

| 状态 | 含义 | 迁移 |
|---|---|---|
| `registered` | 已注册，未解析 | → resolving |
| `resolving` | 依赖/变体/清单校验中 | → installed_disabled / blocked / unsupported / invalid / **failed（安装失败）** |
| `blocked` | 必需能力缺失（可补齐） | → resolving（显式重试或依赖提供者恢复） |
| `unsupported` | 变体或符号不支持（非错误） | 条件改变（换变体）后 → resolving |
| `invalid` | 清单不合法 / 容器损坏 / Host API major 不兼容 | 人工修正后 → resolving |
| `installed_disabled` | 资源已声明、开关关闭；消费者未注册、门禁关闭、无业务副作用 | ⇄ enabled；→ deactivating |
| `enabled` | 资源生效、消费者已注册 | → installed_disabled / deactivating |
| `deactivating` | 摘消费者、等待在途归零、还原资源 | → inactive / failed |
| `inactive` | 资源已还原、能力已注销 | → resolving（幂等重入） |
| `failed` | 安装或回滚失败（**可重试**，非 terminal） | → resolving（显式重试） |

**正交维度（不属于生命周期态）**：

- `degraded`：**健康标记**（bool）。`optional_requires` 缺失或可选依赖失效时置位，插件继续工作；生命周期态不变（通常仍为 `enabled`）。§9.1 状态 API 的 `degraded` 字段即此标记。
- **原因码（`reason`）**：描述进入当前态的原因，不新增状态。`cascade_stopped`（必需依赖被撤销后由宿主停用）→ 生命周期态 `installed_disabled` + `reason=cascade_stopped`；`in_flight_timeout` / `unhook_failed` / `resource_ready_failed` → `failed` + 对应 reason。

语义定义：

1. **disable（enabled → installed_disabled）**：注销消费者、关闭门禁、清空能力暴露；允许保留**空转安装点**（wrapper 存在但立即透传原函数，fail-closed）。
2. **deactivate（→ deactivating → inactive）**：摘消费者 → 等待在途归零 → 还原资源（含物理 unhook，见 §4.2）→ 注销能力。**异步跨帧推进，禁止主线程阻塞等待**（§4.4）。
3. **「零残留」可验证定义**：零业务副作用 + 零消费者 + 零能力暴露 + 登记表占用为 `free` 或 `tombstone`（tombstone 是显式保留占用，见 §4.2）。**不要求** `dlclose`。该保证覆盖内置插件、数据扩展和外部 native 插件的**宿主管理部分**；外部 native 自行创建的线程、hook、patch、内存写入及其它 raw 副作用不在保证范围内。
4. **幂等**：`enable/disable/deactivate` 重复调用返回当前状态；并发调用由唯一执行器串行化。
5. **部分安装失败**：按逆序回滚已申请部分 → `failed`，失败资源与错误码写入状态；禁止半装状态对外可见。
6. **在途不归零**：超过帧数预算（§4.4）→ 资源转 `tombstone`，插件 `failed`（reason=`in_flight_timeout`），并进入隔离：禁止重试直到进程重启。
7. **进程退出**：不执行 deactivate；生命周期保证不跨进程。

### 3.3 插件清单

```cpp
struct PluginDescriptor {
    const char* id;                        // 唯一，Kotlin 侧同 id
    const char* version;                   // 语义化版本
    uint32_t requires_count;               // 显式数量（禁止哨兵终止）
    const CapabilityRequirement* requires;         // 必需能力（id + 版本范围）
    uint32_t optional_count;
    const CapabilityRequirement* optional_requires;// 可选能力（缺失 → degraded）
    uint32_t provides_count;
    const CapabilityProvision* provides;           // 静态声明的能力（id + 版本 + 接口指针 + publish_mode）
    uint32_t resources_count;
    const ResourceClaim* resources;                // 资源声明（不执行）
    uint32_t jni_count;
    const JniBinding* jni;                 // {kotlin_method, native_symbol, signature_hash}
    int  (*install)(PluginContext&);       // 仅自身初始化
    void (*deactivate)(PluginContext&);    // 仅自身清理
    void (*on_enabled)(PluginContext&);    // 可选；禁止资源操作
    void (*on_disabled)(PluginContext&);   // 可选；禁止资源操作
    void (*fill_status)(PluginStatusOut&); // 宿主传入 out 参数填充
    int32_t (*on_resource_ready)(PluginContext&, const char* resource_id, void* backup); // 可选；受限生命周期回调，0=成功（非业务回调）
};
```

规则：
- **数量字段显式**（`*_count`），不允许哨兵终止。
- **存活期**：描述符内的字符串、数组、函数指针均为**静态存储**，生命周期 ≥ 进程；宿主注册时可直接引用。
- **状态所有权**：`fill_status` 用宿主传入的 out 参数填充，避免跨边界所有权问题。
- **资源与能力的启停由宿主通用执行**（声明式）；`install/deactivate` 只做插件自身初始化与清理；`on_enabled/on_disabled` 不得申请/释放资源。
- **能力身份静态、可用性动态**：内置插件可发布的 capability id、版本、接口类型和 `publish_mode` 必须全部列在 `provides`；运行时不得凭空创建未声明能力。`kOnEnabled` 随插件启用自动发布，`kDynamic` 由宿主在条件成立时发布、条件失效时撤销（§3.4.6）。
- **backup 交还**：宿主在资源安装成功后经 `on_resource_ready` 把原函数入口交还所有者；插件不得自行安装资源。
- **`on_resource_ready` 是受限生命周期回调**，不属于 §4.4「安装期间不得调用插件业务代码」中的业务代码：只允许缓存 backup、更新门禁/原子量等常数时间操作；禁止资源操作、游戏内存读写、跨线程等待与长逻辑。返回非 0 → 宿主**按插件安装事务逆序回滚该插件全部已安装资源**，再置 `failed`（reason=`resource_ready_failed`）；**任一资源回滚失败 → 该资源 `tombstone` + 隔离**（§3.2 语义 5「禁止半装状态对外可见」）。
- 拓扑排序输入 = `requires` + `provides`；缺 `requires` → `blocked`；缺 `optional_requires` → 健康标记 `degraded`（生命周期态不变，见 §3.2）。

### 3.4 能力契约与命令端口

| 项 | 规定 |
|---|---|
| 定义位置 | `contracts/<id>.h`：POD 结构体（固定宽度 C 类型 + 函数指针）、`kVersionMajor/kVersionMinor`、命令类型 |
| 类型限制 | 只用 `int8_t..uint64_t`、`float/double`、`const char*`、函数指针、POD 结构体；**禁止 STL 容器、虚函数、异常、`std::string`** |
| 版本兼容 | 消费者声明 `[major, minor]`；提供者 major 必须相等，minor ≥ 声明值；不满足 → `blocked` |
| 对象生命周期 | 提供者保证接口对象在 capability 撤销事务完成前有效 |
| 回调线程 | 接口回调一律在游戏主线程 |
| 错误语义 | 返回 `Result`（§3.4.2）；空指针仅表示「不可用」 |
| 只读性 | 能力接口**只读查询** |
| 写入口 | 命令端口（§3.4.3） |

**3.4.1 能力撤销事务（依赖失效级联）**

provider 停用（disable/deactivate）时：

1. capability 置 `revoking`：拒绝新调用，记录在途计数。
2. `optional_requires` 的消费者收到 `capability_revoked` 事件，摘除接入并置健康标记 `degraded=true`（生命周期态不变，§3.2）。
3. `requires` 的消费者按**逆拓扑序**（深度最深者先）disable（生命周期态 `installed_disabled`，reason=`cascade_stopped`）。
4. 帧回调等待在途归零；超过帧数预算 → provider `failed`（隔离），消费者保持 `degraded` 标记。
5. 注销 capability。
6. provider 恢复后**不自动重启** `cascade_stopped` 的消费者（保持 `installed_disabled` + reason），需显式 enable（避免抖动）。

**3.4.2 Result（跨边界统一）**

```c
typedef struct QolResult {
    int32_t     ok;           /* 0=成功，非 0=错误码 */
    int32_t     code;         /* 域错误码 */
    const char* message;      /* UTF-8，返回方持有，只读；跨边界不得释放 */
    uint32_t    message_len;  /* 字节数；无 NUL 要求；建议 ≤512，超出截断 */
} QolResult;
```

**3.4.3 命令端口**

```c
typedef struct QolCommands {
    QolResult (*post)(QolCommandTicket* out, const QolCommand* cmd);  /* 主线程执行 */
    int32_t   (*state)(QolCommandTicket);      /* pending/done/failed/cancelled */
    QolResult (*cancel)(QolCommandTicket);     /* 取消语义由命令声明 */
} QolCommands;
```

命令类型为 POD（`contracts/` 定义）；结果写回票据，可选主线程回调。

**3.4.4 能力注册时序**

| 阶段 | `provides` 行为 | 可调用性 |
|---|---|---|
| `install`（自身初始化） | 只记录静态声明，不发布 | 不可用 |
| `resolving` 校验通过 | **不发布**（尚未启用） | 不可用；必需消费者 `blocked`、可选消费者 `degraded` 标记 |
| 进入 `enabled` | `kOnEnabled` 自动发布；`kDynamic` 保持未发布，等待 §3.4.6 条件 | 已发布项可用；动态项尚不可用 |
| `installed_disabled`（disable 后） | 已按撤销事务注销 | 不可用 |
| `disable` 事务 | 置 `revoking` → §3.4.1 级联 → 注销 | 撤销期间拒绝新调用；在途归零后不可用 |
| 重新 `enable` | `kOnEnabled` 重新发布；`kDynamic` 回到未发布并等待条件 | 自动发布项可用；动态项等待运行时判定 |
| `deactivate` | 同 disable 撤销路径；终态 `inactive` 不注册 | 不可用 |

消费者拓扑安装与 provider 发布同步进行：发布发生的同帧内，等待中的消费者按拓扑序继续解析。`provides` 的静态声明参与 DAG 构建，但只有实际发布后才满足 `requires` 的运行时可用条件。

**3.4.5 能力可用性事件**

| 项 | 规定 |
|---|---|
| 事件 | `capability_registered(id, version)` / `capability_revoked(id)`，经 §6.3 事件总线在主线程派发 |
| 用途 | `optional_requires` 的消费者在已 `enabled` 后等待能力出现并**延迟接入**（典型：插件先于 `core.api` 启用时，收到 `registered(http.registry)` 后补注册 HTTP 路由；收到 `revoked` 后反注册） |
| 边界 | `requires` 的必需依赖仍走 `blocked` 等待与 §3.4.1 撤销事务，不依赖本事件；事件只是 optional 依赖的补齐通道，禁止用事件重实现必需依赖 |

事件处理规则：

- optional capability 补接成功后清除该依赖对应的 `degraded` reason；仍有其它降级原因时 `degraded` 保持为 true。
- 补接失败、重复 route/consumer 冲突或版本不满足时保持 `enabled + degraded`，记录 reason；不得把事件回调失败冒泡为 provider 发布失败。
- 注册/撤销事件按 `(capability_id, generation)` 幂等；消费者处理过同一 generation 后不得重复接入。
- capability 撤销时先阻止新 lease，再发 `revoked` 让 optional 消费者摘除接入，最后等待在途 lease 归零；消费者不得跨帧缓存裸接口指针。

**3.4.6 动态能力可用性**

动态能力用于“provider 已安装，但能力是否存在只能在游戏运行过程中低频确定”的场景，例如延迟加载子系统、进入存档后才能完成的一次性目标解析。规则如下：

1. 插件仍须在 `PluginDescriptor.provides` 静态声明 id、版本、接口对象和 `publish_mode=kDynamic`；静态声明用于 DAG、冲突和 ABI 校验，不代表当前可调用。
2. provider 只提交 `set_capability_available(id, available, reason)` 请求；真正的 publish/revoke 由主线程生命周期执行器串行执行，并复用 §3.4.1 与 §3.4.5。
3. `unavailable → available` 发布新 generation；`available → unavailable` 先进入 `revoking`，拒绝新 lease、通知消费者并等待在途归零后撤销。重复请求幂等。
4. `requires` 消费者在动态能力未发布时保持/进入 `blocked`，撤销时按 required 级联；`optional_requires` 消费者保持 `enabled + degraded`，发布后延迟接入，撤销后退回降级路径。
5. **禁止用 publish/revoke 表达高频游戏上下文**：进档/退档、当前面板、商店是否打开、当前角色是否存在等频繁变化由稳定 capability 内的 `availability()/probe()`、快照或领域事件表达；接口返回 `not_ready` 不触发生命周期级联。
6. 内置插件不得动态创建未声明 id；P6 外部 namespaced capability 的运行时发现规则另行冻结。

### 3.5 配置、日志、状态

- 配置：每插件命名空间 `plugin.<id>.*`；读写经 `infra/config`；变更经事件广播。
- 日志：native `infra/log`（`qol_log`）单写者，domain = 插件 id；启动早期由 Kotlin 写者承担，交接点见 R8（§1.2）；热路径使用环形缓冲或节流。
- 状态：`{id, source, enabled, state, error, resources[], requires[], provides[], variant, degraded, managed_resources, guarantees}`（§9.1）；每个 `provides[]` 项目标至少含 `{id, version, publish_mode, availability, generation, reason}`，使动态能力的未发布/可用/撤销中状态可查询（**P5 目标**：当前实现仅 `{id, version}`；协议见 §3.4.6，端点扩展见 §9.1）。外部 native 的 `guarantees` 固定说明保证仅覆盖宿主管理资源；不得用插件自报字段声称其受到安全限制。

### 3.6 数据级插件（声明式规则）

- 外部 JSON/表定义规则/配方/阈值/词表；宿主按固定词汇表 + schema 校验；非法项拒收并上报。
- 只用于非热路径查表；热路径不做解释执行；数据文件默认关闭、可禁用；来源与哈希记录在状态中。
- 数据扩展**不包含通用逻辑代码**。需要条件、动作或组合时，只能使用宿主已定义的声明式 schema；需要任意逻辑时必须使用外部 native 插件，并接受 §3.7 的全权代码模型。

### 3.7 外部插件轨（**未冻结设计草案**）

**信任模型**：不做作者认证、证书白名单或插件签名。外部 native 插件定义为「来源未经认证、由用户显式授权加载的游戏进程内全权代码」；模块不保证其不会崩溃、破坏数据、访问文件/网络或绕过宿主。数据扩展因不含可执行代码，可由宿主完整校验。

**容器**：未签名 ZIP（建议扩展名 `.qolplugin`）：`plugin.json` + 可选 `lib/arm64-v8a/lib<id>.so`、`targets/*.json`、`assets/`。宿主计算 SHA-256 仅用于版本识别、完整性诊断和变更记录，**不代表可信身份**。

**目录模型（版本化 + 原子切换）**：

```
plugins/
  staging/<id>-<hash>/          # 导入区（校验前）
  active/<id>/<version>/        # 已导入版本（0444，目录 app 私有）
  active/<id>/current           # 指针文件，内容为版本号（原子 rename 更新）
  cache/                        # 可选解压缓存
```

**导入流程**：staging → 校验容器结构、路径穿越、`id/version/apiVersion/abi`、数据 schema 与可选目标描述 → 计算哈希 → 写入 `active/<id>/<version>/` → 原子更新 `current` → 记录导入日志（来源/哈希/时间）。同 id 多来源不表达身份关系；并存冲突时拒绝加载，由用户选择当前版本。

**更新与回滚语义（重要）**：

- **更新只在下次进程启动生效**：当前进程已加载的映射不会因磁盘替换而改变；本进程只允许 `disable`。
- 当前进程**禁止加载同一插件的新版本**；回滚同样是「改 `current` → 重启后生效」。
- 保留最近 N 个版本（配置），旧版本可清理。

**加载与恢复**：导入后默认关闭，用户逐插件显式启用；每进程每 id 只 `dlopen` 一个版本，不承诺 `dlclose`。宿主在 `dlopen`、入口调用及每个启动回调前持久化启动阶段标记，成功后清除；下次启动发现未清除标记时进入安全模式并提示疑似插件。用户可强制重新启用，宿主不永久拉黑、不把时间相邻自动等同于根因。

### 3.8 外部 Host API（**未冻结设计草案**）

同进程 native `.so` 无法实施「受限/全权」权限分级，因此所有外部 native 插件获得同一版 Host API。API 是开发便利与协作协议，**不是安全边界**；清单中的 `managed_only`、`uses_raw_access` 等字段最多用于诊断，不构成权限保证。

**托管协作面（推荐）**：

```
log | config/store(命名空间) | events | commands | scheduler | snapshot/data
capabilities.provide/require | native_services.provide/require
targets.provide/resolve | resources.declare | status
http.routes（经 `core.api` 注册，命名空间 `/api/ext/<插件id>/...`，受其门控与线程纪律约束）
```

宿主统一处理托管资源的冲突、安装、回滚、状态和依赖级联。插件间共享只读事实使用 §3.4 的 capability；任意 native 逻辑使用带版本的 C ABI `native_service` 函数表。两者都经宿主注册表查找，不直接依赖另一插件的 ELF 符号；字符串、数组和函数表所有权遵守 §3.4 的跨边界类型与生命周期规则。`native_service` 可包含写副作用，其行为不享有命令端口的串行化保证，调用方继承提供者的全权 native 风险。数据扩展只能消费带声明式 schema 的 capability/命令，不能调用任意 C ABI `native_service`。

**raw 便利面（全权，非保证面）**：

```
modules/maps/base | targets.resolve | mem.read/write/protect | 原始函数地址
hook/patch/slot 低层入口 | 主线程投递
```

即使宿主不暴露上述入口，native 插件仍可自行实现同等操作；提供 raw 面的目的只是减少重复适配。插件绕过资源登记后，宿主不负责冲突检测、回滚、状态一致性或停用后的副作用。

**TargetRegistry**：宿主内置公共语义目标；symbol pack 或插件可注册 namespaced 目标（如 `author.plugin:some_target`），按游戏变体提供 VMA、ELF 符号或特征定位、预期原字节和 ABI 描述。宿主可校验映射范围与原字并按最终地址仲裁；未知函数签名的正确性无法运行时证明，由 native 插件作者与启用用户承担。模块不预打包未来全部地址和签名。

**保证边界**：`disable/deactivate` 停止宿主回调并回收托管资源、能力和订阅；插件自行创建的线程、直接内存写入及自行安装的 hook/patch 不保证停止或还原。状态 API 必须显示 `source=external_native`、托管资源数以及 `guarantees=managed-resources-only`，不得把生命周期成功解释为该插件已零残留。

---

## 4. 资源层：hooks/ 与 patch/

### 4.1 资源类型

机制选择按需求决定，不设固定风险序：能满足语义的最小手段即可。任何机制都必须具备目标校验、失败回滚、开关、状态上报。

| 类型 | 机制 | 冲突形态 | 仲裁 |
|---|---|---|---|
| 入口 hook | LSPosed NativeHook | 二次挂载 rc=-1 | 一资源一安装者 |
| 调用点 patch | 指令 BL 替换 | 同调用点被两个功能改写 | 占用声明（`patch/claims`） |
| 指针槽 | PtrHook | 同槽位被覆盖 | 一槽一所有者；或 backup 链式 wrapper |
| 行为所有权 | 非 hook（移动、库存写、存档写） | 并发/时序 | 互斥槽 + 命令队列 |

### 4.2 所有权、占用状态与物理归还

**占用状态**：

| 状态 | 含义 |
|---|---|
| `free` | 未占用，可分配 |
| `disabled` | 安装点存在但空转透传（wrapper 立即调原函数） |
| `live` | 已安装并服务消费者 |
| `tombstone` | 物理还原失败或声明不可还原：**登记表保留占用**，禁止分配给新 owner，状态可查询 |

规则：
1. 所有权在**启动装配期**确定；插件只声明私有资源或注册为共享资源消费者。
2. **禁止运行期把私有资源提升为共享**；升级为共享必须在开发期重构（资源层拥有 + 消费者迁移）。
3. 共享资源 owner 负责 wrapper、backup 调用、门禁、生命周期；消费者只提供回调/登记数据。
4. **物理归还**：deactivate 时先摘消费者、等在途归零，再调 `unhook`（或写回指令原字）。
   - 还原成功 → 占用置 `free`（可重新分配）。
   - 还原失败，或资源声明 `resumable=false` → 占用置 `tombstone`，插件 `failed`，并进入隔离（禁止重试直到进程重启）。
5. `tombstone` 的回收策略（何时允许人工清除、是否进程重启后重试）见 U8。
6. **机制差异（空转能力）**：`kEntryHook` 与 `kPtrSlot` 支持禁用时保留**空转安装点**（wrapper 透传原函数）；**`kCallPatch` 不支持空转**——禁用即物理还原（字替换无可透传的执行体；BL 重定向若需透传由 wrapper 自决，但禁用态统一按还原处理，避免同一机制出现两种禁用语义）。门禁翻转驱动的**单指令 patch** 安装/还原允许在**帧回调内同步完成**（有界原子写，非阻塞等待）；生命周期 `activate/deactivate` 仍走帧步进事务。
7. 外部 native 只有通过 `resources.declare` 进入本登记表的资源才受本节约束；插件自行安装的资源属于 raw 副作用，不参与一资源一安装者、tombstone 或物理归还保证，状态中必须与托管资源区分。

### 4.3 资源描述符与登记表

```cpp
struct ResourceDescriptor {
    const char* id;            // 如 "hook:CHAR_AddDamage"
    const char* symbol;        // 经 game/ 解析的符号名
    Mechanism   mechanism;     // kEntryHook | kCallPatch | kPtrSlot | kOwnership
    Policy      policy;        // kPrivate | kShared
    Requirement requirement;   // kRequired | kOptional
    FailureMode failure;       // kFailSoft | kFailClosed
    bool        resumable;     // 是否承诺物理还原
    const char* owner;         // 所有者插件 id
    const char* reason;        // 选择该机制的理由
    const char* gate;          // 开关配置键
    uint32_t    step_budget;   // 安装/还原允许的帧数预算
    void*       handler;       // 所有者提供的替换入口（入口 hook/patch 的 wrapper）；共享资源同样由 owner 提供，消费者经登记回调接入（§4.5）
};
```

`handler` 是声明式安装的执行体：宿主按 `mechanism` 选择机制引擎并把 `handler` 交给它，不解释其语义（§3.3「资源与能力的启停由宿主通用执行」）。**共享资源与私有资源一样由 owner 提供 `handler`**——owner 的 wrapper 内部负责按 §4.5 分派消费者回调；消费者只登记回调/数据，不提供可安装执行体。

登记表是**代码事实**，由 `check_resources.py` 汇总并与文档表比对。

### 4.4 资源安装/还原事务（异步跨帧）

| 环节 | 规则 |
|---|---|
| 执行模型 | **状态机 + 帧步进**：每次帧回调推进一个资源步骤，禁止主线程阻塞等待 |
| 顺序 | 依赖拓扑序 → 插件 id 字典序（确定性） |
| 超时 | **不用墙钟超时**；用帧数预算 `step_budget`（默认 120 帧）；超预算 → 停止等待、进入 `failed` |
| 取消 | 已开始的 native 安装调用**不可取消**（同步调用执行完即完成）；超时只停止等待并隔离 |
| 在途归零 | 帧回调检查在途计数；超预算 → 资源 `tombstone` + 插件 `failed`(reason=in_flight_timeout) |
| 回滚 | 失败时按逆序还原已安装资源；回滚失败 → `tombstone` + `failed` |
| 死锁 | 安装/还原期间不得调用插件业务代码；插件回调内不得申请/释放资源 |
| 锁 | 资源层不持其他插件锁调用外部代码；跨插件交互经命令队列 |

### 4.5 分派协议（共享资源）

| 项 | 规定 |
|---|---|
| 顺序产生 | 依赖拓扑序 → 插件 id 字典序 → 注册调用序；启动期确定，不提供运行期重排 |
| 权限类型 | 注册时声明 `ConsumerKind`：`Observe`（只读，返回值忽略）/ `Modify`（可改参数或返回值）/ `Intercept`（唯一，可短路）；宿主在注册期强制（Intercept 唯一性、Observe 返回值忽略），并以类型化句柄别名（`ObserveHandle`/`ModifyHandle`/`InterceptHandle` = 同一 `DispatchId`）表达权限 |
| 短路唯一性 | **同一资源最多一个 `InterceptHandle`**；第二个申请者在安装期失败（`resource already has interceptor`） |
| 阶段 | 消费者注册时声明阶段：`before`（注册顺序，输入/参数修改累积）→ `original`（若未被短路）→ `after`（**逆序**，返回值覆盖）。**每个回调每次分派只调用一次** |
| 短路 | 仅 Intercept 可短路；短路后 `after` 仍执行但仅 Observe 生效（可观察，不可改） |
| 原子 before（可选） | 资源可声明 `atomic_before`：先跑全体「校验阶段」，全部通过后再跑「提交阶段」；否则参数修改不回滚 |
| 失败语义 | 消费者返回可恢复错误 → 记录（fail-soft 继续 / fail-closed 资源降级透传）；崩溃/非法访问/ABI 破坏**不可隔离**（R6） |
| 幂等与重入 | 回调内重入同资源由 `thread_local` 门禁拒绝 |

### 4.6 消费者接入形式

| 形式 | 适用 | 例 |
|---|---|---|
| 登记表（声明式） | 同构数据替换 | 文本替换：`{语义键, text_id, 作用域, 文本}` |
| 强类型回调 | 需要参与执行流 | 操作分派、库存链分流 |

---

## 5. game/：版本耦合唯一收口

```
game/
  variants/   变体识别表 + 能力位 + 跳板形态（原版/大修/monster）
  symbols/    常量唯一来源（VMA/偏移/签名），脚本对 .so 校验
  access/     基址定位、符号解析、强类型 fn_* 指针（含线程纪律标注）
  read/       强类型字段读取原语（无 DTO、无业务）
  write/      强类型字段写入原语（仅主线程）
  ops/        游戏函数调用封装（前置条件检查、失败语义）
```

规则：
- 只有 `game/` 允许 include `game/symbols/*` 并出现十六进制地址；其余层**禁止 include symbols 头**（结构性阻止），`check_offsets.py` 做补充文本扫描。
- `game/write/**` 与 `game/ops/**` **只允许在主线程命令处理器中调用**（写入口原则，§3.4）；无生命周期回调例外——`install/on_enabled/on_disabled/on_resource_ready/deactivate` 均不得调用；feature 对 `game/` 的其余依赖（read/abi）不受限。
- 变体未知或必需符号缺失 → 依赖该能力的插件置 `unsupported`，不启动、不半装（§8）。
- 换版本流程脚本化：提取 so → 符号校验 → 生成变体差异 → 真机验证清单。

---

## 6. infra/：运行时基础设施

### 6.1 跨线程访问矩阵

| 调用方 | 允许 | 禁止 |
|---|---|---|
| HTTP 线程 | 读 `snapshot<T>`；读不可变配置；提交命令并持票据查询 | 调用 capability / `LiveApi`；直接读游戏内存；阻塞等待 |
| 游戏主线程 | live model、命令执行、资源安装/还原、事件派发 | 自旋/睡眠/阻塞等待 |
| 模块初始化线程 | 投递命令、读配置、注册插件 | 任何游戏状态访问 |
| 插件回调（主线程） | 只读能力、命令投递、登记数据 | 申请/释放资源；持锁跨插件调用 |
| 预取/后台线程（若存在） | 只读快照与原子标量 | 任何游戏函数调用 |

`domains` 提供两套接口：
- `LiveApi`：主线程专用，直接读游戏内存并返回 DTO。
- `ReadModel`：主线程发布到快照，任意线程只读。

HTTP 取实时数据的路径：优先读 `ReadModel`；必须实时时 `POST` 返回 **202 + 票据**，客户端轮询票据或 `GET /api/system/tickets/{id}`。**HTTP 线程一律不做同步等待**——无「短超时同步返回」例外。

### 6.2 快照规范

| 项 | 规定 |
|---|---|
| 类型约束 | `snapshot<T>` 仅限 `std::is_trivially_copyable_v<T>` |
| 发布一致性 | 双缓冲 + 原子索引（release/acquire）；读取方拿到自洽整块 |
| 版本号 | 每块快照带帧号与序号；health 报告新鲜度 |
| 非平凡对象 | 发布后不可变对象 + `std::atomic<std::shared_ptr>`；回收在主线程延迟进行 |
| 回收 | 读取方持有期间不得释放；主线程按引用计数回收 |
| 禁止内容 | 函数指针、迭代器、指向游戏内存的裸指针 |

### 6.3 事件总线

类型化事件 + 句柄订阅；发布者不依赖订阅者；事件在游戏主线程派发；订阅与注销成对，插件 deactivate 时强制清空。

### 6.4 能力注册表与命令

- `capabilities.register(id, iface, version)` / `require(id, range)`；未注册 `require` 返回空，消费者必须降级并上报 `degraded`。
- 撤销按 §3.4.1 事务执行；注册/注销发布 §3.4.5 能力可用性事件。
- 命令端口见 §3.4.3；票据状态可查询；HTTP 端点 `GET /api/system/tickets/{id}` 与首个返回票据的端点**同时交付**（§9.1）。

### 6.5 日志 / 配置 / 存储 / Result

- 日志：native 侧 `qol_log` 单写者，`<ts> f=<frame> <L> <domain> <src> <msg>`，domain = 插件 id；启动早期由 Kotlin 写者承担，交接点见 R8（§1.2）。
- 配置：外部 `config.json` 唯一来源，由 native `infra/config` 在**启动早期自读**（不依赖插件、不依赖 Kotlin 初始化顺序，R8）；插件命名空间隔离；修改即持久化并广播；Kotlin `ModuleConfig` 过渡期经 JNI 读写同一文件，P5 收敛为经 JNI 读写或只读镜像（跨栈边界见 §7.3）；迁移期允许一次性导入旧键（映射表登记）。
- 存储：插件 sidecar 按存档槽隔离；写入必须走事务（prepare/commit/abort）。
- Result：见 §3.4.2。

---

## 7. 出口面插件与 host/

### 7.1 出口面内置插件

| 插件 | 依赖 | 职责 | 禁止 |
|---|---|---|---|
| `core.api` | `contracts`、`infra`（HTTP 传输由 Kotlin 侧 AndServer 承担，native 经 `host` 注入的传输桥交互，§7.3） | `http.registry` 注册口与**路由表唯一真源**、路由分派、JSON 信封/错误映射、`/api/op` 门控、线程纪律与在途请求 lease；自身只保留 `/api/health`、`/api/system/routes` 等传输级基础端点；不实现 socket/HTTP 解析与监听（归 Kotlin 传输层） | 游戏语义计算；直接读游戏内存；承载 character/world/item 等业务端点 |
| 内置 API 提供插件 | `contracts`、`infra`、`domains` | 注册现有冻结路由并拥有其参数/DTO/语义；默认按领域拆分（如 `api.character`、`api.world`），也允许为独立裁剪边界拆成 `api.info`/`api.action` | 改写既有 URL；绕过命令端口执行变更 |
| `core.ui` | `contracts`、`infra`、`domains`（UI 原语经 `contracts/` 的 ui-kit 接口 + `host` 注入 adapter，adapter 背后为 `game/access/ui*`；自我约束见 §2.2） | 设置页容器、共享控件（`ui_kit`）、面板注册与导航；实现通用组件（首期 `Panel`/`Button`/`Label`/`Toggle`/`Select`/`ScrollView`，见 ADR-0006） | 实现功能面板（归各插件 `ui/`）；游戏语义计算；直接依赖 `game/` |

**注册口契约（P5 冻结，U10）**：

```c
typedef struct HttpRoute {
    const char* method;    /* GET | POST */
    const char* path;      /* 内置：API 权威文档中的冻结路径；外部：必须为 "/api/ext/<插件id>/..." */
    uint8_t     kind;      /* query | command */
    uint8_t     op_gated;  /* 1 → api 统一执行 /api/op 门控 */
    int32_t   (*handler)(const HttpReq* req, HttpRes* res); /* C ABI；HttpReq/HttpRes 为 POD，body 为原始 JSON 字符串 */
} HttpRoute;
```

**路由归属与兼容规则**：

- 插件 id 是内部所有权标识，**不决定内置 URL 前缀**。`docs/reference/api-reference.md` 中既有路径、方法、成功响应保持冻结；P5 产出“冻结路由 → 唯一 owner 插件”表并由 `check_plugins.py` 校验重复/遗漏。
- 新增内置路由按领域命名并进入 API 权威文档评审；外部插件只允许 `/api/ext/<插件id>/...`，不得占用既有命名空间。
- `query` handler 只能在 HTTP 线程读取快照、不可变配置和登记数据；`command` handler 只做语义解析并提交命令，返回票据。`op_gated` 在 handler 前由 `core.api` 统一检查。
- 默认按领域组织 API provider，保持 DTO 与读写语义内聚；`api.info`/`api.action` 可作为需要独立启停的部署边界，但不得据此改变 URL 或复制领域逻辑。

**生命周期与分工原则**：封装/解包、信封、门控、线程纪律归 `core.api`；语义解析与执行归 owner 插件 handler。`core.api` 被禁用时先停止接受新请求并冻结路由表，等待在途 route lease 归零，再撤销 `http.registry` 和关闭 server；超出帧预算进入 `failed + isolated`。依赖它的插件以 `optional_requires` 声明，并按 §3.4.5 在能力发布/撤销时延迟注册或反注册；route 注册失败只使该插件的 HTTP 出口降级，不影响其游戏内主体功能。

**UI 边界**：`plugins/<id>/ui/` 拥有该插件的面板实现与交互；`core.ui` 提供设置页容器、**通用组件**与面板注册表，不实现功能面板；组件分层、方框两拆与首期组件清单见 ADR-0006（记录，实施待排期）。

### 7.2 host 装配与 JNI

- JNI 薄层按插件分组命名：`native<Feature><Action>`；绑定元数据来自 `PluginDescriptor.jni`（`contracts/` 定义 `JniBinding`）。
- `check_jni.py` 当前校验 Kotlin `external fun` ↔ native 导出**双向**一致；描述符 `jni` 表的第三向随 P5 脚本收紧落地（§9.2）。
- 装配顺序：`game` 解析 → `infra` 启动 → `hooks/patch` 引擎就绪 → 插件按 `requires/provides` 拓扑安装（含 `core.api`/`core.ui`）→ 状态上报。
- **生命周期执行器**：唯一执行器为主线程，事件驱动（命令队列 + 帧回调）；初始化线程只投递；所有迁移幂等串行，带帧数预算与回滚。

### 7.3 Kotlin 侧镜像

```
com.inotia4.qol/
  host/       入口、HTTP 传输（AndServer 监听/解析/写回）、JNI 桥
  platform/   配置（过渡）、日志（启动早期段）、静态数据、存储、hook 安装（Java 侧）
  plugins/<id>/  与 native 插件同 id：service、bridge、UI（含 core.api/core.ui 对应面）
  services/   服务契约与实现（可多通道复用）
```

规则：controller 只路由 + 参数校验；JNI 只在 `host`/`platform`/插件 bridge 出现；插件之间通过服务契约或配置事件交互，不互相 import；插件界面实现位于 `plugins/<id>/ui/`，容器与共享控件位于 `core.ui`。

**跨栈边界（HTTP / 配置）**：

- **server**：HTTP 监听、socket 与 HTTP 解析、响应写回归 Kotlin `host/`（AndServer）。native 不实现或持有 server 句柄。
- **路由表**：唯一真源在 native `core.api`（`http.registry` 注册口写入）；P5 产出并校验“冻结路由 → 唯一 owner 插件”表（U10）。
- **JSON**：信封、错误映射、路由模式校验归 native `core.api`；Kotlin 传输层只把请求（method/path/body）交给 native 分派、把 native 返回的信封字符串写回，不解析业务 JSON、不做语义计算。
- **JNI 面形状**：由 `host/` 注入一个按插件分组的传输桥（native↔Kotlin），Kotlin 收包后调用 native 分派并取回响应；具体 ABI 与错误码随 U10 在 P5 冻结。
- **Kotlin 收缩**：`platform/` 保留配置（过渡期经 JNI 读写 native `config.json`，P5 收敛为 JNI 读写或只读镜像）、日志早期段、静态数据、存储与 Java 侧 hook 安装；HTTP 出口面仅剩传输适配，路由与信封逻辑全部下沉 native `core.api`。

---

## 8. 兼容与状态判定矩阵

| 维度 | 取值 | 判定 |
|---|---|---|
| variant | 原版 / 大修 / monster / unknown | `unknown` → 依赖具体符号的插件 `unsupported` |
| plugin 变体约束 | 无独立字段：由 `requires` 资源的**符号存在 + 变体能力位**条件表达（§5） | 不满足 → `unsupported`（reason 指明资源与条件）；`blocked` 仅用于能力缺失 |
| capability | major 相等 + minor 满足 | 不满足 → `blocked`；optional 缺失 → 健康标记 `degraded` |
| resource 声明 | 符号存在 + 变体能力位 + 原字节校验 | 任一失败 → 按 `requirement` 判定（下表） |

> 内置插件**不设** `supportedVariants` 字段；外部轨清单的同名字段（§3.7 草案）只用于**容器导入拒绝**，不影响内置轨判定。`resolve` 期的变体条件预检（符号存在性 + 能力位，产出 `unsupported` + reason）随首个受变体约束的插件落地（P4）。

**资源失败 → 插件状态映射**

| 资源 `requirement` | 安装失败 | 运行期消费者错误（可恢复） | 回滚 / 还原失败 |
|---|---|---|---|
| `kRequired` + `kFailClosed` | `failed`（回滚全部） | 资源降级透传，插件置健康标记 `degraded` | `tombstone` + `failed`（隔离） |
| `kRequired` + `kFailSoft` | `degraded` 标记（功能受限，记录） | 记录并继续，插件保持 `enabled` | `tombstone` + `failed`（隔离） |
| `kOptional` + 任一 | `degraded` 标记（记录，不影响其它） | 记录并继续 | `tombstone` + `failed`（隔离） |

**物理还原失败不按 `required/optional` 区分**：unhook 失败或 `resumable=false` 一律 `tombstone` + `failed` + 隔离（§4.2）。`requirement` 只影响安装失败与运行期可恢复错误的处置。

---

## 9. 可观测、验证与脚本边界

### 9.1 状态 API

P1 最小诊断接口（必须）：

| 端点 | 响应 | 说明 |
|---|---|---|
| `GET /api/system/features` | `{"ok":true,"features":[{id, version, state, enabled, error, resource_count}]}` | 状态登记表导出；`state` 为英文枚举名 |
| `GET /api/health`（既有端点，**加法扩展**） | `{"ok":true,"ready":true,"variant":{"series","version","known","capabilities"}}` | `ok` 保持不变；`ready` 与 503 语义一致；`variant` 取自 `game/` 变体表 |

P2 已落地：`features` 追加 `resources[]/requires[]/provides[]/degraded`。

P5 扩展：`provides[]` 追加 `publish_mode/availability/generation/reason`；新增 `/api/system/routes` 输出 `{method,path,owner,kind,op_gated,state,in_flight}`，用于核对冻结路由 owner、动态接入与在途归零。

P3 落地：

| 端点 | 响应 | 说明 |
|---|---|---|
| `GET /api/system/resources` | `{"ok":true,"resources":[{id, owner, mechanism, policy, state, in_flight, isolated}]}` | 资源登记表导出；`state` = free/disabled/live/tombstone |
| `POST /api/op/features/lifecycle` | `{"ok":true,"ticket":N}` | `op` 门控；body `{id, action: deactivate\|activate}`，经命令端口投递、主线程帧步进执行；`enable/disable` 仍由配置门禁（`/api/config/set`）承担，保持单一写者；查询走 `features` 状态 |
| `GET /api/system/tickets/{id}` | `{"ok":true,"ticket":N,"state":"pending\|done\|failed\|cancelled"}` | 命令票据状态（P3 起，与首个返回票据的端点同时交付） |

`GET /api/system/tickets/{id}` 已随 P3 生命周期端点落地（见上表）。

`safeMode` 移到 P6（外部插件轨）：P1–P5 不存在可禁用的外部插件，恒定 `false` 无诊断价值。

P6 外部插件状态追加：`source=data_extension|external_native`、`content_hash`、`managed_resources`、`guarantees`、`startup_stage`；`uses_raw_access` 若提供只能是插件自报诊断值，不得用于安全判断。

### 9.2 校验脚本族与能力边界

**已落地**（`check_arch.py` 默认入口；`check_symbols.py` 在默认入口外按需单跑）：

| 脚本 | 实际检查方式 | 能保证 | 不能保证 |
|---|---|---|---|
| `check_layers.py` | **include 图**（目录级 include 边 + 强连通分量） | 目录级依赖方向、include 无环 | CMake target 链接层、运行期回调链、数据流 |
| `check_resources.py` | 源码**文本扫描安装点**（entry hook / call patch / 旧式 `PtrHook` 文本三类；`resource_duplicate_owner` 重复归属） | 文本可见范围内的一资源一安装者、未登记安装点与重复安装点 | 声明式 `ownership`/`kPtrSlot` 资源（尚未被扫描）、运行期是否安装成功/是否归还 |
| `check_offsets.py` | 非 `game/` 层 include 符号头（结构）+ 裸地址文本扫描 | 明显裸地址与符号头越权 | 宏展开生成、间接计算地址 |
| `check_jni.py` | Kotlin `external fun` ↔ native 导出**双向**方法名比对 | 命名与数量一致 | 签名/ABI 兼容性本身；描述符 `jni` 表第三向比对（规划） |
| `check_symbols.py` | 常量对 `.so` 校验（默认入口外） | 符号与版本变化可发现 | 结构偏移语义正确性 |

**规划（未落地，目标见括注）**：

- `check_plugins.py`：注册表完整性 + `requires/provides` DAG（编译期常量）（目标：依赖无环、清单与注册一致）。当前由 host 单测（`plugin_registry_tests` / `plugin_lifecycle_tests` / `capability_lifecycle_tests`）覆盖；**落地前不得当作可运行入口**。
- `check_resources.py` 的声明式 `ownership`/`kPtrSlot` 资源扫描（目标：四类机制齐备）。
- `check_jni.py` 的第三向：`PluginDescriptor.jni` 表比对（目标：三向一致，§7.2）。
- `check_layers.py` 的 CMake target 链接矩阵（目标：源码与链接两层）。

运行入口：`uv run python scripts/maintenance/check_arch.py`（`--freeze` 冻结基线，`--only <name>` 单跑）。
基线文件 `scripts/maintenance/arch_baseline.json`（棘轮，只允许收缩）；豁免文件 `scripts/maintenance/arch_exemptions.json`（人工登记，须带文档锚点）。

**脚本只做结构检查**；「零残留」「幂等」「回滚」「在途归零」等运行时保证由**状态机 + RAII + 故障注入测试**提供（§9.3）。

### 9.3 测试计划

| 层 | 手段 | 覆盖 |
|---|---|---|
| 纯逻辑 | host 单测（CMake/ctest） | rules、编码、几何、协议解析 |
| 装配与失败路径 | host + fake 后端 | 安装/停用、资源冲突、部分失败与回滚、重复 enable/disable、缺依赖降级、契约版本不匹配、能力撤销级联 |
| 并发与切换 | host + fake | 配置热切换、迁移中并发切换、在途回调期 deactivate、帧预算超时、死锁检测 |
| 异常终止 | host + fake | 异常终止后的状态一致性（sidecar/事务恢复） |
| 真机 | 验收卡（每插件一张） | ABI、时序、交互、变体差异、崩溃兜底、unhook 后重新挂载（U6） |

### 9.4 文档治理

- 架构规则以本文为准；脚本输出代表当前实现事实。
- 不一致 → 构建失败；处理方式二选一：修改设计（走版本记录）或登记临时豁免（路径 + 允许目标 + 文档锚点 + 清除条件）。
- **基线棘轮**：首次运行生成基线快照（记录既有违规）；此后新增违规即失败，已消除违规在重冻结时移除——基线只允许收缩，不允许扩张。
- **过渡映射寿命**：`arch_common` 的 legacy→目标层过渡映射（`_ROOT_OVERRIDES`/`_CORE_OVERRIDES` 与 `feature/` 前缀映射）随 legacy 目录清空而**删除**；删除后 `unknown_layer` 由警告收紧为**硬失败**（P5 出口条件之一）。
- 「结构」类内容由脚本生成或校验，不手写；人工只维护「为什么」。

---

## 10. 非功能指标框架

> 名称与定位：**指标框架**。阈值需真机测量后回填（U1），回填并签署前不得对外承诺性能。

| 指标 | 度量方式 | 阈值 |
|---|---|---|
| hook wrapper 开销 | 帧内计时 + 开关态对比 | 待测 |
| 每帧回调总预算 | `frame` 调度器统计 | 待测 |
| 事件派发延迟 | 事件时间戳 vs 帧号 | 同帧内完成 |
| 快照新鲜度 | health 报告帧号 | ≤1 帧（目标） |
| 启动到就绪 | 启动日志 | 待测 |
| 日志容量 | 单写者统计 + 滚动 | 待测 |
| 最大插件/消费者数 | 注册表计数 | 待测（超限上报） |
| 配置切换延迟 | 票据耗时 | 待测 |

---

## 11. 重构接入方案

原则：**先闭合协议 → 最小骨架 → 纵向样板反证 → 批量迁移 → 收紧收口**。阶段是**验收里程碑**，不是提交粒度；提交粒度 = 改动单元（每个脚本、每次接口冻结、每个插件）。

| 阶段 | 内容 | 出口验收 | 依赖 |
|---|---|---|---|
| P0 | **校验脚本族落地 + 基线棘轮冻结**（check_layers / check_offsets / check_resources / check_jni）；开工基线签署见下方签署规则 | 第二轮审阅通过无阻断项（设计 v0.3）；脚本可运行；基线快照已冻结，新增违规即失败 | — |
| P1 | 最小骨架：`contracts/` + `infra/`（命令队列、快照、事件、能力、命令端口）+ `game/` 最小面 + **最小诊断接口** | 骨架可编译、host 单测通过、无行为改动、`/api/system/features` 最小字段可用 | P0 |
| P2 | **纵向样板插件**（覆盖入口 hook + 配置 + 状态 + 命令）：只验 `install → enabled` 路径 | 样板真机验收卡通过；状态 API 可见 | P1 |
| P3 | 最小资源事务（异步步进、回滚、在途归零、帧预算）+ `disable/deactivate/tombstone` 全路径 | 故障注入与并发测试通过；接口冻结（内部） | P2 |
| P4 | 批量迁移其余插件（内置静态轨），每插件一个提交 | 每插件：单测 + 验收卡；临时豁免逐条清零 | P3 |
| P5 | `plugins/` 单层收口：`core.api` 路由/注册内核（**HTTP 传输保持 Kotlin AndServer，不迁移**）、内置 API provider、`core.ui` 与注册口落地；冻结路由 owner 表；动态 capability 协议、JNI 分组、状态 API 完整化；**外部 `config.json` 由 native 启动早期自读、Kotlin `ModuleConfig` 收敛为 JNI 读写/只读镜像**；收紧脚本；删除旧路径与过渡映射 | 既有 API 路径/响应兼容；动态发布/撤销与在途归零通过；native 自读 config 可用且与 Kotlin 边界收敛；`check_*` 全绿、豁免清零、单轨（legacy 路径）消失、过渡映射删除 | P4 |
| P6（可选） | 外部插件轨（未冻结草案）：数据扩展、未签名全权 native、TargetRegistry、Host API/SDK、导入加载、安全模式、更新/回滚语义 | 数据 schema 拒绝；native 显式启用/加载/禁用；托管资源回收；启动故障恢复；更新/回滚真机通过 | P5 |

单插件迁移动作：建目录与元数据 → 资源改为声明 → 依赖改能力/命令 → 开关与状态接入 → 单测 + 验收卡。

**签署规则**：每次设计校准（v0.3.x）在**下一阶段开工前**必须通过一次外部复审；「P0–P4 完成」仅指各阶段出口条件达成，**不等于**设计书已签署。当前待签基线 = **v0.3.13**（P5 开工基线候选，含插件拍平、API 出口面与动态 capability 校准、HTTP 传输保持 Kotlin 与外部 config native 自读决策及复盘校准、UI 组件分层审阅记录 ADR-0006）；P5 未签署前不得开工。

冻结与兼容：JNI 导出在重构期保持名称与签名稳定；新增按插件分组命名，P5 一次性切换。既有 HTTP 路径、方法和成功响应保持兼容，P5 只迁移 owner，不改变调用方契约。数据格式（sidecar / `.qol_save`）向前兼容，必要时升版本并写迁移。

回滚：每阶段出口不达标则回滚该阶段改动，不动其它插件。

---

## 12. 风险、不做与未决项

### 12.1 风险与缓解

| 风险 | 缓解 |
|---|---|
| 接入期两套路径并存 | 纵向样板先行；阶段出口脚本强制收敛；豁免逐步清零 |
| 资源数量失控 | 登记强制填写选择理由与验收卡；`check_resources` 统计趋势 |
| 外部 native 可崩溃、破坏数据或绕过宿主 | 导入后默认关闭、用户显式启用、全权风险提示、启动阶段日志与安全模式；不承诺隔离 |
| 出口面插件（`core.api` 等）故障导致不可观测 | 最后观测 = 日志（Kotlin 早期写者 + native `qol_log` 双段，R8）；最后恢复 = 外部 `config.json`（native 启动早期自读）+ 重启；诊断端点自身不做插件间依赖 |
| 真机验证成本 | 每插件独立提交 + 验收卡；host fake 覆盖失败路径 |
| 外部 ABI 成本被低估 | P6 前不冻结、不发布 SDK；冻结时采用带 size/version 的 C ABI 函数表并保留 capability 版本协商 |
| 主线程被长操作占用 | 全部资源操作异步帧步进；禁止阻塞等待 |
| 文档再次腐烂 | 结构由脚本生成/校验；权威层级明确（§9.4） |

### 12.2 明确不做（YAGNI）

- 不对同进程 native `.so` 伪造受限权限模型；所有外部 native 均按全权代码处理。
- P6 首版不引入 WASM、脚本解释器或隔离进程；需要真正受限的逻辑插件时另立阶段设计。
- 不为内置插件冻结 ABI、不发布 SDK（P6 再议）。
- 不做插件市场、在线下载、自动更新；更新仅重启生效。
- 不追求 feature 之间零依赖（能力/命令/事件已足够）。
- 不引入逐功能接口样板：统一能力注册表 + 资源登记。

### 12.3 未决项（逐项归属与结论）

| # | 未决项 | 归属 | 状态 |
|---|---|---|---|
| U1 | 非功能指标阈值回填（§10） | P4 真机测量 | ⬜ 开放（不阻塞 P4 开工） |
| U2 | 共享资源清单与所有者划分 | P4 A4（`hooks/dispatch` 落地时产出） | ⬜ 开放 |
| U3 | 未签名容器导入、app 私有目录 `dlopen` 与启动阶段日志的真机可用性 | P6 真机探针 | ⬜ 开放 |
| U4 | 快照类型清单与发布方式 | P4/P5（随首个 `domains`/`ReadModel`） | ⬜ 开放 |
| U5 | 命令取消语义清单 | P4（随首个可取消命令） | ⬜ 开放 |
| U6 | `unhook` 后同地址重新挂载的安全性 | — | ✅ 已闭合（证据：`verification.md` VM-P3-01，同址二次挂载 rc=0、行为恢复） |
| U7 | Host API、TargetDescriptor、外部 capability/native-service 函数表的首个 C ABI 版本与 SDK 边界 | P6 | ⬜ 开放 |
| U8 | `tombstone` 回收策略与人工清除流程 | P5（随隔离语义收口） | ⬜ 开放 |
| U9 | 需要 `atomic_before` 两阶段分派的资源清单 | P4/P5 | ⬜ 开放 |
| U10 | `http.registry` v1 的 `HttpReq`/`HttpRes` ABI、冻结路由 owner 表与冲突错误码；路由兼容、线程种类、动态注册/撤销和在途 lease 语义已由 v0.3.11 冻结 | P5 出口前 | ⬜ 开放 |
| U11 | `core.ui` 的 ui-kit adapter 接口冻结（`contracts/` 类型 + `host` 注入函数表；adapter 背后为 `game/access/ui*`）；**由 ADR-0006 收敛为 `contracts/ui.h` 标准组件接口（记录，实施待排期）** | P5 出口前 | ⬜ 开放 |

说明：**P0–P3 的出口条件不依赖上述开放项**（「P0–P3 完成」不等于设计书已签署，见 §11 签署规则）；开放项必须在对应阶段开工前闭合，否则该阶段不得宣称完成。

---

## 附录 A：资源登记表模板

| id | symbol | mechanism | policy | requirement | failure | resumable | owner | consumers | gate | 选择理由 | 验收卡 |
|---|---|---|---|---|---|---|---|---|---|---|---|
| `hook:CHAR_AddDamage` | `F_CHAR_ADD_DAMAGE_VMA` | entry hook | private | required | fail-closed | true | `simple_mode` | — | `simpleModeEnabled` | 无函数指针槽，且需要入口前后处理 | VM-x |
| `hook:MEMORYTEXT_GetText` | `F_MEMORYTEXT_GET_TEXT_VMA` | entry hook | shared | required | fail-soft | true | `text` | charinfo 等（登记） | — | 全局文本唯一热点，多消费者必须集中分派 | VM-x |
| `hook:SAVE_SaveInventory` | `F_SAVE_SAVE_INVENTORY_VMA` | entry hook | private | required | fail-closed | true | `extension_bag` | — | `extensionBagEnabled` | 调用点会被第三方 mod 改写，函数入口更稳定 | VM-x |

## 附录 B：插件容器与清单（外部轨草案）

容器：未签名 ZIP / `.qolplugin`（`plugin.json` + 可选 `lib/<abi>/lib<id>.so`、`targets/*.json`、`assets/`）。
清单字段：`id / version / kind(data|native) / apiVersion / abi? / supportedVariants / requires[] / optionalRequires[] / provides[] / resources[] / targets[] / entry?`。宿主计算内容哈希用于诊断，不做作者认证。
目录：`plugins/staging/<id>-<hash>/` → `plugins/active/<id>/<version>/` + `current` 指针；更新与回滚仅在下次进程启动生效。
