# 模块存档容器与保存协调册

> 状态：CURRENT；本册是扩展背包 sidecar、保存调用点、双 journal 和恢复/回滚时序的唯一权威。
>
> 问题 B：未解决 + 已布防。保存协调只减少投影污染和重复提交，不能宣称已经修复原版同号槽丢失。

## 1. 范围和不变量

每个原版存档槽 `0..2` 对应一个模块 sidecar。原版 `save*.dat` 不读取、不写入、不追加、不重命名；模块数据只写具名 section。原版存档成功是模块 section 提交的唯一边界，退出背包、切换页签、恢复投影和普通内存刷新都不是提交事件。

必须保持：

1. sidecar 只存模块数据、版本化 payload 和诊断所需的可序列化记录，不存 native 指针。
2. 原版保存先完成，扩展状态后 commit；原版返回失败或结果未知时不得把 prepare 当 committed。
3. 保存前必须恢复扩展投影和临时容量，确保 `SAVE_SaveInventory` 读取真实原版物理数组。
4. 加载先恢复扩展逻辑状态，再安装视图；`INVEN_pItem` 的原版对象和扩展对象不互换所有权。
5. `module.save.journal` 与 `extensionbags.journal` 是两个不同语义层，绝不按名称合并。

## 2. 现码保存入口与 8 处 patch

### 2.1 八个完整 `SAVE_Save` 调用点

`extension_bag_lifecycle.inc:54-68` 的 `patch_all_save_callsites()` 当前构造 8 个 `SaveCallsiteSpec`，每个 BL 通过 `patch_save_callsite` 改写为 `module_save_game`。VMA 均来自 `game_symbols.h:478-485`，原始指令来自现码 `extension_bag_lifecycle.inc:56-63`。

这里的“prepare journal 完整落盘”只适用于 coordinator 通过 `ModuleSaveCoordinator.prepare`
写入的 `module.save.journal`；它不等于每笔内存 `PendingTransfer` 都已持久化。常规 native
pending 当前明确为 `durable=false`，其状态只会在后续 prepare snapshot 中被捕获，详见 §4.2。

| # | callsite VMA | 原始指令 | 原版 caller | 现处置 |
|---:|---:|---:|---|---|
| 1 | `0xc4488` | `0x9401945e` | `UINpcRevive_Revive_Confirm` | patch 到协调器；复活确认后的完整保存 |
| 2 | `0xcbea0` | `0x940175d8` | `UIQuestMenu_ClearUIInAppProcess` | patch 到协调器；任务流程完成保存 |
| 3 | `0xcbf4c` | `0x940175ad` | `UIQuestMenu_ClearUIInAppProcess` | 同一 caller 的第二个完整保存点，独立校验 |
| 4 | `0x125c88` | `0x94000e5e` | `QUESTSYSTEM_AcceptReivew` | patch 到协调器；接受任务后的保存 |
| 5 | `0x129850` | `0x97ffff6c` | `SAVE_ProcessSave` | patch 到协调器；原版保存状态机 |
| 6 | `0x15d708` | `0x97ff2fbe` | `NetworkStore_AddItem` | patch 到协调器；网络商店完成后的保存 |
| 7 | `0x15da84` | `0x97ff2edf` | `NetworkStore_Process` | patch 到协调器；网络商店流程保存 |
| 8 | `0x15dcf0` | `0x97ff2e44` | `NetworkStore_Process` | 同一流程的第二个完整保存点，独立校验 |

这 8 处是“完整保存请求”的 patch 清单，不是 8 个不同业务。`SAVE_LoadCharacterAll` 内部调用点
`0x129f24` 不属于 `patch_all_save_callsites` 八项，当前 `game_symbols.h` 也没有为该调用点
建立独立 `F_*_VMA` 常量，因此不纳入本表。完整保存函数 VMA 是 `F_SAVE_VMA=0x129600`
（`game_symbols.h:348`），由 `module_save.cpp` 的 `fn_save` 调用。

### 2.2 `SAVE_SaveInventory` 内部 gate

另有一处不是完整保存 caller 的内部 patch：`SAVE_Save` 内唯一 `SAVE_SaveInventory` BL 在 `0x129770`，VMA 和原始指令由 `extension_bag_lifecycle.inc:609-646` 当前代码使用；目标 `SAVE_SaveInventory` 函数 VMA 是 `0x127d8c`（`game_symbols.h:476`）。该 BL 改到 `save_inventory_wrapper`，流程为：

```text
SAVE_Save
  → 0x129770 save_inventory_wrapper
      → 若投影存在：restore_module_view_locked
      → 原版 SAVE_SaveInventory@0x127d8c
```

wrapper 只负责恢复投影后调用原版子步骤，不能据此认为保存已经成功。原版完整 `SAVE_Save@0x129600` 返回值仍由 core 协调器判断。

## 3. `module_save_game` 时序

`core/native/module_save.cpp:62-112` 是完整保存唯一协调实现；`api/native/game_save.cpp:51` 的 API 也只调用它。时序如下：

```text
检查 game_in_world / fn_save / 当前 slot 0..2
  → thread_local 重入门禁
  → participants_snapshot
  → 每个 participant.prepare(slot, tx)
  → 原版 fn_save() = SAVE_Save
       → 内部 SAVE_SaveInventory gate 恢复投影
       → 原版序列化真实物理库存
  → result == 0：abort 已 prepare participant，返回失败
  → result != 0：每个 participant.commit(slot, tx)
  → 全部 commit 成功才返回 true
```

关键现码：

* `module_save.cpp:63-69` 拒绝非游戏态、无 `fn_save` 和非法 slot；重入时当前线程直接返回 true，避免保存面板/状态机递归二次完整保存。
* `module_save.cpp:71-87` 为 participant 生成 `save-N` transaction id，按注册顺序 prepare；中途失败按已成功 prepare 的逆序 abort。
* `module_save.cpp:89-97` 只把明确的 `fn_save()==0` 当已知失败并 abort；当前代码没有“结果未知”回调，进程中断只能依赖 journal/启动恢复策略，不能误判为成功。
* `module_save.cpp:100-111` 按注册顺序 commit；任一 commit 失败只返回 false，已完成的前序 commit 不会自动逆向覆盖，故 sidecar 提交必须原子化。
* 扩展 participant 在 `extension_bag_public_runtime.inc:249-275` 注册：prepare 会 ensure/load、恢复投影、恢复 pending，然后进入 JNI；commit 通过 `extension_bag_persistence.cpp:199-237` 写 section，成功后才清 pending/dirty。

### 3.1 prepare/原版/commit 的责任表

| 阶段 | 扩展背包动作 | 禁止事项 |
|---|---|---|
| prepare | 锁内确保状态已加载；恢复模块视图；调用 `recover_pending_transaction_locked`；序列化当前 `State`；Java coordinator 写 `module.save.journal` | 不调用原版完整保存；不清 dirty 作为已保存；不把视图状态写进原版物理数组 |
| 原版保存 | `save_inventory_wrapper` 解除投影和临时容量；原版读 `INVEN_pItem` 并写 `save*.dat` | 不在 `SAVE_SaveInventory` 子调用直接提交 sidecar；不以子调用返回代替完整保存结果 |
| commit | 校验 coordinator journal 的 tx/slot/stage/section；原子替换 `extensionbags.items` 并删除 coordinator journal；成功后清 pending/dirty | 不重读不匹配 tx；不将失败/中断结果标记 committed |
| known failure abort | 删除匹配 tx 的 `module.save.journal`，保留已提交 section | 不删除不匹配 tx 的 journal；不擅自清理功能 pending |

## 4. 双 journal：必须分开理解

### 4.1 `module.save.journal`：协调 journal

它由 `ModuleSaveCoordinator` 所有，section 名和版本在 `ModuleSaveCoordinator.kt:13-16`：`module.save.journal` / v1。它解决的是“这次完整原版保存与多个 module section 提交到哪一步”。prepare 写入 JSON：`transactionId`、`slot`、`stage=0` 和参与 section 的 name/version/base64 payload，代码 `ModuleSaveCoordinator.kt:23-45`。

commit 读取并严格校验 journal：tx、slot、stage、section 数量、name 和 version 必须匹配当前 participant；随后 `ModuleSaveStore.replaceSections` 替换 section 并删除 `module.save.journal`，代码 `ModuleSaveCoordinator.kt:48-90`。已知原版失败时 `abortKnownFailedSave` 只删除同 tx 协调记录，代码 `:92-103`。

它不理解 bag/slot、payload 方向或对象所有权，不保存 native 指针，也不决定事务重放/回滚。当前实现没有独立的启动 `recover()` 调用；因此进程在“原版成功但 commit 尚未完成”窗口中断时，残留协调 journal 的自动恢复语义仍是缺口。

### 4.2 `extensionbags.journal`：ExtensionBagJournal

它是扩展背包事务层的功能 journal，section 常量在 `virtual_bag_transaction_types.h:17`，Kotlin 存储辅助在 `ExtensionBagJournal.kt:15-70`。其 payload 描述一笔扩展事务：transaction id、阶段、generation、方向、source/target bag/slot、目标 payload 和 source payload；native 编解码在 `virtual_bag_serialization.inc:73-195`。

它解决的是“某笔跨原版/扩展移动在 crash 或恢复时应重放、回滚还是隔离”，不是“多个 module section 是否完成保存”。功能 journal 的 stage 与 `module.save.journal` 的 stage 没有可互换含义。

当前工作区还存在一个重要现状：`ExtensionBagJournal.kt` 是可用的 section 存储 helper，但常规 native 事务 `txn_record_pending_locked` 当前明确写 `durable=false`，见 `extension_bag_runtime.inc:560-576`；`state.pending` 会随 `extensionbags.items` 的状态 JSON 被 coordinator prepare 捕获，见 `virtual_bag_state_json.inc:36-55`。所以：

* 不能宣称每次内存 pending 都已写入 `extensionbags.journal`；
* 不能把 `module.save.journal` 改名或合并为功能 journal；
* 后续若启用独立功能 journal，必须定义它与 `state.pending`、generation 和 sidecar commit 的原子关系，再更新本册和 rulebook。

一句话结论：`module.save.journal` 记录“原版保存—模块 section 提交”的协调进度；`extensionbags.journal` 记录“扩展移动事务如何恢复”的业务进度，二者不得按名称合并。

## 5. sidecar 格式

### 5.1 容器格式

`ModuleSaveStore.kt:34-43,162-191` 使用大端 `DataOutputStream`：

```text
u32 magic = "MSAV"
u16 formatVersion = 1
u8  slotId
u64 generation
u16 sectionCount
repeat sectionCount:
  u8  nameLength
  u8[nameLength] ASCII sectionName
  u16 sectionVersion
  u32 payloadLength
  u8[payloadLength] payload
  u32 payloadCrc32
u32 containerCrc32       // 覆盖前述全部字节
```

section 名满足 `[a-z0-9._-]{1,64}`；单 section 不超过 1 MiB，容器不超过 4 MiB；未知 section 在替换时保留，只有显式 `removeSection` 才删除。读取时校验 magic、格式、slot、重复名、版本、长度、section CRC、container CRC 和尾部无多余字节，代码 `ModuleSaveStore.kt:194-227`。

### 5.2 section 约定

| section | 版本 | 内容 | 所有者 |
|---|---:|---|---|
| `extensionbags.items` | 4 | 5 个扩展逻辑袋的类型、容量、descriptor、数量、base64 原版 payload、必要的 pending 状态；唯一读写目标 | `ExtensionBagUiBridge`；常量 `ExtensionBagUiBridge.kt:16-21` |
| `module.save.journal` | 1 | 本次完整保存的协调 envelope 和待提交 section 快照 | `ModuleSaveCoordinator` |
| `extensionbags.journal` | 1 | 扩展功能事务 journal；当前 helper 存在，但常规 native pending 尚未由它持久化 | `ExtensionBagJournal.kt:17,69` |

`extensionbags.items` 只保存序列化 payload，不保存 `g_module_objects`、`g_module_object_handles`、控件指针或原版 `INVEN_pItem` 记录；加载时用 `SAVE_LoadItem` 重建对象，代码 `extension_bag_runtime.inc:459-468`。

**加载兼容性（当前实现）**：读取只认 section 名 `extensionbags.items`，数据一律按 v4 布局解析（未知版本同样尽力解析，解析失败才回退空状态），不做旧版本迁移或回写；容器 `formatVersion` 必须为 1；不使用游戏签名/安装身份门禁。payload 字段按 JSON 字符串反转义后再 base64 解码（兼容 Android `org.json` 把 `/` 序列化为 `\/`），否则带 `/` 的 payload 会被判长度不足、整状态回退空。代码 `ExtensionBagUiBridge.kt:loadStateJson`、`virtual_bag_serialization.inc:json_unescape_into`。

### 5.3 文件、原子性和损坏处理

根目录是 `context.getExternalFilesDir(null)/module-saves/`，文件由 `ModuleSaveStore.kt:255-277` 生成：

* `slot-{n}.module-save`：当前主 sidecar；
* `slot-{n}.module-save.last-good`：更新主文件前保存的有效副本；
* `slot-{n}.module-save.corrupt.<timestamp>`：主文件或 last-good 校验失败后的隔离副本。

写入经 Android `AtomicFile`。更新 section 时先把当前有效主文件复制到 last-good，再原子替换主文件；主文件坏时隔离并尝试 last-good；两者都坏时以空容器继续，不影响原版存档，代码 `ModuleSaveStore.kt:125-159,230-251,280-285`。新建原版槽调用 `resetSlot` 删除主文件和 last-good 后建立 generation=1 的空容器，代码 `:115-123`。

## 6. 加载、恢复和 pending 隔离

### 6.1 正常加载流

当前 native 流在 `extension_bag_runtime.inc:471-497`：

```text
发现 current_save_slot 改变
  → module_cache_clear_preflight_locked
  → restore_module_view_locked（若有视图）
  → clear_module_cache_locked
  → g_virtual_bag_state = {}
  → extension_bag_load_state_from_store(slot)
       → Java 读 extensionbags.items
       → 校验版本、JSON 和 payload
       → 恢复逻辑 descriptor/capacity/pending
  → 清理无 pending 的残留 module view state
  → 后续 enter/draw 才 materialize 并安装投影视图
```

这是“先扩展状态、后视图”：`ensure_state_loaded_locked` 在安装控件前加载逻辑状态；`install_module_view_locked` 再按 descriptor 物化对象并借给控件，代码 `extension_bag_render.inc:498-542`。原版袋窗口容量只在视图安装期间临时改变，恢复时先归还 view borrow，再写回真实容量并刷新原版区域，代码 `extension_bag_render.inc:579-608`。

sidecar 读档的数量语义独立于运行时堆叠配置：descriptor 的 canonical `count` 只接受
`0..999`，越过绝对上限时收敛到 `999` 并记录日志；不会按当前 `stackLimitIncrease`
把已有值截到 `99`。可堆叠类别的 payload 数量位按 S2 布局解释（无版本标识，唯一
布局），并与 descriptor 的 canonical 值对齐；非堆叠类别的 payload 数量位按原始字节
保留。运行时 `99/999` clamp 仅用于新建、合并、消费和派生操作，因此切换配置后重新
读档不会改写或截断已有数量。

### 6.2 pending 恢复

`extension_bag_load_state_from_store` 在 `extension_bag_persistence.cpp:56-127` 解析 state JSON 中的 pending。malformed 或非法 transaction domain 不进入事务执行，而是构造 isolation record，保留 direction、源/目标、payload 和 transaction id 后清 pending；payload 校验失败同样隔离。隔离记录当前是内存诊断字段，不因 `state_json` 默认参数写入 sidecar，依据 `virtual_bag_state_json.inc:56-57`。

进入世界后，`virtual_bag_module_save_prepare` 会先 `recover_pending_transaction_locked`，顺序为：域校验 → payload 校验 → 恢复原版视图 → 按方向决定 action。ext→orig 只有在目标 payload 已存在，或重建对象并经 `SaveItemOnEmpty` 写入、再序列化确认目标后才清 pending；失败保留 pending，代码 `extension_bag_runtime.inc:646-778`。ext→ext 不重放到原版，只按 rollback 语义清理非法/损坏 pending。

恢复是隔离而非猜测：任务袋、越界 bag/slot、非法 payload、目标内容不匹配时不自动“修正”为普通袋，也不调用 `RemoveItemDirect` 删除未知对象。加载失败则使用空扩展状态并记录日志，原版 `save*.dat` 仍可继续读取。

### 6.3 `module.save.journal` 恢复缺口

当前 `ModuleSaveCoordinator.kt` 提供 prepare、commitAfterOriginalSave、abortKnownFailedSave，但没有被启动路径调用的 `recover`。因此应区分：

* `extensionbags.items` 内已被成功 commit 的 pending：由 native 加载/恢复流处理；
* `extensionbags.items` 内随 prepare snapshot 捕获、但未 commit 的 pending：是否可安全采用，取决于原版保存结果和功能 journal 证据；不能无证据自动应用；
* 仅残留 `module.save.journal`：当前代码没有完整的原版保存探针和 section 重放器，必须记录为待补缺口，不能写成启动时已自动恢复。

**未定-需真机证据**：进程在“原版 `SAVE_Save` 已成功、participant commit 尚未完成”窗口中断后，
现有文件、last-good、协调 journal 与原版槽内容的最终组合状态，不能由静态代码判定为安全重放或
回滚。取证方法：在 `module_save.cpp:89-111` 的原版返回与 participant commit 之间注入可控
force-stop，保留 `module.save.journal`、`extensionbags.items`、last-good、原版 `save*.dat`
和重启日志，再按 tx/slot/stage 对比下一次加载结果。

### 6.4 S2 数量编码（当前实现：无版本标识、统一 S2）

sidecar 状态 JSON 不携带版本标识字段，数量位只有 S2 一种布局，由
`virtual_bag_state_json.inc` 的 `state_json`/`parse_state_json` 统一读写：

1. 布局：`a` 落 bits22–24、`b` 落 bits25–31，`count=128a+b`，编码总量范围
   `0..1023`，业务上限 `999`。sidecar 与 descriptor（canonical 域）的读写统一经
   `s2_read_count`/`s2_write_count`；运行时操作面（展示/查询/消耗/合并/卖出/创建）
   统一经模式感知层 `effective_read_count`/`effective_write_count`/`effective_clamp`/
   `effective_view_count`（启用态等价 canonical 全量、关闭态低 7 位视图且只写 b，
   R-47 决策 b）。模式绝不影响 sidecar 的解码、编码或 canonical 对齐。
2. 数据面：①sidecar 状态 JSON 中可堆叠 descriptor 的 canonical count 与 payload
   数量位（载入时 payload 与 canonical 不一致则以 S2 重编码对齐）；②原版物理袋与
   扩展物化对象的 `+0x10` 数量字由操作层写点（S2-P3 门控/进位）与读侧 getter hook
   （S2-P2）按 S2 读写。非可堆叠类别不参与任何数量位段改写：宝石选项位 bits18–23、
   袋容量位 bits0–24、装备 marker 位 bits25–31 均逐字节保留。
3. 无迁移代码：S1→S2 迁移函数、原版袋 `+0x10` 扫描状态机与 `encoding_version`
   字段均已删除；历史 sidecar 若携带 `encodingVersion` 字段，解析时宽容忽略（不
   报错、不迁移），存量数据一律按统一 S2 语义读取。当前存档视为已迁移。
4. 存量损坏值：历史漂移写坏的 canonical（如 count=705）在 canonical 语义下读档
   原样保留，不经读档自动修正；恢复需经操作层写点改写或手动修 sidecar 后保存。
5. payload 身份布局（`SAVE_SaveItem@0x1274f0`）：`[0]` 长度前缀、`[1..8]` u64 UID、
   `[9..10]` u16 I_TYPE 位域、`[11..14]` u32 数量位域（bits22–31）、`[15]` I_MAGIC_RATE、
   `[16]` I_SOCKET、`[17..18]` u16 I_ENCHANT、`[19..]` 词缀链。合并/收编身份判据排除
   UID 与数量位两个每实例可变字段（`mergeable_identity_equal`），原版判据只比较类别与
   可堆叠位，详见规则册 R-53。

**当前依赖与未决**：`+0x10` 的读侧解码依赖 S2-P2 的 `ITEM_GetCumulateCount@0x106094`
getter hook（模式视图），写侧持久化依赖 S2-P3 的类别门控与进位。`pending.payload` 的
数量位随恢复重放路径按统一 S2 处理。S2 专属真机卡为验证矩阵 `VM-32`、`VM-35`
（读档跨配置往返与模式不变性）与 `VM-36`（关闭态低 7 位视图与 `a` 保留）。

## 7. 失败路径与问题 B

### 7.1 失败分类

| 失败点 | 当前处理 | 结论 |
|---|---|---|
| prepare 写 journal 失败 | participant prepare 返回 false，已 prepare participant 逆序 abort | 不调用原版保存，不提交 section |
| 原版 `SAVE_Save` 返回 0 | `module_save_game` abort prepared participant 并返回 false | 保留已提交 section；未提交快照不生效 |
| 原版保存结果未知/进程中断 | 当前线程 guard 只恢复内存活动状态；依赖现有 section、功能 pending 和人工/后续恢复策略 | 不得标记 committed；coordinator 自动恢复仍未实现 |
| commit section 失败 | 返回 false；`ModuleSaveStore` 的单次替换依靠 AtomicFile | 前序 participant commit 不自动逆向，新增 participant 前需保证原子提交策略 |
| 主 sidecar 损坏 | quarantine primary，回退 last-good；都坏则空容器 | 不污染原版存档 |
| payload 无法重建 | `load_item_payload_exact` 拒绝并记录原因；pending 按隔离处理 | 不把坏对象交给原版对象池或物理数组 |

### 7.2 “物理空槽被固化”的当前机理边界

问题 B 的口径必须是“未解决+已布防”。当前可确认的根因假设是：

1. 扩展投影只应占 UI 控件，不应改写 `INVEN_pItem`；但原版 TouchHandle/移动链仍可能在投影 session 中进入物理移动或释放路径。
2. `call_original_event_with_inventory_guard` 在原版事件前后比较 `0..5×16` 指针快照；检测变化时恢复的是指针数组，代码 `extension_bag_lifecycle.inc:116-166`。
3. 若原版事件已经清槽、释放或破坏了原指针所指对象，恢复指针并不能恢复对象内容。之后 `SAVE_SaveInventory@0x127d8c` 的序列化过程可能继续读取被毁内存，读出的“空/坏”状态便会被原版保存固化。
4. `save_inventory_wrapper` 能在保存前恢复投影，但它无法重建已经被原版释放的对象，也没有证明快照数组恢复覆盖所有对象内容和所有并发写入。

因此 0x18 单一 owner、物理快照恢复、`orig_event pre/post` 插桩只是防线，不是修复证明。H3/H4 复测日志仍缺少 `ERROR physical inventory mutation` 和 pre/post digest 触发证据；不得将保存册写成“保存已修复”。

## 8. 拾取、奖励、商店和保存交互边界

### 8.1 拾取和奖励

`CHAR_PickItemAll@0xec4d8` 的模块导航旁路只复刻掉落数组筛选和 `NOTIFIER_Add`，主线程回调最终仍走 `INVEN_SaveItem`，见 `game_world_movement.inc:4-35`。SaveItem wrapper 因而是单件入库漏斗，但不替代任务奖励分类、掉落对象释放、通知线程和多产物回滚。

任务物品维持袋 `5` 原版语义；普通奖励先尝试物理袋 `0..4`，原版失败才允许扩展 adopt。保存协调不把“奖励调用了 SaveItem”解释为扩展 section 已提交：只有后续完整 `SAVE_Save` 成功才 commit。

### 8.2 商店

商店扩展视图会临时改原版袋容量字来复用控件绘制。`save_item_wrapper` 在任何原版写入前调用 `extension_bag_store_restore_for_original_write`，先还原窗口和投影；否则原版可能按放大容量把商品写入真实容量外，视图恢复后表现为物品丢失。商店购买的两处 `FindSaveSlot` gate 只负责放行，最终所有权转移仍由 SaveItem wrapper 决定。

商店卖出不触发原版完整保存，也不直接提交 sidecar；它更新扩展逻辑状态并等待后续完整保存。卖出失败先退款/减回，详情和 popup callback 必须清理；保存中若存在 in-flight sell，commit 前仍需以当前 descriptor/object 身份复核。

### 8.3 保存中的 in-flight 事务

保存 prepare 前必须恢复扩展视图；`virtual_bag_module_save_prepare` 还会尝试恢复 pending。prepare snapshot 是逻辑状态的不可变序列化副本，保存期间不能继续用已改变的 UI 投影或失效对象覆盖它。当前实现通过 `g_virtual_bag_mtx` 串行化扩展 participant，但 Java sidecar 调用发生在该锁内，见 `extension_bag_public_runtime.inc:249-255`、`extension_bag_persistence.cpp:161-197`；这形成 native lock→JNI→ModuleSaveStore lock 的跨层耦合，后续必须由 rulebook 登记并验证重入/阻塞边界。

保存成功后 commit 重新序列化当前 state，而不是直接从 participant prepare 参数传入 payload；因此 commit 前若有未被禁止的并发状态变化，可能出现“原版存的是 prepare 前物理状态，sidecar 是 commit 时逻辑状态”的窗口。当前主线程锁和保存 participant 顺序是防线，但不构成并发模型证明。

## 9. 验收与维护

* 每次修改保存入口，先按 `game_symbols.h:476-486` 和 `extension_bag_lifecycle.inc:54-68,609-646` 对码，确认原始指令、patch 数和 `fn_save` 返回语义。
* 每次修改 sidecar，验证 MSAV/version/slot/generation、section CRC、last-good、未知 section 保留；不得写入 native 指针。
* 每次修改恢复，分别覆盖合法 pending、非法 domain、坏 payload、目标已存在、目标写入失败、原版保存失败和 commit 失败；记录是 rollback、replay 还是 isolation。
* 问题 B 验收必须保留“未解决+已布防”字样，并补采 H3/H4 pre/post digest 与 physical mutation 日志后再评估。
* 本册不要求本批 Android 构建；主代理按仓库阶段规则执行 `git diff --check`、相关 Host tests、Debug 构建和真机保存/重启验收。
