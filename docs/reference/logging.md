# 日志与崩溃取证（当前权威）

本文是模块日志与崩溃取证文件的**当前权威**：行格式、级别与域、文件清单与路径、保留/轮转策略、
`qol-crash.log` 与 `java-crash.log` 的格式契约、线程模型与符号化工具用法。
`docs/archive/development/logging.md` 是历史归档材料，不作为当前规则依据。

实现位置：`module/app/src/main/cpp/infra/log/qol_log.{h,cpp}`、
`module/app/src/main/cpp/infra/log/qol_crash.{h,cpp}`、
`module/app/src/main/java/com/inotia4/qol/JavaCrashReporter.kt`、
`module/app/src/main/java/com/inotia4/qol/RuntimeEnvironment.kt`。

## 1. 行格式

统一单行文本，native 与 Kotlin 共用同一格式化函数（`qol_log_format`）：

```text
<yyyy-MM-dd HH:mm:ss.SSS> f=<frame> <D|I|W|E> <domain> <src:line> <message>
```

- `f=<frame>`：帧号未就绪（启动早期、帧源安装前）输出 `f=-1`。
- `src:line`：`__FILE__` 压成 basename + 行号；Kotlin 转发路径同样压 basename，不落构建机路径。
- `message` 内的换行转义为字面 `\n`（Debug 级保留原样，仅 debug 视图用）；`\r` 丢弃。
- 单条消息缓冲 4096 字节，溢出截断并补 `...`。

native 出口为 `QOL_LOG_DEBUG` / `QOL_LOG_INFO` / `QOL_LOG_WARN` / `QOL_LOG_ERROR`（含
`QOL_LOG_DEBUG_EVERY` / `QOL_LOG_DEBUG_ON_CHANGE` 限流变体，`infra/log/qol_log.h`）。
Kotlin 经 `LogFile` facade 转发 `qol_log_write_kotlin`，D 级门控与 native 一致
（debug 未开启时格式化前短路）。

## 2. 级别与限流

| 级别 | 行为 |
|---|---|
| D | 仅 debug 开关开启时落盘/上 logcat |
| I | **每帧每域最多 16 行**（`kQolLogInfoPerFrameCap`）；超限时先写一行封顶标记 `log INFO cap reached domain=... frame=... limit=16`（保留越限调用点的 src），随后同帧同域 INFO 丢弃 |
| W / E | 不限流 |

帧号未就绪阶段 INFO 不限流（该阶段日志量由初始化次数决定）。

## 3. 域（domain）

域 token 由 `QolDomain` 枚举到文本的唯一映射表（`qol_log.cpp` `kDomainTable`）定义，当前 18 个：
`platform`、`core`、`http`、`api`、`op`、`inventory`、`extension_bag`、`save`、`save_backup`、
`autosell`、`craft`、`attr_range`、`ui`、`config`、`catalog`、`custom_recipe`、`world_tuning`、
`special_equip`。token 文本是唯一契约，新增域必须进该表。

## 4. 文件清单与路径

目录：`/sdcard/Android/data/<包名>/files/qol/`（包名取 `/proc/self/cmdline`，缺省回退原版包名）。

| 文件 | 内容 |
|---|---|
| `qol.log` | 主日志，文件首行 `=== Inotia4Qol log start <ts> ===` |
| `qol.log.1` | 主日志写满 4 MiB（`kQolLogFileCapBytes`）后轮转出的旧文件，**只保留一份**，再满即被覆盖 |
| `qol-crash.log` | native 崩溃现场（SIGSEGV/SIGBUS/SIGILL/SIGFPE），每次进程启动截断重建 |
| `java-crash.log` | Java 未捕获异常现场（完整栈 + 全线程转储） |
| `qol.log.prev` / `qol.log.prev.1` | 上一会话的 qol.log 与其轮转件 |
| `qol-crash.log.prev` / `java-crash.log.prev` | 上一会话的崩溃现场 |

logcat：同一行文本经 `__android_log_print` 以 tag `Inotia4Qol` 输出（`adb logcat -s Inotia4Qol:V`）。
logcat 与文件是同一行文本的两个输出端，**logcat 内容不写入文件**。

## 5. 保留 / 轮转策略

- **启动前 rename**（保留上一会话取证，不做时间戳轮转）：
  - `qol.log.1` → `qol.log.prev.1`，随后 `qol.log` → `qol.log.prev`。顺序固定：先挪轮转件再挪主文件，
    若先挪主文件，旧 `.1` 会在下次轮转被覆盖、失去 prev.1 语义（`qol_log.cpp` 初始化段）。
  - `qol-crash.log` → `qol-crash.log.prev`（`qol_crash_init`），随后打开新文件截断写。
  - `java-crash.log` → `java-crash.log.prev`（`JavaCrashReporter.dump` 写入前）。
- 所有 rename 失败（含 ENOENT）一律忽略，不阻断日志/崩溃初始化。
- 运行期只有主日志按 4 MiB 轮转；崩溃文件靠启动 rename 保留上一份现场。

## 6. 设备身份与 config 指纹

`RuntimeEnvironment.initialize(context, gameVersionName)`（`ModuleRuntimeBootstrap` 取得 context 后
调用一次，可幂等重入）统一构造设备/软件信息，两个用途共用同一批字段：

1. **单行 INFO**（platform 域）：`runtime env: device=... fingerprint=... android=... abis=... bits=... screen=... module=... game=... variant=...`
2. **crash 文件 identity 段**：经 JNI 下发 native（`qol_crash_set_identity` / `qol_crash_set_config`），每行自带 `ident.` 前缀：`ident.device` / `ident.fingerprint` / `ident.android` / `ident.screen` / `ident.proc` / `ident.module` / `ident.game`。

config 指纹（`ident.config=` 与 java-crash.log header 的 `config=` 同源）：

```text
fp=<config.json SHA-256 前 12 个 hex 字符> enabled=<启用 section 总数> list=<排序后前 32 个启用 section，`,` 替换为 `;`>
```

config.json 缺失或读取/解析失败退化为 `fp=none enabled=0 list=`。

## 7. `qol-crash.log` 格式契约

```text
=== qol crash ===
crash_at=<ts>
pid=<pid> tid=<tid> tid_name=<name> frame=<frame|-1>
signal=<num> <SIGNAME> code=<n> (<CODE>) fault=0x...
ident.device=...                      ← identity 行（signal= 之后、寄存器之前）
ident.fingerprint=...
ident.android=... abis=... bits=...
ident.screen=...
ident.proc=... pid=...
ident.module=...
ident.game=... variant=...
ident.config=fp=... enabled=N list=...
fault_address=0x...                   ← 寄存器（arm64：fault_address/pc/sp/pstate/x0..x30）
pc=0x... sp=0x... pstate=0x...
x0=0x... ... x30=0x...
bt0=0x...                             ← FP 链回溯（寄存器之后、--- maps --- 之前）
bt1=0x...
...
--- maps ---
/proc/self/maps 原文
--- log tail ---
qol.log 尾部 4 KB 原文
=== end ===
```

**四段顺序契约（禁止调整）**：header / regs / `--- maps ---` / `--- log tail ---`。
段内扩展行只有两类，均为 `key=value` 形式：

- `ident.*` identity/config 行：位于 header 段内 `signal=` 之后、寄存器之前；identity 文本由
  Kotlin 生成并自带前缀，native 原样整段写出（§6）。
- `btN` FP 链回溯行：位于寄存器之后、maps 之前。arm64 从 `x29`（FP）起链，`fp[0]` 是上一帧 FP、
  `fp[1]` 是本帧返回地址；经 `process_vm_readv` 读 `[fp, fp+16)`，坏 FP/非递减链即截断，
  最多 64 帧（`kBacktraceMaxFrames`）。回溯失败只截断 `btN` 行，不影响后续段；
  非 arm64 写 `backtrace=unavailable`。

identity/config 缓冲用 seqlock 防撕裂：读到写入中途输出 `ident.unstable=1`；未下发时输出
`ident.device=(not set)` / `ident.config=(not set)`。不输出半截内容。

其他语义不变：崩溃处理器只用异步信号安全接口（预开 fd + 栈上缓冲，不 malloc/printf）；时间格式化
在 init 阶段预热；写完后恢复原处理器并重新抛出信号，系统 tombstone 与游戏自身处理器照常记录；
`g_capturing` 原子门禁防递归崩溃/多线程同时崩；不接管 `SIGABRT`。

## 8. `java-crash.log` 格式

Java 未捕获异常（`JavaCrashReporter.install` 在 `onModuleLoaded` 阶段安装，幂等）写入独立文件，
四块顺序固定：

```text
ts=<ts> pid=<pid> proc=<进程名> thread=<线程名> module=<applicationId> <versionName>(<versionCode>) config=<config 指纹行>
ident.device=...                      ← RuntimeEnvironment.identityBlock() 原文（多行）
--- throwable ---
<printStackTrace 全文，含 cause 链>
--- threads ---
=== <线程名> id=<tid> state=<state> daemon=<bool> ===
<帧 0>
<帧 1>                                ← 每线程最多 64 帧
...
```

上限：总字节 **256 KiB**（硬上限，预算预留截断标记长度；任一段写不下时追加
`...(truncated threads=<已写线程数>)` 并停止）、**128 线程**、每线程 **64 帧**。
写前把旧文件 rename 为 `java-crash.log.prev`。

handler 语义：安装时保存前序 uncaught handler，取证后**始终转发**给前序 handler，保持宿主既有
崩溃处理语义；无前序 handler 时按 ART 惯例自行终止（`Process.killProcess` + `exitProcess(2)`）。
取证失败不阻断转发。单行摘要同时进统一日志（`LogFile.error`，platform 域）。

## 9. 线程模型

| 路径 | 写入方式 |
|---|---|
| 普通日志 | 调用线程**同步**格式化并写文件：`line + '\n'` 在同一 `g_sink_mtx` 临界区内落盘（单写者、行不打散）；同一行文本同步上 logcat。`g_in_emit` thread_local 门禁防格式化/后端重入 |
| 崩溃信号内 | 信号处理器**同步直写** `qol-crash.log`：只用异步信号安全原语（open/read/write/lseek/close/rename/prctl/process_vm_readv + 栈上缓冲），预开 fd + `sigaltstack` 备用栈 |
| `java-crash.log` | uncaught handler 线程内同步写（带界写入器） |

**不引入统一日志线程**，原因：

1. 崩溃处理器只允许 async-signal-safe 调用；任何队列/后台消费线程都会把「崩溃现场落盘」依赖到
   另一个可能已先死的线程，违背取证优先原则。
2. 同步写保证 `--- log tail ---` 复制的正是崩溃前最后几行的真实顺序，「崩溃文件与日志同一时间线」
   的对齐语义成立。
3. 文件写入频率受级别限流约束（逐帧无高级别、INFO 每帧每域 16 行封顶），同步写的代价可控，
   无需后台线程摊平。

## 10. `symbolize_crash.py` 用法

```bash
adb -s <设备序列号> pull /sdcard/Android/data/<包名>/files/qol/qol-crash.log <任务目录>/
uv run python scripts/analysis/symbolize_crash.py <任务目录>/qol-crash.log --obj-dir module/app/build
```

- 解析器按**段结构**工作：`signal=` 正则、`key=value` 行收进 header/寄存器字典、`--- maps ---` 后按
  maps 行格式解析。`ident.*` 与 `btN` 都是段内 `key=value` 行，天然落入既有解析路径，
  **脚本对本次扩展零改动**。
- 基址由 maps 反推：同一文件所有映射 `(start - offset)` 的最小值即 load bias，换包重装后仍能解析。
- `pc`/`lr`/`fp` 优先符号化；`btN` 按寄存器同一路径做 maps 解析。找到 NDK `llvm-symbolizer` 时附函数名，
  否则只输出 `模块+偏移`。
- 只读输入、结果打 stdout，不写文件。
