# 架构设计书：分层收口与插件化

> 状态：设计稿 **v0.3**｜日期：2026-09-16
> 适用范围：**§1–§9、§11、§12 对内置插件轨（含全部核心架构）为权威规范**；**§3.7/§3.8 外部插件轨为未冻结设计草案**（P6 前允许大改，不构成实施承诺）。
> 权威层级：架构规则以本文为准；校验脚本输出只代表「当前实现事实」。二者不一致时构建失败，必须显式走「改设计」或「登记临时豁免（带文档锚点）」之一。
> 本文只描述目标结构；结构类内容由脚本生成或校验，人工只维护「为什么」。

**版本记录**

| 版本 | 变更 |
|---|---|
| v0.1 | 初稿：分层、资源仲裁、版本收口、线程约束、插件化 |
| v0.2 | 闭合第一轮审阅：依赖矩阵、生命周期、能力契约、资源所有权、分发模型、脚本边界、纵向迁移 |
| v0.3.3 | P2 实施校准：`PluginDescriptor` 增加 `on_resource_ready`——宿主声明式安装后须把 backup（原函数入口）交还所有者，否则 wrapper 无法调用原函数 |
| v0.3.2 | P2 实施校准：资源声明补充 `handler`（所有者提供的替换入口）——否则「宿主声明式安装」在入口 hook 机制下无可执行体 |
| v0.3.1 | 实施校准（P1）：诊断接口按实测收敛——`features` 为 `{"ok":true,"features":[...]}` 信封并含 `version/resource_count`；`/api/health` 以加法扩展承载 `ready/variant`；`safeMode` 移至 P6 |
| v0.3 | 闭合第二轮审阅：contracts 定为 L0 中立契约层、跨线程访问矩阵、能力撤销级联事务、hook 物理归还与 tombstone、外部 Host API 只读默认档、资源异步安装状态机、分派三段式与短路唯一、required/optional × failure 状态映射、外部更新仅重启生效、迁移阶段依赖修正 |

---

## 0. 摘要

| 项 | 结论 |
|---|---|
| 分层 | **六个逻辑层**（含 L0 契约层）、**九个顶层目录**：`contracts / game / infra / hooks / patch / domains / features / products / host` |
| 依赖 | 完整依赖矩阵（§2.2），环由结构与脚本双重排除 |
| 功能形态 | **一切功能皆插件**：内置静态插件（默认），外部动态插件（P6 草案） |
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
L3  features/<id>/   模块增强功能（插件）
L4  products/   产品面：api_export（HTTP/JSON） ‖ qol（游戏内 UI 面）
L5  host/       模块入口、JNI 边界、HTTP server、装配与拓扑、插件注册表
```

`contracts` 是 **L0 中立横切层**：不计入业务分层，不依赖任何层，被所有层允许依赖（见矩阵）。

### 2.2 依赖矩阵（机器可读，脚本以此为准）

| 目录 | contracts | game | infra | hooks | patch | domains | features | products | host |
|---|---|---|---|---|---|---|---|---|---|
| `contracts/` | 自身 | — | — | — | — | — | — | — | — |
| `game/` | ✓ | 自身 | — | — | — | — | — | — | — |
| `infra/` | ✓ | —（经注入接口） | 自身 | — | — | — | — | — | — |
| `hooks/` | ✓ | ✓ | ✓ | 自身 | — | — | — | — | — |
| `patch/` | ✓ | ✓ | ✓ | — | 自身 | — | — | — | — |
| `domains/` | ✓ | ✓ | ✓ | — | — | 自身（聚合域例外） | — | — | — |
| `features/<id>/` | ✓ | ✓ | ✓ | ✓（仅声明资源） | ✓（仅声明资源） | ✓ | —（零 include） | — | — |
| `products/api_export/` | ✓ | — | ✓ | — | — | ✓ | — | 自身 | — |
| `products/qol/` | ✓ | — | ✓ | — | — | ✓ | —（经能力注册表） | 自身 | — |
| `host/` | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | 自身 |

规则：
- `domains` **不依赖 hooks/patch**：域层不感知机制；资源声明归 `features` 或独立资源提供者。
- `products/*` **不依赖 features 实现**：经 `infra/capabilities` 注册表取接口（接口类型在 `contracts/`），因此不存在编译期 feature 依赖。
- `products/api_export` **不依赖 game**：所有游戏读取经 `domains`。
- `domains/system` 是唯一允许 include 其他 domain 头的聚合域；其余域禁止水平互调；跨域复用原语下沉 `game/`（读）或 `contracts/`（类型）。
- `infra → game` 禁止：帧时钟、主线程派发、变体信息由 `host` 以注入接口提供。
- `contracts` 只使用固定宽度 C 类型（§3.4），不使用 STL 容器、虚函数、异常。

### 2.3 各层职责与禁止

| 层 | 职责 | 禁止 |
|---|---|---|
| `host/` | 模块入口、JNI 薄层（按插件分组）、HTTP 服务、装配顺序、插件注册表 | 业务逻辑；直接读写游戏内存 |
| `products/api_export/` | 端点、参数校验、JSON 序列化、路由注册 | 调 JNI；直接读游戏；include feature 实现 |
| `products/qol/` | 游戏内 UI 面：设置页、面板、文案与布局装配 | 游戏语义计算 |
| `features/<id>/` | 一个增强功能：生命周期、规则、UI、对外能力与命令 | include 其他 feature；执行 hook/patch 机制；出现裸地址 |
| `domains/` | 语义域：`LiveApi`（主线程 live model）与 `ReadModel`（快照）两套接口 | 依赖 features/products；输出 JSON；依赖 hooks/patch；水平互调（聚合域除外） |
| `hooks/` | 资源登记与仲裁、安装/还原引擎、consumer 分派 | 业务语义 |
| `patch/` | 指令 patch、指针槽：占用声明、原字校验、回滚 | 业务语义 |
| `infra/` | 调度、命令队列、快照、事件、能力注册表、命令端口、日志、配置、存储、Result | 依赖 game；持有游戏语义 |
| `game/` | 变体、符号、偏移、ABI 签名、强类型读写/调用原语 | 依赖任何上层；构造 DTO；注入业务名 |
| `contracts/` | POD 接口、版本常量、命令类型、JNI 绑定元数据 | 任何实现、状态、STL 容器 |

### 2.4 目录布局（native）

```
module/app/src/main/cpp/
  host/        gateway.cpp / assembly.cpp / surface/http.cpp / plugin_registry.{h,cpp}
  products/
    api_export/ endpoints/*.cpp
    qol/        panels/*.cpp
  features/<id>/  plugin.cpp  rules.{h,cpp}  ui/*.cpp  api.cpp  ports.h
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

### 3.1 双轨模型

| 轨道 | 形态 | 何时用 | 成本 |
|---|---|---|---|
| **内置静态插件**（默认） | 编译进主 `.so`，以 `PluginDescriptor` 注册；CMake target 独立 | 全部内置功能 | 零 ABI 成本，编译期类型安全 |
| **外部动态插件**（P6 草案） | 签名容器 + `dlopen` + C ABI | 第三方扩展 | ABI 版本管理与同进程崩溃面 |

### 3.2 生命周期状态机（正式定义）

```
registered → resolving → { installed_disabled ⇄ enabled } → deactivating → inactive
                ↓                ↓
             blocked         failed（可显式重试）
                ↓
          unsupported / invalid（不可重试，除非条件改变/人工修正）
```

| 状态 | 含义 | 迁移 |
|---|---|---|
| `registered` | 已注册，未解析 | → resolving |
| `resolving` | 依赖/变体/清单校验中 | → installed_disabled / blocked / unsupported / invalid |
| `blocked` | 必需能力缺失（可补齐） | → resolving（显式重试或依赖提供者恢复） |
| `unsupported` | 变体或符号不支持（非错误） | 条件改变（换变体）后 → resolving |
| `invalid` | 清单不合法 / 契约 major 不兼容 / 签名校验失败 | 人工修正后 → resolving |
| `installed_disabled` | 资源已声明、开关关闭；消费者未注册、门禁关闭、无业务副作用 | ⇄ enabled；→ deactivating |
| `enabled` | 资源生效、消费者已注册 | → installed_disabled / deactivating |
| `deactivating` | 摘消费者、等待在途归零、还原资源 | → inactive / failed |
| `inactive` | 资源已还原、能力已注销 | → resolving（幂等重入） |
| `failed` | 安装或回滚失败（**可重试**，非 terminal） | → resolving（显式重试） |

语义定义：

1. **disable（enabled → installed_disabled）**：注销消费者、关闭门禁、清空能力暴露；允许保留**空转安装点**（wrapper 存在但立即透传原函数，fail-closed）。
2. **deactivate（→ deactivating → inactive）**：摘消费者 → 等待在途归零 → 还原资源（含物理 unhook，见 §4.2）→ 注销能力。**异步跨帧推进，禁止主线程阻塞等待**（§4.4）。
3. **「零残留」可验证定义**：零业务副作用 + 零消费者 + 零能力暴露 + 登记表占用为 `free` 或 `tombstone`（tombstone 是显式保留占用，见 §4.2）。**不要求** `dlclose`。
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
    const CapabilityProvision* provides;           // 提供的能力（id + 版本 + 接口指针）
    uint32_t resources_count;
    const ResourceClaim* resources;                // 资源声明（不执行）
    uint32_t jni_count;
    const JniBinding* jni;                 // {kotlin_method, native_symbol, signature_hash}
    int  (*install)(PluginContext&);       // 仅自身初始化
    void (*deactivate)(PluginContext&);    // 仅自身清理
    void (*on_enabled)(PluginContext&);    // 可选；禁止资源操作
    void (*on_disabled)(PluginContext&);   // 可选；禁止资源操作
    void (*fill_status)(PluginStatusOut&); // 宿主传入 out 参数填充
    void (*on_resource_ready)(PluginContext&, const char* resource_id, void* backup); // 可选；资源安装成功后宿主通知（backup = 原函数入口）
};
```

规则：
- **数量字段显式**（`*_count`），不允许哨兵终止。
- **存活期**：描述符内的字符串、数组、函数指针均为**静态存储**，生命周期 ≥ 进程；宿主注册时可直接引用。
- **状态所有权**：`fill_status` 用宿主传入的 out 参数填充，避免跨边界所有权问题。
- **资源与能力的启停由宿主通用执行**（声明式）；`install/deactivate` 只做插件自身初始化与清理；`on_enabled/on_disabled` 不得申请/释放资源。
- **backup 交还**：宿主在资源安装成功后经 `on_resource_ready` 把原函数入口交还所有者；插件不得自行安装资源。
- 拓扑排序输入 = `requires` + `provides`；缺 `requires` → `blocked`；缺 `optional_requires` → `degraded`。

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
2. `optional_requires` 的消费者收到 `degraded` 事件，状态置 `degraded`。
3. `requires` 的消费者按**逆拓扑序**（深度最深者先）disable，状态置 `cascade_stopped`。
4. 帧回调等待在途归零；超过帧数预算 → provider `failed`（隔离），消费者保持 `degraded`。
5. 注销 capability。
6. provider 恢复后**不自动重启** `cascade_stopped` 消费者，需显式 enable（避免抖动）。

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

### 3.5 配置、日志、状态

- 配置：每插件命名空间 `plugin.<id>.*`；读写经 `infra/config`；变更经事件广播。
- 日志：统一经 `infra/log` 单写者，domain = 插件 id；热路径使用环形缓冲或节流。
- 状态：`{id, source, enabled, state, error, resources[], requires[], provides[], variant, degraded}`（§9.1）。

### 3.6 数据级插件（声明式规则）

- 外部 JSON/表定义规则/配方/阈值/词表；宿主按固定词汇表 + schema 校验；非法项拒收并上报。
- 只用于非热路径查表；热路径不做解释执行；数据文件默认关闭、可禁用；来源与哈希记录在状态中。

### 3.7 外部插件轨（**未冻结设计草案**）

**容器**：签名 APK（zip）：`plugin.json` + `lib/arm64-v8a/lib<id>.so` + 可选 `assets/`。清单不再包含 `signature` 字段（签名由容器承担），可选 `contentHash`。

**目录模型（版本化 + 原子切换）**：

```
plugins/
  staging/<id>-<hash>/          # 导入区（校验前）
  active/<id>/<version>/        # 已校验版本（0444，目录 app 私有）
  active/<id>/current           # 指针文件，内容为版本号（原子 rename 更新）
  cache/                        # 可选解压缓存
```

**导入流程**：staging → 校验（证书白名单 + `apiVersion` + `supportedVariants` + 可选 `contentHash`）→ 写入 `active/<id>/<version>/` → 原子更新 `current` → 记录导入日志（来源/哈希/签名/时间）。

**更新与回滚语义（重要）**：

- **更新只在下次进程启动生效**：当前进程已加载的映射不会因磁盘替换而改变；本进程只允许 `disable`。
- 当前进程**禁止加载同一插件的新版本**；回滚同样是「改 `current` → 重启后生效」。
- 保留最近 N 个版本（配置），旧版本可清理。

**安全模式**：启动配置可禁用全部外部插件；外部插件默认不加载。开放前不冻结 ABI、不发布 SDK。

### 3.8 外部 Host API（**未冻结设计草案**）

**默认档 `read-only`（只读能力表）**：

```
log | sym.resolve + 常量表 | mem.read | data(静态表) | frame(主线程投递 + 只读快照)
resources.declare（声明，不执行） | commands.post | config(命名空间) | status
```

- **不暴露** `mem.write`、裸游戏函数指针表、`patch/slot` 执行入口。
- 需要写操作 → 声明资源（宿主执行安装）+ 命令端口提交命令；游戏函数调用一律由宿主命令实现。
- 高权限档 `trusted-write`（P6 再议，未冻结）：仅对签名证书属于「官方白名单」的插件授予；该档位下写侧不再受控，文档必须显式说明风险。

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
    void*       handler;       // 所有者提供的替换入口（入口 hook/patch 的 wrapper）；共享资源为 nullptr
};
```

`handler` 是声明式安装的执行体：宿主按 `mechanism` 选择机制引擎并把 `handler` 交给它，不解释其语义（§3.3「资源与能力的启停由宿主通用执行」）。

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
| 权限类型（类型系统强制） | `ObserveHandle`（只读，返回副本）/ `ModifyHandle`（可改参数与返回值）/ `InterceptHandle`（唯一，可短路）；不使用权限枚举 |
| 短路唯一性 | **同一资源最多一个 `InterceptHandle`**；第二个申请者在安装期失败（`resource already has interceptor`） |
| 阶段 | `before`（顺序执行，参数修改累积）→ `original`（若未被短路）→ `after`（**逆序**执行，返回值覆盖，逆序对称） |
| 短路 | 仅 Intercept 可短路；短路后 `after` 仍执行（可观察，不可改） |
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

HTTP 取实时数据的路径：优先读 `ReadModel`；必须实时时 `POST` 返回 **202 + 票据**，客户端轮询票据或 `GET /api/system/tickets/{id}`；可选同步等待上限（默认 200ms，可配置），超时返回票据。

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
- 撤销按 §3.4.1 事务执行。
- 命令端口见 §3.4.3；票据状态可查询（HTTP 暴露见 §9.1）。

### 6.5 日志 / 配置 / 存储 / Result

- 日志：单写者，`<ts> f=<frame> <L> <domain> <src> <msg>`，domain = 插件 id。
- 配置：外部 config.json 唯一来源；插件命名空间隔离；修改即持久化并广播；迁移期允许一次性导入旧键（映射表登记）。
- 存储：插件 sidecar 按存档槽隔离；写入必须走事务（prepare/commit/abort）。
- Result：见 §3.4.2。

---

## 7. products/ 与 host/

### 7.1 产品与依赖

| 产品 | 依赖 | 说明 |
|---|---|---|
| `products/api_export` | `domains`、`infra`、`contracts` | HTTP/JSON 端点面；不依赖 game / features |
| `products/qol` | `domains`、`infra`、`contracts`（经注册表取能力接口） | 游戏内 UI 面 |

两者互不依赖；共享部分下沉 `domains`/`infra`。

### 7.2 host 装配与 JNI

- JNI 薄层按插件分组命名：`native<Feature><Action>`；绑定元数据来自 `PluginDescriptor.jni`（`contracts/` 定义 `JniBinding`）。
- `check_jni.py` 校验三向一致：Kotlin `external fun` ↔ native 导出符号 ↔ 描述符 `jni` 表。
- 装配顺序：`game` 解析 → `infra` 启动 → `hooks/patch` 引擎就绪 → `features` 拓扑安装 → `products` 起 HTTP/UI → 状态上报。
- **生命周期执行器**：唯一执行器为主线程，事件驱动（命令队列 + 帧回调）；初始化线程只投递；所有迁移幂等串行，带帧数预算与回滚。

### 7.3 Kotlin 侧镜像

```
com.inotia4.qol/
  host/       入口、ApiServer、JNI 桥
  platform/   配置、日志、静态数据、存储、hook 安装（Java 侧）
  products/   api_export 端点面、qol 界面面
  features/<id>/  与 native 插件同 id：service、bridge、UI
  services/   服务契约与实现（可多通道复用）
```

规则：controller 只路由 + 参数校验；JNI 只在 `host`/`platform`/feature bridge 出现；feature 之间通过服务契约或配置事件交互，不互相 import。

---

## 8. 兼容与状态判定矩阵

| 维度 | 取值 | 判定 |
|---|---|---|
| variant | 原版 / 大修 / monster / unknown | `unknown` → 依赖具体符号的插件 `unsupported` |
| plugin | `supportedVariants` 声明 | 不命中 → `blocked`（可配置强制忽略，风险自担） |
| capability | major 相等 + minor 满足 | 不满足 → `blocked`；optional 缺失 → `degraded` |
| resource 声明 | 符号存在 + 变体能力位 + 原字节校验 | 任一失败 → 按 `requirement` 判定（下表） |

**资源失败 → 插件状态映射**

| 资源 `requirement` | 安装失败 | 运行期消费者错误（可恢复） | 回滚失败 |
|---|---|---|---|
| `kRequired` + `kFailClosed` | `failed`（回滚全部） | 资源降级透传，插件 `degraded` | `tombstone` + `failed`（隔离） |
| `kRequired` + `kFailSoft` | `degraded`（功能受限，记录） | 记录并继续，插件保持 `enabled` | `tombstone` + `failed`（隔离） |
| `kOptional` + 任一 | `degraded`（记录，不影响其它） | 记录并继续 | `tombstone` + `degraded` |

---

## 9. 可观测、验证与脚本边界

### 9.1 状态 API

P1 最小诊断接口（必须）：

| 端点 | 响应 | 说明 |
|---|---|---|
| `GET /api/system/features` | `{"ok":true,"features":[{id, version, state, enabled, error, resource_count}]}` | 状态登记表导出；`state` 为英文枚举名 |
| `GET /api/health`（既有端点，**加法扩展**） | `{"ok":true,"ready":true,"variant":{"series","version","known","capabilities"}}` | `ok` 保持不变；`ready` 与 503 语义一致；`variant` 取自 `game/` 变体表 |

P2 起扩展：`features` 追加 `resources[]/requires[]/provides[]/degraded`；新增 `GET /api/system/resources`（资源占用状态、消费者、权限类型、失败模式）与 `GET /api/system/tickets/{id}`（命令票据状态）。

`safeMode` 移到 P6（外部插件轨）：P1–P5 不存在可禁用的外部插件，恒定 `false` 无诊断价值。

### 9.2 校验脚本族与能力边界

| 脚本 | 检查方式 | 能保证 | 不能保证 |
|---|---|---|---|
| `check_layers.py` | include 图 + CMake target 链接矩阵（+ 可选产物符号扫描） | 目录级依赖方向、无环（源码与链接两层） | 运行期回调链、数据流 |
| `check_resources.py` | 扫描资源声明/注册（hook / call patch / ptr slot / ownership 四类） | 一资源一安装者、未登记资源、字段完整 | 运行期是否安装成功、是否归还 |
| `check_offsets.py` | 禁止非 `game/` include symbols 头（结构）+ 十六进制字面量与 `g_base +` 算术扫描 | 明显裸地址与符号头越权 | 宏展开生成、间接计算地址 |
| `check_jni.py` | Kotlin external ↔ native 导出 ↔ 描述符 `jni` 三向比对 | 命名与数量一致 | 签名/ABI 兼容性本身 |
| `check_symbols.py` | 常量对 `.so` 校验 | 符号与版本变化可发现 | 结构偏移语义正确性 |
| `check_plugins.py` | 注册表完整性 + `requires/provides` DAG（编译期常量） | 依赖无环、清单与注册一致 | 能力运行期行为正确性 |

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
| P0 | 签署本设计 + **校验脚本族落地 + 基线棘轮冻结**（check_layers / check_offsets / check_resources / check_jni） | 评审通过无阻断项；脚本可运行；基线快照已冻结，新增违规即失败 | — |
| P1 | 最小骨架：`contracts/` + `infra/`（命令队列、快照、事件、能力、命令端口）+ `game/` 最小面 + **最小诊断接口** | 骨架可编译、host 单测通过、无行为改动、`/api/system/features` 最小字段可用 | P0 |
| P2 | **纵向样板插件**（覆盖入口 hook + 配置 + 状态 + 命令）：只验 `install → enabled` 路径 | 样板真机验收卡通过；状态 API 可见 | P1 |
| P3 | 最小资源事务（异步步进、回滚、在途归零、帧预算）+ `disable/deactivate/tombstone` 全路径 | 故障注入与并发测试通过；接口冻结（内部） | P2 |
| P4 | 批量迁移其余插件（内置静态轨），每插件一个提交 | 每插件：单测 + 验收卡；临时豁免逐条清零 | P3 |
| P5 | `products/` + `host/` 收口：JNI 分组、端点归位、状态 API 完整化；收紧脚本；删除旧路径 | `check_*` 全绿、豁免清零、双轨消失 | P4 |
| P6（可选） | 外部插件轨（未冻结草案）：容器校验、导入、`dlopen`、白名单、安全模式、更新/回滚语义 | 导入/拒绝/禁用/崩溃兜底真机通过 | P5 |

单插件迁移动作：建目录与元数据 → 资源改为声明 → 依赖改能力/命令 → 开关与状态接入 → 单测 + 验收卡。

冻结与兼容：JNI 导出在重构期保持名称与签名稳定；新增按插件分组命名，P5 一次性切换。数据格式（sidecar / `.qol_save`）向前兼容，必要时升版本并写迁移。

回滚：每阶段出口不达标则回滚该阶段改动，不动其它插件。

---

## 12. 风险、不做与未决项

### 12.1 风险与缓解

| 风险 | 缓解 |
|---|---|
| 接入期两套路径并存 | 纵向样板先行；阶段出口脚本强制收敛；豁免逐步清零 |
| 资源数量失控 | 登记强制填写选择理由与验收卡；`check_resources` 统计趋势 |
| 同进程崩溃无法隔离 | 外部默认关闭 + 白名单 + 安全模式；内置插件随宿主编译 |
| 真机验证成本 | 每插件独立提交 + 验收卡；host fake 覆盖失败路径 |
| ABI 成本被低估 | 开放前不冻结、不承诺；冻结时一次性设计版本协商与签名 |
| 主线程被长操作占用 | 全部资源操作异步帧步进；禁止阻塞等待 |
| 文档再次腐烂 | 结构由脚本生成/校验；权威层级明确（§9.4） |

### 12.2 明确不做（YAGNI）

- 不做沙箱/权限隔离（外部轨默认只读档 + 白名单）。
- 不为内置插件冻结 ABI、不发布 SDK（P6 再议）。
- 不引入脚本解释器执行任意逻辑；只做声明式数据规则。
- 不做插件市场、在线下载、自动更新；更新仅重启生效。
- 不追求 feature 之间零依赖（能力/命令/事件已足够）。
- 不引入逐功能接口样板：统一能力注册表 + 资源登记。

### 12.3 未决项（实现前必须闭合）

| # | 未决项 | 归属 |
|---|---|---|
| U1 | 非功能指标阈值回填（§10） | P1/P2 真机测量 |
| U2 | 共享资源清单与所有者划分 | P2/P3 |
| U3 | 外部插件签名校验可用性（`signingInfo` 空回退） | P6 真机探针 |
| U4 | 快照类型清单与发布方式 | P1 |
| U5 | 命令取消语义清单 | P2/P3 |
| U6 | `unhook` 后同地址重新挂载的安全性 | P3 真机探针 |
| U7 | `trusted-write` 高权限档的证书策略 | P6 |
| U8 | `tombstone` 回收策略与人工清除流程 | P3 |
| U9 | 需要 `atomic_before` 两阶段分派的资源清单 | P3/P4 |

---

## 附录 A：资源登记表模板

| id | symbol | mechanism | policy | requirement | failure | resumable | owner | consumers | gate | 选择理由 | 验收卡 |
|---|---|---|---|---|---|---|---|---|---|---|---|
| `hook:CHAR_AddDamage` | `F_CHAR_ADD_DAMAGE_VMA` | entry hook | private | required | fail-closed | true | `simple_mode` | — | `simpleModeEnabled` | 无函数指针槽，且需要入口前后处理 | VM-x |
| `hook:MEMORYTEXT_GetText` | `F_MEMORYTEXT_GET_TEXT_VMA` | entry hook | shared | required | fail-soft | true | `text` | charinfo 等（登记） | — | 全局文本唯一热点，多消费者必须集中分派 | VM-x |
| `hook:SAVE_SaveInventory` | `F_SAVE_SAVE_INVENTORY_VMA` | entry hook | private | required | fail-closed | true | `extension_bag` | — | `extensionBagEnabled` | 调用点会被第三方 mod 改写，函数入口更稳定 | VM-x |

## 附录 B：插件容器与清单（外部轨草案）

容器：签名 APK（`plugin.json` + `lib/<abi>/lib<id>.so` + 可选 `assets/`）。
清单字段：`id / version / apiVersion / supportedVariants / requires[] / optionalRequires[] / provides[] / resources[] / entry / contentHash?`（签名由容器承担）。
目录：`plugins/staging/<id>-<hash>/` → `plugins/active/<id>/<version>/` + `current` 指针；更新与回滚仅在下次进程启动生效。
