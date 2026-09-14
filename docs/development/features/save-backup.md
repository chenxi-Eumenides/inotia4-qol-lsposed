# 存档管理器（save-backup）设计与实现

> 状态：CURRENT。本册是 `.qol_save` 备份格式、导出/导入事务、跨槽/跨设备语义与真机证据的唯一权威。
> 端点契约以 `../../reference/api-reference.md` §7.3 为准；原版存档格式见 `../../reference/game/save.md`。

## 1. 范围与职责

- 把「原版 `save{slot}.dat` + 模块 sidecar」导出为自包含 `.qol_save` 备份，并可还原到任意槽位（原版与模块一起）。
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

## 3. `.qol_save` 备份格式（大端）

当前为 v3；v1/v2 旧备份仍可读（`parse_bundle` 接受 version 1、2 与 3）。

```text
# v3（当前）
u32 magic = 0x51534231 ("QSB1")
u16 formatVersion = 3
u8  sourceSlot                 # 导出源槽（元数据；payload 内 slot 已归一化）
u8[3] reserved = 0
u64 exportTimeMs
u32 origLen
u8[origLen] origPlain          # 解密明文，块0 slot 归一化为 0xFF
u32 moduleLen
u8[moduleLen] module           # ModuleSaveStore sidecar 原始字节（可为空 = 仅原版）
u32 warehouseLen
u8[warehouseLen] warehouseBlob # 个人仓库伴生文件集合（见下；空 = 0 字节）
u16 metaLen
u8[metaLen] metaJson           # UTF-8 元数据
u32 crc32                      # 覆盖此前全部字节
```

```text
# v2（旧备份，仓库段无 flags）
... | u32 warehouseLen | warehouseBlob | u16 metaLen | metaJson | u32 crc32
# v1（更旧备份，无仓库段）
... | u32 moduleLen | module | u16 metaLen | metaJson | u32 crc32
```

- **v1/v2 兼容**：`formatVersion==1` 时无 `warehouseLen/warehouseBlob` 段，解析结果 `has_warehouse=false`、仓库列表为空；`formatVersion==2` 时仓库段按旧编码（无 flags）解析，每项 `flags` 恒为 0。导入时完全跳过仓库写回（v1），或按旧「可解密才写」路径处理（v2），见 §5。
- `warehouseBlob` 编码（v3）：空列表编码为**空 blob（0 字节）**——保证无仓库时 bundle 摘要与旧版逐字节一致；非空时为 `u16 count` + 每项 `u16 suffixLen | suffix | u32 dataLen | u8 flags | data`。`flags` bit0=1 表示 `data` 为**明文**（导出时已用源设备密钥解密；导入时补目标槽、重算 `+0x38` 完整性校验后重加密），bit0=0 表示 `data` 为原始容器密文（源端解密失败时的兜底，也是 v1/v2 旧备份的形态）。未定义的 flags 位在解析时 fail-closed。v2 的每项为 `suffixLen | suffix | dataLen | data`（无 flags）。**wh4 明文 `+0x14` 槽位号可改写，但必须同步重算 `+0x38` 的 FNV-1a 64 校验**（`docs/reference/game/save.md` §1.1）；只改槽不重算会被游戏拒绝（真机 A/B 进档 SIGSEGV）。
- `suffix` 为相对 `save{slot}.dat` 的后缀（如 `.wh4-000001a043a2bba1`、`.wh4-000001a043a2bba1.bak`）；主文件与 `.bak` 各自独立为一项。
- 上限（解析 fail-closed）：文件数 ≤64、suffix ≤64B、单文件 ≤4MiB、blob ≤8MiB；suffix 必须 `.wh4-` 开头、字符为 ASCII 字母数字与 `.`/`-`、不含 `..`。
- 文件名：`<yyyyMMdd-HHmmss>_s<sourceSlot>_<checksum>.qol_save`（全 ASCII，同秒冲突加 `_1`、`_2`）。
- **备份标识 `checksum`** = `sha256(origPlain ‖ module ‖ warehouseBlob)` 前 12 位小写 hex，与文件名末段一致；导入/删除/去重均以它定位备份，不使用文件名。无仓库时 `warehouseBlob` 为空 ⇒ checksum 与旧版一致。
- `metaJson`：`source_slot`、`export_time`、`map_id`、`class_idx`、`class_name`、`hero_level`、`hero_index`、`save_version`、`save_time`、`original_sha256`、`module_sha256`、`checksum`、`warehouse_count`、`warehouse_sha256`（`warehouse_sha256` 为 blob 的 SHA-256，空仓库时为空串；旧备份缺字段时 UI 退化显示）。字段集合不随 v3 变化。
- 目录：`getExternalFilesDir(null)/save_backup/`；回滚暂存目录 `save_backup/.rollback/`。
- **只存明文**：原版 `save{slot}.dat` 与可解密的 wh4 都以明文入包，备份跨设备可移植（导入端用自己的密钥重加密）；无法解密的 wh4 才原样存密文并标记 `flags=0`。

## 4. 导出流程

1. native `read_original_plain(slot)`（`feature/save_backup/save_backup.cpp`）：`SAVE_LoadData` 解密 → 拷贝明文并 `MEM_Free` → 块0 slot 写 `0xFF` → 读 map_id/save_time/version → 元数据。
   - `hero_level`/`hero_index` 取运行时槽结构；主菜单（STATE==4）下先调 `SAVE_CreateSaveSlot` 刷新三槽，否则为 -1。
2. native 读 sidecar 原始字节（`<external>/module-saves/slot-{n}.module-save`，magic+整体 CRC32 轻校验；缺失/损坏时仅导出原版）。
3. native 收集个人仓库伴生文件（monster 改版）：`locate_save_directory` 定位存档目录后，取该目录下文件名以 `save{slot}.dat.wh4-` 开头的**普通文件**（含游戏自建 `.bak`；排除目录与 `.tmp`/`.wh-tmp` 写入中间态），按文件名升序（确定性）逐个读入；单文件 >1MiB 视为异常跳过并记日志；目录不可得/无命中则为空列表。
   - **导出解密（v3）**：对每个 wh4 取容器负载（`len-3`）用本机密钥 `ENCRYPT_Process2(mode=1)` 解密；成功 → 以明文 + `flags=1` 入包（导入端用目标设备密钥重加密，跨设备可移植）；失败（长度不足/无 key/双校验和不通过）→ 原样存密文 + `flags=0` 兜底，并记 `warehouse kept as ciphertext (decrypt unavailable)`。主文件与 `.bak` 各自独立处理，后缀原样保留。
   - **导出统一分离（内嵌段剥离为外部 wh4）**：**仅当本次确实收齐了外部 wh4** 时，在存档明文 block3 内定位 ASCII `WH96v002` 段（读 u32 段长，段总长 = 8 + 段长），**仅当该段之后只剩零填充（或恰好结束于 block3 末尾）**才剥离，使存档退回基础形态、仓库一律走外部 wh4。剥离时**连同段后零填充一起截断**（真机实测：只去段、保留零填充会让改版校验失败并崩溃；截断是已验证形式）。剥离后把各块按索引顺序重排为连续布局、重写块表项（`offset_rel`/`len`），`origLen` 缩短；随后的 `original_sha256`/`checksum`/尾部 CRC32 自然重算（见第 4/5 步）。保守规则：段后存在非零字节或块表异常 → 维持原状并记日志；无外部 wh4 → 不剥离（否则丢仓库）并记日志。
4. 算 `checksum = sha256(origPlain ‖ module ‖ warehouseBlob)` 前 12 位 → 按 `_<checksum>.qol_save` 后缀扫描 `save_backup/*.qol_save` 去重：已存在同 `checksum` 备份则**不写新文件**，返回既有备份 meta（`deduplicated:true`）。
5. 组装 `.qol_save`（v3，CRC32，metaJson 含 `checksum`/`warehouse_count`/`warehouse_sha256`）→ tmp+rename 原子写入。

## 5. 导入流程

0. native 按 `checksum` 定位 bundle（`_<checksum>.qol_save` 后缀；未命中→`backup not found`）。

1. 读并校验 magic/version/CRC/分段边界（`save_backup_bundle.cpp`）；v2/v3 额外解码 `warehouseBlob`（v2 = 无 flags 旧编码，v3 = 带 flags），`has_warehouse=true`；v1 无仓库段，`has_warehouse=false`。
2. native `encrypt_plain_to_slot(targetSlot, origPlain)`：块0 slot 写目标槽 → `ENCRYPT_Process2(mode=0)` → 密文（len+3 字节）。
3. 定位存档目录：优先 `dataDir` 下含 `save*.dat` 的子目录；否则 `dataDir/hex(MD5(key))` 并建目录。
4. 安全备份：把目标槽现存 `save{slot}.dat` 与 sidecar 主文件复制到 `.rollback/`。
5. 原子写原版：`save{slot}.dat.tmp` → rename。
6. sidecar：**native 直改字节**——只改容器第 6 字节 slot + 重算尾部 u32 CRC32（不解析 section），primary 与 last-good 双写；空 module 跳过。
7. 仓库写回（**仅 `has_warehouse==true`**）：
   - **能力门禁**：`qol::game_feature_state(kWarehouseCompanionFile)` 为 `unsupported`（该版本没有伴生文件仓库机制，如原版/大修）→ 整个仓库步骤跳过（等价 v1 处理），不触碰现存文件。
   - **写盘前预校验/预加密（fail-closed，位于第 4 步之前，保证 `.rollback/` 与现存文件不被触碰）**：
     - `flags=1`（v3 明文）：改写明文 `+0x14` 为目标 `slot`（小端 u32）→ **重算并写 `+0x38` 的 FNV-1a 64 校验** → `ENCRYPT_Process2(mode=0)` 整体重加密（`len+3` 字节）→ **往返自检**：再 `mode=1` 解密，要求返回 1、`+0x00` 前 8 字节 = `"WH4JRN01"`、`+0x14` = 目标槽、重算 hash == `+0x38` 存储值。任一项失败（无 key/长度不足/自检失败）→ **整次导入失败** `op_err("warehouse companion reencrypt failed (<reason>): <suffix>")`。
     - `flags=0`（v1/v2/兜底密文）：用本机密钥试解容器（`len-3`，双校验和）。**任一项解不开 → 整次导入失败** `op_err("warehouse companion not decryptable on this device: <suffix>")`，不得静默跳过、不得写任何文件。语义：**v2 旧备份（密文 wh4）只支持同设备导入**；导入后再导出（v3）即变为可跨设备。
   - **落盘**：预校验通过后，逐项按既有 additive 语义写入——已存在则先复制到 `.rollback/`，再原子写；**只新增/覆盖 bundle 内后缀对应的目标文件，不删除目标槽其它后缀的 wh4 文件**（模块不掌握游戏仓库文件完整集合语义，批量删除可能误删其它存档数据）。`suffix` 已由解码校验（`.wh4-` 开头、无路径分隔/`..`），target 由目录 + 槽号 + suffix 直接拼接安全。`.bak` 与主文件同样各自独立处理。
   - 日志：汇总 `warehouse companion files wrote=N skipped=0 reencrypted=K (state=...)`；失败为错误响应而非跳过明细。
   - 背景：wh4 与 `save{slot}.dat` 同容器方案、同一设备相关密钥。写入本机不可解的 wh4 会让游戏读档时 block3 校验失败（`SAVE_LoadFile` 返回 0 → 不挂接角色 → `GAMESTATE_EnterPlay` 空指针 SIGSEGV）。
8. 任一步失败：先还原本函数已处理的全部 wh 文件（existed 从 `.rollback/` 取回、新增的删除、清理副本），再从 `.rollback/` 还原 `save{slot}.dat` 与 sidecar（existed 还原 / not-existed 删除）。
9. 成功：清理 `.rollback/` 中 dat/sidecar 与 wh 全部副本。
10. v1 旧备份（`has_warehouse==false`）：仓库步骤**完全跳过**，绝不触碰现存 wh4 文件。
11. 不自动刷新游戏内存态；主菜单下一次 `enter_slot`/`system/info` 会重载。

## 6. 跨槽与跨设备

- **跨槽**：真机实证。save0 明文块0 slot 改 1 后重加密为 save1.dat，游戏识别、`enter_slot` 进世界、角色完整加载；导入后明文与源仅差 slot 字节。
- **跨设备**：v3 导出时把可解密的 wh4 解密为明文入包（`flags=1`），导入端用本地 `HubSave_GetKey` **补目标槽 + 重算 `+0x38` 完整性校验后重加密**并做往返自检，故仓库可随存档跨设备/跨槽迁移。真机 A/B 实测：同槽重加密正常（`screen=world`、`party=3`）；只改 `+0x14` 未重算 `+0x38` 被游戏拒绝（进档 SIGSEGV）；补上重算后接受。`flags=0`（v2 旧备份密文）只支持同设备导入，导入后再导出 v3 才可跨设备。
- 反篡改（`Protection` 类、`thpcheckslotN.dat`、`Com2usProtection.sav`）**未阻止**注入的槽改写存档。monster 改版的「个人仓库」（96 格）以伴生文件 `save{slot}.dat.wh4-<16hex>`（+ `.bak`）落在与原版存档同目录，随存档槽位；v3 bundle 会把它随存档一起备份/恢复。

## 7. 模块分层

> 2026-09-12 底层迁移（方案 A）：bundle 逻辑从 Kotlin `SaveBackupStore.kt` 下沉到 native `feature/save_backup/`，Kotlin 侧只保留日志包装与转发；`api/native/game_save_export.{h,cpp}` 已删除。

| 层 | 内容 |
|---|---|
| native 符号 | `game_symbols.h` + `symbol_registry.h` + `game_access.{h,cpp}` + `game_access_globals.inc`：`F_HUBSAVE_GET_KEY_VMA=0x9001c`、`F_SAVE_LOAD_DATA_VMA=0x129260`、`F_MEM_FREE_VMA=0xa8f18`、`F_ENCRYPT_PROCESS2_VMA=0xa413c` |
| native 纯逻辑 | `feature/save_backup/save_backup_bundle.{h,cpp}`：bundle 组装/解析（大端+CRC32）、SHA-256（FIPS 180-4）、MD5（RFC 1321，存档目录名 hex(MD5(key))）、CRC32（IEEE，与 `java.util.zip.CRC32` 一致）、RFC4648 base64、极简扁平 metaJson 字段提取、MSAV sidecar 容器最小校验/重定槽；纯 STL，编入 host 单测 |
| native 功能 | `feature/save_backup/save_backup.{h,cpp}`：`save_backup_init` / `save_backup_list_json` / `save_backup_export_json` / `save_backup_import_json` / `save_backup_delete_json`（返回体即 HTTP 响应 JSON；导出去重、导入两段事务 + `.rollback/` 回滚、sidecar 由 native 直改第 6 字节 slot + 重算尾部 CRC32 后 primary/last-good 双写）；`save_backup_slot_delete_hook_install_if_ready`（游戏删档回调 GOT 槽 PtrHook，删档后同步清该槽 sidecar） |
| JNI | `bridge/native/gamebridge_operations.cpp` + `NativeBridge.kt`：`nativeSaveBackupInit(dataDir, externalDir)` / `nativeBackupList()` / `nativeBackupExport(slot)` / `nativeBackupImport(checksum, slot)` / `nativeBackupDelete(checksum)` |
| Kotlin | `service/action/SaveBackupActions.kt`：`LogFile.op` 端点日志包装 + 入参防御校验，直接返回 native 响应 JSON |
| API | `SaveController.kt` + `ActionApiService(Core)`：`POST /api/system/backup/export`、`GET /api/system/backup/list`、`POST /api/system/backup/import`（按 `checksum`）、`POST /api/system/backup/delete`（按 `checksum`）；Service 方法 `backupExport`/`backupList`/`backupImport`/`backupDelete` 签名不变 |

## 8. 真机验证证据（2026-09-12，原版包）

### 8.1 底层 API（native `feature/save_backup/`，checksum 标识）

- 导出 slot0 → `20260912-155026_s0_8f03df248aec.qol_save`（10080 B），`map_id=37`、`map_name=帝国首都`、`hero_level=5`、`hero_index=0`、`save_version=5`。
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

### 8.3 个人仓库随存档备份（2026-09-12，monster v24 包）

真机：Phh-Treble（`192.168.3.54:5555`），包 `com.com2us.inotia4.normal.freefull.google.global.android.common`，存档目录 `fcea920f7412b5da7be0cf42b8c93759`；设备内 `save0/1/2.dat.wh4-000001a043a2bba1`（+`.bak`）均 675 B。流程：安装本改动 debug APK → force-stop + monkey 重启 → 处理 `agreement` → 轮询 `/api/health`。

- **导出 v2（slot0）**：`POST /api/system/backup/export {"slot":0}` → `20260912-224437_s0_a71e04a8dcb5.qol_save`（11642 B）。拉回离线解析：`magic=QSB1`、`version=2`、`warehouseLen=1410`、`metaJson.warehouse_count=2`、`warehouse_sha256=64c0692ed96b65aaa694f4a57c25f8cda2fea71f8361c71d26aa9a2580794616`；两段 suffix 与内容 md5 与设备文件逐一相符：`.wh4-000001a043a2bba1`=675B/`5e26d815990eeb0caa8795a559e90ebe`、`.wh4-000001a043a2bba1.bak`=675B/`23b9930ccc39737dab3fe5cdf40bd69a`。
- **导入 v2（slot0→slot1，覆盖写 + 回滚清理）**：`POST /api/system/backup/import {"checksum":"a71e04a8dcb5","slot":1}` → `ok:true`；`save1.dat` 生成，`save1.dat.wh4-000001a043a2bba1` 由 `e9bc4c82…` 变为 `5e26d815…`、`.bak` 由 `0435a466…` 变为 `23b9930c…`（与 bundle 一致）；`save_backup/.rollback/` 成功清空。
- **v1 兼容（旧备份不触碰 wh4）**：导入改动前的 v1 备份 `20260912-194431_s1_9cceaef965b5` → slot2 → `ok:true`；`save2.dat` 生成，而 `save2.dat.wh4-*` 四个文件 md5 全部与导入前相同（`has_warehouse=false` 跳过）。
- **additive 语义**：再把 v2 `a71e04a8dcb5` 导入 slot2 → `save2.dat.wh4-000001a043a2bba1`(+`.bak`) 覆盖为 `5e26d815…`/`23b9930c…`，而 bundle 未包含的旧后缀 `save2.dat.wh4-000001a090b9cd93`(+`.bak`) md5 保持 `c3bd83c4…`/`69e7bb14…` 不变（不误删）。
- **去重稳定性**：同一状态连续导出 → 稳定 `checksum=03d8d3d710f9`，第二/三次返回 `deduplicated:true`。
- host tests 全绿（`host_tests: 2547 passed, 0 failed`）；`scripts/build-debug.sh` `BUILD SUCCESSFUL`，APK `output/inotia4-qol-lsposed-debug-2609122241-393ba19f603f.apk`（旧命名；新命名规则见 [`build-and-deploy.md` §3.2](../../guides/build-and-deploy.md)）。
- **未真机触发**：导入仓库写失败的回滚路径（`.rollback/` 还原 wh）未做故障注入实测，仅有代码审查覆盖。

### 8.4 游戏删档同步清理模块 sidecar（2026-09-13，原版包）

真机：Phh-Treble（`192.168.3.54:5555`），模块 debug `0.7.5`（`output/inotia4-qol-lsposed-debug-2609131227-dcae13c32981.apk`，旧命名；新命名规则见 [`build-and-deploy.md` §3.2](../../guides/build-and-deploy.md)）。流程：安装 → force-stop + monkey 重启 → 同意页 `POST /api/ui/dialog/select {"action":"ok"}` → 轮询 `/api/health` → 主菜单。

- **Hook 安装**：logcat `Inotia4Qol ... domain=save_backup ... slot delete hook installed slot=0x7080cf6fa8 orig=0x7080b4f4e8`；`orig-base=0x14c4e8`、`slot-base=0x2f3fa8`，fail-closed 校验通过（GOT 槽值 == `SaveSlot_Delete`）。
- **测试前置**：`POST /api/system/backup/export {"slot":0}` → `20260913-122932_s0_a17dd08f71b3.qol_save`（11840 B，`module_sha256=de9cb8d1a454f45d5894d30512032032b2194b74689376e730a04bfa01a285e8`）；`POST /api/system/backup/import {"checksum":"a17dd08f71b3","slot":1}` → `ok:true`，`slot-1.module-save`(5640 B) + `.last-good`(5640 B) 出现；`/api/system/info` slot0/1 `exists=true`。
- **游戏内删档（slot1）**：主菜单「开始游戏」→ 存档面板（slot0/1 有档、slot2 EMPTY）→ 点 slot1 删除按钮 → 确认弹窗「是」。
- **结果**：logcat `slot delete hook: removed sidecar .../slot-1.module-save`、`removed last-good .../slot-1.module-save.last-good`；`ls module-saves/` 仅剩 `slot-0.*`（slot-1 两文件消失）；root `find` 存档目录仅剩 `save0.dat`（`save1.dat` 已删）；`/api/system/info` slot1 `exists=false`、slot0 `exists=true`。
- **不误伤**：删档前后 `slot-0.module-save` SHA-256 恒为 `de9cb8d1…`（与备份 `module_sha256` 一致）；打开存档面板本身不触发删除。
- **崩溃韧性**：删档后立即 force-stop + monkey 重启，`module-saves/` 仍只有 `slot-0.*`，slot-1 不重建。
- **未覆盖（残余）**：`SaveSlot_GoToNewGame`（点空槽开始新游戏）与 `SAVE_FileDelete`（Hive 云上传前清空）未经该 GOT 槽；本轮未对「删档后在游戏内新建档并打开扩展背包」单独构造实测。

## 9. 约束与残余

- **时机**：导出读已落盘文件，不触发保存；导入不刷新内存态。调用方负责时序。
- **已实现（2026-09-12 改造）**：`checksum` 作为备份标识（metaJson 权威 + 文件名兜底）；导出按 `checksum` 去重（命中不写新文件，响应带 `deduplicated:true`）；`backup/delete` 按 `checksum` 删除。
- **已实现（2026-09-12 底层迁移，方案 A）**：bundle 逻辑下沉 native `feature/save_backup/`（详见 §7）；技术原语 native 自实现（SHA-256/MD5/CRC32/base64/`clock_gettime` 毫秒/`localtime_r` 时间戳/MD5 目录名/扁平 metaJson 提取），删除 Kotlin `SaveBackupStore.kt` 与 `api/native/game_save_export.{h,cpp}`；sidecar 导入从「`ModuleSaveStore.importContainer` 按 section 解码重编码」改为「native 直改第 6 字节 slot + 重算尾部 CRC32」。迁移后四端点已真机回归（§8.1）。
- **已实现（2026-09-12 阶段 2 游戏内 UI）**：见 §10。
- **已实现（2026-09-12 个人仓库随存档备份）**：bundle 升到 v2（v1 仍可读）；导出收集 `save{slot}.dat.wh4-*`（含 `.bak`），导入 additive 写回目标槽（仅 v2 且 `has_warehouse`）。决策见 §3/§5。
- 残余：① 备份重命名未实现；② 跨设备导入未实测；③ `hero_level` 依赖运行时槽结构，无离线解析；④ 备份容量/清理策略未定；⑤ sidecar 校验从「section 全解码」放宽为「magic+整体 CRC32」（不解析 section），损坏 section 的容器在导出侧可能被带上（导入侧重定槽后写回）；⑥ 面板内两步确认与「← 返回」为当前 UI 交互，后续可评估改回原版弹窗或增加键盘返回。
- 残余（个人仓库）：⑦ wh4 文件整体加密，模块**未解析其格式**，只做字节级整体备份/恢复；⑧ 导入为 additive（只新增/覆盖 bundle 内后缀，不删除目标槽其它后缀），因此导入不会清除目标槽中 bundle 未包含的陈旧 wh4 文件；⑨ `save_backup_delete_slot_json`（删除游戏存档）**未清理 wh4 伴生文件**，删除槽位后可能残留仓库文件。
- **已实现（2026-09-13 游戏删档同步清理）**：`save_backup_slot_delete_hook_install_if_ready()` 由 `nativeInit` 在 `bridge_init` 成功后安装，覆盖游戏删档回调 `SaveSlot_Delete@0x14c4e8` 的 GOT 槽 `G_SAVESLOT_DELETE_GOT_VMA=0x2f3fa8`（PtrHook）。该槽位于 GNU_RELRO 覆盖的 `.got`，安装前将该页 mprotect 为 RW；fail-closed：GOT 当前值不等于预期函数地址时不安装。wrapper 在游戏主线程回调原函数（删 `save{n}.dat` + `SAVE_DestroySaveSlot`/`SAVE_CreateSaveSlot`）后，按 `module_sidecar_paths` 路径 `unlink` 该槽 `slot-{n}.module-save` 与 `.last-good`，短取 `g_sb_mtx` 与导入/导出串行；不取 `g_virtual_bag_mtx`、不走 JNI/Kotlin、不再调 `SAVE_CreateSaveSlot`。扩展背包内存态失效沿用主菜单既有清理（`prepare_main_menu_locked` 置 `g_loaded_slot=-2`），wrapper 不处理。
- 残余（游戏删档同步清理）：① 仅覆盖「存档面板删档按钮确认」路径；`SaveSlot_GoToNewGame`（空槽开始新游戏）与 `SAVE_FileDelete`（Hive 云上传前清空）不经该 GOT 槽，不在覆盖内；② 本 Hook 为即时删除，删档瞬间已完成，进程随后被杀不会残留；但「删档后才发生的中断」不会补清理（不做磁盘对账）；③ `save_backup.cpp` 未编入 host 单测，行为验证依赖真机；④ 真机已验收（§8.4）；「删档后在该槽游戏内新建档、扩展背包为空」未单独构造新档实测，但 sidecar 已删除、无数据可继承。
- 变更本功能前须核对 `game_symbols.h` 的 VMA 与 `scripts/maintenance/check_symbols.py`；改动触及行为面须补真机证据。

## 10. 游戏内 UI（阶段 2）

- **入口**：设置页配置网格中的一格「存档备份」（左描述 + 右侧「使用」样式按钮，复用物品详情页 `UIDesc_DrawMenuButton@0xb24bc` 的贴图 loc `0x0e`；图组未就绪时退化自绘），点击进入备份面板；面板「← 返回」回到设置页。
- **PopupState 占用**：复用 IAP 屏蔽后的死条目 `F_PANEL_UNK2_ENTER`(0x15e740, `Scene_Init_POPUP_SC_INAPP_GOODS`)；设置页占 `F_PANEL_UNK1_ENTER`(GEMSHOP)，二者互斥。**只在主菜单注入**，离开主菜单时还原原始 enter/process/f3/f4/event（设置页同策略）。
- **布局**（逻辑 960×640，root 居中）：顶部居中区（标题/消息）+ 左栏 3 槽 + 中栏 3 操作（`导出` / `导入` / `删除`）+ 右栏备份列表（**每页 5 行**，两行显示 `Lv<等级> <地图名>` / `<MM-DD HH:mm> #<checksum前6>`）+ 底部 `上一页 [N/M] 下一页`（页码居中、无框）+ 左上「← 返回」。
- **左右栏**：同宽（`COL_W=0x196`）、不同高（左 `SLOT_FRAME_H=0x108` 容纳 3 槽；右 `FRAME_H=0x13E` 容纳 5 行）；中栏在两者之间水平居中。
- **配色**：左栏空槽灰、有档金；右栏首行（等级+地图）金、次行（时间+校验值）白；中栏按钮可用时金色字、不可用时灰色字；选中项为**半透明深琥珀**背景（无边框）。
- **`ui_fill_rect_alpha` 颜色格式**：**RGB565 + alpha 百分比 0..100**（经 `GRPX_GetColorFromGRPWithAlpha@0x8fc88` 展开），与 `GRPX_FillRect`（ABGR8888）**不同**；选中色 `COLOR_SEL_BG=0x5182` ≈ RGB(0x50,0x30,0x10)、alpha 80。写裸 ABGR 会被当作 RGB565 → 颜色错乱（历史 bug）。
- **文本绘制**：`fn_grpx_set_font_color_rgb`(ABGR 拆包) + `fn_grpx_draw_string_with_font(text, x, y, align, font)`，align **0=左 / 1=右 / 2=中**；**不可用** `fn_ui_draw_string_halign`、`ui_draw_text_centered`（自定义面板下不出字）。
- **交互**：左槽单选、右备份单选；导出（选槽 + `导出`）；导入/删除两步确认（首击武装并在提示区提示，再击同 checksum+槽执行，其它交互取消）；提示区在顶部居中、独立显示、不消失、新消息覆盖旧消息；列表按 `export_time` 倒序（新的在最上），导出成功后回到第 1 页。
- **数据**：列表每次进入读目录（后台线程，IO 不阻塞游戏线程）；槽位等级/地图在游戏线程刷新（主菜单先 `SAVE_CreateSaveSlot`）。
- **API 拦截**：面板/设置页打开时 `data_op_save`/`enter_slot`/`create_slot` 返回 `ui occupied: backup panel` / `ui occupied: settings panel`。
- **实现文件**：`feature/ui/game_ui_savebackup.{h,cpp}` + `_panel.inc`/`_render.inc`/`_injection.inc`；设置页 `game_ui_settings*`（网格一格 + 「使用」按钮）；JNI `bridge/native/gamebridge_settings.cpp`（5 个 UI 导出）+ `gamebridge_operations.cpp`（`nativeSaveBackupSetMapNames`）；Kotlin `NativeBridge.kt` / `ApiServer.kt` / `StaticData.kt`。
