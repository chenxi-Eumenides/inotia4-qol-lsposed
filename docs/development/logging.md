# 统一日志系统规范

> 日期：2026-09-14 ｜ 状态：✅ 现行（统一日志系统 P4） ｜ **本文件是日志规范的唯一权威来源**
> 其他文档（architecture/api-reference）中的日志描述应引用本文件，不再重复维护。

## 1. 目的与范围

本文件规定模块 native（C++）与 Kotlin 两侧日志的级别语义、domain 词表、行格式、落盘通道、
帧号、逐帧节流、变量入日志要求与配置接线，并提供可执行的合规检查脚本。

**权威性**：任何新增或修改日志调用点，其级别、domain、格式必须符合本文件；本文件与其他
文档/注释冲突时以本文件为准。代码实现为唯一出口时，本文件描述的是**目标契约**；实际迁移进度
由 §10 的 `check_log_policy.py` 判定。

**不在范围**：HTTP API 的响应契约见 `docs/reference/api-reference.md`；日志不作为 API 契约的一部分。

**实现位置**：

| 组件 | 文件 | 职责 |
|---|---|---|
| native 单写者 | `module/app/src/main/cpp/core/native/qol_log.{h,cpp}` | 格式化、logcat + 文件发射、调试门控、节流原语 |
| JNI 薄层 | `module/app/src/main/cpp/bridge/native/gamebridge_log.cpp` | 仅参数转换（`nativeQolLogInit` / `nativeQolLogWrite` / `nativeQolLogSetDebugEnabled`） |
| Kotlin 门面 | `module/app/src/main/java/com/inotia4/qol/LogFile.kt` | 经 JNI 转发 native；不再自己持有文件句柄 |

## 2. 级别定义与语义

四级日志，语义与终态处理固定：

| 级别 | 字符 | 语义 | 运行时开关 | 落盘 |
|---|---|---|---|---|
| debug | `D` | 开发/诊断细节，逐帧或高频可接受（须节流） | **仅 debug 有开关**，默认关闭 | 开启后写入 |
| info | `I` | 正常流程里程碑（初始化、状态转换、操作完成） | 常开 | 写入 |
| warn | `W` | 可恢复异常/降级/门禁拒绝 | 常开 | 写入 |
| error | `E` | 失败/崩溃前错误，需要定位 | 常开 | 写入 |

**debug 开关**：

- 默认 `false`（关闭）。关闭时 debug 宏不产生任何 logcat 与文件输出。
- 打开方式（三者等价，读取同一配置）：
  1. 外部 `config.json` 写入 `"debugLogEnabled": true`；
  2. `POST /api/config/set {"debugLogEnabled": true}`；
  3. 游戏内 **模块设置页第 8 行「调试日志」** 开关。
- info/warn/error **无开关**，任何配置下都输出。禁止给它们加运行时开关。

## 3. domain 受控词表

domain 是**受控词表**（closed vocabulary），共 15 项。`<domain>` 字段只能取下列 token：

| # | token | 覆盖范围 |
|---|---|---|
| 1 | `platform` | 注入、进程/生命周期、Native Hook、call_patch、状态转换派发、帧宿主等平台能力 |
| 2 | `core` | 共享原语：JSON/缓存/寻路/帧任务/ops_common/save_enter/save_exit/module_save |
| 3 | `http` | HTTP 服务、路由绑定、拦截器、控制器守卫 |
| 4 | `api` | 原版信息与操作导出的 Kotlin service/controller（含 native api 域） |
| 5 | `op` | 越权操作入口（OpApiService / OpController） |
| 6 | `inventory` | 背包读写与库存集成（含 native_inventory_hook、inventory_trade） |
| 7 | `extension_bag` | 扩展背包运行时、投影、拖拽、持久化 |
| 8 | `save` | 存档生命周期：进入/退出/保存/槽位 |
| 9 | `save_backup` | `.qol_save` 导出/导入/列表/删除 |
| 10 | `autosell` | 自动出售扫描与配置 |
| 11 | `craft` | 新合成系统（合成器界面层 craft_ui + 配方层 custom_recipe） |
| 12 | `attr_range` | 属性显示范围着色 |
| 13 | `ui` | 模块 UI：设置页、存档管理面板、经验/自定义 UI |
| 14 | `config` | 模块配置读写与下发 |
| 15 | `catalog` | 静态表/瓦片等目录数据 |

**新增 domain 流程**（缺一不可）：

1. 在 `qol_log.h` 的 `QolDomain` 枚举尾部追加一项；
2. 在 `qol_log.cpp` 的 `kDomainTable` 追加 `{QolDomain::kXxx, "token"}`（token 小写 snake_case）；
3. 在 Kotlin `LogDomain` 增加同名 token；
4. 在本文件 §3 表补一行（token + 覆盖范围）；
5. 运行 `uv run python scripts/verification/check_log_policy.py`，R3 必须通过。

## 4. 行格式语法与逐字段定义

**唯一格式**（字段间单空格，禁止多空格/制表符）：

```text
<ts> f=<frame> <L> <domain> <src> <msg>
```

| 字段 | 定义 |
|---|---|
| `<ts>` | `yyyy-MM-dd HH:mm:ss.SSS`，**本地时区**；由 `qol_log_format_ts()` 生成 |
| `f=<frame>` | `data_frame_count()` 帧号；未就绪或未知为 `f=-1` |
| `<L>` | 级别字符：`D` / `I` / `W` / `E` |
| `<domain>` | §3 的 15 个 token 之一 |
| `<src>` | native 为 `basename:line`（如 `qol_log.cpp:42`）；Kotlin 为 `File.kt:line`（如 `LogFile.kt:12`） |
| `<msg>` | 正文；非 debug 一律单行，debug 允许多行 |

**示例**：

```text
2026-09-14 10:03:21.417 f=10231 I save enter_slot:118 slot=0 stage=load_data
2026-09-14 10:03:21.509 f=10237 W extension_bag extension_bag_persistence.cpp:81 reason=slot busy slot=3
2026-09-14 10:03:22.001 f=-1 E http ApiException.kt:44 endpoint=/api/system/save code=500
```

**`f=-1` 语义**：帧提供者未安装、或提供者返回负值、或调用发生在帧未就绪阶段（注入早期、
bridge-init、部分线程回调）时为 `-1`。`f=-1` 不是错误，表示"当前不在有效的游戏帧内"。

**多行规则与强制方式**：

- **非 debug（I/W/E）一律单行**：`qol_log_format()` 在 `level != kDebug` 时把正文中的换行
  转义为字面 `\n`、丢弃 `\r`。调用方无需自行转义，也不得依赖多行。
- **仅 debug 允许多行**：writer 保留真实换行。多行 debug 的**首行必须给结论**，续行给明细（§8）。

## 5. 文件通道与 logcat

**文件通道（native 唯一写者）**：

- 路径：`/sdcard/Android/data/<游戏包名>/files/inotia4-export.log`。
- 启动**截断模式**：`qol_log_init()` 以 `O_TRUNC` 打开，每次进程启动清空旧日志并写起始横幅。
- 单写者：只有 `qol_log.cpp` 持有文件 fd；`gamebridge_log.cpp` 与 Kotlin 侧均不得自行打开
  或写入 `inotia4-export.log`。Kotlin `LogFile` 只经 `nativeQolLogWrite` 转发。
- 早期 backlog：`LogFile.onNativeReady()` 之前日志进有界队列（512，logcat 兜底）；native 就绪后
  调 `nativeQolLogInit()` 并重放 backlog，之后直写。此阶段不产生第二写者。
- 写入在 `g_sink_mtx` 临界区内完成（行 + `\n` 原子落盘）。
- `qol_log_init()` 幂等、任意线程可调；`qol_log_file_ready()` 查询 sink 是否就绪。

**logcat**：

- **单一 tag：`Inotia4Qol`**。全仓库收敛到该 tag，`__android_log_print` 只能出现在
  `qol_log.cpp`（唯一出口）与 host 测试桩中。
- 旧 tag（`Inotia4Export`/`Inotia4VirtBag`/`Inotia4Move` 等）已废弃，见 §11 映射表。

## 6. 帧号来源与 `f=` 取值时机

- 帧号来源固定为 `data_frame_count()`，经 `qol_log_set_frame_provider()` 注册，
  `qol_log_current_frame()` 读取。
- **取值时机**：每条日志构造行时（`emit_line` 内）实时取一次并写入 `f=`；不缓存、不跨行复用。
- **线程安全**：provider 指针用 `std::atomic` 存取；`data_frame_count()` 本身可被任意线程安全调用，
  因此从 HTTP 线程、预取线程、hook 回调打日志都能取到当时帧号。
- **返回值语义**：provider 未注册 → `-1`；provider 返回 `< 0` → 归一为 `-1`；其余按原值输出。

## 7. 逐帧政策与节流原语

**政策**：

- debug 允许逐帧，但**必须节流**：使用 `QOL_LOG_DEBUG_EVERY`（按帧间隔）或
  `QOL_LOG_DEBUG_ON_CHANGE`（值变化才打印）。禁止无节流的逐帧 `QOL_LOG_DEBUG`。
- **info/warn/error 不得逐帧**：不得出现在 `*_tick` 任务、`frame_task_add` 回调、`*_process`
  绘制回调等每帧路径中（软规则 R4）。

**节流原语**（`qol_log.h`）：

| 原语 | 用途 | 语义 |
|---|---|---|
| `QOL_LOG_DEBUG_EVERY(dom, every_frames, ...)` | 按帧间隔打印 | 首次放行；帧号可用时按 `frame - last_frame >= every_frames`；无帧 provider 时退化为 1s 时间窗 |
| `QOL_LOG_DEBUG_ON_CHANGE(dom, gate, value, ...)` | 值变化才打印 | `QolLogChangeGate::changed(value)` 与上次不同则放行 |
| `QolLogThrottle::due(every_frames)` | 上述宏的底层 | 见 `every` 语义 |
| `QolLogChangeGate::changed(value)` | 状态门 | 首次必放行 |

使用规范：周期性状态用 `EVERY`（如每 60 帧）；离散状态机/计数器用 `ON_CHANGE`；两者多用于
自动出售扫描、移动/寻路 tick 等逐帧路径的诊断输出。

**扩展背包级别策略**（与 R4 启发式同源）：

- **事件驱动日志用 INFO/WARN/ERROR**：状态迁移、事务提交/拒绝、所有权交接、详情身份失效等
  低频关键事件属于 VM 锚，按 §2 语义使用 I/W/E。
- **逐帧/高频路径只用 DEBUG**：`*_tick` 任务、`frame_task_add` 回调、触摸/拖拽循环、
  绘制/渲染路径（`*_process`、名字含 `draw`/`render` 的绘制函数，含 `*_drawing`）不得
  出现 I/W/E，逐帧 debug 仍必须经 `QOL_LOG_DEBUG_EVERY`/`ON_CHANGE` 节流。
- **命名启发式与状态门的边界**：R4 按函数名判定（`*_tick`/`*_process`/`*_draw*`/`*_render*`
  及 frame_host 的 `render_pre_wrapper`/`logic_pre_wrapper`）；若函数名命中，但函数体内有
  状态门保证每次进程只输出一次（如 `save_enter_tick` 用 `g_pending.exchange`），该 I/W/E
  可按事件驱动保留，复核时在该点标注“状态门保证一次性”；没有此类状态门的逐帧路径必须
  降 DEBUG。
- **`*_wrapper` 属事件驱动入口**：hook wrapper 仅在用户操作或 hook 命中时执行，并非逐帧，
  已从 R4 命名启发式中移除（避免误报）。其诊断输出仍按事件语义使用 I/W/E；仅安装/一次性
  初始化等低频事件用 INFO，逐事件高频诊断宜用 DEBUG。VM 锚（如 `MoveItem GUARD reject`）
  保留 ERROR 时以低频证据确认，不按“wrapper 内一律 DEBUG”机械降级。

**VM 验收锚不得降级清单**：

以下日志虽出现在 Hook wrapper 或运行时常驻函数中，但均由**事件驱动（非逐帧）**触发，且是 VM/
验收文档明文点名的字符串锚，必须保持 I/W/E 级，**不得降为 debug**（否则默认
`debugLogEnabled=false` 时验收证据消失）：

- `original-only native_call=1`（`virtual_bag_native_call_active()` 分支的每次原生调用观察，INFO）。
- `MoveItem pre` / `MoveItem post`（`move_item_wrapper` 的 `observe` 门控路径，INFO）。
- `EquipItemFromInvenToSlot bag=... slot=... equip_slot=... result=...`（INFO）。

边界同 `*_wrapper` 事件驱动入口：若把这些锚改成 `QOL_LOG_DEBUG_EVERY`/`ON_CHANGE`，等效于降
DEBUG，复核时按验收锚丢失处理。

## 8. 重要变量入日志要求

- **状态类正文用 `key=value`**：`slot=0 stage=load_data count=12 dur=8ms`，便于 grep 与解析。
- **每条 warn/error 至少含一个定位变量**：如 `reason=`、`total=`、`endpoint=`、`slot=`、
  `code=`、`error_code=`。禁止只有自然语言、无法定位的 warn/error（软规则 R5）。
- **debug 首行给结论、续行给明细**：多行 debug 的首行必须能独立说明状态（如
  `state=idle pending=0`），后续行给逐项字段/数组内容。

**已接受的 R5 软告警基线**：大量既有 message 文案受「逐字保留（保验收子串）」红线约束——
日志文本本身是 VM/验收卡的字符串锚（如 `verification-matrix.md` 的 `MoveItem GUARD reject`、
`deferred free drain`、`tab commit handled=1` 等），改动即破坏既有证据——因此无法补
`key=value`。R5 的「缺少定位变量」项按**软告警**处理，不作为阻断或拒收依据；已接受基线
条目以 `check_log_policy.py` 实时输出为准，逐点复核时不需要为它们改文案。**新增代码必须
满足本节要求**：每条 warn/error 至少含一个定位变量；R5 的「级别错配」（INFO/DEBUG 正文以
`ERROR ` 开头）不属于本基线，必须清零。

## 9. 配置接线

`debugLogEnabled` 是全链路唯一的日志运行时开关：

```text
config.json (debugLogEnabled, 默认 false)
   → ModuleConfig (Kotlin, @Volatile, 每次修改立即持久化)
   → ConfigApiService / ModuleConfigUiBridge (下发 native)
   → NativeBridge.nativeQolLogSetDebugEnabled(boolean)
   → qol_log_set_debug_enabled()   // std::atomic<bool>，任意线程可见
   → QOL_LOG_DEBUG / _EVERY / _ON_CHANGE 门控
```

- `GET /api/config/list` 返回该字段，`POST /api/config/set` 写入该字段（见 `api-reference.md` §7.6）。
- 游戏内设置页第 8 行「调试日志」经 `ModuleConfigUiBridge`（JNI，不依赖 HTTP）切换。
- `debugLogEnabled` 变化**无需重启**，即时生效。

## 10. 合规检查

```bash
uv run python scripts/verification/check_log_policy.py [--warn-only]
```

纯标准库脚本，输出 `文件:行: 规则号: 说明`，末尾汇总计数。默认存在 R1/R2/R3 违规时退出码非 0；
`--warn-only` 恒返回 0（迁移期使用）。R4/R5 为软告警，不影响退出码。

| 规则 | 性质 | 内容 |
|---|---|---|
| R1 唯一出口 | 违规 | native `module/app/src/main/cpp/**` 除 `core/native/qol_log.cpp` 与 `tests/stubs/android/log.h` 外不得出现 `__android_log_print`；Kotlin `module/app/src/main/java` 除 `LogFile.kt` 外不得出现 `android.util.Log` |
| R2 旧 tag 清零 | 违规 | native 与 Kotlin 字符串字面量中不得出现 §11 的 23 个废弃 tag（`Inotia4Qol`、文件常量 `inotia4-export.log` 允许保留）；只按「整串等于 tag」或「`tag:` 旧前缀」判定，避免把同名 Kotlin 类/标识符（`ApiServer`/`OpController` 等）误报 |
| R3 domain 词表一致 | 违规 | Kotlin `LogDomain` token 集合 == native `kDomainTable` token 集合 |
| R4 逐帧无高级别 | 软告警 | `*_tick`/`*_process`/`*_draw*`/`*_render*`（含 frame_host 的 `render_pre_wrapper`/`logic_pre_wrapper`）函数体与 `frame_task_add(` 回调内出现 I/W/E；别名宏（`VIRTBAG_LOG`/`VIRTBAG_LOG_WARN`/`VIRTBAG_LOG_ERROR`/`MOVE_LOG`/`SETTINGS_LOG`/`SB_LOG`/`SB_UI_LOG`/`EXP_LOG`/`CUSTOM_LOG`/`AUTOSELLUI_LOG`/`TILES_LOG`）按其 `QOL_LOG_*` 级别展开后再判定。通用 `*_wrapper`/`*_gate` 后缀已移除（事件驱动、非逐帧） |
| R5 级别与定位 | 软告警 | `WARN/ERROR`（含别名宏）格式串不含 `=`；INFO/DEBUG 正文以 `ERROR ` 开头（级别错配）。「缺少定位变量」为已接受的软告警基线（§8）；「级别错配」必须清零 |

## 11. 迁移状态

统一日志系统的**硬性收敛已完成，R4/R5 软告警收尾进行中**（与 `../planning/backlog.md`
「统一日志系统代码迁移收尾」条目同一状态）：

- **native 单写者**：`core/native/qol_log.cpp` 是唯一直接使用 `__android_log_print` 与打开
  日志文件的模块，格式化、节流、调试门控都在此实现。其他 native 调用点统一走 `QOL_LOG_*` 宏。
- **Kotlin 经 JNI 转发**：`LogFile.kt`（P1 重写）不再自持 `BufferedWriter`/文件句柄，所有输出经
  `nativeQolLogWrite` 进入 native 单写者，行格式与 native 完全一致；`LogFile.kt` 是 Kotlin 侧
  唯一允许 `android.util.Log` 的兜底通道。
- **单一 tag**：logcat 只保留 `Inotia4Qol`，23 个旧 tag 已清零。`Inotia4Export` 等旧 tag 的最终
  归宿是历史记录，只保留在本附录映射表；旧 tag 到新 domain 的对照见附录 A。
- **进度判定**：以 §10 脚本实时输出为准。R1/R2/R3（唯一出口、旧 tag、domain 词表）清零即硬性
  合规，当前已为 0；R5 的「级别错配」项当前已清零。R4 已把命名启发式收窄到真正的逐帧上下文
  （`*_tick`/`*_process`/`*_draw*`/`*_render*` 与 frame_host 相位 wrapper），仅剩
  `save_enter_tick` 这一状态门回调为已知基线；R5 的「缺少定位变量」属已接受软告警基线（§8）。
  R4/R5 均不影响退出码，按 §7/§8 逐点复核，不机械改文案；backlog 仍以「进行中」跟踪收尾，
  不标完成。

### 附录 A：旧 tag → domain 映射（迁移追溯用）

| 旧 tag | 新 domain |
|---|---|
| `Inotia4NativeHook` / `Inotia4CallPatch` / `Inotia4Transition` / `Inotia4Export` | `platform` |
| `Inotia4Move`（按文件归属）/ `Inotia4FrameHost` | `core` |
| `ApiServer` | `http` |
| api/native 与 Kotlin service/controller | `api` |
| `OpApiService` / `OpController` | `op` |
| 背包 / `native_inventory_hook` | `inventory` |
| `Inotia4VirtBag` | `extension_bag` |
| `Inotia4ModuleSave` / `Inotia4SaveEnter` / `Inotia4SaveExit` | `save` |
| `Inotia4SaveBackup` | `save_backup` |
| `Inotia4AutoSell` / `Inotia4AutoSellUI` | `autosell` |
| `Inotia4Craft` | `craft` |
| `Inotia4AttrRange` | `attr_range` |
| `Inotia4UISettings` / `Inotia4UISaveBackup` / `Inotia4UIExp` / `Inotia4UICustom` | `ui` |
| `Inotia4ModuleConfig` / 配置下发 | `config` |
| `Inotia4Tiles` | `catalog` |
