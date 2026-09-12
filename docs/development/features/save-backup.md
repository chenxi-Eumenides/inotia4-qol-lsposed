# 存档管理器（save-backup）设计与实现

> 状态：CURRENT。本册是 `.qsb` 备份格式、导出/导入事务、跨槽/跨设备语义与真机证据的唯一权威。
> 端点契约以 `../../reference/api-reference.md` §7.3 为准；原版存档格式见 `../../reference/game/save.md`。

## 1. 范围与职责

- 把「原版 `save{slot}.dat` + 模块 sidecar」导出为自包含 `.qsb` 备份，并可还原到任意槽位（原版与模块一起）。
- **模块只保证**：原子写、CRC/校验和验证、内容完整、失败回滚。**导入导出时机与责任由调用方负责**，模块不因游戏状态拒绝（唯一例外是参数与文件合法性）。
- 不覆盖、不修改原版保存链；不改动 `save{slot}.dat` 的既有格式。

## 2. 逆向基础（详见 `../../reference/game/save.md`）

- 原版文件 `save{slot}.dat` = `[ciphertext L][sum_cipher][seed][sum_plain]`，N=L+3；逐字节 XOR 流加密 + 双重 8 位加和校验。
- 密钥 = App Property `Hub2uniqueDeviceId`，缺失时回退常量 `"1234567"`；目录名 = `hex(MD5(key))`，故路径为 `<dataDir>/<32hex>/save{slot}.dat`。
- 读取链：`SAVE_LoadData@0x129260(slot, &buf, &len)` → `HubSave_GetKey@0x9001c` → `SAVE_LoadDataAsNameAndKey@0x1290f8` → `ENCRYPT_Process2@0xa413c(mode=1)`；返回 `MEM_Malloc@0xa8d94` 明文，需 `MEM_Free@0xa8f18`。
- 写入加密：`ENCRYPT_Process2@0xa413c(buf, L, mode=0, key)`，**就地**写入 L+3 字节（`sum_cipher, seed, sum_plain` 追加在尾部），缓冲区需 L+3 容量。
- 明文块表：头 8 字节；块目录自 +8 起，每项 `u16 offset_rel + u16 len`；块 i 数据 = `plain + 8 + offset_rel`。
  - 块0：`+0 u8 slot`、`+13 u64 save_time`、`+29 u32 version(≤5)`；块1：`+0 i16 map_id`。
- `SAVE_IsValidInformation@0x125f88` 要求块0 slot == 请求槽 ⇒ **跨槽导入必须改写该字节并重新加密**。

## 3. `.qsb` 备份格式（大端）

```text
u32 magic = 0x51534231 ("QSB1")
u16 formatVersion = 1
u8  sourceSlot                 # 导出源槽（元数据；payload 内 slot 已归一化）
u8[3] reserved = 0
u64 exportTimeMs
u32 origLen
u8[origLen] origPlain          # 解密明文，块0 slot 归一化为 0xFF
u32 moduleLen
u8[moduleLen] module           # ModuleSaveStore sidecar 原始字节（可为空 = 仅原版）
u16 metaLen
u8[metaLen] metaJson           # UTF-8 元数据
u32 crc32                      # 覆盖此前全部字节
```

- 文件名：`<yyyyMMdd-HHmmss>_s<sourceSlot>_<sha256(origPlain‖module)前12位>.qsb`（全 ASCII，同秒冲突加 `_1`、`_2`）。
- **备份标识 `checksum`** = `sha256(origPlain ‖ module)` 前 12 位小写 hex，与文件名末段一致；导入/删除/去重均以它定位备份，不使用文件名。
- `metaJson`：`source_slot`、`export_time`、`map_id`、`hero_level`、`hero_index`、`save_version`、`save_time`、`original_sha256`、`module_sha256`、`checksum`。
- 目录：`getExternalFilesDir(null)/save_backup/`；回滚暂存目录 `save_backup/.rollback/`。
- **只存明文**：备份跨设备可移植（导入端用自己的密钥重加密）；不存原版密文。

## 4. 导出流程

1. native `read_original_plain(slot)`（`feature/save_backup/save_backup.cpp`）：`SAVE_LoadData` 解密 → 拷贝明文并 `MEM_Free` → 块0 slot 写 `0xFF` → 读 map_id/save_time/version → 元数据。
   - `hero_level`/`hero_index` 取运行时槽结构；主菜单（STATE==4）下先调 `SAVE_CreateSaveSlot` 刷新三槽，否则为 -1。
2. native 读 sidecar 原始字节（`<external>/module-saves/slot-{n}.module-save`，magic+整体 CRC32 轻校验；缺失/损坏时仅导出原版）。
3. 算 `checksum = sha256(origPlain ‖ module)` 前 12 位 → 按 `_<checksum>.qsb` 后缀扫描 `save_backup/*.qsb` 去重：已存在同 `checksum` 备份则**不写新文件**，返回既有备份 meta（`deduplicated:true`）。
4. 组装 `.qsb`（CRC32，metaJson 含 `checksum`）→ tmp+rename 原子写入。

## 5. 导入流程

0. native 按 `checksum` 定位 bundle（`_<checksum>.qsb` 后缀；未命中→`backup not found`）。

1. 读并校验 magic/version/CRC/分段边界（`save_backup_bundle.cpp`）。
2. native `encrypt_plain_to_slot(targetSlot, origPlain)`：块0 slot 写目标槽 → `ENCRYPT_Process2(mode=0)` → 密文（len+3 字节）。
3. 定位存档目录：优先 `dataDir` 下含 `save*.dat` 的子目录；否则 `dataDir/hex(MD5(key))` 并建目录。
4. 安全备份：把目标槽现存 `save{slot}.dat` 与 sidecar 主文件复制到 `.rollback/`。
5. 原子写原版：`save{slot}.dat.tmp` → rename。
6. sidecar：**native 直改字节**——只改容器第 6 字节 slot + 重算尾部 u32 CRC32（不解析 section），primary 与 last-good 双写；空 module 跳过。
7. 任一步失败：从 `.rollback/` 还原两个文件（existed 还原 / not-existed 删除）；成功则清理回滚副本。
8. 不自动刷新游戏内存态；主菜单下一次 `enter_slot`/`system/info` 会重载。

## 6. 跨槽与跨设备

- **跨槽**：真机实证。save0 明文块0 slot 改 1 后重加密为 save1.dat，游戏识别、`enter_slot` 进世界、角色完整加载；导入后明文与源仅差 slot 字节。
- **跨设备**：明文不含设备信息，理论上可移植（导入端用本地 `HubSave_GetKey` 重加密）。**尚未真机实测**。
- 反篡改（`Protection` 类、`thpcheckslotN.dat`、`Com2usProtection.sav`）**未阻止**注入的槽改写存档；游戏会为新槽自建 `save{N}.dat.wh4-*` 缓存。

## 7. 模块分层

> 2026-09-12 底层迁移（方案 A）：bundle 逻辑从 Kotlin `SaveBackupStore.kt` 下沉到 native `feature/save_backup/`，Kotlin 侧只保留日志包装与转发；`api/native/game_save_export.{h,cpp}` 已删除。

| 层 | 内容 |
|---|---|
| native 符号 | `game_symbols.h` + `symbol_registry.h` + `game_access.{h,cpp}` + `game_access_globals.inc`：`F_HUBSAVE_GET_KEY_VMA=0x9001c`、`F_SAVE_LOAD_DATA_VMA=0x129260`、`F_MEM_FREE_VMA=0xa8f18`、`F_ENCRYPT_PROCESS2_VMA=0xa413c` |
| native 纯逻辑 | `feature/save_backup/save_backup_bundle.{h,cpp}`：bundle 组装/解析（大端+CRC32）、SHA-256（FIPS 180-4）、MD5（RFC 1321，存档目录名 hex(MD5(key))）、CRC32（IEEE，与 `java.util.zip.CRC32` 一致）、RFC4648 base64、极简扁平 metaJson 字段提取、MSAV sidecar 容器最小校验/重定槽；纯 STL，编入 host 单测 |
| native 功能 | `feature/save_backup/save_backup.{h,cpp}`：`save_backup_init` / `save_backup_list_json` / `save_backup_export_json` / `save_backup_import_json` / `save_backup_delete_json`（返回体即 HTTP 响应 JSON；导出去重、导入两段事务 + `.rollback/` 回滚、sidecar 由 native 直改第 6 字节 slot + 重算尾部 CRC32 后 primary/last-good 双写） |
| JNI | `bridge/native/gamebridge_operations.cpp` + `NativeBridge.kt`：`nativeSaveBackupInit(dataDir, externalDir)` / `nativeBackupList()` / `nativeBackupExport(slot)` / `nativeBackupImport(checksum, slot)` / `nativeBackupDelete(checksum)` |
| Kotlin | `service/action/SaveBackupActions.kt`：`LogFile.op` 端点日志包装 + 入参防御校验，直接返回 native 响应 JSON |
| API | `SaveController.kt` + `ActionApiService(Core)`：`POST /api/system/backup/export`、`GET /api/system/backup/list`、`POST /api/system/backup/import`（按 `checksum`）、`POST /api/system/backup/delete`（按 `checksum`）；Service 方法 `backupExport`/`backupList`/`backupImport`/`backupDelete` 签名不变 |

## 8. 真机验证证据（2026-09-12，原版包）

### 8.1 底层 API（native `feature/save_backup/`，checksum 标识）

- 导出 slot0 → `20260912-155026_s0_8f03df248aec.qsb`（10080 B），`map_id=37`、`map_name=帝国首都`、`hero_level=5`、`hero_index=0`、`save_version=5`。
- 同内容再次导出 → `deduplicated:true`（返回既有备份，不写新文件）。
- `POST /api/system/backup/import {checksum, slot:1}` → `ok:true`；`/api/system/info` 三槽 `exists=true`、`hero_level=5`；`enter_slot(1)` → world、party=1。
- `POST /api/system/backup/delete {checksum}` → `ok:true`（list 清空）；错误分支 `backup not found` / `bad checksum`。
- 磁盘核对：`save1.dat` 离线解密正常，明文与源 save0 **仅差 offset 36（块0 slot）1 字节**；`slot-1.module-save`/`.last-good` 同步写入；`.rollback/` 成功清理；`save0.dat` SHA-256 前后一致。
- 跨槽：save0 明文块0 slot 改 1/2 后重加密为 save1/save2，游戏识别并进世界、角色完整加载。

### 8.2 游戏内 UI（设置页入口 + 备份面板）

- 设置页「存档备份」按钮 → 打开备份面板；「← 返回」→ 回设置页；设置页关闭 → 主菜单。
- 三栏：左 `槽0/1/2 Lv5`、中 `→导出 / ←导入 / 删除`、右列表每页 4 行（两行：`Lv5 帝国首都` / `09-12 15:48 #8f03df`）+ 翻页 + 提示区。
- 导出/导入/删除（两步确认）/取消/翻页均真机通过；导入成功后三槽 `exists=true`、`hero_level=5`。
- 面板打开时 `POST /api/system/enter_slot` → `ui occupied: backup panel`；设置页打开时 → `ui occupied: settings panel`。
- 关闭面板 → 设置页关闭 → `enter_slot(1)` → world、party=1，无崩溃。
- 地图名：启动期 Kotlin 下发 `map_id→名称`（MAPINFOBASE 416 条），native 内存表供 `entry_json` 输出 `map_name`。
- host tests 2/2；`scripts/build-debug.sh` 通过。

## 9. 约束与残余

- **时机**：导出读已落盘文件，不触发保存；导入不刷新内存态。调用方负责时序。
- **已实现（2026-09-12 改造）**：`checksum` 作为备份标识（metaJson 权威 + 文件名兜底）；导出按 `checksum` 去重（命中不写新文件，响应带 `deduplicated:true`）；`backup/delete` 按 `checksum` 删除。
- **已实现（2026-09-12 底层迁移，方案 A）**：bundle 逻辑下沉 native `feature/save_backup/`（详见 §7）；技术原语 native 自实现（SHA-256/MD5/CRC32/base64/`clock_gettime` 毫秒/`localtime_r` 时间戳/MD5 目录名/扁平 metaJson 提取），删除 Kotlin `SaveBackupStore.kt` 与 `api/native/game_save_export.{h,cpp}`；sidecar 导入从「`ModuleSaveStore.importContainer` 按 section 解码重编码」改为「native 直改第 6 字节 slot + 重算尾部 CRC32」。迁移后四端点已真机回归（§8.1）。
- **已实现（2026-09-12 阶段 2 游戏内 UI）**：见 §10。
- 残余：① 备份重命名未实现；② 跨设备导入未实测；③ `hero_level` 依赖运行时槽结构，无离线解析；④ 备份容量/清理策略未定；⑤ sidecar 校验从「section 全解码」放宽为「magic+整体 CRC32」（不解析 section），损坏 section 的容器在导出侧可能被带上（导入侧重定槽后写回）；⑥ 面板内两步确认与「← 返回」为当前 UI 交互，后续可评估改回原版弹窗或增加键盘返回。
- 变更本功能前须核对 `game_symbols.h` 的 VMA 与 `scripts/maintenance/check_symbols.py`；改动触及行为面须补真机证据。

## 10. 游戏内 UI（阶段 2）

- **入口**：设置页底部新增可点击按钮「存档备份」（非开关），点击后 push 独立备份面板；面板「← 返回」回到设置页。
- **PopupState 占用**：复用 IAP 屏蔽后的死条目 `F_PANEL_UNK2_ENTER`(0x15e740, `Scene_Init_POPUP_SC_INAPP_GOODS`)；设置页占 `F_PANEL_UNK1_ENTER`(GEMSHOP)，二者互斥。**只在主菜单注入**，离开主菜单时还原原始 enter/process/f3/f4/event（设置页同策略）。
- **布局**（逻辑 960×640，root 居中）：顶部标题 + 左栏 3 槽纵列 + 中栏 3 操作（`→ 导出` / `← 导入` / `删除`）+ 右栏备份列表（每页 4 行，两行显示 `Lv<等级> <地图名>` / `<MM-DD HH:mm> #<checksum前6>`）+ 底部「下一页 N」+ 提示区 + 「← 返回」。
- **交互**：左槽单选、右备份单选；导出（选槽 + `→`）；导入/删除两步确认（首击武装并在提示区提示，再击同 checksum+槽执行，其它交互取消）；提示区独立显示、不消失、新消息覆盖旧消息。
- **数据**：列表每次进入读目录（后台线程，IO 不阻塞游戏线程）；槽位等级在游戏线程刷新（主菜单先 `SAVE_CreateSaveSlot`）。
- **API 拦截**：面板/设置页打开时 `data_op_save`/`enter_slot`/`create_slot` 返回 `ui occupied: backup panel` / `ui occupied: settings panel`。
- **实现文件**：`feature/ui/game_ui_savebackup.{h,cpp}` + `_panel.inc`/`_render.inc`/`_injection.inc`；设置页改动 `game_ui_settings*`；JNI `bridge/native/gamebridge_settings.cpp`（5 个 UI 导出）+ `gamebridge_operations.cpp`（`nativeSaveBackupSetMapNames`）；Kotlin `NativeBridge.kt` / `ApiServer.kt` / `StaticData.kt`。
