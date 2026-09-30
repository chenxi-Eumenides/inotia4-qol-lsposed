# 问题台账（issues）

> 本文件**只记录缺陷问题**：插件审计与真机事件调查中确认的问题条目。每条记录归属（所属插件/模块组件）、证据与状态。
> 分工：验收证据欠账（待补真机日志的 VM 卡）唯一登记处是 `verification.md` §5，本文件不重复；本文件登记的是**缺陷本身**。
> 维护纪律：新问题按所属组件追加条目，编号不复用、不重排；修复后更新状态并把条目移入文末「已修复」附录（正文只留确实存在的问题）。
> 排序：正文 §1–§9 全部为开放/挂账问题；已修复条目在文末附录存证。
> 严重度：P1 行为错误或契约破坏；P2 风险/缺陷；P3 质量/演进项。
> 范围记录：插件逐个审计（core.world → core.item → core.character → …）与真机验证中产生的问题均入此账。

## 1. core.world（开放）

| ID | 级 | 问题 | 证据 | 状态 |
|---|---|---|---|---|
| WLD-01 | P2 | 快照更新失败时保留旧快照：暂态（切图/reader 失败）内 7 条 GET 返回上一帧数据且无 stale 标记；响应是否加标记属冻结面变更 | `plugins/core.world/plugin.cpp` update_snapshot；审计 W-04 | 开放（观测用限速 warn 已随 `39b90e4e` 加入；响应语义待裁决） |
| WLD-02 | P3 | `on_enabled` 中 scheduler.add 失败仅告警，无重登记通路（插件无后续生命周期钩子可重试） | 审计 W-06 | 开放（引擎提供重试面前保持 warn） |
| WLD-03 | P3 | 写面 `move_to` 的票据 result 不区分「可达」与「不可达→走最近可达点」；调用方仅能事后读坐标或先调 distance 预检（读面有 found/nearest 完整表达） | 审计讨论 + 真机 T1-T3；`game/access/world.cpp` world_move | 开放（契约演进项：候选=票据附 found/nearest 或文档固化预检模式） |
| WLD-04 | P3 | distance 在 HTTP 线程整块拷贝快照（~137KB）+ BFS 栈上 ~53KB scratch，无栈预算文档 | 审计 I-05；`contracts/nav_bfs.h` | 开放（性能观察项） |

## 2. core.item（开放）

| ID | 级 | 问题 | 证据 | 状态 |
|---|---|---|---|---|
| ITM-01 | P1 | 读面丢失任务袋 bag5：owner 逐袋用 `item_ref_at`（实现拒绝 bag>=5），legacy 读面含 bag5 → 有任务物品的存档 `/api/item/inventory`、`/items`、`/bag/5/*` 读不到（旧版可读）；既有真机证据存档 bag5 恰为空漏检 | `game/state/game_state.cpp:306`、审计 F-01、fixture 无 bag5（T-01） | 代码已实现（只读放开：`inventory_item_ref_at`/逐袋遍历允许 bag5、16 槽；写面 `inventory_item_at` 仅拒绝 bag5、逻辑袋 6..10 解析不受影响；capacity 输出与迁移前逐字一致）；真机证据（含任务物品存档读面）待统一批 |
| ITM-02 | P1 | `character.state` 运行时 `capabilities.require` 未在 descriptor requires/optional 声明（equip 族+accept_dice 投影）：停 core.character 时 core.item 不级联、不 degraded，依赖图/装配不可见 | `plugins/core.item/plugin.cpp:214-228,285-295`；审计 F-02=CHR 侧 F5 | 代码已实现（descriptor optional_requires 补 character.state；registry host 用例覆盖 provider 在场/缺失两态，optional 边拓扑 provider 先行）；真机证据 VM-K2-02/VM-K2-03 降级可见性待统一批 |
| ITM-03 | P1 | EB move 结果缓冲 `char[4096]`：真机实测 result 紧凑长度 3753B（91.6%）；provider 先落库后判超限返回 0 → 「移动已发生但票据 failed」，重试可能反向移动；成功判定用子串 `find("\"ok\":false")` | `plugins/core.item/plugin.cpp:1334-1349`、`plugins/extension_bag/extension_bag_port.cpp:224-229`；审计 F-03 | 代码已实现（EB move 结果缓冲提至 262144 heap、provider 报告完整长度不再先落库后判败、core.item 判定改 `{"ok":false` 前缀）；超限结算语义并入 ITM-05；真机 VM-D2D-01/02、VM-T2O-01 待统一批 |
| ITM-04 | P1 | 背包快照内部缓冲 64KB（契约/读取端按 256KB 设计）：超限 `serialize_inventory_json` 失败 → update_snapshot 静默 return，快照永久停更且无告警 | `plugins/core.item/plugin.cpp:32,626-639`；审计 F-04 | 代码已实现（`kInventoryCapacity`=契约 262144，工作缓冲改 thread_local vector；失败路径限速 WARN）；满背包真机证据待统一批 |
| ITM-05 | P1 | 票据结果上限 16KB 双重语义：owner 结果超限时 return false → 票据 failed，「游戏写已成功却报失败」；api-reference §7.9 规定超限应 `done + result_retained=false` 未实现。同型：core.item 9 条附 state 写路由、core.character 全部附 state 写路由（party 满配可超 16KB，真机现值 ~7.7KB） | `infra/command_queue.h:53`、`plugins/core.item/plugin.cpp:1112-1321`、审计 F-05/F-4/F-14 | v1 已实现（done+超位+`result_retained:false`，`45805105`）。**用户新裁决替代 A/B**：①result 只返回与该操作相关的数据（不再全量 state）；②单结果上限按瘦身结果重新规划（候选 4KiB，待逐路由真机量测后冻结；本轮 infra 暂保持 16KiB）；③仍超限→票据终态照成功 done/ok，result=合成摘要 `{"ok":true,"truncated":true,"reason":"result_limit_exceeded"}`（失败命令为 ok:false 版本；不再任意截断字节）。规格=`.tmp/itm15-result-slimming/spec.md`（§2 逐路由瘦身表=公开契约变更逐条标注；§5 按 owner 分批+验证门/VM 卡）。infra 批已实现（超限改用合成摘要，上限暂保 16KiB/512KiB；host 79/79、check_arch/routes/build 全绿）；真机统一批欠账。用户定案（2026-09-30）：单结果上限**暂保 16KiB**，逐路由真机量测后再定值；串行顺序=**ITM-05 全链 → ITM-08 全链**；§2 逐路由瘦身 schema 已全部获用户批准并落地：infra `edb818e5`；批① core.item `7b374815`；批② core.character 11 条（withdraw 经 item.state 尾追加 kEnrichedInventory 1.0→1.1 前后快照差分，只过滤/拷贝不重组=ADR 0002 合规；退化登记：能力缺失/解析失败省略 inventory；attack_target 生产读面不发布 target_slot/combat→实际只回 `{"ok":true}` 受理不伪造；cast_skill/switch_player/add_stat/add_skill/reset_skill 读面不可用时省略对应键或退化 `{"ok":true}`；set_auto_attack 回传受理值非读回）；批③ core.world move_to/walk_dir/interact→player{map_id,x,y}（tile→px 既有换算，不承诺已到达）+core.save save→{slot,sidecar:"ensured"}/enter→{slot,entered:true}/create→{slot,created:true}（延迟结算/settlement timeout/失败体逐字不变）。host 79/79、check_arch、routes 143/143、build 复验通过。单结果上限暂保 16KiB（候选 4KiB 待真机逐路由量测定值）。真机统一批欠账（VM-K5-04b/04c/04e/06x/08h、VM-T2O-01、VM-D2D-01/02 等）NOT_ACCEPTED |
| ITM-06 | P2 | `json_lite` 数字解析忽略指数/小数（`1e2`→1、`1.5`→1），Kotlin 传输层 org.json 语义为 100/截断：校验放行后 native 可能操作错误 bag/slot/money | `plugins/core.item/json_lite.cpp:77-99`；审计 F-06 | 代码已实现（parse_number 按 JSON 文法完整识别，小数/指数/前导 `+`/文法非法/超 int32 →解析失败走既有 400，负整数仍合法；守卫收紧口径同 `39b90e4e` 先例——较迁移前 org.json 强转语义**更严**（`1e2`/`+1`/`1.5` 由接受改为拒绝），属有意收紧）；host dispatch 拒绝体字节断言已加；真机证据待统一批 |
| ITM-07 | P2 | 路径参数数字累加无位数/上限保护（`value*10+digit` int64 可溢出 UB）；query 解析负数无范围校验 | `plugin.cpp:918-924`、`:682-699`；审计 F-07/F-13；core.character `char_path_int`（plugin.cpp:1024-1044）同型（审计 CHR F3） | 代码已实现（item `parse_path_int`/`write_path_int` 与 character `parse_path_int`/`char_path_int` 全部改位数≤10+逐位 INT32 溢出界守卫；负数/非数字/超界→既有 400/404 格式，较迁移前 parseInt 语义收紧属有意（守卫批口径同 `39b90e4e`）；dispatch host 两文件补拒绝体字节断言）；真机证据待统一批 |
| ITM-08 | P2 | `item.facts` 跨插件暴露游戏内存裸指针（`physical_item_at`/`detail_context`/`item_ref_at`），违背「窄接口不暴露游戏指针」；仅主线程安全靠约定 | `contracts/item_facts.h:23-28`；审计 F-08 | **用户裁决=本批直接做 typed handle**（不备案、不分阶段）。规格=`.tmp/itm08-typed-handle/spec.md`：slot 表+代际号防 ABA、物品/UI 句柄分类型空间、失效挂钩点全清单（§3，实现前置门=全仓 free/槽写扫描）、读面=按字段族 typed view+语义操作（禁通用 write_field）、ABI 各 major（item.facts/ItemPort/inventory_extension→2.0、EB capability→3.0）、四批切分（句柄核心+挂钩→EB→hooks→detail_context）、固定容量满表 fail-closed。用户已裁串行序=ITM-05 全链→ITM-08 全链（ITM-05 已落地 `edb818e5/7b374815/d0e53892/710f0ce0`）；**detail_context=原子详情槽查询（A 案）**。**批 1 已实现**（本提交）：`contracts/item_handle.h`（物品/UI 分型 POD slot+generation，禁地址编码）+ `game/access/item_handle_registry`（1024 固定容量、满表 fail-closed、回绕退休、provider 身份校验、主线程约束）+ 生命周期挂钩全链接管（终版 free 审计表：EB 物化/所有权退役/延迟释放、hooks RemoveItem 提交-回滚事务与 merge 消费源失效/adopt 重绑保留、sell/discard 事务、save_slots_reload 仅 Native provider 失效、enter 转场挂起-提交失效；fn_mem_free/save_mem_free=buffer 非池对象、character_write 无池 free、fn_popup_free 无调用点=不可达证明）+ EB/hooks 经 **Host SDK 尾追加 `QolItemHandleApi`（minor 11→12）**注入登记/失效/重绑通道（不向插件暴露 resolve→void*，跨插件契约消费者表面零改动=留批 2-4）+ R-44 锁用例（持锁 register/invalidate 刷新计数 0）。host 80/80、arch 新增 0、routes 143/143、JNI 146/146、build 通过；真机未部署=NOT_ACCEPTED。**批 2 已实现（规格=`.tmp/itm08-typed-handle/spec-batch2-v2.md`，用户终裁逐点二分支①/②）**：item.facts/Item Port/inventory_extension→2.0、EB capability→3.0，Ref/参数/回调全 handle 化，删 `native_item` 与插件面 resolve（契约静态红线测试 `item_handle_contract_redline` 防回归）；core.item provider 物品经 handle-keyed typed view 序列化（golden 逐字节复绿）；autosell/attribute_quality/craft_ui/custom_recipe/special_equip/world_teleport/inventory_trade/inventory_hooks 调用面迁移；EB 私有 custody 表定性=**已批准②（限 provider 自有物化对象持有，逐点举证+`item_handle_custody_tests` 八规则失败注入）**，指针不出 EB 实现；Host SDK minor 终值 14（旧尺寸 fail-closed）。host 82/82、arch 新增 0、routes 143/143、JNI 146/146、build 复验通过；真机未部署=NOT_ACCEPTED。**批 2 已知欠账（B2 残留登记表）**：inventory_hooks 原版 ABI 槽位指针访问三处 fail-closed（`native_inventory_hook.cpp:187/255-276/540`，归批 3 私有 hook bridge，相关原版查找/槽扫描行为未宣称等价）；detail_context/UI 控件指针留批 4 原子详情槽；game_host item ops/write port/extras/craft 契约裸签名按表归后续专项（②不授权）。**批 3 已实现（主代理复验）**：game 层新增 handle 键 Game dispatch 原语（`game_host.h`/`game/access/inventory.h/.cpp`/`game_host_port.cpp`：`game_original_find_item_slot`/物品身份查询**只回结果不回物品指针**，现场指针仅限当帧 hook ABI 入参栈内即弃）；inventory_hooks 三处 fail-closed 回退（原 `:187/:255-276/:540`）删除并重接原版查找/槽扫描/装备/消费/R-55/R-63 链（R-20/21/44/55/63 → VM-43/44/P4-07/T2O-01 真机欠账）；EB port sell-price handle 路径与 original-only 门控延续；红线 cmake 断言覆盖 hooks 迁移面。host 82/82、arch 新增 0、routes 143/143、JNI 146/146、build-debug 通过（d8ce04b73401）、diff --check、旧 fail-closed helper grep 绝迹；真机未部署=**NOT_ACCEPTED**。剩余：批 4 detail_context 原子详情槽（A 案）、B2 残留契约专项（②不授权） |
| ITM-09 | P2 | `update_snapshot` 各失败路径静默 return（无 warn），快照停更不可观测 | `plugin.cpp:605-651`；审计 F-12 | 代码已实现（update_snapshot 各失败分支一次性 WARN；EB 段沿用既有限速；随容量批落地） |
| ITM-10 | P3 | `enrich_shop_items` 白名单重放（slot/category/count/price），上游新增字段被静默丢弃 | `response.cpp:396-407`；审计 F-15 | 开放（schema 锁定注释+测试） |
| ITM-11 | P3 | `fill_status` 在 installed_disabled 仍报 enabled=true 且无消费者（与 core.world I-01 及全局同型） | `plugin.cpp:1596-1599`；审计 F-11 | 并入 ENG-04 统一处置 |
| ITM-12 | P2 | core→feature 依赖方向倒置：`core.item` optional_requires `stack_limit.item_count`（enabled 门控+canonical 解码双需求），而 **optional 边参与安装拓扑**（registry `topo_build(include_optional)`），使 feature 插件 stack_limit 必然先于 core owner 安装；check_layers 只校验 include 面，capability 边的方向违例静态不可见。行为当前正确，问题在架构方向与次序。候选 A：解码走既有 host facade、enabled 改由宿主状态面查询，能力边删除；候选 B：堆叠编码语义下沉 game 层归口物品域，stack_limit 仅保留 clamp 资源与开关 | `plugins/core.item/plugin.cpp:129-144,280-285`、`plugins/stack_limit/plugin.cpp:263`（facade set-once 常驻）、`contracts/item_count.h`、`host/plugin_registry.cpp:420-465` | 代码已实现（port 路由+驻留 active probe：facts 经 `read.cumulate_count` gate-aware 路由、序列化经新 port 成员 `effective_cumulate_count`（Item Port 1.2 尾部追加）；`stack_limit.item_count` 能力类型/provides/optional 边已删除，core.item 不再 include item_count.h；game 层直接 facade 消费者的 gate 无关语义未动）；真机证据 VM-P1-S2/VM-P6-01/VM-K2-02/VM-K5-04c 待统一批后销账 |
| ITM-13 | P2 | 扩展袋知识散落在模块与 core.item（违背「模块/核心插件不得知道扩展背包」裁决）：core.item 知道逻辑袋 6..10 并直调 `extension_bag` 能力（`write_item_move_result` 的 `extension_requested` 分支、`append_extension_inventory_bags`、快照 EB 段与 R-47b 修正）；game 层同样直接取能力——`game/ops/item_inventory_state_write.cpp:255-266`（move 的 EB 分支）、`game/state/game_state.cpp:285-310`（ref_at 的 kExtension 分支）、`core/native/inventory_trade.cpp:26-33`。用户裁决：全部扩展袋语义收归 `extension_bag` 插件，core.item 与模块只经**中立扩展点**（物品域自有的扩展 provider 槽，同 f811d916 count facade 型），EB install 时注册 | 上列四处 + 本批过渡修复提交（缓冲提额只治容量，未去知识） | 批 1 已实现（中立 provider 通道：`contracts/inventory_extension.h` 13 成员表 + game facade set-once + host 转发 + resident/available 双 probe；core.item 与 game 物品路径 EB 感知红线 grep 清零；写端口签名中立化 1.0→1.1、Item Port 1.3、`kExtension`→`kProvider`（wire 字符串不变）；EB install 注册 provider、deactivate 置 unavailable、disable 不注销、provides 保留；R-47b 唯一实现点移入 provider。host 77/77、143/143、check_arch、Debug 构建通过；真机统一批 NOT_ACCEPTED）。批 2（v2 通用参与者机制，规格=`.tmp/itm13-extension-reversal/spec-batch2-v2.md`）已实现：新增 `contracts/lifecycle_participant.h`+host 注册表+game facade（assembly 注入；Host SDK 尾追加 minor 10→11）；save enter/create 与 ui_state 四点位迁移（冻结文案逐字保持、无参与者=旧能力缺失路径）；legacy_read 扩展段改 provider `inventory_json_fragment` 唯一实现点（T2h-B0 逐字节 golden）；provider 尾追加 `use_item` 1.0→1.1 并接 host 写端口与 bridge domain_operations；EB install 最末双独立注册（provider+participant，失败登记部分注册态）；删 `contracts/extension_bag_port.h`/`host/extension_bag_port.inc`（逐符号复核，EB 内部直连符号保留）；红线 grep 清零面 0 命中；autosell/inventory_hooks feature↔feature 消费与 EB capability provides 保留。host 78/78、check_arch 零新增、routes 143/143、JNI 156/156、Debug 构建通过；真机未部署=NOT_ACCEPTED。证据缺口（如实）：save.cpp 无 host target（enter/create 外层 JSON 字节、动态 `not in main menu (state=…)`、`save_preflight_for_enter` 完整 JSON、回滚链仅真机可锁→VM-26/VM-K5-06i）；`state_id` 回退扫描与 main-menu 成功路径仅单元级覆盖；EB install 注册失败分支与 Host SDK 旧尺寸运行时 fail-closed 无集成用例。批 1 文案偏离 `"provider move unavailable"` 已裁决不还原不扩散。VM-43 引用错位更正：EB 册 VM-43=monster 数量上界（R-59）、重构册 VM-43=商店部分出售，均不得作 ITM-13 证据；ITM-13 真机卡=VM-26/VM-K5-06i/VM-D2D-01/02/VM-P4-07/VM-K4-03。与 ITM-08 typed handle 同族 |
| ITM-14 | P3 | bridge 扩展袋 JNI 收口（用户裁决「零调用即删；在用若非仅 Kotlin 可行则搬 cpp」）：实扫 10 个扩展袋导出中 9 个 Kotlin 零调用（含反射/字符串检索两轮零命中），已删声明+实现+专用 helper（`data_virtual_bag_ui_status_json`）；check_jni 基线 156→147。在用 2 个：`nativeExtensionBagStatusJson` 与 `nativeSetExtensionBagEnabled` | `NativeBridge.kt`、`bridge/native/gamebridge_extension_bag.cpp`、`gamebridge_settings.cpp`、`plugins/extension_bag/*`、`docs/refactor/baseline.md` | 死导出已删（`eb077827`）；statusJson 已搬 cpp（EB 声明式 native query handler：路由 143 面 owner 不变、启用态成功响应逐字节一致、JNI 147→146、host 79/79、构建 `66ad011d1a19`。**停用态行为差异（登记）**：EB installed_disabled/inactive 时 handler 随 owner 撤销，旧 JNI 仍返回 `enabled=false` 状态 JSON，现改 owner 不可用响应——**已裁决：接受 owner 撤销语义**，旧 JNI 停用态特例不保留）。setEnabled **保留**，exp-1 事实更正此前表述：Kotlin 门禁传导=六个功能插件同型「命令队列专用 JNI setter」（stack_limit/simple_mode/autosell/push_all/moveMerge/extension_bag 均如此，`gamebridge_domain_operations.cpp:443-488` 等），仅 `customRecipeEnabled` 直写+同步事件例外（`:490-495`）；启用状态机本就在 native（`commands_drain→lifecycle_step→sync_gates`），JNI 只是设置值传导通道，EB 非特例。是否把六个专用 setter 归一为通用 gate-set 属**模块级议题**（非 EB 收口范围）——待裁决 |两批真机统一批欠账 VM-P4-07/VM-K4-03=NOT_ACCEPTED |

## 3. core.character（开放）

| ID | 级 | 问题 | 证据 | 状态 |
|---|---|---|---|---|
| CHR-01 | P1 | `POST /api/op/character/{role}/hp` 无效：`ops::set_hp` 写 `ch+0x1F0`（C_HP）成功且票据 done，但该字段是引擎每帧按权威来源重写的**镜像**——真机 10ms 高频采样确认「写入瞬时可见、≤1 帧内被覆盖」；战斗中回血不能维持生存（436 次维持写入仍阵亡）；金钱写入（不同函数链 `fn_minus_money`）对照有效。`set_mp` 按同字段体系推定同型（待取证）。读路径同患：战斗中 party hp 为帧缓存值（滞后 ~1s，观测到读 0 而 raw 4772） | `.tmp/hp-write-probe/`（poke/trace/hifreq/slots 系列）、`game/ops/character_write.cpp:56-69`、`game_symbols.h:20,73-76` | 开放（修复方向：改用游戏自身 HP 变更/治疗函数链或写权威结构；先 RE 权威来源。阻塞战斗类真机流程） |
| CHR-02 | P1 | `set_skill_usage` 迁移断裂：HTTP 路由挂 `core.feature` 501 占位，底层 `ops::set_skill_usage`（全局位域）已实现未接通；文档规划语义（action_id/mode 档位）与底层能力（无单技能档位）冲突；正确 owner 应为 core.character | `game/ops/character_write.h:39-42`、`core.feature/plugin.cpp`；审计 F1；VM-P5-CORE-FEATURE-01 NOT_ACCEPTED | 开放（裁决：按全局 onoff 实现并入 core.character+删 core.feature，或挂账） |
| CHR-03 | P1 | 6 条 Kotlin `notImpl()` 占位路由（skill-point/skill-level/heal/rest/revive/hate）`op_gated=1` 实际失效：501 抛于 op 门禁之前，op 开/关两态不可区分，与 api-reference §8 门控口径矛盾 | `plugins/core.api/routes.inc:85-95`、`OpController.kt`；审计 F2；smoke 基线仅记 501 | 开放（候选：迁 native dispatch 占位/改文档口径/Kotlin 补门禁） |
| CHR-04 | P2 | `set_exp` int64 入参静默截断 int32（exp>2^31 回绕写入错误值），Kotlin/native 均只查下界 | `plugin.cpp:1147-1151`、`character_write.cpp:113`；审计 F8 | 开放 |
| CHR-05 | P2 | `add_stat` attrs 数量无上界，单请求可触发数百次主线程逐点写入与缓存刷新 | `plugin.cpp:1092-1120`；审计 F9 | 开放（上界待裁决：≤255 或 ≤剩余能力点） |
| CHR-06 | P2 | `json_slice`（~250 行不可信输入切片解析器）无直接单测，仅 fixture 间接覆盖 | `json_slice.cpp`；审计 F13 | 开放（补截断/转义/嵌套/超长 key 用例） |
| CHR-07 | P2 | `QOL_PORT_CHARACTER_HTTP` 未在端口解析处限 core.character 私有，port 实现无主线程断言（头文件声明 HTTP 线程不得直调） | `host/plugin_registry.cpp:207-211,271-366`、`contracts/game_ports/character_http_port.h:10-11`；审计 F7 | 开放 |
| CHR-08 | P2 | 15 条 GET 每次在栈上构造 CharacterSnapshot（≈113KB），seqlock `read()` 最多 8 次重试逐次整块拷贝（最坏 ≈900KB/请求） | `snapshot.h:30-39`、`plugin.cpp:495-496`；审计 F11 | 开放（按需切片/双缓冲） |
| CHR-09 | P3 | `write_character_state_json` 以 `message_len==11 && "not in game"` 字符串比较识别非 world 态，port 错误文案变更即静默改行为 | `plugin.cpp:366-373`；审计 F16 | 开放（改用 result code 判定） |
| CHR-10 | P3 | kRoutes/kQueries/kHttpQueries 三数组手工索引对齐，增删路由易错位；constexpr 数据经 const_cast 入契约 | `plugin.cpp:57-82,1492-1543`；审计 F15 | 开放（单表生成） |
| CHR-11 | P3 | 写后读己写：GET 快照节拍 15 帧（≈250ms）内返回旧值，契约未声明该窗口 | `plugin.cpp:400-426`；审计 F17 | 开放（文档声明或事件化刷新） |

## 4. core.ui_state / core.custom_ui（开放）

| ID | 级 | 问题 | 证据 | 状态 |
|---|---|---|---|---|
| UIS-01 | P2 | wipeout `game_over` 后的 YesNo 确认弹窗（TextData 0x14）缺正确的「Yes」推进 handler：`game_over`（`F_WIPEOUT_BUTTON_GAMEOVER_VMA`，game_symbols.h:926）弹出该确认。**并非无法识别**：screen 实测由 `dialog_wipeout` 变为 `dialog_popup`；popup 栈顶仍为 wipeout 入口（FIX-03 的 fail-closed 判定与 `ok` 被拒均已实测）。`GET /api/ui/dialog` 的弹窗分支可输出 `type:popup`、文本、`G_POPUP_FPOK/FPCANCEL` 对应的选项，但该确认弹窗的具体响应尚未真机取证；问题是通用 `F_BUTTON_OK_EXE` 不适用该弹窗（误用即 FIX-03 崩溃根因），游戏原生点按正常 → 其自身 button 链存在、待定位并接入 | `game_symbols.h:924-926`、`game/access/ui_state.cpp:522-540`；真机受控实验 | 开放（先捕获此弹窗的 `GET /api/ui/dialog` 内容，再定位 YesNo 按钮 handler 并新增 action；修复前死亡恢复流程需人工一次点按） |
| CUI-01 | P3 | `native_choice_open` 在 push 进入外部调用后的并发交错（close/disable 与 push 同时发生）仅有 generation 门禁，真机长时观察未覆盖 | FIX-01 修复报告风险项 | 挂账（真机统一批观察） |

## 5. core.settings / 配置链路（开放）

| ID | 级 | 问题 | 证据 | 状态 |
|---|---|---|---|---|
| SET-01 | P2 | `POST /api/config/set` 写 `autoSellEnabled/simpleModeEnabled/stackLimitIncrease` 恒「回读原值」：三键 setter 经 `ConfigApiService.applyOnChange` 把值下发到 native **门禁 key**（`plugin.<id>.enabled`），而 `GET /api/config/list` 由 core.settings 读**裸 key**；写读不同 key → 显示永不更新，门禁实际已被改（曾致 stack_limit 门禁翻转→存档进不去，见 INF-01）。POST 响应回显 Kotlin `ModuleConfig` 状态，与 list（native）三态不一致；`opEnabled` 显示与真实门控同样分歧 | `ModuleConfig.kt:181-247`、`ConfigApiService.kt:97-116`、`bridge/native/gamebridge_{domain_operations,simple_mode,autosell}.cpp`、`core.settings/plugin.cpp:85-136`；真机复测（写 false 回读 true） | 开放（统一 key 体系或 list 合并门禁真值；属 core.settings↔Kotlin 契约修正） |
| SET-02 | P3 | 门禁/开关与插件启停的双通道语义未在文档声明：config 三键实际=生命周期开关而非「配置」 | 同 SET-01 | 开放（并入 SET-01 处理） |

## 6. 引擎 / host（开放）

| ID | 级 | 问题 | 证据 | 状态 |
|---|---|---|---|---|
| ENG-01 | P1 | 级联停用（cascade_stopped）不调用 owner 的 `deactivate`，仅 unregister_http_queries+on_disabled：写面 owner（core.character/item/save/world/ui_state/quest/EB）自注册的 typed result handler 残留，窗口内已接受命令可经残留 handler 用未清理的 ops 上下文执行（违反「停用后无业务副作用」判据） | `host/plugin_registry.cpp:1361-1388,2099-2101`；审计 character F6；core.world deactivate 实测消费者不自动恢复 | 开放（引擎统一修复：cascade 终态调 deactivate；关联 VM-K2-01/VM-FIX-01） |
| ENG-02 | P2 | `http_dispatch.unregister_owner` 与 registry `sdk_unregister_owner` 在主线程条件变量等 inflight/lease 归零**无超时**：任一 handler 挂起即卡死生命周期（hooks 有帧预算，dispatch 面无） | `host/http_dispatch.cpp:241-247`、`host/http_registry.cpp:111-122`；审计 item-G02/char-G02 | 开放（加帧预算/超时+失败语义） |
| ENG-03 | P2 | `http_dispatch.register_handler` 对 `unregister_pending && 同 owner` 的槽直接重置，不等待该槽 inflight 归零（现由生命周期串行不可达；并发注册场景会丢计数/悬挂 selected 指针） | `host/http_dispatch.cpp:178-187`；审计 world-G-01 | 开放（重置前检查 inflight==0+断言注释） |
| ENG-04 | P2 | `PluginDescriptor.fill_status` 回调 host 侧**零调用点**，全项目 60+ 插件维护实现且部分语义错误（installed_disabled 报 enabled） | `contracts/plugin.h:155`；审计 I-01/F-11/F-10 | 开放（统一裁决：接线或删除；删除即全项目清理批） |
| ENG-05 | P3 | POST 路由响应超初始容量不触发扩容重派发（仅 GET 有 `body_size>capacity` 扩派）；owner 超容量直接 500 | `host/http_dispatch.cpp:389-425`；审计 world-G-03 | 开放 |
| ENG-06 | P3 | `HttpQueryHandlerDescriptor.ctx` 为 `void*`，声明式插件被迫 const_cast（core.world 7 处等） | `contracts/http.h:72-91`；审计 world-G-02 | 开放（契约级 `const void*` 统一裁决） |
| ENG-07 | P3 | registry 与 dispatch 两层 owner 停用文案不一致（`http registry inactive` / `http owner inactive`），且 registry 注销删槽 vs dispatch 注销留墓碑，语义分层无文档 | `host/http_registry.cpp:123-125` vs `http_dispatch.cpp:248-256`；审计 world-G-05 | 开放（文档+测试区分） |
| ENG-08 | P3 | `check_http_routes.py` 不交叉核对各插件 descriptor 的 `http_query_count` 与 routes.inc owner 计数（core.character 35 handler vs 41 路由的差值长期不被发现） | `scripts/maintenance/check_http_routes.py:39-50`；审计 char-G-03 | 开放（工具补校验） |

## 7. infra / game 适配层（开放）

| ID | 级 | 问题 | 证据 | 状态 |
|---|---|---|---|---|
| INF-01 | P1 | stack_limit 驻留（residency）无保护：开启期间存档写入 >99 堆叠后，关闭开关（config 或 lifecycle）使 enter_slot 预检判 `slot corrupt` 无法进档（stage load_data）；真机复现：SET-01 的门禁翻转致 slot0 不可进，重开门禁+重启进程后恢复。回滚通道=门禁开或存档回档，两者均未在开关语义中提示 | 真机（tickets 18/23/26 failed vs 28 done）；VM-P6-01 驻留证据欠账 | 开放（策略待裁决：关闭前 clamp/禁止关/关闭态预检豁免并声明） |
| INF-02 | P2 | `game_cache` 惰性槽在**请求线程持 `g_cache_mtx` 调 builder**，builder 内 `wait_frame_boundary` 以 1ms sleep 轮询：HTTP 线程可被长时间阻塞；若任何持 `g_virtual_bag_mtx` 的路径触达缓存刷新即重入 `for_each_logical_item` 死锁——现靠「EB 的 op_ok 全在锁外」约定维持，无机制校验 | `core/native/game_cache.cpp:101-119,143-164`；审计 item-G-03；AGENTS 锁规则 | 开放（校验手段：断言/测试或锁序文档化） |
| INF-03 | P2 | `frame_cache_force_refresh` 每次 op_ok/卖店/丢弃全量重建 12 个缓存槽（含地图/玩家/队伍），且写结果已即时序列化一次，重复成本高 | `game_cache.cpp:185-195`；审计 item-G-05 | 开放（性能项） |
| INF-04 | P3 | 插件私有 `Snapshot` 副本与 infra 版同构但互不同步：publish 无多写者断言（单写者不变式仅靠生命周期串行约定），distance 等 HTTP 读依赖该约定 | `plugins/core.world/snapshot.h`、`plugins/core.character/snapshot.h`；审计 I-04/G-07/F-18 | 开放（断言或统一快照组件） |
| INF-05 | P3 | `item_count_facade` 裸指针 `g_provider` set-once 无原子、deactivate 不清除（stack_limit 刻意常驻）；契约未记录该约束 | `game/adapter_ports/item_count_facade.cpp`；审计 item-G-06 | 开放（注释/断言固定） |
| INF-06 | P3 | `capabilities.require` 无锁：注册/撤销在主线程，契约未禁止 HTTP 线程 require（数据竞争窗口） | `infra/capabilities.cpp:68-81`；审计 char-G-05 | 开放（契约声明线程约束+断言） |
| INF-07 | P3 | 注册容量 `kMaxHandlers=128`、`kMaxPending=1024` 无余量诊断，冲突/超限错误仅落装配日志 | `http_dispatch.cpp:17`、`command_queue.cpp:22`；审计 char-G-06 | 开放 |
| INF-08 | P3 | handler 全局唯一 id（如 `0x57540001`）依赖前缀约定，无中央登记表，新增 owner 有碰撞风险 | `plugins/core.world/plugin.cpp` kWorldWriteHandlerId；审计 I-08 | 开放（contracts 集中登记） |
| INF-09 | P3 | result handler 自注册模式（core.feature 等）与声明式 `http_queries` 并存无统一约定；部分注册失败的 `unregister_owner` 回滚影响面大于失败项 | `host/http_dispatch.cpp:431-449`；审计 char-G-08/G-09 | 开放（约定文档化） |

## 8. 工具链 / 验证设施（开放）

| ID | 级 | 问题 | 证据 | 状态 |
|---|---|---|---|---|
| TL-01 | P2 | `smoke_all.py` 响应体记录截断 400 字符（routes 20000）：长读面「与基线 diff=0」结论不成立——bag5 读回归（ITM-01）即因此逃逸既有验收 | `scripts/verification/smoke_all.py:343-345`；审计 item F-16 | 开放（改完整 body/哈希记录并重录基线；影响所有读面既有 PASS 证据口径） |
| TL-02 | P2 | `smoke_all.py` 无副作用隔离：动作面直打致真机 party 团灭（本会话实测 `dialog_wipeout`，靠备份导入恢复） | 真机 smoke 记录 + backup 恢复流程 | 开放（只读模式或写面白名单+危险动作排除） |
| TL-03 | P3 | `check_arch.py` 不覆盖运行时 `capabilities.require` 依赖边（ITM-02 类错漏静态不可见） | `scripts/maintenance/check_arch.py` + 审计 char-G-04 | 开放（descriptor 静态可解析部分可校验） |
| TL-04 | P3 | `arch_exemptions.json` 存量条目指向已不存在路径 `plugins/core.ui/panels/settings.cpp:*`，陈旧豁免可能未来误匹配 | 审计 world-G-06 | 开放（清理；未随 FIX 批做因属工具独立小改） |
| TL-05 | P3 | 设备环境漂移：文档记录 frida-server 17.16.4 已部署，实测设备 `/data/local/tmp/frida-server` 缺失（本会话重推恢复）；frida 直写/采样是 HP 缺陷定位的唯一手段，环境项需固化 | 真机操作记录 | 开放（文档或开机脚本固化） |

## 9. 文档漂移（各插件验收后统一更新）

| ID | 归属 | 问题 |
|---|---|---|
| DOC-01 | api-reference × core.world | `/map/id` 示例键名 `id` 实际 `map_id`；distance 示例含 `in_map`、`nearest:null(found=true)` 与实现不符；「drops 恒为空」被真机非空证据证伪；4 条写端点章节仍描述同步响应应改票据契约并交叉引用 §7.10 |
| DOC-02 | api-reference × core.item | `/items` 每项「附 bag 字段」实际无；EB 容量「16/8/4/0/0」实际按袋派生 [16,16,12,12,12] |
| DOC-03 | api-reference + game_symbols.h × core.character | §2 章首「设计草案（代码待实现）」过时；装备「position」字段实际为 `slot`；`max_level` 标注「待实现」实际已输出；`set_skill_usage` 规划语义与底层能力冲突（关联 CHR-02）；`game_symbols.h` bit0-2 注释实际 bit0-3 |
| DOC-04 | build-and-deploy × 全局 | 真机流程补充：阵亡恢复链（wipeout→game_over→**人工一次点按**→enter_slot，API 无法推进 UIS-01）；smoke 写副作用警示（TL-02）；config 三键的门禁语义警示（SET-01） |

## 10. 无法验证项（用户确认后登记，不再主动验收）

| ID | 归属 | 项 | 原因 |
|---|---|---|---|
| UNVER-01 | core.world（写执行链 game/access/world.cpp 的 `tutorial_block_error` 分支） | 教学态拦截：药水教学激活时 world 写面被拒绝、读面暂态语义 | 无教学存档可进入教学态（用户 2026-09-28 确认）；不再主动验收，代码保持现状 |

## 附录：已修复问题（存证）

| ID | 归属 | 问题 | 修复与复验 |
|---|---|---|---|
| FIX-01 | core.custom_ui（触发面 world_teleport 及一切 ui_choice 消费者） | `native_choice_open()` 持 `g_mutex` 调用游戏 `POPUPSTATE_Push`，游戏同线程同步回调 `choice_init_wrapper` 再取同一锁 → GLThread 自锁（点世界地图入口必死锁） | 两阶段 open（锁内准备、锁外 push、generation 回滚）：`d89d272c`；host `native_choice_tests` 含同步重入用例；真机点图弹窗/取消/选择三步 PASS |
| FIX-02 | patch/（call_patch、instr_patch、claims；元数据 game/access/patches；载体资源 world_teleport `patch:UIPlay_CallMapName`） | branch-return 型 call patch 的 revert 回写编译期常量（原版 `mov x0,#1`）而非安装时实际观测指令；monster v29 停用后大修原生 `B` 指令被覆盖 → 点图呈原版行为且大修指令丢失（需进程重启恢复） | install 保存 saved_word、revert 写回保存值、缺失 fail-closed：`c1e136bc`；真机停用→monster 原生行为、启用→弹窗恢复三步 PASS |
| FIX-03 | core.ui_state（`game/access/ui_state.cpp` dialog 路径） | `dialog_select` 中 `G_POPUP_ON` 通用分支先于 wipeout 判定：阵亡后对 wipeout 子弹窗发 `ok` → 调通用 `F_BUTTON_OK_EXE` → wipeout 场景控制对象失效 → 下一帧 `ControlScroll_Process` 空指针 SIGSEGV（fault 0x58，libgame.so；同签名崩溃 tombstone_25/26/27，受控实验定位触发步） | wipeout 判定前移 + `dialog_ok/cancel` 防御纵深 fail-closed：`8cf061ef`；真机复验 `ok`→`no such option in wipeout` 且无新 tombstone；人工点击弹窗游戏正常（引出 UIS-01） |
| FIX-04 | core.world + game/nav + contracts | BFS 双实现（插件私有 `navigation_bfs` 与 `game/nav/nav_bfs` 重复约 80 行，修复需双改）；写面输入无 INT32/溢出守卫、快照端口检查缺口、message_len 31/30 不一致 | 统一为无状态纯函数 `contracts/nav_bfs.h`（game 与插件共用）+ 守卫/限速告警：`39b90e4e`；新增 `nav_bfs_tests` 与写路径 host 用例；行为差异点（越界目标不再索引别名伪 found→走最近可达）已真机目视 PASS |
| FIX-05 | contracts/world_facts + core.world + core.item + world_teleport | money 经 `world.facts` 重复暴露（与 `core.item` 金钱 owner 冲突，违背领域归口） | 迁移：world.facts 2.0 移除 money、item.facts 1.1 追加、world_teleport 查钱改 item.facts（fail-closed）：`9e2c2384`；真机传送扣费/余额不足路径 PASS。备注：`tutorial_state` 经用户裁决保留于 world.facts（暂停世界的游戏状态） |
