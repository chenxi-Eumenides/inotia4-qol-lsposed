# 开发待办总清单

## 完成标准

- **只有真机验证通过才算完成**：实现后必须在真机（当前主力设备，完全 API 操控）验证行为符合预期且无崩溃。
- **验证结论写入对应主题文档**（产出类型 → 归属）：
- 写操作函数签名/VMA/调用机制 → 随代码（game_symbols.h/game_access）或 `../refactor-plan.md` 域映射
- 数据结构/偏移/全局 VMA → 随代码（symbol_registry.h）或 `../refactor-plan.md`
- 操作分级判定/实现状态 → `../../reference/api-reference.md` §八（op 域，操作分级）
- 端点规格（路由/参数/返回）→ `../../reference/api-reference.md`
- 静态表字段语义 → `../../reference/api-reference.md`（静态数据章节）
  - UI 点击坐标 → `scripts/device/touch_automation.py`（2026-08-16 原 docs/reference/ 系列已清理）
  - 部署/模拟器结论 → 历史记录已归档，不再维护（2026-08-16 清理）
- **未真机验证 = 未完成**，状态标为 `待真机验证` 而不是完成。
- **完成一项后删除该条**，不留历史；新缺口随时在此追加。

状态标记：未开始 / 探索中 / 实现中 / 待真机验证

---

## P0 阻塞级

> ✅ **2026-08-17 v0.6.6：本批次 P0 四项全部完成并真机验证，按完成标准删除**：
> ① **地图出口 API**：`GET /api/world/maps/{map_id}/exits` 静态端点实现（DataController 读 StaticData maps/exits.json），真机切图验证 map0→30 出口 targetMapId==切图后实际地图 ID；
> ② **NPC 对话选择框**：逆向确认选择框型 NPC 选项存储在 NPCTASKLIST 槽数组（32×16B：+0 type/+2 id/+8 文本指针），data_dialog_content_json 原误用 UICHOICE 判定致输出 next；修复为输出槽选项列表 + 关闭选项（close action → data_op_panel_close），真机验证杂货商人 [杂货商店/关闭] 两选项，index=0 进商店、close 关面板；
> ③ **tutorial_pause 阻断**：残血→state=6→tutorial_pause 真机复现，move 的 tutorial_block_error 已能取消；修复 data_ui_screen 检测 state==6 即自动 tutorial_cancel 不再返回 tutorial_pause，真机验证写 state=6→screen=world+state=0+move 正常；按键教学（GAMESTATE_PressKeyPlay 劫持按键）不暂停游戏、不影响 API 内存读写，无需阻断；
> ④ **item_type 五类**：逆向确认 ITEMDATABASE 记录 +2 字节 = 用途类型（0-21 装备/22-23 药水/24 卷轴/25 宝石/26+ 消耗·材料·任务·货币），G_ITEMCLASS_DATA 实际指向 ITEMDATABASE（非 36 类 ITEMCLASSBASE）；实现 StaticData.itemType 五类映射，真机验证药水→potion/长剑盾牌→equipment，宝石卷轴消耗品经 frida 运行时 +2 字节核实与静态一致。

| 状态 | 待办项 | 现状 / 卡点 | 需要的探索 / 实现 | 验收标准 | 来源 |
|---|---|---|---|---|---|
| 待定 | **存档完整性检测与安全进档** | 已实现 v0.6.12：原版检查链记录于 `docs/reference/game/save.md`；`save_preflight` 五态判决、hero 指针语义检查、`enter_slot` 内部硬门禁已接线。真机当前 save0 已确认 `slot_state=2` 但 `SAVESLOT_GetHero()==null`，原版 UI 显示 `UNKNOWN`；修复后 enter_slot 返回 `corrupt/character/error_code=7`，进程不崩溃且停留主菜单。截断/随机密文均返回 `corrupt/load_data`；检查不修改 save0。当前仍缺真机验证和其他深层解析样本；事故原始字节已被覆盖。 | ① 逆向并记录游戏原版检查链：存档文件枚举、大小/版本/槽位/校验和、解密、记录边界、角色解析、失败返回和 UI“存档损坏”判定；② 保持只读完整性判定作为 `enter_slot` 内部门禁，不提供独立 preflight API；③ 改造 `enter_slot`：非 `valid` 时只返回结构化错误，禁止调用会触发崩溃的原版进档路径；④ 对当前存档 0 获取原始文件、模块 sidecar、版本/签名/日志，定位究竟是哪一层无法解析及损坏发生时点；⑤ 增加 host 样本测试和真机故障注入，验证检查不会修改存档且拒绝进档不会崩溃。 | 对存在、损坏、缺失、版本不兼容、校验失败、截断、空槽和解析异常样本，API 均在调用原版进档前返回明确结构化结果；不可进入时游戏进程不崩溃、不改变原版存档；可进入样本成功进入；根因与预防措施有字节、日志、调用链证据。未完成真机验证前保持“探索中”。 | 用户 2026-08-27 真机反馈 |
| 待定 | **世界加载完成前保存的崩溃防护** | 2026-08-28 真机 v0.6.14 复现：`enter_slot` 返回 `ok:true` 后世界异步加载（约 1–3 秒），期间调用 `POST /api/system/save`，原版 `SAVE_Save`→`CHAR_GetStatusPoint` 解引用空角色指针，`SIGSEGV fault addr 0x32a`（tombstone_13），崩溃于模块 HTTP-worker 线程，进程退出、API 失联。世界完全加载后（screen=world、money>0、leader 非空）同一路径保存正常。enter_slot 已有弹窗拦截与 preflight 门禁，但 save 侧无对应的世界就绪门禁。 | ① 在 `data_op_save` 增加 fail-closed 门禁：世界未完全加载（角色/leader 指针为空等可判定信号）时返回结构化错误，不进入 `virtual_bag_save_game()`/`fn_save()`；② 真机复现进档后立即 save 的时序，验证防护生效且不崩溃；③ 评估 `virtual_bag_save_game` 内部保存前自检，覆盖 API 保存与自动保存路径。 | 进档后立即调用 save 不崩溃并返回明确错误；世界就绪后保存行为不变；防护覆盖显式与自动保存路径；真机验证通过。 | 2026-08-28 P0 smoke 执行中复现 |
| 待定 | **preflight 初始化未就绪误判** | 2026-08-28 真机 v0.6.15 复现：冷启动后协议同意页出现两页（非典型单页流程），第二页关闭后立即 `enter_slot`，preflight 返回 `slot corrupt/stage=character/error_code=7`，且 `/api/system/game` 报 `hero_level=0`；磁盘 `save0.dat` 自上次成功保存后 mtime/大小均未变（文件完好）。干净重启（单页协议 + 主菜单稳定约 40 秒）后同一文件 `hero_level=1`、进档成功。 | ① 逆向 `SAVE_LoadInformation` 刷新时序：判定 slot 信息（hero 指针）何时就绪、协议/Hive 初始化对它的影响；② preflight 在 slot 信息未就绪时不应判 `corrupt`，应返回 `unknown/初始化未完成` 类中间态并提示重试；③ 真机复现双协议页时序验证修复。 | 双协议页/快速重进主菜单场景下 preflight 不再误报 corrupt；文件确实损坏时仍正确拒绝；真机验证通过。 | 2026-08-28 v0.6.15 验证中复现 |

## P1 高优先级

> 审计高优（稳定/一致性问题）+ 用户指定 P1 的功能与治理项。

### 扩展背包分阶段验收

| 状态 | 待办项 | 现状 / 卡点 | 需要的探索 / 实现 | 来源 |
|---|---|---|---|---|
| 待定 | **存档备份 API（重做）** | 已实现并真机验证：`.qol_save` 备份（解密明文原版 + sidecar 原始字节 + 元数据 + CRC32）；跨槽导入真机实证（save0→slot1/2，角色完整加载，明文仅 slot 字节不同）；导入前备份现存文件、失败自动回滚。2026-09-12 改造：`checksum`（`sha256(origPlain‖module)` 前 12 位小写 hex）为备份标识，端点迁至 `/api/system/backup/export|list|import|delete`，导出按 checksum 去重（命中不写新文件，响应带 `deduplicated:true`），导入/删除按 checksum 定位；**新端点待真机回归**。 | 残余：① backup/* 四端点真机回归（去重、删除、按 checksum 导入）；② 备份重命名未实现；③ 跨设备导入未实测（明文可移植，导入端用本地密钥重加密）；④ `hero_level` 依赖运行时槽结构（主菜单自动刷新），未做离线解析；⑤ 备份容量/清理策略未定 | 用户 2026-08-27 要求 |
| 未开始 | **扩展物品全局裸指针反查 generation 化** | `extension_bag_render.inc:419-430` 仍使用全局裸指针匹配；确认使用 v2 暂以槽位身份反查作为授权遗留。权威路由：[`control-plane.md`](../features/extension-bag/control-plane.md) §4.2 | 为扩展物品反查补 generation/token，覆盖物化、投影、释放和确认回调，消除地址复用窗口 | Oracle 复审授权遗留 |
| 未开始 | **确认使用 token mismatch 后 active 槽滞留** | `module_use_finish_locked()` 发现 generation/item/owner 不一致时只能记录完整诊断；若 `module_use_abort_locked()` 也失败，槽可能继续保持 active，当前无安全恢复机制。权威路由：[`control-plane.md`](../features/extension-bag/control-plane.md) §4.2 | 先裁决可接受的恢复/隔离策略，再实现不释放未知对象的安全清理；覆盖 active、pending_release、generation、handle、owner 线程和 token 全字段 | F4 静态审计 |
| 未开始 | **data_op_equip 与 UI 详情装备路径口径统一** | **缺口-未实现，非刻意边界**：`data_op_equip` 拒绝逻辑袋 6..10，与 UI 详情装备路径口径不一致 | 建议下一步：先裁定 API 口径，再决定实现和验收边界 | 审计报告 |
| 待定 | **未定-需真机证据：SaveItem 全 caller 覆盖** | H-13 `SaveItem` wrapper 已存在，但不能由单一 wrapper 推导所有生产者覆盖；权威路由：[`control-plane.md`](../features/extension-bag/control-plane.md) §2 | 逐 caller 核对拾取、奖励、开箱、解封、拆包、合成、商店、脱装备和快捷键的成功/失败/释放/任务袋边界，并补真机证据 | `inventory-integration-decision-plan.md` §2.2、§3.2；`runtime-architecture.md` §3.6；`verification-matrix.md` §2 VM-19–VM-22 |
| 待定 | **NetworkStore_* 两写点勘察（S2 写侧）** | `native_inventory_hook.cpp` 语义表将 `NetworkStore_InitializeMenuData@0x15b7b0`、`NetworkStore_AddItem@0x15d640` 列为「待勘察」：数量来自网络数据 `+0x18`，离线不可达无法冻结值域；权威路由：[`control-plane.md`](../features/extension-bag/control-plane.md) §2 | 联网抓包/内存 dump 商品表与收货数据 `+0x18` 域，或真机挂回调记录实际数量域；冻结后判定是否需要 S2 写侧回写 | `NetworkStore_InitializeMenuData`/`NetworkStore_AddItem` 数量域有真机证据，结论登记进 `rulebook.md` R-49 与 `runtime-architecture.md` §2.1.1 | 本轮语义表反汇编冻结 |
| 未开始 | **host guard 抽象** | `HostCapacityGuard`（ext→orig 宿主容量 RAII）为事务内局部结构，类似「临时替换原版状态并保证写回」的守卫可能在其他原版调用点复用；权威路由：[`control-plane.md`](../features/extension-bag/control-plane.md) §2 | 评估抽成通用 RAII 守卫模板，覆盖容量字/投影/显示袋等临时替换场景；保持 R-44 锁纪律 | 守卫复用有明确契约，Host 覆盖写回与失败路径 | 本轮 R-54 实现 |

> ✅ **2026-08-16 v0.5.34：static_options 键一致性检查完成并真机验证**：核实 ITEMSTATICOPTBASE.item_id = ITEMDATABASE u16[0]（物品 id，全 1018 条恒 = 记录下标+30，如 95=短剑=rec[65]、105=冷漠匕首=rec[75]），而物品 category = ITEMDATABASE 记录下标——两套键体系差 30，原 buildStaticOptions 直接用 item_id 作 key 而调用方 staticOptionNames(category) 用记录下标查询致错位（category<75 物品词条查不到、≥75 查到错位物品词条）。修复：buildItemIdToIndex 建 id→下标映射桥接。真机验证：短剑 cat65→[力量/敏捷/体力]（修复前查不到）、皮质手套 cat354→[智力/体力/暴击伤害增加率]、木质盾牌 cat390→[力量/暴击率/武器格挡率/盾牌格挡率]，与静态表精确一致。文档 inventory.md §2.4.6 同步修正（原「75=短剑」错误记载）。

> ✅ **2026-08-14 v0.5.16：本批次 8 项 P1 全部完成并真机验证**：
> ① 物品名映射错位检查（假警报：category=记录下标，键已一致，无需改动）；② 全代码库判空审查（审计 346 处 fn_* 引用，修复 3 处遗漏判空）；③ VMA 治理真机验收（修复 symbol_resolver PT_DYNAMIC 定位 load_bias bug——第二 LOAD 段 p_offset≠p_vaddr 差 0x10000，用 base+p_offset 定位致 SIGSEGV，改用 bias+p_vaddr，游戏真机正常启动）；④ 日志系统（LogFile 线程安全 + 全 POST 端点统一操作日志）；⑤ info 端点主菜单报错（native 缓存层 world_only 门 + Kotlin 诚实转发）；⑥ Connection reset（注入 ServerSocketFactory 中和 AndServer 硬编码 SO_LINGER=0，大响应 10/10 稳定）；⑦ 商店系统（数据结构+购买已存在；出售全局已存在；补 DEALSYSTEM_AddSale 逆向签名 + shop/items name 富化）；⑧ 释放技能（确认 cast_skill=底层 CHAR 技能使用链，已实现，仅文档修正）。

| 状态 | 待办项 | 现状 / 卡点 | 需要的探索 / 实现 | 来源 |
|---|---|---|---|---|
| 未开始 | 商店上下文出售（buyback） | DEALSYSTEM_AddSale 签名已逆向（v0.5.16 objdump：AddSale(item)→slot / AddSaleDirect(item,slot) / FindEmptySaleSlot()）；物品所有权转移（背包槽去引用而不 ITEMPOOL_Free）未真机验证 | 验证 UIStore_SellItem 所有权转移语义，实现 shop 出售端点 | 本会话 2026-08-14 |
| 未开始 | OP 剩余 7 个 native 已备函数（无定稿路由，不新增路由） | v0.5.47 门禁 + 4 端点接线（D4）后，仍有 7 个 native OP 函数已实现但无对应定稿路由：`nativeOpAddMoney`/`nativeOpMinusMoney`（金币增量）、`nativeOpAddExperience`（经验增量）、`nativeOpRemoveItem`（移除物品）、`nativeOpSetSkillUsage`（技能自动使用开关）、`nativeRecoverAfterHiveBlock`（蜂巢阻塞恢复）、`nativeQuestList`（任务列表） | 等 api-reference §8 相应端点定稿（或用户裁决接线）后，走 OpApiService 门禁形态接线 | refactor-plan P1-v0.5.47 |
| 未开始 | OpController 剩余 11 个占位端点（NOT_IMPL 501） | v0.5.47 接线后仍占位：quest/accept、quest/complete、skill-point、skill-level、inventory/set-slot、inventory/set-equip、craft/mix-direct、combat heal/rest/revive/hate——结构已定稿，待权限机制与底层实现 | 对应 native 已备者接线（nativeOpSetSkillUsage 等）；未备者先补 native | api-reference §8.2 + OpController |

## P2 中优先级

> 审计中优（并发/错误语义/参数校验）+ 用户实测问题与明确要求的功能 + 进行中的数据缺口（N3/Q2）。

| 状态 | 待办项 | 现状 / 卡点 | 需要的探索 / 实现 | 来源 |
|---|---|---|---|---|
| 未开始 | StaticData 缓存并发安全 | `cache` 为普通 HashMap，多客户端并发 GET /api/system/tables/* 竞争 | 换 ConcurrentHashMap（或 computeIfAbsent 原子化） | 审计 M1 |
| 未开始 | 写操作并发互斥 | 全部 POST 端点无锁，native 锁仅初始化用；并发 move/equip 并发调游戏函数，attach 快照 read-modify-write 竞态 | 写操作全局 ReentrantLock，操作与快照读取同锁 | 审计 M2 |
| 未开始 | native 调用前置 ready 检查 | controller 直接调 external，loadLibrary 失败抛 UnsatisfiedLinkError（Error 捕不到）→ 500 | 入口统一检查 `NativeBridge.ready`，未就绪返回 503 JSON | 审计 M4 |
| 未开始 | op_* 参数校验补齐 + OP 能力隔离 | teleport(x/y/map 无范围)、learn_action(actionId/level 任意、无技能点校验)、sell(price 无约束)、move(x/y 无上限)、exp 截断 int32 校验策略不一致；OP native 实现（money/exp/statuspoint/teleport/sell 任意定价）仅靠「不挂路由」隔离，无权限机制（原 P0 H3 挂靠）。**2026-09-14 新实证**：世界连续传送测试中调用 teleport 复位 API 触发 `SPR_Draw → MAP_DrawLayer` SIGSEGV（世界加载/切图时序期 API 直切图），证据 `.tmp/world-teleport-insufficient/ten-cycle-crash.log`——API 侧切图缺世界就绪门禁，与本轮已修复的面板链路（`UIPopupMsg` 崩溃）无关 | 统一入口边界校验（坐标/槽位/枚举/价格≥0/int32 范围）；teleport/切图类 API 加世界就绪门禁（对照 P0「世界加载完成前保存崩溃防护」模式）；learn_action 先读技能点；OP 隔离：加全局开关（默认关闭）或移除，验证无 HTTP 路径可触发 | 审计 M6/M8 + 审计 H3 + 2026-09-14 连续传送测试 |
| 未开始 | 弹窗文本安全 | `G_POPUP_TEXT` 野指针读（256B 无校验）+ 手写转义弱于 json_escape | 指针有效性校验 + 复用 json_escape | 审计 M9 |
| 未开始 | DebugController 处置 | /api/debug/ui 未登记（architecture 表与 api-reference 均无），release 无排除 | 登记文档或 release 排除/鉴权 | 审计 M12 |
| 未开始 | 升级技能（技能点校验机制） | `CHAR_ProcessSkillBook`(0xe2488)（技能书路径）；`UISkill_ButtonUpExe` 依赖 UI；**实测问题**：2026-08-09（存档1 LV2）`character/skill {"actionId":80,"level":2}`（80=凯恩第一个真技能，治疗）成功升级但 **skillPoints 恒 1 未消耗**——与 api-reference 声称「学习技能消耗技能点」矛盾（可能改版机制/未校验） | 探索升级函数与技能点校验/扣减；与改版技能点机制对齐 | 本会话测试 2026-08-09 |
| 未开始 | **声音/光效/语言设置操作 API** | 用户 2026-08-09 要求测试「主菜单环境设置」修改（声音/光效/语言各一次），**实测无对应操作端点**（底层已逆向）：声音 `APPINFO_Set/GetSound`@0xd8538/0xd8528、音量 `APPINFO_Set/GetVolume`@0xd84e8/0xd84f0（开=6 关=0）、画质 bit2 置/清（child 2/3）、语言 `SGL_SetLanguage`@0x944f8 + UI 语言索引 `*(0x2f9000+0xf34)`（0-4 循环，0=简体中文） | 按设置项实现 `/api/action/settings/*` 或 options 类别端点（主菜单 SC_OPTION_MMENU 面板场景，不需 world） | 用户 2026-08-09 告知 |
| 未开始 | **创建新存档操作 API** | 用户 2026-08-09 确认未开发：无 new-game/创建存档端点；现只有 enter-slot（进已有存档，v0.4.18 用 SAVE_CreateSaveSlot 初始化槽区） | 探索新建存档链（SAVE_CreateSaveSlot + 角色初始创建 + 新手流程），实现 `/api/system/save/create` 或类似 | 用户 2026-08-09 告知 |
| 未开始 | **地图非敌人物件（出口指示符/宝箱/泉水）识别** | 用户 2026-08-09 实测：当前地图实际只有 2 个地图出口，units 输出 4 个「地图出口」（slot1-4，status=2）——**出口两侧各有指示符/标识物，名称同为「地图出口」（CHAR_GetName 来源）无法区分**；enemies 端点同样包含宝箱/泉水等非敌人物件（status==2 过滤语义）；宝箱/恢复泉水等交互点数据结构未探索 | 核对 status/type 语义或地图物件类型字段，区分真出口/指示符/交互物；enemies 过滤条件是否应排除非敌人（宝箱/泉水/出口）；探索交互点数据（宝箱/泉水结构 + 交互函数） | 本会话测试 2026-08-09 + 本会话决策 |
| 未开始 | **events 增强：被动触发事件** | 用户 2026-08-09 要求（保持现有差异检测逻辑，不做 since）：新增战斗/移动中被动触发的事件——**敌人数量变化、单位死亡、切换地图** 等（当前仅 money/inventory/move/hp/mp/level_up/exp） | 快照结构 Snapshot 增补字段（敌人数/单位死亡标志/地图 ID），diff 时输出新事件类型 | 用户 2026-08-09 告知 |
| 未开始 | **基础动作 actionId 语义（0-7 普攻非技能 + 5/6/7 区别监听）** | 用户 2026-08-09 澄清：**0-7 号是普攻/基础动作（非技能，同 5/6/7 普攻），80 才是凯恩第一个技能（治疗）**；角色技能列表应从**职业（CHARCLASSBASE 等）**静态数据推导，而非仅读技能链表 actionId（此前把 0 号当「第一个技能」加点有误）；5/6/7 均为普攻（无 MP 消耗、同 -62 伤害、击杀怪），行为无法从 API 区分——需监听普攻按钮触发的 actionId 判断是连击段（依次使用）还是独立技能；SKILLDESCBASE 不含 0-7（基础技能无静态名） | API 技能端点（party/{slot}/skills）按职业静态表区分真技能与基础动作，文档修正 skills 语义；frida hook 普攻按钮/攻击键回调（UIPlay 攻击链）记录每次普攻的 actionId 序列 | 用户 2026-08-09 告知 |
| 未开始 | **attack/cast 行为确认（attack=锁定、cast5=普攻）** | 2026-08-09 实测确认（存档0 LV27，用户怀疑成立）：`combat/0/attack {"targetSlot":20}` 后**持续 10 秒不停止，目标怪 hp 恒 4095 无伤害**（仅怪物信息条出现=目标锁定）；对照 `cast 5`（普攻）立即 -403——api-reference 声称的 CHAR_MakeDefaultAttack 未触发实际攻击帧（仅让 AI 决策默认攻击），**普攻必须用 cast 5**。**用户决策（不改实现）**：attack 端点保持「仅锁定目标」语义（实际攻击链 = attack 锁定 → cast 5 普攻）；cast 保持现状（设置动作由 AI 帧执行，有效），仅完善校验。**新发现**：cast 5 后若不 stop，角色自动继续攻击（AI 自动连击）——「普攻一下」需 cast 5 后立即 stop | cast 完善校验（如 actionId 白名单/MP 校验）；文档记录「attack=锁定、cast5=普攻、不停止=自动连击」 | 本会话测试 2026-08-09 + 用户 2026-08-09 决策 |
| 未开始 | **skill-reset 未完全还原（基础技能等级保留）** | 2026-08-09 实测：skill-reset 后 0 号技能仍 LV2（未还原到 LV1），仅移除 80 号非基础技能；技能点 1→2 还原 | 确认 skill-reset 是否应还原基础技能等级（CHAR_InitializeSkill 语义） | 本会话测试 2026-08-09 |
| 未开始 | **mercenary/party 两套索引一致性（含 discharge 清理）** | 2026-08-09 实测（存档0）：`discharge slot1`（西雷斯在队）返回 ok 后 **mercenary 列表西雷斯消失但 party role2 西雷斯仍在**（hp=8184）——discharge 删 mercenary 登记但 party 角色实例未清理；基线观察：party 含西雷斯(role2) 但 mercenaries 中 slot1 西雷斯 `inParty=false`、多个 `name=null` 槽 `inParty=true`；exclude 确认沃尔达克=quest npc（`cannot exclude quest npc`）；discharge 边界正确（空槽 not found/quest npc 拦截/leader 拦截） | 核对 discharge（MERCENARYSYSTEM_Release）与 party 槽关联清理；核对 mercenary 槽标志位（flags bit1）与 party 成员映射（两套索引），确认 inParty 语义与 name 注入 | 本会话测试 2026-08-09 |
| 未开始 | **掉落系统：掉落实体 + 生成链 + 权重表** | 2026-08-09 **测试完成**：击杀怪（slot9 狼 hp→0，exp+4902 确认）后，`/api/info/current-map/drops` 硬编码返回 `{"drops":[]}`（InfoApiServiceImpl.currentMapDrops 占位）、units 无掉落实体、events 无掉落事件——**掉落物当前无法通过任何 API 获取**（用户确认场景实际有大量掉落物）；**2026-08-12 研究进展**：掉落生成链已确认（CHARSYSTEM_Die 0xf5418→DropItem 0xf4d30，frida 实测 3 怪全触发）；MAPITEMSYSTEM_ProcessDrop 读 *(0x2f5000+0x5d8) 链表（实测空）；RemoveItem 反汇编得实体=0x20 步长数组 +0x08=物品type，计数[实例+0x818]；奖励/掉落生成链（ITEMSYSTEM_MakeItem 系列）与掉落表权重（分母 1000）未探索 | 逆向地面掉落实体结构（卡点：存储位置不在 MAPITEMSYSTEM 链表/CHARSYSTEM 池/CHARLOC 池，疑 EFFECTSYSTEM_ProcessDropItem 0xf828c）+ ITEMSYSTEM_MakeItem 生成链 + 掉落表权重，实现 drops 端点（掉落实体逆向原 P0 物品逆向⑧ 已归并本条目；来源含 api-reference §3.1） | 本会话测试 2026-08-09 + 用户 2026-08-08 指定 P2/A4 |
| 待真机验证 | **craft mix 合成器交互** | 合成器交互/宿主已完成：页签/投影/状态门控（`extension_bag_mix.inc`）、R-56 按类别扣料（H-21 扩展桥接）、R-57 原版袋列高亮遮蔽；待真机验收 VM-22/VM-23。**API 未完成**：`craft/mix-direct` 等仍为占位（NOT_IMPL 501），Service 层接线未做。底层执行函数探索归 P4「合成执行」。遗留 ITEMPOOL_Free(0x108160) 符号登记消除产物入库失败的有界泄漏 | 合成器交互真机验证 + Service 层接线 | P0 端点收敛 + v0.5.21 |
| 进行中 | N3 MAXLEVELBASE 语义逆向 | 结构定案：48=6职业×8档，+0=职业索引(低字节 0,1,4,2,3,5)×档位(高字节 0,1,2,8,3,4,5,6)，+2=装备名 text_id（档6 仅职业0 有值）；运行时表与静态一致；语义=职业×等级档→装备，**引用点待定** | 符号 .bss 0x301620/0x301628/0x30162a；frida 运行时 dump 验证 | 2026-08-16 探索存档 |
| 待真机验证 | **switch 切换主控未生效** | 根因：`fn_set_active_player`（PARTY_SetActivePlayer）只写 PLAYER_pActivePlayer，不同步 SAVE_nMainMercenarySlot → main_mercenary_slot/leader 不更新。**v0.5.9 修复**：data_op_switch_player 成功后补写 g_main_merc_slot；真机验证 slot0 自身 ok、无角色 slot1 返回 switch failed | 多成员档验证 main_mercenary_slot 变化（当前档仅 1 人） | 本会话测试 2026-08-09 + v0.5.9 修复 |
| 未开始 | **宝石随机属性优秀程度标注** | 用户 2026-08-16 要求：宝石信息中，用颜色或文字标明该随机属性的优秀程度（如随机区间的前 5%） | 确定宝石随机属性取值范围（随机区间来源）与优秀程度分档规则；宝石信息输出（socketInfo/enchantInfo 等）中加优秀度标注（颜色/文字） | 用户 2026-08-16 要求 |
| 未开始 | **人物数据面板汉化 + 上限值金色显示** | 用户 2026-08-16 要求：人物数据面板英文改为中文；数值达到（超过）上限时显示为金色 | 定位人物数据面板渲染（UI 面板文本/数值绘制）；属性名汉化（映射表）+ 上限值金色着色 | 用户 2026-08-16 要求 |
| 未开始 | **优秀佣兵徽章有概率出剧情角色满技能佣兵** | 用户 2026-08-16 要求：优秀的佣兵徽章，有概率出「剧情角色立绘、名称」的满技能佣兵 | 探索佣兵徽章/招募生成链（佣兵创建随机逻辑、剧情角色模板、满技能注入）；实现有概率生成剧情角色立绘+名称+满技能的佣兵 | 用户 2026-08-16 要求 |

## P3 低优先级

> 审计低优 + 原 P2 探索型（数据结构未逆向/需样本）+ 已有实现待验证项。

| 状态 | 待办项 | 现状 / 卡点 | 需要的探索 / 实现 | 来源 |
|---|---|---|---|---|
| 未开始 | 版本与产物同步 | README/verification 已同步至 v0.3.13/48（2026-08-08）；仍缺 output v0.3.9-0.3.13 历史产物；需按 README 规则 6 走版本闭环 | 补建历史产物（可选）；版本闭环走 README 规则 6 | 审计 L1/L2 |
| 未开始 | HookMain 轮询容错 | context 未就绪/init 失败无限重试无终止条件；`nativeGetInitReport()` 无 try-catch 抛异常致 ApiServer 永不启动 | 最大重试/退避；initReport 包 try-catch | 审计 L4 |
| 未开始 | 输入白名单 | DataController `lang`/`tables/{name}` 直接拼路径无校验 | lang 白名单 + name 格式校验 `^[A-Z0-9]+$` | 审计 L7 |
| 未开始 | native 杂项清理 | 哨兵值 -1/0xFFFF/0/null 混用；CMakeLists 无 -Wall/-Wextra、dl 冗余；同址双常量 F_GET_EQUIP_VMA；g_inven 绕 resolve_global；g_party 死代码；json_escape 无长度上限；NewStringUTF 无异常检查；瓦片索引无列上界 | 统一哨兵约定；CMake 警告/标准；删冗余与死代码；补判空与上限 | 审计 L5/L8-L12 |
| 未开始 | 佣兵遣散（需确认与 party/discharge 关系） | `MERCENARYSYSTEM_Release`(0x118ab4) 未逆向；P0 已记录 party/discharge ✅v0.4.8——mercenary/discharge 是否同源需确认 | 逆向签名 + `POST /api/action/mercenary/discharge` | 2026-08-08 降 P2 |
| 未开始 | 静态表字段语义全逆向（含 B1 装备表/B2 技能表/B3 怪物表） | `field_catalog.json` 已验证 71 字段，其余待逆向；B1 ITEMDATABASE/ITEMENCHANTBASE 字段（强化/耐久/宝石孔/附魔）、B2 90+ 技能公式参数（Z% 随等级）、B3 怪物属性/掉率/首领强化（等级+3 ATK×1.2 HP×3.6）均未全逆向（B1/B2/B3 原 P3 暂缓，其中 B1 与 P4 A3 强化机制关联） | 逐表解析（`*BASE_pData` + `record_index * nRecordSize`），覆盖装备/技能/怪物表 | 用户 2026-08-08 指定 P3 |
| 未开始 | 背包移动/整理 | `INVEN_MoveItem`(0x104934) 4 参签名复杂（item+3） | 逆向 4 参签名 + 真机验证 | 2026-08-08 指定 |
| 未开始 | 队友 AI 设置 | 队友自动控制决策选项（是否用技能/是否主动攻击），在技能界面设置；只需**读/写选项**，不关心内部运作 | 逆向 AI 选项数据结构（读写选项），做 `GET/POST /api/action/player/{role}/ai` | 用户 2026-08-08 指定 P2 |
| 未开始 | 融合器/调合箱结构 | 配方表（Class D-S 五级）/材料合成链结构未逆向（网络资料见 game-systems §6.4） | 反汇编融合器/调合箱相关表结构 + 配方数据 | 用户 2026-08-08 指定 P2 |
| 未开始 | 佣兵技能系统 | 佣兵技能不出战也对全队生效、同种不叠加（game-systems §6.5）；数据结构未逆向 | 逆向佣兵技能表 + 全局生效逻辑 | 用户 2026-08-08 指定 P2 |
| 未开始 | 休息（营地恢复） | `PARTY_ApplyRest`/`PARTY_GetRestCost` 未逆向 | 逆向 + 费用校验 | 用户 2026-08-08 指定 P2 |
| 未开始 | NPC 交互数据结构 | npc_dialog 面板已识别（v0.3.9），但对话选项/分支结构未探 | hook `UINpc_*` 抓对话选项 + 反汇编 NPC 系统 | 本会话页面探索 |
| 未开始 | `/api/world/movement/path` 真机验证 | v0.2.34 实现（原 /api/info/path，v0.3.13 迁至 /api/action/get-path，v0.4.29 重做为 /api/world/movement/path 自研 BFS） | 真机寻路对比 | api-reference §5 |
| 未开始 | N6 槽记录→角色对象指针偏移 | R4 槽数组/角色池结构已确认；槽记录字段 → 角色对象指针偏移关系待样本 | 槽数据样本（多佣兵存档）frida dump 验证 | 2026-08-16 探索存档 |
| 未开始 | S8 text/story-events 并入 tables 适配 | text（带 lang 参数）与 story-events 为特殊表，需在 tables 端点适配（lang 参数传递/特殊表路由） | 实现特殊表分发逻辑 | api-reference §7.4 |
| 未开始 | **帮助页面特殊属性按实际测试修正** | 用户 2026-08-16 要求：帮助页面中特殊属性说明按实际测试结果修正 | 对照实际测试结论（属性实测结果）修正帮助页特殊属性说明 | 用户 2026-08-16 要求 |

## P4 暂缓 / 待定

> 深度数值逆向（A/C 系列，原 P3 暂缓）、依赖内购/高风险项、占位文档项、模拟器/转译层待定项。

| 状态 | 待办项 | 现状 / 卡点 | 需要的探索 / 实现 | 来源 |
|---|---|---|---|---|
| 未开始 | A1 伤害公式逆向 | 属性系数(0.66/0.5)×1.2、逐项取整、技能公式 Z%（参照 game-systems §6.1） | 反汇编找 ×1.2/0.66 常量 + 取整逻辑；突破口：布甲 12% 魔攻加成 | 用户 2026-08-08 指定 P3 |
| 未开始 | A2 经验/成长曲线 | 105 级经验表、升级规则（参照 game-systems §6.2） | 对比经验表/成长曲线公式 | 用户 2026-08-08 指定 P3 |
| 未开始 | A3 强化/混沌机制 | 卷轴增量、混沌±50%/深渊±100%、宝石上限(CRT 6.1/11.1/17)（参照 §6.3）；原 P0 物品逆向⑫ 已归并 | 反汇编强化计算 + 混沌随机常量 + 上限表 | 用户 2026-08-08 指定 P3 |
| 未开始 | C2 元素属性系统 | 风火冰神圣暗黑毒伤害判定（待确认游戏是否含此系统） | 确认存在性后再探索 | 用户 2026-08-08 指定 P3 |
| 未开始 | C4 悬赏任务/无限地下城 | Bounty Hunter/5-6 层地下城结构（待确认） | 确认存在性后探索 | 用户 2026-08-08 指定 P3 |
| 未开始 | 合成执行 | `UIMix_ButtonMixingExe`(0xc21ec) 依赖材料槽选中态；`MIXSYSTEM_CheckMixture` 仅检查非执行 | 探索 `MIXSYSTEM_*` 底层执行函数 + 材料上下文构造 | 2026-08-14 抽出 |
| 未开始 | 读档 | `SAVE_Load*`/`GAMELOADER`（主菜单操作） | 风险高，暂缓 | 2026-08-08 |
| 未开始 | 技能点重置 | `UISkill_ButtonSkillPointResetExe` 含 UIInAppProcess=内购 | 依赖内购 | 2026-08-08 |
| 未开始 | 复活 | `CHAR_ProcessReviveScroll`/`PARTY_AddHPMP`；角色死亡后复活选项 | 用不到（死亡重进即可），暂缓 | 2026-08-08 |
| 未开始 | 敌人 AI / 队友 AI 决策逻辑 | 决策算法本身（如何决策，非选项读写） | 麻烦且不影响正常游玩，暂缓 | 本会话决策 |
| 未开始 | S6 help 帮助文档内容 | `/api/system/help` 与 `/api/system/download` 为占位 | 提供帮助文档内容（API 概览/示例）与文件格式 | api-reference §7.5 |
| 未开始 | S7 tables/{table}/download 与 /api/system/download | 两个 download 端点已决定**先占位**（暂不实现） | 后续需要时实现文件流输出 | api-reference §7.4/§7.5 |
| 待定 | 模拟器游戏本体启动 | 官方 Emulator API30 / Waydroid A13 + libndk 未实证 | PoC 启动测试 | emulator-research §5 |
| 待定 | 模块 arm64-v8a 打包后 guest 内 dlopen/dlsym | 转译层内 dlopen/dlsym 是否可用未实证（高风险；与 P1 VMA 治理的 dlsym 化关联） | PoC：模块仅 arm64-v8a → 游戏启动 → System.loadLibrary | emulator-research §5 |
| 待定 | LSPatch bootstrap 稳定性 | liblspatch.so x86_64 与 guest 进程混合无公开先例 | PoC | emulator-research §5 |
| 待定 | LSPatch 0.6 与 libxposed 101 兼容性 | 内置 runtime 较旧 | 必要时降级 API 93 构建 | README 已知待办 |
| 未开始 | **文档锚点保鲜校验** | 需要周期检查文档中的测试名与 `文件:行` 锚点仍可 grep；脚本尚未实现 | 新增 `scripts/maintenance/check_docs.py`，校验测试名/文件:行可 grep 性并登记失效锚点 | P4 文档治理 |
| 进行中 | **统一日志系统代码迁移收尾** | **硬性收敛已完成**：规范 `docs/development/logging.md` 与合规脚本 `scripts/verification/check_log_policy.py` 已落地；native 单写者 `core/native/qol_log.{h,cpp}` + JNI `bridge/native/gamebridge_log.cpp` + Kotlin `LogFile` 经 JNI 转发；单一 tag `Inotia4Qol`，脚本 R1/R2/R3 当前为 0。与 logging.md §11 状态一致 | 清理持续软告警：按 logging.md §7「扩展背包级别策略」把逐帧/高频路径（`*_tick`/`*_process`/`*_wrapper`/`*_draw`/`*_render`/`*_gate`/`*_drawing`，含别名宏）的高级日志降为 DEBUG；补齐 R5 定位变量与级别错配；保持 `--warn-only` 退出码语义 | `docs/development/logging.md`（P4） |
