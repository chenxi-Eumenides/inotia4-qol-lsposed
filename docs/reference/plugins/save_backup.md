# `save_backup` 插件（唯一权威文档）

> 本文是 `save_backup` 的设计、架构、能力、职责、数据与验收的唯一权威来源。
> 代码路径：`module/app/src/main/cpp/plugins/save_backup/`。
> 文档中的行号/文件名只在需要定位实现时使用；行为以本文为准。

## 1. 定位与职责

`save_backup`（插件 ID `save_backup`，版本 `0.1.0`）是存档备份管理插件：把原版 `save{slot}.dat` 的明文、模块 sidecar 与元信息打包为自包含 `.qol.bak`，并负责列出、导入、删除、自动备份和游戏内备份面板。插件无配置项；`g_active` 只用于状态上报，不门控 HTTP 或 UI。

**属于本插件：**

- `.qol.bak` bundle 格式、编解码、CRC32 与 checksum。
- `externalFilesDir/qol/backups/` 目录、去重、导出/导入/删除、自动备份。
- 旧备份 `.qol_save` 的读取/导入/删除兼容。
- 旧 metaJson 的职业/等级回填（只改 meta 段）。
- 游戏内备份面板、设置页「存档备份」入口。
- 游戏内存档面板删档后的 sidecar/wh4 清理 GOT wrapper。
- 面板槽卡的文件级状态判定（空/损坏/有效）。

**不属于本插件：**

- 存档槽事实、`save`/`enter_slot`/`create_slot` 写路由（`core.save`）。
- sidecar MSAV 容器的实际读写/原子写/last-good/quarantine（native `infra/store`）。
- 面板栈、PopupState、控件绘制（`core.custom_ui` / `ui_panels`）。
- HTTP 路由注册/墓碑/dispatch（`core.api` / 宿主）。
- 游戏明文格式的逆向定义（见 `docs/reference/game/save.md`）。

## 2. 分层架构

只允许高层 include 低层。`feature.cpp` 是公共 API facade，不是业务实现。

| 层 | 目录/文件 | 职责 |
|---|---|---|
| L0 | `model/types.h`、`bundle.*`、`backup_files.*`、`rollback.h` | 结果/错误类型、bundle 纯逻辑、快照收集、回滚结果 |
| L1 | `state/state.*` | 运行期上下文、map names 快照、槽元数据快照、职业名表 |
| L2 | `persistence/persistence.*`、`port/save_port.*` | 文件原语/目录/bundle 枚举；游戏 load/encrypt/free/stale 端口 |
| L3 | `transaction/transactions.*` | 导入事务、删除事务、回滚、trash 恢复、槽操作门 |
| L4 | `usecases/backup_usecases.*`、`usecases/meta_backfill.*` | 导出/列表/条目/导入/删除/自动备份/回填用例 |
| L6 | `plugin.cpp`、`api/commands.h`、`api/json_codec.*`、`ui/*`、`feature.cpp/h`、`settings_contribution.*` | 生命周期、HTTP、typed command、JSON、面板、公共 API 适配 |

依赖方向：

```text
plugin / api / ui
        ↓
     usecases
        ↓
   transaction
    ↓        ↓
persistence  port
    ↓        ↓
   state → model / contracts
bundle / backup_files → model
```

关键约束：

- L1–L4 不出现 UI 控件、rect、JSON 字符串或 `_bridge`。
- `bundle`/`backup_files` 不依赖 Android/游戏运行时。
- 游戏原生函数只经 `port`/`state` 的窄接口进入；插件业务代码不出现裸地址。
- `api/json_codec` 是唯一拼 HTTP JSON 的地方。

## 3. 能力、端口、资源与入口

### 3.1 required capabilities

| 能力 | 版本 | 用途 |
|---|---|---|
| `ui_components` | 1.22 | 面板控件创建/绘制/事件 |
| `ui_panels` | 1.3 | 面板 provider 注册、嵌套打开 |
| `save.facts` | 1.1 | 明文读取、槽事实、map id/current slot |
| `save.pre_save` | 1.0 | 保存前自动备份观察者 |
| `world.facts` | 2.0 | 主菜单判定（import 门禁） |
| `ui_confirmation` | 1.1 | 面板内确认框 |

### 3.2 optional capabilities

- `http.registry`、`http.dispatch`：由 descriptor 的 `http_endpoints` 声明四条路由，宿主负责注册/墓碑。

### 3.3 ports

- `QOL_PORT_SAVE_CALL` 1.0：`free_data`/`encrypt`/`mark_stale`；wh4 解密/加密也走该端口。

### 3.4 resources

| 资源 | 机制 | 语义 |
|---|---|---|
| `ptrslot:SAVESLOT_DELETE_GOT` | `kPtrSlot` | private/required/fail-closed/resumable；`step_budget=120`；wrapper 先回调原 `SaveSlot_Delete`，再删该槽 sidecar/wh4 |

`on_resource_ready` 只缓存原函数入口，常数时间、无资源操作。

### 3.5 装配与 ABI 门禁

`plugin_install` 要求：

- `ctx.sdk->abi.size >= sizeof(QolHostSdk)` 且 `ctx.sdk->abi.minor >= 9`。
- `ctx.sdk->ticket_result.register_typed_result_handler` / `unregister_typed_result_handler` 非空。
- 六个 required capability、一个 save-call port、全部必需函数指针可用。
- 失败即 fail-closed：解绑上下文并返回 1。

注册/注销：

- install 注册 3 个 typed result handler（export/import/delete）；失败时逆序注销并回滚 context/provider/pre_save。
- deactivate 注销命令、panel provider、pre_save observer、帧任务，并解绑 context。
- enabled/disabled 只翻转 `g_active`；不改变已注册命令。

### 3.6 面板与功能入口

- panel provider：`panel_id="save_backup"`、owner=`save_backup`；install 注册，deactivate 撤销。
- feature entry：`save_backup.open_manager`（Tools/entry，标题「存档备份」，priority `-2`），经 `UiPanelsApi::open("save_backup")` 打开。
- 面板由 `core.custom_ui` 的 PopupHost 托管，插件只提供回调与绘制。

## 4. HTTP 面

| 方法 | 路径 | 模式 | 校验/结果 |
|---|---|---|---|
| GET | `/api/system/backup/list` | 同步 | 目录不可用/不可读返回 `{"ok":true,"backups":[]}`；逐 bundle 校验 CRC |
| POST | `/api/system/backup/export` | 票据 | `slot∈[0,2]`；native 返回 `{"ok":true,"ticket":N}`；result 为原 `BackupMeta` JSON（去重时带 `deduplicated:true`） |
| POST | `/api/system/backup/import` | 票据 | `checksum ^[0-9a-f]{12}$`、`slot∈[0,2]`；result 为原 `BackupMeta` JSON |
| POST | `/api/system/backup/delete` | 票据 | `checksum` 校验；result 为 `{"ok":true}` |

### 4.1 票据链路

```text
Kotlin SaveController
  → NativeHttpCommand.post(路径, body)
  → nativeHttpDispatch
  → save_backup plugin sb_http_handler
  → 参数校验
  → QolCommands::post_typed(命令 id, payload)
  → 返回 {"ok":true,"ticket":N}
Kotlin postAndAwait
  → NativeBridge.nativeTicketStateJson(ticket) 轮询
  → state=done/failed 时取 result 原样返回
```

- 命令 id/payload：`api/commands.h`（`kCmdExport=0x53420101`、`kCmdImport=0x53420102`、`kCmdDelete=0x53420103`）。
- 命令在主线程执行；result 由 `QolTypedResultCommandHandler` 写入票据。
- 对外 `POST backup/{export,import,delete}` 的 **JSON 形状保持原同步响应**；票据是内部实现细节。
- list 仍同步；面板列表/条目/auto 直接调用 typed usecase，导出/导入/删除调用公共 API 适配（内部仍走同一 usecase），均不经 HTTP/票据。

### 4.2 参数与错误语义

- 参数错误 → 400：`bad slot`、`bad checksum`。
- 业务失败 → HTTP 200 + `{"ok":false,"error":"..."}`（由 `api/json_codec` 组装）。
- 命令不可用/票据异常 → 500 `command unavailable` 或 Kotlin 503 `ticket unavailable/timeout`。
- import 非主菜单 → `import requires main menu`；槽事实不可读 → `save state unavailable`。
- 具体导出/导入/删除错误见 §8。

## 5. 数据模型与存储

### 5.1 原版存档

- 文件：`save{slot}.dat`（slot 0/1/2）。
- 目录：应用私有 files 下的 32 位小写 hex 子目录（游戏用设备唯一 ID 的 MD5）。模块按“含 `save*.dat` 的子目录”扫描定位，找不到时退回 `hex(MD5(HubSave 密钥))`。
- monster 个人仓库伴生文件：`save{slot}.dat.wh4-<16hex>`，可带 `.bak`；与 dat 同密钥，跨设备不可直接解密。

### 5.2 模块 sidecar

- 目录：`<externalFilesDir>/qol/saves/`。
- `slot-{N}.qol-save`：primary MSAV 容器。
- `slot-{N}.qol-save.last-good`：上次有效副本。
- `slot-{N}.qol-save.unresolved-corrupt`：坏容器隔离标记。
- sidecar 由 native `infra/store` 读写；本插件只经 Host SDK store 面操作，不自行拼路径。

### 5.3 备份 bundle

- 目录：`<externalFilesDir>/qol/backups/`。
- 手动导出：`<yyyyMMdd-HHmmss>_s<slot>_<checksum>.qol.bak`；同名冲突追加 `_1.._99`。
- 旧版 `.qol_save` 继续列出、解析、导入、删除、去重命中。
- 自动备份：`<externalFilesDir>/qol/backups/autobackup/autosave_s<N>.qol.bak` + `autosave_s<N>.fingerprint`；旧 `.qol_save` 回退读取。
- 回滚副本：手动 `backups/.rollback/`、自动 `autobackup/.rollback/`。
- 删除事务中间态：`qol/saves/.sb-trash-<id>/`、`qol/backups/.trash-<id>.index`。

### 5.4 bundle 格式

- 大端分段，magic `QSB1`，当前 v4；解析兼容 v1/v2/v3。
- v3：`header | origLen | origPlain | moduleLen | module | warehouseLen | warehouseBlob | metaLen | metaJson | crc32`。
- v4：在 v3 warehouse 段后增加 `c18Len | normalizedC18Image`，再接 `metaLen | metaJson | crc32`；无 C18 image 时 `c18Len=0`。
- C18 normalized image 保存 source version label、source slot、96/192 source record_count、header `+0x10..+0x12` 保留字节和 opaque records；其他原生 header 字段在导入时按目标重建，不保存另一文件族副本。
- v2：warehouse 段无 flags；v1：无 warehouse 段；v1/v2/v3 的分段字节布局与 checksum 公式保持兼容。
- `checksum`：v2 起 `sha256(origPlain ‖ module ‖ warehouseBlob)[0:12]`（v2 引入仓库段时即如此，v3 只改段内编码）；v1 无仓库段（空 blob，等价旧公式）；v4 再追加 `normalizedC18Image` 输入。
- `metaJson` 字段：`source_slot`、`export_time`、`map_id`、`hero_level`、`hero_index`、`save_version`、`save_time`、`original_sha256`、`module_sha256`、`checksum`、`class_idx`、`class_name`、`warehouse_count`、`warehouse_sha256`、`module_state`。
- 原版明文块 0/1/2：块表、Information、Player、CharacterInfo；主角等级/职业取 CharacterInfo 第 0 条记录。

## 6. 业务流程

### 6.1 导出

1. 取槽明文（`save.facts.load_data`）、sidecar、wh4 伴生文件；若运行版本为支持 native bag 的 C18L，则从当前活动 per-slot 文件或其 shared fallback 收集一份 C18 image。
2. 有外部 wh4 时尝试剥离 block3 内嵌 `WH96v002` 段。
3. C18 文件严格校验 magic/version/count/slot/精确长度；记录体保持 opaque。C18 image 按原始文件字节读取，不调用会 `system()` 复制文件的游戏 selector，也不调用 `SAVE_Save` / C18 helper 改写当前槽；C18 文件无设备密钥加密证据，不做加解密。
4. 计算 checksum、构造 meta/bundle、CRC32。
5. 按内部 checksum 去重；命中返回既有 entry + `deduplicated:true`。
6. 取 slot operation lease；`.tmp-<id>` 原子写后 rename。
7. 返回 `BackupMeta`；不触发游戏保存。

### 6.2 导入

1. 主菜单门禁（`world.facts.state==4`，失败回退 `current_slot`）。
2. 按内部 checksum 定位 bundle；校验 payload checksum、`module_state`。
3. 读取当前 `game_identity` 判定目标 C18 文件族：目标是 C18 且布局已知时，带 C18 image 的备份按目标布局转换；旧备份（v1-v3）没有 image 时生成空背包镜像（record 全零，扩展背包为空）。目标不是 C18、身份不可读或 C18 版本布局未知时，跳过 C18 数据（不写 C18 文件，目标已有 C18 文件保持原字节）并继续导入。变体/版本差异本身不导致导入失败；C18 转换或写盘失败仍按事务回滚报错。
4. 取目标槽 lease；预加密 dat；仅当目标 `warehouse_companion_file_state == available`（monster 仓库能力）时预加密 wh4（补目标槽、重算 wh4 `+0x38`、往返自检），否则跳过 wh4 并记日志。C18 目标按已知布局构造唯一活动 per-slot 文件。
5. 目标槽现存 dat/sidecar/C18 活动文件复制或快照到 `.rollback/`。
6. 原子写 dat → sidecar → wh4 → C18 活动文件；失败按逆序回滚，C18 回滚记录包含原文件存在状态与原字节。
7. 成功后清理回滚副本；`save_mark_stale()`；返回 `BackupMeta`。

### 6.2.1 C18 native bag 版本转换

- 文件位于 Native init 传入的动态 `data_dir` 根目录。slot 0/1/2 对应后缀 N=1/2/3。布局按版本号动态判定：首个 192 条版本为 v507，低于它的所有版本（含 v0.2/v0.38）无 native bag；v507–v520 使用 `C18Lbag{N}.dat`，每槽 192 条、每条 `0x230` bytes；v521 及更高版本（含 v586/v588）使用 `C18Lwh{N}.dat`，每槽 96 条。边界由 `kC18LegacyBagFirstVersion` 定义，证据更新时只改该常量。
- 两族使用相同 `C18BAG01` magic、20-byte header 和记录 schema；header count 是一致性字段，loader 的循环数由目标版本代码固定，不由 header count 决定，因此必须写完整目标数量记录。
- 导入旧目标时，96 条源记录后补 96 条全零；导入新目标时，192 条源记录只取前 96 条，尾部直接丢弃且不另存。bundle 源 image 不变；导入只覆盖目标版本活动文件，不写 shared fallback，也不写另一族副本。
- 历史 v514/v520→v586 未发现 `bagN`→`whN` 游戏迁移；此备份行为是用户确认的单 image 规范化策略，不表示游戏本身会迁移。
- `data_dir/C18Lbag.dat` 只作为 primary 缺失时的只读 shared fallback，fallback header 可为 96 或 192 条；导入时 materialize 到目标版本 per-slot primary。目标不是 C18 或布局未知时整段跳过，不写任何 C18 文件；目标已有文件保持原字节。导出时 primary 与 shared 都不存在则 fail-closed；导入旧备份（bundle 无 image）时按目标布局生成空 image（全零记录），扩展背包为空。

### 6.3 删除

- 删备份：checksum 定位 → 重算 payload checksum → `unlink` 单文件。
- 删槽：dat、sidecar、wh4 分别移入同文件系统 `.sb-trash-<id>/`，写 `.trash-<id>.index`，全部成功后统一 unlink；失败移回；启动时 `save_backup_recover_deletes` 恢复。
- 游戏内删档 hook：原函数返回后调用 `save_backup_slot_deleted_sync`，只清该槽 sidecar/wh4 并 `mark_stale`。

### 6.4 自动备份

- `save.pre_save` 观察者在保存写盘前触发。
- 以 dat/sidecar/wh4 及当前 C18 活动 primary/shared fallback 的 size+mtime 指纹判断是否跳过。
- 每槽一份 `autosave_sN.qol.bak` 原子覆盖；失败只记 WARN，不阻断保存。
- 自动备份不进入 `backups/` 顶层列表、去重、导入、删除扫描。

### 6.5 旧 meta 回填

- 触发：`list_backup_files`/`describe_backup`/`auto_entry`/`import` 解析到 meta。
- 从明文块 2 第 0 条主角记录取 level/class；与 meta 不一致时重建 metaJson。
- 只替换 meta 段；bundle 版本、checksum、其余分段逐字节保留；CRC32 重算；`.tmp-<id>` + rename 原子写。
- `persistence::file_mutex` 串行 bundle 读改写；失败只记 WARN，下次再试。

### 6.6 面板

- 打开：设置页「存档备份」→ `ui_panels.open("save_backup")` → provider 回调。
- 左栏三槽：以 `save{slot}.dat` 是否可读判定有效/损坏/空；有效槽从明文解析职业/等级/地图；不再使用运行期槽状态。
- 右栏：手动备份列表（每页 5 项，逐帧增量解析）+ `auto` 卡片。解析走 `describe_backup_for_panel`：仓库密文项（`flags=0`）用本机密钥试解，任一项解不开（典型为跨设备 v2 备份）时把该行时间文本标红，提示该备份在本机不可导入。
- 操作：导出/删除/导入；导入/删除弹确认框；列表/条目/auto 直接消费 typed usecase，导出/导入/删除经公共 API 适配；面板不解析 JSON。
- 面板状态（选中、页码、缓存）只在 `ui/panel.cpp`，不进入核心层。

## 7. 线程、快照与锁

- **查询**（list/entry/list_files/auto_entry）：任意线程；只读文件 + `state` 快照，不调用游戏。
- **面板条目探测**（`describe_backup_for_panel`）：在 `describe_backup` 基础上用游戏密钥/`ENCRYPT_Process2` 试解 warehouse 密文项，因此只能在游戏主线程调用（面板逐帧解析路径）；HTTP 的 entry 接口不探测。
- **写/游戏读取**（export/import/delete/delete_slot/auto_backup/slot_summary）：游戏主线程；HTTP 写经 typed command 投递，面板直接调用。
- **state 快照**：`state::Context`、map names、槽元数据用不可变 `shared_ptr` 发布；RenderPre 每 30 帧刷新槽元数据，HTTP/绘制线程只读快照。
- **file_mutex**：只保护 bundle 读改写（回填/删除/导入定位），不承载业务状态。
- **slot_operation lease**：导出/导入/删槽按槽串行，与游戏保存/删档互斥。
- **锁序**：`slot_lease → file_mutex → store`；不用 `recursive_mutex`，不用 `try_lock` 做重入检测。
- 无全局业务互斥量 `g_sb_mtx`。

## 8. 错误语义

| 错误串 | 场景 |
|---|---|
| `bad slot` | slot 缺失/非整数/越界（400） |
| `bad checksum` | checksum 非 `^[0-9a-f]{12}$`（400） |
| `backup not found` | 目录/文件/自动备份不存在 |
| `backup unreadable or corrupt` | bundle 解析/CRC 失败 |
| `backup checksum mismatch` | 内部 checksum 与 payload 不一致 |
| `slot operation unavailable` | 槽被保存/导入/删档占用 |
| `import requires main menu` | 非主菜单导入 |
| `save state unavailable` | current_slot 读取失败 |
| `encrypt failed` | 目标槽 dat 预加密失败 |
| `save file write failed` | dat 写盘失败 |
| `module container import failed` | sidecar 导入失败 |
| `module container unreadable` | sidecar 损坏/不可读 |
| `bundle write failed` | 备份落盘失败 |
| `backup name collision` | `_1.._99` 用尽 |
| `warehouse read failed` / `warehouse file too large` / `warehouse limit exceeded` | wh4 收集/容量失败 |
| `load failed` | 明文读取失败 |
| `game identity unavailable` / `C18 game version unsupported` | 导出时运行时身份不可读或未知 C18 候选版本（导入时改为跳过 C18 数据，不报错） |
| `C18 data directory unavailable` | C18 活动文件无法从 Native init 的动态 `data_dir` 定位 |
| `C18 bag file not found` / `C18 bag file unreadable or corrupt` | C18 primary 与 shared fallback 均不可用，或文件 header/count/长度不符合证据 |
| `C18 bag image invalid` / `C18 bag file write failed` | normalized image 转换失败或活动 per-slot 文件写入失败 |
| `command unavailable` | typed command 未注册/post 失败 |
| `ticket unavailable` / `ticket timeout` | Kotlin 票据回取失败/超时 |

目标变体不支持的 C18 image 与 wh4 属于跳过项，不是错误：导入仍成功返回 `BackupMeta`，只写日志说明跳过原因；目标槽已有对应文件保持原字节。

L4 用 `Outcome<T>`/`BackupError`；`api/json_codec` 负责映射到上述文案与 HTTP 形状。

## 9. 测试与验收

当前基线（2026-10-30；含跨版本导入降级）：

- `save_backup_gate_tests`：**401 passed / 0 failed**（含 L6 typed command payload/票据 ACK 契约、跨版本变体数据跳过 5 用例）。
- `host_tests`：**2791 passed / 0 failed**。
- `save_warehouse_validate_tests`：**252 passed / 0 failed**；`save_backup_rollback_tests` 通过。
- `check_arch`：layers/offsets/jni/resources 全绿；`check_jni` 0 违规；`git diff --check` 通过。
- Android Debug：`scripts/build-debug.sh` 成功（2026-10-30，APK `output/inotia4_qol_lsposed_debug_2610301929_03df5527425a.apk`）。

**禁止真机验证**：当前策略要求多代理不得同时使用一台真机；本文不把真机证据作为本次重构的完成条件。后续如获授权，按以下清单复跑：

1. v514/v520 对应游戏版本安装集成包；确认模块加载、`/api/health` ready。
2. 主菜单导出 → list → 导入到另一槽 → `enter_slot` 成功；确认对外仍返回 `BackupMeta` 而非 ticket。
3. 删除备份、删槽、启动后 trash 恢复。
4. auto backup 生成/指纹跳过/源变化重导。
5. 旧 `.qol_save` 列表/导入/删除；旧 meta 回填后条目显示职业/等级。
6. 面板空/损坏/有效槽显示；导入后左栏立即刷新。

**C18 native bag 设备证据未覆盖**：本实现尚未进行设备 A/B。Host 逻辑测试不能证明 v520/v586 loader 接受重建后的文件；不得将 C18 行为标记为真机通过。需获授权后分别验证 v520 192-record 写入、v586 96-record 写入、跨槽 slot header、96→192 补零与 192→96 有意截断。

## 10. 文件地图与扩展点

| 子职责 | 文件 |
|---|---|
| descriptor、capabilities/ports/resources、typed command 注册、HTTP handler、生命周期、panel provider、帧任务 | `plugin.cpp` |
| 公共 API facade（40 行）、测试 seam | `feature.cpp`、`feature.h` |
| 结果/错误/请求响应类型 | `model/types.h` |
| bundle 编解码、checksum、明文摘要、warehouse 编解码/重加密 | `bundle.*` |
| 槽快照收集（dat/sidecar/wh4） | `backup_files.*` |
| 上下文/快照/map names/槽元数据/职业名 | `state/state.*` |
| 文件原语/目录定位/bundle 枚举/自动备份路径 | `persistence/persistence.*` |
| 游戏 load/encrypt/free/stale/warehouse helpers | `port/save_port.*` |
| 导入/删除/回滚/trash 恢复/槽操作门 | `transaction/transactions.*`、`rollback.h` |
| 导出/列表/条目/导入/删除/自动备份用例 | `usecases/backup_usecases.*` |
| 旧 meta 回填 | `usecases/meta_backfill.*` |
| typed command id/payload/票据 ACK | `api/commands.h` |
| JSON 组装 | `api/json_codec.*` |
| 面板 provider/状态/绘制/事件 | `ui/panel.*`、`ui/panel.inc`、`ui/render.inc`、`ui/injection.inc` |
| 空 settings catalog | `settings_contribution.*` |
| 路由静态清单 | `routes.inc` |
| host 测试 | `tests/test_save_backup_gate.cpp`、`test_save_warehouse_validate.cpp`、`test_host.cpp`（bundle 纯逻辑） |

**新增能力的顺序**：`model` 类型 → `usecases` 用例 → `api/commands`（如写端点）→ `plugin.cpp` handler/descriptor → Kotlin/文档 → host 测试。不要从 UI 反向 include 业务实现。

## 11. 设计决策与非目标

- **票据化写端点**：export/import/delete 用 `post_typed` 回主线程；Kotlin 隐藏票据，对外 JSON 不变。list 保持同步。
- **`.qol.bak` 命名**：新导出统一 `.qol.bak`；旧 `.qol_save` 永久读取兼容，避免升级丢备份。
- **文件级槽状态**：面板不看运行期槽状态，按 `save{slot}.dat` 可读性判定，避免导入后槽结构未重载时误报损坏。
- **明文优先**：等级/职业/地图从存档明文解析，运行期槽结构只作回退；避免世界内槽结构指向受控佣兵或已释放对象。
- **meta 回填**：只修复旧备份的职业/等级，不改 checksum/版本/其他段。
- **非目标**：不重写游戏存档格式；不做跨设备 dat 解密；不把 sidecar 存储实现搬回 Kotlin；不为了 UI 便利在 L1–L4 引入 JSON/控件依赖。
