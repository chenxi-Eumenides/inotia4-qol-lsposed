# `save_backup` 插件

## 1. 作用与 owner 定位

`save_backup` 是存档备份管理的功能插件，注册 ID `save_backup`、版本 `0.1.0`（`module/app/src/main/cpp/plugins/save_backup/plugin.cpp:26-27`）。它负责 `.qol_save` 备份文件的导出、列表、导入与删除，以及游戏内存档备份面板和“内存档面板删档后同步清理 sidecar”的挂钩；存档事实、进档/建档与保存协调仍归 `core.save`（`docs/reference/plugins/core.save.md:59`；边界见 §4）。

插件无配置项：descriptor 不声明 config，`g_active` 只用于状态上报，不门控 HTTP 或面板（`plugin.cpp:72-73,327-330,376-378`）。

## 2. 功能面

### HTTP 路由

descriptor 通过 `http_endpoints` 声明四条端点，由宿主在插件 enabled 时注册、停用时置路由墓碑摘除；插件不自注册，也不自行注销（`plugin.cpp:122-124,226-239,379-381`）。`routes.inc` 是同一组路由的静态清单（`module/app/src/main/cpp/plugins/save_backup/routes.inc:1-4`）。

| 方法 | 路径 | 入口 | 参数校验 | 失败语义 |
|---|---|---|---|---|
| GET | `/api/system/backup/list` | `sb_http_handler` list 分支（`plugin.cpp:181-182`） | 无 | 目录不可用/不可读时仍返回 `{"ok":true,"backups":[]}` |
| POST | `/api/system/backup/export` | export 分支（`plugin.cpp:183-187`） | `slot` 必须为整数且 ∈[0,2]，否则 400 `bad slot` | 业务失败为 200 + `{"ok":false,"error":...}` |
| POST | `/api/system/backup/import` | import 分支（`plugin.cpp:188-197`） | `checksum` 满足 `^[0-9a-f]{12}$`，再校验 `slot`；错误依次为 400 `bad checksum` / `bad slot` | 同上；非主菜单时业务错误 `import requires main menu` |
| POST | `/api/system/backup/delete` | delete 分支（`plugin.cpp:199-206`） | `checksum` 同上，否则 400 `bad checksum` | 同上 |

- handler 同步执行，**不创建票据**，无异步结算与超时（`plugin.cpp:172-213`）。
- 空 request/response/context 返回 400 `bad request`；`body == nullptr && body_len > 0` 返回 400；响应缓冲不足返回内部错误并置 500（`plugin.cpp:152-173,172-180`）。
- `slot` 由严格整数 token 解析（`bundle.h` 的 `json_find_slot`）：拒绝小数、指数、十六进制、尾随字符、溢出、负数与 >2（`bundle.cpp:846-892,1030-1038`）。
- 成功响应统一经 `sb_http_body` 置 HTTP 200；业务错误在 body 内表达，因此“参数错误 400”与“业务错误 200”是两种状态（`plugin.cpp:165-173`）。
- **导入仅在主菜单允许**：`save.facts.current_slot >= 0`（已加载存档）时拒绝；取值返回 `NotReady` 且值为 -1 视为主菜单放行，其余错误 fail-closed（`feature.cpp:170-190,1538`）。导出不受此限制。

参数、响应体与错误文案的公共契约以 [API 参考「存档备份」小节（`docs/reference/api-reference.md:1509-1580`）](../api-reference.md) 为准；路由与 Kotlin 入口映射见 `docs/refactor/api-route-map.md:145-148`。

### Capabilities、ports 与资源

| 面 | 内容 |
|---|---|
| required capabilities | `ui_panels` 1.3、`save.facts` 1.0（`plugin.cpp:32-41,364`） |
| optional_requires | `http.registry`、`http.dispatch`（`plugin.cpp:42-47,365`） |
| provides | 无（`plugin.cpp:366`） |
| ports | `QOL_PORT_SAVE_CALL` 1.0（`plugin.cpp:49-52,375-376`）；不声明 `QOL_PORT_SAVE_WRITE` |
| 资源 | `ptrslot:SAVESLOT_DELETE_GOT`（kPtrSlot，private/required/fail-closed/resumable）、`own:popup.savebackup`（kOwnership，private/required/fail-soft/resumable）（`plugin.cpp:100-129`） |
| descriptor JNI | `jni=0`；旧 `nativeBackupList/Export/Import/Delete` 已删除，无遗留备份 JNI（`plugin.cpp:368`） |

`install` 要求 `ui_panels`/`save.facts` 能力可用、恰好一个 save-call port，并校验 `free_data`/`encrypt`/`mark_stale`/`load_data`/`slot_exists`/`hub_key`/`current_slot` 函数指针，任一不满足即安装失败（`plugin.cpp:241-279`）。插件使用 `save.facts.load_data/slot_exists/slot_hero_index/hero_level/hero_class/hub_key/current_slot`；`QOL_PORT_SAVE_CALL` 提供 `write.free_data/encrypt/mark_stale`（`contracts/save_facts.h:15-36`；`contracts/game_ports/save_call_port.h:11-21`）。

### 游戏内面板与设置入口

- 面板 provider：`panel_id="save_backup"`、owner=`save_backup`，install 时经 `sdk->panels.register_provider` 注册，deactivate 时 `unregister_owner`（`plugin.cpp:54-62,266-279,284-301`）。
- 面板由 `own:popup.savebackup` 占用 PopupState 死条目承载，acquire 注入、release 写回快照（`plugin.cpp:64-69,116-128`；`ui/panel.cpp:655-697`）。
- 设置入口为 descriptor feature `save_backup.open_manager`：默认 `1`、分类 Tools、类型 entry、标题「存档备份」、说明「打开存档备份管理器。」、按钮「打开」、priority `-2`；回调经 `ui_panels.open("save_backup")` 打开面板（`plugin.cpp:333-359`）。该入口在 settings 注册表中的位置见 `docs/reference/plugins/settings.md:107`。
- `settings_contribution.cpp` 的 catalog 注册为空实现（`settings_contribution.cpp:3-7`）。
- 面板布局：左侧 3 个槽位、中间「导出/导入/删除」、右侧备份列表每页 5 项并带上一页/下一页；面板缓存条目数不设上限（`g_backups` 为无界 vector），空列表显示「加载中…」或「暂无备份」（`ui/panel.cpp:74-116,413,515-522`；`ui/render.inc:93-119`）。
- 面板按钮**直接调用** native 业务函数（`save_backup_export_json` 等），不经 HTTP、不经 JNI/Kotlin（`ui/injection.inc:29,105,170-171`；`ui/panel.cpp:571-576`）。导入失败会显示响应中的 `error` 文案（`ui/injection.inc:11-25,108-125`）。
- 地图名与等级显示：未知等级显示 `Lv?`；地图名先按 UTF-8 码点边界截断到 16 字节，再按宽度截断（`ui/render.inc:159-168,233-241,314-340`）。

## 3. 使用的模块功能与消费者

- **Host SDK**：capability require、panel provider 注册/注销、RenderPre 帧任务（槽元数据快照）、owner-bound store（sidecar `.qol-save`/`.last-good`）、slot operation lease、指针槽资源回调、日志（`plugin.cpp:241-301`；`feature.cpp:48-51,194-223`）。
- **save 领域**：`save.facts` 提供明文、槽事实与 `current_slot`；`QOL_PORT_SAVE_CALL` 提供加解密、释放与 stale 标记；存档目录定位基于 `hub_key` 的 `hex(MD5(key))` 回退（`feature.cpp:393-419,614-644`）。
- **UI 领域**：`ui_panels` 提供面板注册与嵌套 push/返回；控件创建与绘制走 `QolUiAdapter`（`ui/panel.cpp:175-228`；`ui/panel.inc:127-179,226-255`）。
- **消费者**：插件不 provides 能力，没有 capability 消费者；对外可观察面是四条 HTTP 路由、设置 entry 与备份面板，均由 settings/catalog 与宿主 dispatch 消费（`plugin.cpp:355-381`）。
- Kotlin 侧 `SaveController.backupExport/backupList/backupImport/backupDelete` 做轻量校验后经 `NativeHttpCommand.post`/`NativeHttpQuery.get` 转发（`module/app/src/main/java/com/inotia4/qol/api/controller/SaveController.kt:39-79`）；无旧直连 JNI 入口。

## 4. 职责边界

- **属于本插件**：`.qol_save` bundle 格式与编解码、`externalFilesDir/backups/` 目录、checksum 去重、导出/导入/删除、备份面板、`save_backup.open_manager` entry，以及内存档面板删档后清理同槽 sidecar 的 GOT wrapper（`plugin.cpp:28-29,74-129`；`bundle.h:7-29`）。
- **不属于本插件**：存档槽事实与 `save`/`enter_slot`/`create_slot` 写路由、slot-data 参与者、进档/新档转换与保存协调（`core.save`）；回主菜单（`core.ui_state`）；面板栈与控件绘制（`core.custom_ui`/`ui_panels`）；路由注册、墓碑与 dispatch（`core.api`/宿主）（`docs/reference/plugins/core.save.md:58-60`；`plugin.cpp:122-124`）。
- 删档 hook 只做“原函数返回后同步删除 sidecar 并 `mark_stale`”，不重造存档槽、不取虚拟背包锁、不走 JNI（`feature.cpp:1083-1160`）。

## 5. 读写线程与生命周期

- **槽元数据快照**：install 注册 RenderPre 帧任务（每 30 帧），在游戏主线程读取 0..2 槽的 exists/hero_index/hero_level/hero_class 写入缓存；HTTP 导出只读缓存，不跨线程读游戏槽结构（`plugin.cpp:75-83,269-279`；`feature.cpp:636-660,1049-1081`）。缓存建立前导出仍成功，元数据为 -1。
- **备份读写**：不再用 `g_sb_mtx` 跨越游戏调用与文件 IO；共享状态（目录根、地图名表）只在短临界区内拷贝快照（`feature.cpp:1221-1250,1517-1525`）。导出持槽 operation lease，与保存/导入/删槽串行，失败返回 `slot operation unavailable`（`feature.cpp:1517-1525`）。
- **导出**：取明文 → 读 sidecar（`kOk`=present、`kNotFound`=absent、损坏=整次失败 `module container unreadable`）→ 收集 `save{slot}.dat.wh4-*`（读失败或超 4 MiB 即失败，数量/总量预检 64/8 MiB）→ 有外部 wh4 时尝试剥离 block3 内嵌 `WH96v002` 段 → `checksum` → 去重（校验既有 bundle 的内部 checksum 与 payload）→ `.tmp-<id>` 写入后 `rename`（`feature.cpp:1290-1500`）。
- **checksum**：v1/v2 为 `sha256(明文‖sidecar)[0:12]`；v3+ 为 `sha256(明文‖sidecar‖warehouseBlob)[0:12]`，覆盖全部可还原内容段（`bundle.h:207-220`；`bundle.cpp` `compute_checksum`）。导入/删除前按 bundle 版本重算并与 meta 比对，不一致返回 `backup checksum mismatch`（`feature.cpp:1435-1465,1549-1556`）。
- **sidecar 三态**：metaJson 的 `module_state`（0=absent，1=present）；导入 present 写入/覆盖 sidecar，absent 则删除目标槽现存 sidecar（完整替换，`feature.cpp:1301-1315,1665-1690`）。
- **导入**：主菜单门禁 → 解析并校验 checksum/module_state → 取槽 lease → 预加密 dat/warehouse → 现存 dat 复制到 `backups/.rollback/`（事务 id 命名）、sidecar 快照留内存 → 原子写 dat → 写/清除 sidecar → 逐项原子写 warehouse → 失败按 dat/sidecar/warehouse 逆序回滚（`feature.cpp:1527-1810`；`rollback.h:5-15`）。warehouse 写回是 additive，不清理目标槽未列出的旧文件（`feature.cpp:1720-1730`）。
- **删除备份**：解析并校验 checksum 后 `unlink` 单个 bundle（`feature.cpp:1815-1850`）。
- **删除槽**：dat 与 sidecar 各自在父目录内移入 `.sb-trash-<id>/`，写 `backups/.trash-<id>.index` 后统一 unlink；任一步失败把已移动文件移回；进程中断时下次 `save_backup_init` 调 `save_backup_recover_deletes()` 回滚优先（`feature.cpp:845-1050,1852-1950`）。删档 hook 只处理 sidecar，同样取 lease + trash + index（`feature.cpp:1083-1160`）。
- **列表**：先 `stat`，跳过 >64 MiB 的文件；单次扫描累计 >512 MiB 停止；锁外枚举/解析/排序，按 `export_time` 降序（`feature.cpp:57-58,1221-1290`）。
- **文件系统原语**：`lstat` 判目录/普通文件，读写用 `O_NOFOLLOW`；原子写临时名与回滚副本使用事务唯一名（`feature.cpp:293-370`）。
- **生命周期**：install 绑定上下文、注册 panel provider 与槽元数据帧任务；enabled/disabled 只翻转 `g_active`；deactivate 注销帧任务、撤销 owner 面板、注销 provider、解绑上下文；资源物理还原由宿主执行，`on_resource_ready` 只缓存删档原函数指针（`plugin.cpp:241-301`）。

## 6. 文件地图

| 子职责 | 文件 | 关键位置 |
|---|---|---|
| descriptor、能力/资源/端口声明、HTTP handler、生命周期、面板 provider、帧任务 | `plugins/save_backup/plugin.cpp` | 资源 100-129；HTTP 127-239；install/lifecycle 241-301；entry 333-359；descriptor 361-383 |
| bundle 编解码、摘要/CRC、warehouse 序列化与重加密、metaJson、checksum、严格 slot 解析 | `plugins/save_backup/bundle.h`、`bundle.cpp` | 格式与上限 bundle.h:10-35；仓库编解码 103-176；`compute_checksum` 207-220；bundle 组装/解析 222-260；实现 bundle.cpp:754-960 |
| 目录定位、槽元数据缓存、列表、导出、导入、删除、回滚、sidecar、删档同步、启动恢复 | `plugins/save_backup/feature.cpp`、`feature.h` | 门禁 170-190；原语 293-370；删除事务 845-1050；缓存 1049-1081；列表 1221-1290；导出 1517-1525；导入 1527-1810；删除 1815-1950 |
| 面板状态、异步刷新、生命周期、UI adapter 包装 | `plugins/save_backup/ui/panel.cpp`、`panel.h` | 布局 74-116；刷新 561-603；open/close 701-750 |
| 控件/几何/事件、按钮回调与两步确认、渲染与文本截断 | `plugins/save_backup/ui/panel.inc`、`injection.inc`、`render.inc` | 事件 panel.inc:275-343；操作 injection.inc:22-188；渲染 render.inc:93-348 |
| 路由静态清单 | `plugins/save_backup/routes.inc` | 1-4 |
| 空 catalog 注册 | `plugins/save_backup/settings_contribution.cpp` | 3-7 |
| host 测试 | `module/app/src/main/cpp/tests/test_save_backup_gate.cpp`、`test_save_backup_rollback.cpp`、`test_save_warehouse_validate.cpp` | gate 删除/回滚/门禁/checksum/导出；rollback 结果组合；warehouse 校验与校验和 |
