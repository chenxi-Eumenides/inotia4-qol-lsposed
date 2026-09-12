# 存档系统逆向（SAVE 域）

> 来源：arm64 libgame.so 静态反汇编（2026-08-27，工具链 `tools/ndk/.../llvm-objdump`）。
> 所有 VMA 对应当前游戏版本（盗版大修 20260704）；符号未 strip，换版本按符号名重解析。

## 1. 文件枚举与路径

- 槽位固定 3 个（0/1/2），文件名 `save%d.dat`（`SAVE_GetSaveFileName` 0x125d08：`CS_knlSprintk("save%d.dat", slot)`）。
- 目录 = `CC_MD5(deviceUniqueId)` 16 字节逐字节 `%02x` 拼成 32 字符小写 hex（`HubSave_GetFolderName` 0x90070）。
- 完整路径 = `HubSave_MakeFullName("%s/%s", folder, name)`（0x90178），落在应用私有 files 目录下——即
`/data/data/<pkg>/<32hex>/save{slot}.dat`（目录名随设备/UID 变化，模块侧通过扫描定位）。
- 存在性判定：`FILE_IsExist`（0xa47f8），`SAVE_CheckFile`（0x12a4c8）/ `SAVE_IsExist`（0x128a3c）循环 3 槽调用。

## 2. 文件容器格式（加密 + 校验和）

`SAVE_LoadDataAsNameAndKey`（0x1290f8）+ `ENCRYPT_Process2`（0xa413c）实证：

```text
文件大小 N（必须 ≥ 3）
├─ ciphertext[0 .. N-3)      有效负载密文，长度 L = N-3
├─ byte[N-3] = sum_cipher    sum(ciphertext[0..L)) & 0xff
├─ byte[N-2] = seed          保存时 MATH_GetRandom 写入的随机种子（0-255）
└─ byte[N-1] = sum_plain     sum(plaintext[0..L)) & 0xff（解密后校验）
```

- 读取：`FILE_Open(name,1,1)` → `FILE_GetSizeFromHandle` → `MEM_Malloc(N)` → `FILE_Read` 必须读满 N 字节（短读=失败）。
- 解密（`ENCRYPT_Process2(buf, L, mode=1, key)`，就地）：
  `i = seed + j`（j ∈ [0,L)）；`plain[j] = cipher[j] XOR (key[i % keylen] + ENCRYPT_GetKey(i & 0xff))`。
- **校验层**：解密前计算 `sum(cipher[0..L)) & 0xff` 与 `cipher[L]` 比对；解密后计算 `sum(plain[0..L)) & 0xff` 与 `cipher[N-1]` 比对。两者都相等才返回 1，任一不等 → 返回 0（加载失败）。
  这是原版唯一的完整性校验——**无魔数、无 CRC32，只有双重 8 位加和校验 + 逐字节 XOR 流加密**。
- 密钥：`HubSave_GetKey`（0x9001c）返回设备相关密钥（`HubSave_LoadKey` 0x8ff1c 在 `[0x2f6000+0x80]==2` 时加载）；
  `SAVE_LoadData`（0x129260）在 `[0x2f6000+0x80]==3` 时用 `CS_hlpGetAppProperty` 取旧版密钥重试（v119  legacy 路径，
  配套 `SAVE_v119FileToBeforeVersion` 0x12aa10 / `SAVE_GetMergeData_v119` / `SAVE_MergeDataDelete_v119`）。
- **密钥设备相关** ⇒ 存档文件不可跨设备直接解密；host 侧不做解密，预检复用游戏自身函数（见 §7）。

## 3. 明文结构（块表）

`SAVE_LoadBlock(data, idx, &out)`（0x125e78）：跳 `(idx+2)*4` 字节 → 读 u16 → `out = data + u16 + 8`。

```text
[0..8)     头部（8 字节，LoadBlock 计算块指针时 +8 跳过）
[8 + i*4)  块 i 目录项（4 字节：u16 offset_rel + u16 标志/长度，写侧 SAVE_SetBlockInfo 0x125f0c 同样 4 字节双 u16）
块 i 数据  = base + u16(offset_rel) + 8
```

- 块 0 = Information（`SAVE_LoadInformation` 0x1271b4，按序读）：
  | 偏移 | 类型 | 去向 | 校验 |
  |---|---|---|---|
  | +0 | u8 | 全局 `[0x2f4000+0x238]` | **必须等于请求的槽位号**（`SAVE_IsValidInformation` 0x125f88） |
  | +1 | u32 | `[0x2f3000+0x870]`（sxtw 存储） | 无 |
  | +5 | u64 | `[0x2f3000+0xea8]` | 无 |
  | +13 | u64 | 读后丢弃 | 无 |
  | +21 | u32 | 读后丢弃 | 无 |
  | +25 | u32 | `[0x2f6000+0x888]` | 无 |
  | +29 | u32 | `[0x2f5000+0x860]` | **版本号，必须 ≤ 5**（IsValidInformation） |
- 块 1 = Player（`SAVE_LoadPlayer` 0x1273e8）。
- 角色：主佣兵槽索引 = `[0x2f6000+0x4f8]` 有符号字节（**必须 ≥ 0**）；3 个角色槽索引来自 `[0x2f4000+0x120]+i`
  有符号字节（<0 跳过），逐个 `SAVE_LoadCharacterDirectEx`（0x128abc）；与主索引相等者写入槽结构 +0x1c。

## 4. 槽结构（内存判决，preflight 的直接依据）

`SAVE_pSaveSlot`（.bss 0x729858，3×0x1d 字节；`SAVE_GetSaveSlot` 0x1289e4：`base + slot*0x1d`，slot>2 返 0）：

| 偏移 | 类型 | 语义 |
|---|---|---|
| +0x00 | u16 | 地图 ID（**仅加载成功时**由 `[0x2f3000+0xca8]` 写入） |
| +0x02 | u8 | **状态：0=缺失/空（文件不存在或未加载），1=加载失败（UI 显示「存档损坏」），2=加载成功** |
| +0x03 | u8 | bits[2:0]=**失败阶段码**，bits[5:3]=槽位号（失败路径 `UTIL_SetBitValue` 写入） |
| +0x04/0x0c/0x14 | ptr×3 | 角色对象指针（`SAVESLOT_Initialize` 清零；失败时保持 null） |
| +0x1c | i8 | 主控角色索引（hero_index） |

**失败阶段码**（`SAVE_LoadSaveSlot` 0x1298dc 逐级短路）：

| 码 | 阶段 | 含义 |
|---|---|---|
| 0 | load_data | 文件打开/读取/解密/校验和失败（文件不存在时同时清 b2=0） |
| 1 | block_table | 块 0 目录项不可读 |
| 2 | information | 块 0 解析失败 |
| 3 | validation | **版本 >5 或槽位号不匹配（不兼容）** |
| 4 | block_table | 块 1 目录项不可读 |
| 5 | player | Player 块解析失败 |
| 6 | mercenary_slot | 主佣兵槽索引 <0 |
| 7/8/9 | character | 角色 0/1/2 加载失败（3 位字段对 8/9 截断为 0/1） |

## 5. UI「存档损坏」判定与进档崩溃机制

- **UI 判定**：`SAVESLOT_DrawSlotText`（0x14ce38）`ldrb [slot+2]; cmp #1` → b2==1 分支显示「存档损坏」。
- **原版进档保护**：UI 层只在 b2==2 时放行进档；b2==1 显示损坏不可进入。
- **API 崩溃根因**（2026-08-27 存档 0 事故）：模块旧检查 `b0==0 && b2==0` 把 b2==1（损坏）误判为「存在」，
  直接调 `GAME_StartResumeGame`（0x1002e8：写当前槽全局 + `STATE_Set(5)` + `GAMESTATE_SetState(3)`）。
  状态机推进世界加载时角色指针为 null（Initialize 清零、LoadSaveSlot 失败未回填）→ 空指针解引用 → 进程崩溃。
- **模块预检唯一可靠依据 = 游戏自身判决（b2/b3）**：与 UI 显示严格一致，且经历真实解析全程（解密/校验和/块表/版本/角色），
  任何自研字节级重解析都不如它权威。`SAVE_CreateSaveSlot`（0x129b38）每次调用都会重新加载 3 槽刷新判决。

## 6. 写入路径（非原子，损坏风险来源）

`SAVE_Save`（0x129600，内部先校验 SV_GoldGet/StatPoint/SkillPoint，失败返回 0 不写盘）
→ `SAVE_SaveData`（0x1290c0）→ `SAVE_SaveDataAsKey`（0x129050）→ `SAVE_SaveDataAsNameAndKey`（0x128f50）：

```text
ENCRYPT_Process2(就地加密) → FILE_Open → FILE_Write → FILE_Close
```

**无临时文件、无原子 rename、无备份**——写入中途进程被杀/断电 = 截断或半新半旧文件 → 校验和不匹配或解析失败 → 存档损坏。
这是后续 P1 存档备份设计需要覆盖的原版缺陷。

## 7. 模块预检（`enter_slot` 内部行为）设计依据

- 预检 = `SAVE_CreateSaveSlot()` 刷新判决 + 读槽结构 b2/b3，**对存档文件零写入**（原版加载全程只读文件）。
- 判决映射：b2==0 → `missing`；b2==2 → `valid`；b2==1 且阶段码 3 → `incompatible`；b2==1 其余 → `corrupt`（带阶段名）。
- `enter_slot` 内部硬门禁：仅 `valid` 放行 `GAME_StartResumeGame`；其余返回结构化错误（verdict/stage/enter:false），
  从机制上杜绝 §5 的崩溃路径。

## 8. 模块侧 sidecar 容器级恢复（仅保模块数据，不覆盖原版 save*.dat）

模块侧仅在 `store/ModuleSaveStore.kt` 保留 AtomicFile + last-good 容器级恢复，用于模块自有 section 数据的可靠写入。
原版 `save*.dat` 字节的备份、恢复和导出能力已删除，新的备份设计记录在 `../../development/planning/backlog.md` 的 P1，待后续单独实现。

## 9. 2026-08-27 存档 0 取证

- 真机（`<设备序列号>`）当前原版文件为
  `/data/data/com.com2us.inotia4.normal.freefull.google.global.android.common/fcea920f7412b5da7be0cf42b8c93759/save0.dat`，
  文件大小 3711 字节，拉取副本保存在 `.tmp/save-forensics/save0.dat`。
- 当前文件字节证据：`L=N-3=3708`；`sum(ciphertext[0..L)) & 0xff = 0x75`，文件 checksum 字节为 `0x75`，两者匹配；seed=`0xf9`，尾字节=`0x1a`。
- 部署 v0.6.12 后，主菜单调用 `POST /api/system/enter_slot` 会在内部执行相同完整性检查；原版 `save_slots` 同样报告 slot 0 存在。
- 模块 sidecar `slot-0.module-save` 与 `slot-0.module-save.last-good` 均存在（各 4466 字节）；当前未发现 sidecar 损坏证据。
- 事故期间的 tombstone `tombstone_00`（11:11）、`tombstone_31`（11:09）、`tombstone_30`（11:07）均为：
  `SIGSEGV / fault addr 0xe / MAP_AddNPCItemLocation+232 → MAP_LoadLayer → MAP_Load → SAVE_Load`。
  这证明旧 `enter_slot` 确实把失败进档路径推进到了地图加载空指针崩溃。
- 事故时点的原始存档字节、模块旧日志已被后续运行覆盖，无法仅凭当前 3711 字节文件确认当时失败发生在解密校验、块解析、版本校验或角色解析层；不得对事故原始根因作未经证据支持的归因。
- 当前文件的 container checksum 与游戏预检均通过，因此当前可取得的证据结论是「当前存档有效；历史崩溃调用链已确认；历史损坏层待原始快照或更早日志」。
- 后续真机复测发现：原版槽状态为 `slot_state=2`，但模块同时读取到槽结构原始 `slot+0x04` 指针为 null，`SAVESLOT_GetHero` 也返回 null；原版 UI 因此走 `UNKNOWN` 分支（`SAVESLOT_DrawSlotText` 0x14d0dc）。这确认不是 Getter 计算错位，但 `slot_state=2` 按 `SAVE_LoadSaveSlot` 的控制流又表示角色加载函数已返回成功；因此当前证据只能确定“最终槽结构没有主控对象指针”，还不能区分角色块缺失、角色解析后被清零、或预检刷新后的生命周期覆盖。
- 结合 `SAVESLOT_Initialize`（0x128958）与 `SAVE_LoadSaveSlot`（0x1298dc）完整分支，当前 save0 已通过单槽加载实验确认：`main_merc_slot=0`，Player 块的三个角色索引为 `-1,-1,-1`。负值分支（0x129aa4→0x129b10）写入 null 并继续循环，不会把槽判为失败；而 `SAVESLOT_Initialize` 会把 `slot+0x1c` 默认置为 0。因此 save0 的直接根因是“主佣兵索引存在，但三个队伍角色引用均为空”，形成 `slot_state=2 + hero_index=0 + slot[+0x04]=null`；这不是文件中保存了错误的动态指针。
- v0.6.12 修复后，`POST /api/system/enter_slot` 返回 `corrupt / character / error_code=7 / raw hero pointer is null`，进程保持存活、screen 保持 `main_menu`，不再调用 `GAME_StartResumeGame`。历史 Frida 角色加载阶段探针未作为当前项目脚本保留，尚未取得 `SAVE_LoadCharacterInfoBlock/CHARSYSTEM_Allocate/SAVE_LoadCharacter` 的逐阶段返回值。
- 只读内存验证实验（未写 save0）：临时把 `SAVE_LoadPlayer` 的角色索引写入指令替换为 0，重新调用一次原版加载后，save0 变为 `verdict=valid`、`hero_level=1`、hero 指针非空；但该单条指令实际作用于 3 次循环，Player 索引读回 `0,0,0`、`hero_index=2`，产生三个重复角色槽，未作为修复保留。该结果仅证明角色记录 0 可以被原版解析、分配并挂接；正确修复仍需在 `SAVE_LoadPlayer` 返回后的循环前只修改第 0 项。

## 10. 真机故障注入结果（v0.6.12）

在确认 slot1 原先不存在后，使用与原版文件相同的应用 UID/权限写入测试文件：

| 样本 | preflight | enter_slot | 进程状态 |
|---|---|---|---|
| 32 字节截断文件 | `corrupt / load_data / error_code=0` | `ok=false, slot corrupt` | `/api/health` 仍为 `ok=true` |
| 3711 字节随机密文（校验和不匹配） | `corrupt / load_data / error_code=0` | `ok=false, slot corrupt` | `/api/health` 仍为 `ok=true` |
| 清理测试文件后 | `missing` | `slot empty` | `/api/health` 仍为 `ok=true` |

两种损坏样本均未进入 `GAME_StartResumeGame`，没有产生新的崩溃；测试文件已删除，slot1 恢复为 missing。该结果覆盖了截断、校验失败、拒绝进档和清理恢复，但尚未覆盖版本不兼容、内部块/角色解析失败样本，也未替代真机验证。

## 11. save0 运行时恢复与原生保存修复（2026-08-27）

### 11.1 原始文件保护

- 在任何原生保存操作前，从真机拉取未修改的 save0：
  `.tmp/save-forensics/save0-before-recovery-20260827.dat`。
- 原始文件大小为 3711 字节，SHA-256 为
  `034be740d395db25aaea6e509c0745952956e99f34d26f5409e694b7f9f3c59d`。
- 原生保存前再次从设备校验该 SHA-256，并保存
  `.tmp/save-forensics/save0-pre-native-save-20260827.dat`；哈希仍一致。
- 本轮没有用 save1 覆盖 save0，也没有在原生保存前直接修改 save0 文件。

### 11.2 可逆运行时 patch 试验

1. 仅修复 `SAVE_LoadSaveSlot` 返回后的槽指针：preflight 变为 `valid`、`hero_index=0`，但真实进档仍在 `MAP_AddNPCItemLocation+232` 崩溃。
2. 通过 ELF 反汇编确认崩溃指令为：

   ```asm
   MAP_AddNPCItemLocation+0xe8:
       ldr x0, [PLAYER_pMainPlayer]
       ldr x0, [x0]
       ldrb w0, [x0, #0xe]
   ```

   崩溃现场 `x0=0`，调用链为
   `MAP_AddNPCItemLocation → MAP_LoadLayer → MAP_Load → SAVE_Load`。

3. 随后确认真正的进档链会再次执行 `SAVE_Load → SAVE_LoadCharacterAll`，因此 patch 改为覆盖：
   - `SAVE_LoadSaveSlot+0x1b8/+0x1bc/+0x1c0`：临时把第一个槽挂接为角色 0，并跳过空队友循环；
   - `SAVE_LoadCharacterAll+0x6c`：只将第一个角色索引视为 0；
   - `SAVE_LoadCharacterAll+0x84`：处理第一个角色后跳过两个已知为空的队友槽。
- 该 patch 只改进程内指令，所有指令均先校验原始 opcode，并在实验结束后回退；没有把 patch 作为最终交付逻辑保留。
 - 正确 patch 版本在启动弹窗点击 `(<启动弹窗坐标>)` 后验证成功：
   `screen=world`，主角“凯恩”可读取，等级 1、HP 1672/1672、MP 200/200，`party_count=1`（其余队伍槽为空），进程保持健康。
- 可复用脚本已登记为 `scripts/analysis/frida/save0-repair-patch.js`；执行前必须按当前 `libgame.so` 反汇编核对五个 opcode，成功保存后重启进程移除 patch。

### 11.3 使用游戏自身保存完成文件修复

- 在 `screen=world`、主角和队伍数据可读、且 save0 原始哈希已复核后，只调用一次游戏原生保存接口：
  `POST /api/system/save`。
- 原生保存返回：`ok=true`，`map_id=30`，`x=272`，`y=384`，`leader_slot=0`，`party_count=1`。
- 保存后文件副本：`.tmp/save-forensics/save0-post-native-save-20260827.dat`，大小仍为 3711 字节，SHA-256 为
  `50fab681d7088489b03aae55240507a162a6fcf0abadd13bc166d0f86be7a341`。
- 回到主菜单后再次调用 `enter_slot` 触发内部预检 save0：
  `verdict=valid`、`enter=true`、`slot_state=2`、`map_id=30`、`hero_level=1`、`hero_index=0`，并且 `player_indices=0,-1,-1`。

### 11.4 重启验收

- 移除临时运行时 patch、重新构建并部署模块。
- 重启游戏后按真机既定前置点击启动弹窗 `(<启动弹窗坐标>)`，再调用 `enter_slot {"slot":0}`。
- 无 patch 的最终验收仍为内部 `preflight=valid`，`screen_after=world`，`/api/health={"ok":true}`；主角数据可读。
- 结论：save0 已通过“运行时恢复 → 游戏原生保存 → 回主菜单预检 → 移除 patch 后重启读档”闭环修复。原始备份必须保留，后续若继续做地图、装备或技能完整性研究，应以修复后的 save0 和原始备份对照，禁止覆盖原始备份。

## 12. 2026-08-27 再次修复与查询副作用修复

- save0 再次出现无主角状态后，按 §11.2 的五条加载 patch，并额外在目标槽加载完成后将 `player_indices[0]` 恢复为 `0`；否则内存中虽可进入，原生保存仍会把三个角色索引写回 `-1,-1,-1`。
- patch 下进入 save0 后原生保存返回 `ok=true`、`map_id=30`、`leader_slot=0`、`party_count=1`。
- 移除 patch 并重启，在同意页关闭后无 patch 进入 save0；最终 `/api/ui` 为 `screen=world`，`/api/system/info` 显示 save0 `hero_level=1`、`hero_index=0`，`/api/health` 为 `ok=true`。
- 修复 `data_save_slots_json()`：仅主菜单 `state=4` 调用 `SAVE_CreateSaveSlot()` 刷新槽区；world/启动过渡阶段只读槽结构，避免查询 `/api/system/info` 覆盖角色运行时全局。
- 启动确认弹窗的 native `UIPopupMsg` 现在优先于状态机参与 `screen` 判定；`enter_slot` 检测到活动弹窗返回 `ui occupied: dialog_popup`。Android `AgreementUIActivity` 属于 Java 同意页，不由 native `UIPopupMsg` 表示，独立为 `screen=agreement`（Kotlin 层覆盖，与 native 弹窗以 in_world 守卫分域）；`enter_slot`/`create_slot` 在同意页存在时返回 `ui occupied: agreement`，`dialog/select {"action":"ok"}` 定位并点击同意页的「同意」元素关闭。

## 13. 2026-08-28 裸窗口损坏实测与同意页启动拦截

- 时序实测（0.6.13）：冷启动 `13.35s main_menu → 14.01s agreement`，存在 ~0.66s「裸窗口」——native 已 `state=4` 且同意页未出现，`enter_slot` 双门禁全放行。
- 损坏复现（save1，用户批准牺牲）：裸窗口内 `enter_slot(1)` 被接受（13.84s ok），同意页 14.35s 弹出打断读档；关闭同意页后 world 出现但角色丢失，退档后 slot1 `hero_level=0`，re-enter 返回 `slot corrupt / stage:character`——与 §12 save0 事故同源。
- 消失窗口实测（良性）：`select ok` 后 tracker 在 pause 瞬间失明（~1.7s），此时抢进 `enter_slot` 成功进 world，主角在场、存档完好；同意动作已完成后进档链与正常流程等价。
- 修复：`patch/AgreementGate.kt` hook `Instrumentation.execStartActivity`（启动点为 `CheckPermission$1/$2` 持 Activity 调 `Activity.startActivity`，必经此路径），`AgreementUIActivity` 启动仅在「native screen=main_menu 且不在进档宽限期」时放行，否则丢弃；`enter_slot`/`create_slot` 调用前设置 15s 宽限（`AgreementGate.beginWorldLoad()`）。探针失败放行（API 层门禁仍 fail-closed）。
- 拦截后验证：裸窗口 `enter_slot(1)` → `14.30s` 直接 `world`（同意页未出现），主角凯恩在场，退档后 save1 `hero_level=1` 完好；正常启动同意页照常弹出（`main_menu` 时放行）。模块日志出现 `blocked AgreementUIActivity launch (outside main menu or world load in progress)`。
- 未联网时同意页不弹出（用户确认），因此不能用「见过 agreement」做门禁；拦截方案不依赖该信号。
- save1 损坏后未修复，直接 `create_slot {"slot":1,"class_idx":0}` 重建，回主菜单后 `hero_level=1` 恢复。
