# `save_backup` 插件审阅登记（2026-10-20）

- 审阅对象：功能插件 `save_backup`（`module/app/src/main/cpp/plugins/save_backup/`，19 文件；生产源 `bundle/feature/plugin/settings_contribution/ui`）。
- 基线参考说明（阶段 0.75）：`docs/reference/plugins/save_backup.md`（本批新建，已在 `docs/INDEX.md:30` 补链接）。
- 阶段 0.5 讲解：`plugin-explainer.md`（同目录）。
- 阶段 1 静态审计：@oracle 只读，A–G 七维，findings SB-01…SB-14。
- 现场基线：仓库 HEAD `1c9df1a4`；工作区存在与本插件无关的未提交改动（autosell / move_merge / ui_adapter 等），本次审阅未触碰。
- 已执行：静态审计、四批修复、host 全量、`check_arch`/`check_http_routes`/`check_jni`、Debug 构建。未执行：部署、真机 API/smoke、面板目视（阶段 4–5）。

## §1 本批问题与处置

| 编号 | 严重度 | 文件:行 | 问题 | 处置 |
|---|---|---|---|---|
| SB-01 | P1 | `feature.cpp:496-514,529-556`；`game/access/save.h:10-11`；`host/http_dispatch.cpp:384-389` | HTTP 导出在 dispatch 线程调用 `read_hero_meta()`，经 `save.facts.slot_exists/slot_hero_index/hero_level/hero_class` 读取 live 槽结构；`game/access/save.h` 明确 `save_slot_*`/`save_hero_*` 仅限游戏主线程 | **待裁决**：改用主线程发布的槽快照，或把 export 改为票据化主线程执行（影响契约面，需用户选型） |
| SB-02 | P1 | `plugin.cpp:127-137,183-185,192-205`；`SaveController.kt:42-64` | `strtol` 后直接窄化 `int`，不检查 `ERANGE`/尾随字符；`1.5`→1、`4294967296`→0 可命中槽 0；`body==nullptr && body_len>0` 构造 `std::string` 越界读；Kotlin `optInt` 与 native 双重解析语义不一致 | **可直接修**（严格整数解析 + 空指针守卫 + Kotlin 对齐），附 host 边界用例 |
| SB-03 | P1 | `feature.cpp:561-566`；`bundle.cpp:793-798`；`tests/test_host.cpp:2049-2057` | `len<8` 通过后立即读 `plain+8/+12` 的块表；`len` 8–13 时越界读。`parse_bundle` 接受极小 `orig_plain`，导入路径 `encrypt_plain_to_slot` 仅检查 `size<8` | **可直接修**：解析/加密前校验块表最小长度与所有 block 边界，补短输入导入用例 |
| SB-04 | P1 | `feature.cpp:1033-1058,1280-1282`；`docs/refactor/slot-data-contract.md:185-187` | 导入只校验槽范围并取 lease，不检查 `current_slot`/session；成功后仅 `mark_stale()`。覆盖当前加载槽后游戏内存态仍为旧档，后续保存可能再次覆盖 | **已裁决**：禁止游戏运行时导入；`current_slot >= 0` 拒绝，取值失败 fail-closed（批 1）。导出不受限 |
| SB-05 | P1 | `feature.cpp:844-854,1163-1170` | 导出把所有非 `kOk` sidecar 结果记为“仅原版”并成功；导入在 `module.empty()` 时跳过 sidecar。无 sidecar 备份导入已有槽会保留旧 sidecar，形成存档与模块数据错配 | **已裁决 C+B**：bundle 显式三态（present/absent/corrupt），absent 导入时清除目标旧 sidecar，corrupt 导出直接失败（批 4） |
| SB-06 | P1 | `bundle.cpp:663-703`；`feature.cpp:1226-1235,1194-1205` | decoder 不拒绝重复 warehouse suffix；导入回滚副本名取 `file_name_of(target)`，两个同 suffix 项共用 `.rollback` 路径，第二项覆盖第一项的导入前副本，回滚可能恢复到错误数据 | **可直接修**：解码拒绝重复 suffix；回滚副本使用唯一事务/条目标识；补故障注入用例 |
| SB-07 | P1 | `feature.cpp:1321-1348,744-756`；`slot-data-contract.md:185-189` | 删槽先 `unlink(dat)` 再删 sidecar，sidecar 失败留下半删状态且不回滚；原版删档 hook 不取同槽 operation lease（与并行导入竞争），失败仅记日志；进程中断后的多文件事务无启动恢复扫描 | **已裁决**：同目录两阶段改名 + txn 标记，逐删；失败改回原名；启动未提交 txn 回滚优先；hook 取 lease（批 3） |
| SB-08 | P2 | `feature.cpp:827-855,857-930`；`core.save/save_coordinator.cpp:208-218`；`ui/render.inc:226` | 导出持 `g_sb_mtx` 跨游戏调用与磁盘 IO，且未取槽 lease，dat/sidecar/wh4 可能来自不同时间点；游戏线程绘制 `save_backup_map_name()` 也取同一锁，慢 IO 会卡面板 | **可直接修**：导出用同槽 lease 固定读快照；只锁共享状态，不跨 Host 调用/文件 IO 持锁 |
| SB-09 | P2 | `bundle.h:31-35`；`feature.cpp:862-900`；`bundle.cpp:647-659`；`feature.cpp:304-316,795-799` | 编码端只按单文件 1 MiB 跳过，不限制文件数/总量；decoder 限制 64 项/4 MiB/项/8 MiB 总量，导出可能写出自身无法解析的备份；列表与读取无大小/数量上限 | **已裁决 fail-closed**：导出前预检 64 项 / 4 MiB 每项 / 8 MiB 总量，任一不满足拒绝导出；列表另加读取上限（批 2） |
| SB-10 | P2 | `bundle.cpp:837-854` | `entry_json()` 直接拼接 `file_name`/meta 字符串，不转义；重命名备份含引号或控制字符会破坏列表 JSON 与面板解析 | **可直接修**：统一 JSON 字符串转义 |
| SB-11 | P2 | `NativeBridge.kt:172-175`；`gamebridge_operations.cpp:192-237`；`plugin.cpp:122-124` | `nativeBackupList/Export/Import/Delete` 直连业务函数，绕过声明式路由与生命周期门控；停用后仍可访问备份目录 | **已裁决删除**：移除 4 个 Kotlin 声明与 4 个 native 导出；业务函数保留供面板使用（冻结面显式例外，批 4） |
| SB-12 | P2 | `feature.cpp:960-964`；`bundle.cpp:778-781,930-939`；`api-reference.md:1513,1547,1573` | API 文档写 checksum=`sha256(origPlain‖module)`，实现为 `sha256(plain‖module‖warehouseBlob)`；`bundle_matches_checksum` 信任 meta 字段不按 payload 重算；文档称列表“只读文件头”，实现完整读取并解析；BackupMeta 未列 `map_name/class_idx/class_name` | **已裁决**：checksum 覆盖全部可还原内容段（plain‖sidecar‖warehouse‖未来段），导入/删除按 payload 重算；旧 v1/v2 空段等价兼容（批 4） |
| SB-13 | P2 | `feature.cpp:293-296,319-333,337-356,1121-1129` | `ensure_dir`/`file_exists` 跟随 symlink，`copy_file` 以 `fopen("wb")` 写回滚副本；外部目录若可布置符号链接可被引导读写；`stat`→`open` 存在 TOCTOU。warehouse suffix 白名单已阻断 `../` | **可直接修（部分需真机）**：固定目录用 `lstat/openat/O_NOFOLLOW`，回滚副本唯一临时名 + 原子提交；确认 Android 外部目录 symlink 行为 |
| SB-14 | P2 | `tests/CMakeLists.txt:164-177,1193-1201`；`plugin.cpp:176-217` | host target 不编译 `plugin.cpp`/`ui/panel.cpp`：路由 handler、JNI 生命周期、列表、活动槽拒绝、重复 suffix、数值溢出、磁盘/warehouse 失败均无覆盖；`save_backup_rollback_tests` 只测 `RollbackResult` 布尔组合；无真实 `.qol_save` golden fixture | **可直接修（分批补测）**：handler 层 target + 边界/故障注入 + golden fixture |

审阅裁决（阶段 2）：已取得，见 §5。

## §2 无法验证项

- SB-01：`SAVE_LoadData`/`ENCRYPT_Process2` 在 HTTP worker 上的实际版本行为；静态线程约束已违反，崩溃/竞态需真机或故障注入确认。
- SB-04：导入当前加载槽后继续游戏并保存，是否在所有变体都覆盖导入文件；需真机。
- SB-07：磁盘 IO 失败与进程中断点的部分完成状态；需故障注入或真机。
- SB-13：`getExternalFilesDir()` 对 symlink、原子 rename、掉电持久性的实际支持。
- VM-K4-04b / VM-K5-05c / VM-K5-06i 的 PASS 均为 2026-09 历史版本证据，不代表当前 HEAD；VM-D1 仍待真机。
- 本批已运行 host 全量与 `scripts/build-debug.sh`；部署、真机 API/smoke 与面板目视尚未执行。

## §3 已完成验收登记

| 项 | 结果 | 证据 |
|---|---|---|
| 架构检查 | PASS | `uv run python scripts/maintenance/check_arch.py`（2026-10-20）：层依赖/偏移/JNI/资源全部通过，无 save_backup 豁免；插件无 infra include/link 边 |
| 路由基线 | PASS | `check_http_routes.py`：147/147，重复/遗漏/额外 0，owner `save_backup=4` |
| JNI 基线 | PASS | `check_jni.py`：Kotlin external 159 / native 导出 159，违规 0（删除 4 个旧 backup JNI 后） |
| host 全量 | **147/147 PASS** | `ctest --test-dir module/app/src/main/cpp/tests/build`；`save_backup_gate_tests` pass=137、`save_warehouse_validate_tests` 172、`host_tests` 2656 |
| Debug 构建 | PASS | `scripts/build-debug.sh` → `output/inotia4_qol_lsposed_debug_2610201948_9089ace53372.apk`，SHA-256 `9089ace53372…` |
| 文档 diff 格式 | PASS | `git diff --check` 干净 |
| 插件事实说明 | 已完成（阶段 0.75 复检） | `docs/reference/plugins/save_backup.md` 按修复后实现更新；API 契约同步 `docs/reference/api-reference.md` 备份小节 |
| 真机验收（阶段 4–5） | **未执行** | 未安装 APK、未进档、未做 API/smoke/面板目视；VM-K5-05c / VM-K5-06i / VM-D1 仍未重验 |

## §6 修复提交

| 提交 | 内容 |
|---|---|
| `3a0347e9` | 严格 slot 解析、明文块表边界、重复 suffix 拒绝、JSON 转义、游戏运行中导入拒绝 |
| `045a0b26` | 导出容量 fail-closed（64 项 / 4 MiB / 8 MiB）与列表 64 MiB/512 MiB 读取上限 |
| `df84dff7` | 删槽两阶段 trash 事务、启动回滚优先、`QolSlotOperationApi` 新增 `delete_container_to_trash`、文件系统原语 O_NOFOLLOW |
| `fba8e432` | checksum 覆盖全部可还原内容并按 payload 重算；sidecar `module_state` 三态与完整替换 |
| `df977bcf` | 删除 4 个旧 backup JNI；面板标题/等级/删除文案与 UTF-8 截断修正 |
| `720fc57e` | 主线程槽元数据快照（RenderPre 30 帧）、导出持 lease、导出/列表锁收窄 |
| `c36efcaa` | 修正 `json_find_slot` 命名空间限定（生产构建） |
| `0e6d4975` | API 参考同步备份契约 |

## §4 G 基础设施观察

- `host/http_dispatch.cpp:384-389` 在请求线程同步调用 query/command handler，不切游戏主线程、不自动建票据；插件把槽事实读取放进同步 handler 时直接触碰 `game/access/save.h:10-11` 的线程约束。该机制对其它插件同样成立。
- 声明式路由 + `lease_required=1` + in-flight 生命周期保护本身闭合；旧 JNI 直连是绕过点（SB-11）。
- `slot_operation` lease 由 `core.save`/Host 提供，但“导出与保存事务共用同一 lease”目前没有强制入口（SB-08），属 owner 交界欠账。
- `entry_json` 一类本地 JSON 拼接在其它插件可能同样存在；本批只登记本插件实例，不扩散修改。

## §5 审阅裁决（阶段 2，用户）

1. **导入门禁（SB-04）**：禁止在游戏运行时导入，**导出不受限制**。判定用 `save.facts.current_slot`：`>= 0` 拒绝（`import requires main menu`），取值失败 fail-closed。主菜单值为 -1 已确认；`main_menu`/`world`/`story`/`loading` 的实际取值在阶段 4 真机记录。
2. **sidecar 语义（SB-05）**：bundle 显式三态（present/absent/corrupt），完整替换——absent 导入时清除目标旧 sidecar；corrupt 在导出阶段直接失败，不生成"仅原版"成功备份。
3. **warehouse 容量（SB-09）**：fail-closed。导出前预检 64 项 / 4 MiB 每项 / 8 MiB 总量，任一不满足即拒绝导出；列表另加读取上限。
4. **checksum（SB-12）**：覆盖**全部可还原内容段**（当前 plain‖sidecar‖warehouse，未来新增段追加并升 bundle 版本）；导入/删除按 payload 重算校验，不再只信 meta。旧 v1/v2 空 warehouse 段与旧公式逐字节等价，无需改写历史备份。
5. **旧 JNI（SB-11）**：**删除** 4 个 Kotlin `external fun` 与 4 个 native 导出；业务函数保留供面板使用。这是对 `ARCHITECTURE.md:71` 冻结面的显式裁决例外，历史证据中的 163 不改写。
6. **删槽事务（SB-07）**：两阶段同目录改名（`saveN.dat` 与 sidecar/`last-good` 各自目录），统一 commit 删除；失败改回原名；**启动时未提交 txn 采用回滚优先**（全部改回原名）；hook 取 slot lease。
7. **实施范围**：四批——输入/格式防护（SB-02/03/04/06/10）→ 容量（SB-09）→ 删除事务与文件系统（SB-07/13）→ 校验/快照/JNI（SB-01/05/08/11/12）。每批 host 全绿后一功能一提交，全部完成后进入阶段 4 真机。

## §7 行为重构与自动备份（2026-10-20 追加）

用户裁决：

1. **自动备份先不接线**：面板与 HTTP 不展示、不提供恢复入口；面板 UI 后续重置时再接入。
2. **绝不阻断保存**：自动备份失败只记 WARN，原版自动/手动保存继续。
3. **相同则跳过**：导出前比较 dat/sidecar/wh4 的大小与 mtime 指纹，与上次成功导出相同则跳过。
4. **结构重构边界**：只重构 save_backup 内部（文件类别注册表、统一收集/写回顺序）；bundle 格式、HTTP 契约、面板交互不变；逐点核对行为。

实现：

| 提交 | 内容 |
|---|---|
| `50ea8fbd` | `core.save` 新增观察型 `save.pre_save` 1.0；coordinator 在 capture/lease 之前通知，磁盘仍为旧档 |
| `c5647d67` | 新增文件类别注册表 `original_dat` / `module_sidecar` / `warehouse_companion` 与 `SlotSnapshot`；导出改为先收集再构包 |
| `a228ec83` | 导入与删槽按注册表顺序执行；删槽与删档 hook 新增 wh4 伴生文件事务删除（唯一行为变更） |
| `ae8cd781` | 抽出 `save_backup_export_slot_to_dir`；接入 `save.pre_save` 自动备份到 `backups/autobackup/autosave_sN.qol_save` |

自动备份语义：

- 每槽固定 1 份，原子覆盖；导出走与手动导出相同的快照/checksum/编码路径。
- 指纹文件 `autosave_sN.fingerprint`：dat（size + mtime）、sidecar primary（size + mtime / absent）、wh4 列表（名字 + size + mtime，排序）。完全一致才跳过。
- 失败（构包/写文件/写指纹）只记 WARN；旧 bundle 与旧指纹保留。
- `backups/autobackup/` 不参与顶层列表、checksum 定位、去重与删除扫描。

验证：

- host 全量 **148/148 PASS**；`save_backup_gate_tests` 216 断言；`save_pre_save_tests` 42；`save_coordinator_tests` 138。
- `check_arch` 全绿；路由 147/147；JNI 159/159。
- Debug 构建：`output/inotia4_qol_lsposed_debug_2610202100_db3c0f9cbee3.apk`（sha256 `db3c0f9c…`）。

未验证：

- 真机未部署；自动备份在游戏自动保存/手动保存前的实际触发、指纹跳过效果、失败保留未在设备观察。
- 面板/API 不展示自动备份是用户裁决的当前状态，不是最终交互。

## §8 UI 重排与确认对话框（2026-10-21）

用户提供的目标：左列 3 槽位、中列导出/删除/导入、右列 auto 卡片 + 手动备份列表 + 分页；导出立即执行，导入/删除改用确认对话框。

- 提交：`84697508`（auto 条目/导入 native 接口）、`1235a22c`（面板重排与确认框）。
- 交互：仅选槽→导出/删除；仅选手动备份→删除；槽+条目→导入；auto 可导入不可删除；按钮金色可用、灰色不可用；灰态点击给出提示。
- 对话框：面板内覆盖层，确认/取消；导入在目标有存档时提示覆盖；移除旧两次点击确认状态机。
- auto 数据：跟随左侧选中槽，在后台刷新线程读取 `save_backup_auto_entry_json(slot)`，绘制路径零 IO。
- 构建：`scripts/build-debug.sh` 成功 → `output/inotia4_qol_lsposed_debug_2610210130_b43186504387.apk`（sha256 `b4318650…`）。
- 未验证：未安装/未进档；版式、圆角近似、字号与 960×640 适配、确认框命中、auto 导入回同槽均需真机目视。

## §9 面板刷新改单线程增量（2026-10-21）

用户要求：不使用后台线程；打开先显示 UI，随后每帧解析一个；只解析当前页与选中槽的 auto；未变化缓存复用；不显示的不解析。

- `8e82787a`：新增 `save_backup_list_files_json`（只 stat 枚举）与 `save_backup_entry_json`（单条解析 + checksum 校验）；HTTP 列表不变。
- `84bc9d51`：移除面板 `std::thread`/detached 刷新；改为打开时枚举、process 每帧最多解析 1 项、`name+size+mtime` 缓存、翻页/切槽重建队列、损坏条目移除重算页数。
- 开销：枚举只做 `opendir/stat`；单条解析在读入时执行 CRC/checksum；每帧最多一项，但单个超大 bundle 的解析仍可能占一帧，后续可按时间预算再细化。
- 构建：`scripts/build-debug.sh` 成功（两 ABI）。真机版式与逐帧回填节奏未验证。
