# `custom_recipe` 单 owner 合并设计卡与实施记录

> 基线 HEAD：`ac0dfc73`。本卡区分**已实现事实**与**尚未验证项**：合并目标已实现（L0 契约/端口、L1 结果层、L2 UI 子层与 descriptor、L3 Host 资源/装配、L4 测试/CMake 与集成），Host 集成构建、受影响 CTest、静态校验与 Debug 构建均已有证据；集成包已安装且运行身份与构建产物一致（尚未进入功能态），`VM-CR-MERGE-01` 仍 **NOT_ACCEPTED**。协议页须由用户确认；当前未发放材料，用户确认后按其授权通过 API 发放。

## 1. 合并结构（已实现）

合并沿用现有 `custom_recipe` plugin id，把原独立 `craft_ui` 的运行时 UI 子层与 hooks/ports 并入同一 owner；UI/runtime、结果 feature、rules、catalog/dynamic recipes、table ownership、text presentation 继续分层，不合并为单文件。

- **M1 单一 owner（已实现）**：装配只注册 `custom_recipe` descriptor（`host/assembly.cpp:223`）；原 `plugins/craft_ui/` 源文件已删除，UI 迁入 `plugins/custom_recipe/ui/`。唯一可见 feature 为 `custom_recipe.recipes`（default false，`plugin.cpp:297-331`）。
- **M2 hidden feature 清理（已实现）**：原 `craft_ui.custom_crafting` descriptor/feature 与其运行时持久化接线已删除，不再参与注册投影或运行。Store 既有残留保持原样：不迁移、不改写、不删除旧 key/配置文件。
- **M3 `craft_result` 内部化（已实现）**：`result::CraftResultApi` 改为 owner 内部 UI→结果调用面（`result.h:10-104`、`feature.h:34-35`）。descriptor `provides` 为 0（`plugin.cpp:297-331`），不再对外提供该 capability，外部消费者不再依赖它。
- **M4 write ports（已实现）**：`QOL_PORT_CUSTOM_RECIPE_WRITE` v1.2 与 `QOL_PORT_CRAFT_UI_WRITE` v1.1 由同一 descriptor 声明并校验（`plugin.cpp:278-285,92-120`），保持具体函数语义与 item handle 类型；未新增 wildcard/catch-all port。
- **M6 resource transaction（已实现）**：13 条声明——5 核心 required/fail-closed + 7 装饰 optional/fail-soft + ownership（required/fail-soft）置于末尾（`ui/runtime.cpp:526-566`、`plugin.cpp:55-90`）。核心失败由 Host 事务逆序回滚；optional 失败只 degraded。
- **M7 typed command/帧时序（已实现）**：保留 `commands.post_typed` 入队与 Host 主线程 drain（`ui/feature.cpp:205-222`、`plugin.cpp:289-295`），未改成同步写；handler 先检查 `enabled()`，停用后 drain 中的命令失败（`feature.cpp:1195,1193`）。关闭时先关业务 gate 并作废 pending token，再按 Host drain/资源逆序释放。
- **M8 fixtures/CMake（已完成）**：owner fixtures 与 CMake 已由 L4 落地，新增 `custom_recipe_owner_fixture_tests`，受影响 17 目标 CTest 全部通过；全量 141/142，唯一失败为无关旧断言（见 `verification-log.md` §3）。
- **M9 文档（已实现）**：参考说明只记录当前代码事实（`docs/reference/plugins/custom_recipe.md`）；合并验收与边界登记在本目录与 `docs/refactor/verification.md`。
- **CR04（已实现）**：text scope 改由 UI 子层本地 TLS + RAII 守卫持有（`ui/feature.cpp:20,246-261`），查表为显式 `text_lookup(scope, id)`（`text_table.h:71-78`）；继续用既有 `text_scope.push/pop`，未新增 Host SDK getter。

## 2. 保留的依赖与行为等级

- 保留 required `item.facts` v3.3、`craft.facts` v1.0、`text_layer` v1.0，以及 optional `attribute.quality` v2.0（`plugin.cpp:259-276`）。quality 缺失/查询失败只阻断宝石 tier-up，不阻断 owner 或三格配方。
- 保留 `craft.facts` 的独立 `core.craft` owner 与 `extension_bag` consumer；未把 facts 生产或 EB 读取迁入 `custom_recipe`。
- 5 required/7 optional 的 resource failure 语义照级别保留；核心 hook 不完整时不留下可业务执行的 table/handler 半状态（门禁只在 ownership 就绪且核心 backup 齐备时打开，`plugin.cpp:233-240`）。
- 保留 typed handles、Host typed command payload、SaveEnter 动态配方重建、TextLayer after/modify 订阅（`plugin.cpp:44-47,172-178`、`feature.cpp:440-461`）；UI controls/native pointers 未成为跨插件 contract。

## 3. 不迁移的 API 与遗留语义

- **API 例外（未变）**：`POST /api/op/craft/mix-direct` 继续由 `core.craft` 标记 owner，并保持 Kotlin `OpController` 501 `not implemented`；路径、method、响应与 NativeBridge external 集合不变；未新增 JNI。
- **CR09（已实现，保持原语义）**：`UIMix_ResetStuffItemControl` 保持 optional，且 wrapper 不受 feature gate（`ui/runtime.cpp:508-512`）。
- **`craft_ui.custom_crafting` 旧 key**：仅移除 runtime 接线；Store 残留不迁移/不改写/不删除，不参与运行。

### 3.1 CR01 已实现的分阶段合成失败处理

实现位于 `feature.cpp:695-1005`，阶段结果见 `result.h:35-46`：

1. **precheck（扣料前）**：pending/token 有效性、动态表 generation、输入 typed identity、live handle/类别/可堆叠性/类别持有数、扣料入口就绪、重复对象身份。阶段 1 冲突 → `kStaleSlots`，阶段 2 就绪失败 → `kPrecheckFailed`；持有数不足 → `kNotEnoughHeld`。该检查只是前置校验，不是 reservation/锁定/强原子保证。
2. **产物准备（未入包）**：扣料前完成产物创建与必要改写；此阶段失败即丢弃未提交产物并返回 `kProductPrepareFailed`（尚未扣料）。
3. **原版入口扣料 + 数量观察**：经原版 inventory entry（可堆叠 `remove_item_data` / 不可堆叠 `remove_item`），并核对类别数量变化。扣料失败 → `kMaterialRemoveFailed`；观察失败或变化不符 → `kMaterialObservationFailed`，均停止后续产出。
4. **原版保存**：`save_item` 成功且 `out_saved & 0xff != 0` 才 `kOk`，否则 `kProductSaveFailed`。`save_item` 之后 handle 所有权可能已转移，代码不再 free 或回读该 handle，因此**save 失败后不承诺能清理该 handle**，也不把 save 失败称为「无消耗」。
5. **失败口径**：扣料后失败允许非原子结果且允许不回退；UI 按阶段区分提示——扣料后阶段（remove / observation / save）统一「合成失败，材料可能已扣，请检查背包。」并清格（save 后所有权未知，不得提示空间不足或断言无产物入包），产物准备失败（扣料前）独立「产物准备失败，材料未扣。」并清格，未扣料的库存/前置失败保持「材料不足」且不清格。不伪称已回滚，也不采用「先保存产物、扣料失败时删产物即可恢复材料」的错误等价假设。未新增库存事务 provider/plugin 依赖、journal 或公共 transaction 接口。

三格流程继续调用受 inventory hooks 管理的原版函数入口，保留 H-03 类别计数、H-05 对象删除、H-13 保存与 H-21 类别补扣的扩展背包支持；不绕过入口调用 backup，不新增 EB 依赖，也不默认限制为物理袋。支持以对应 hook live、EB capability 可用且 enabled 为条件。宝石升阶的 Game-owner 写入限制与三格流程分开核验（见 `static-audit.md`）。

### 3.2 CR10 材料写回顺序（已实现）

宝石升阶 `api_make_item`（`feature.cpp:1026-1101`）顺序为：quality 查询 → item word 交叉核对 → 材料需求前置校验 → 计算升阶候选（随机）→ 提交 need 写入 → 最后写 jewel word；need 写入失败即返回失败且不写 jewel。失败时已写入的展示值允许残留；该顺序不等价于 full atomic。

### 3.3 确认/取消与错误分类（2026-10-15 回归修复；不改变原 CR finding）

- **确认必须双回调**：`kConfirmPending` 时 UI 必须提供非空 `ok` 与 `cancel`（adapter 契约拒绝任一为空）。历史实现传 `nullptr` cancel，导致弹窗未创建、正确配方无反应；本轮补齐取消回调。
- **取消只作废**：`cancel_pending_confirmation()` 仅丢弃 pending recipe/identity 并递增 token，不写物品、不随机、不扣料、不保存、不清空已填格（`feature.cpp:1152`、`feature.h:51`、`ui/runtime.cpp:160-163`）。
- **失败分类**：`api_prepare` 的「未命中」与「已匹配但槽位身份/类别上下文不可用」分开——前者 `PrepareOutcome::kNoMatch`，后者尾追加 `PrepareOutcome::kUnavailable = 3`（`result.h:20-26`、`feature.cpp:637-693`）。槽位/类别读取错误经 `QolResult` 上浮，**不进入配方判定**，不伪装空槽（`ui/feature.cpp:158-203`）；合法空槽要求 handle 的 `slot` 与 `generation` **同时为 0**，半无效 handle（任一为 0、另一非 0）返回 `kQolErrInvalidArgument`("malformed item handle")。
- **提示分类**：本地 scope 由 5/6/7 扩为 5–11（新增配方不匹配、合成暂不可用、状态已变化、放料写入失败），popup id 94/94/94/99；真正库存不足仍用材料不足原文（`text_table.h:14-33`、`ui/runtime.cpp:138-163`）。弹窗创建失败明确提示「合成暂不可用，材料未扣。」而非静默（`ui/runtime.cpp:343-351`）。
- **边界不变**：CR01 的阶段划分、`save` 失败后不承诺清理 handle、扣料后允许不回滚、12 成员结果 API 表、无新 provider / Host ABI 均保持。
- **本轮证据与状态**：受影响 7 target 重新构建后 **7/7 PASS**（`.tmp/custom-recipe-fix-20261015/final-ctest.log`，确保畸形 handle 分支进入二进制）；`check_arch` 通过、`check_http_routes` 147/147；`scripts/build-debug.sh --offline` 成功、`scripts/patch-apk.sh` 成功 3/失败 0，canonical 交付入口 `output/Inotia4_v1.3.2_monster_v31_lspatched.apk`（SHA-256 `00dcbd36b16b27efe517dc99a374b6ccd1e797669d3683b04ca3b2921def84c2`，内嵌模块 `6ef35a38129e…baa77`）。已 `install -r` 并重启：`/api/health` ready / monster v31、`screen=main_menu`、运行身份 `192/0.7.13`/`sha256=6ef35a38129e…` 与模块严格一致、`registered=28 install_failed=0`。本轮未执行合成/视觉手测；部署身份≠业务验收，**VM 仍 NOT_ACCEPTED**。

### 3.4 子集 A：启动恢复与表状态域（2026-10-15；实现核对新增 P1-A/P1-B）

> P1-A/P1-B 是**更早实现核对新发现**的缺陷项（非 CR01–CR11 重定义或重编号），也不改变 static-audit 的既有 finding。
> Gate 1A 另列 **G1A-01/G1A-02**（attempt1 阻断项）与 P3（诊断口径）；两组编号互不覆盖。

- **P1-A（Android bridge 自置 busy）**：修前 bridge 先置 `g_state.busy = true` 再调核心，被 core 首行 `busy → kBusy` 自我拒绝，真实 Android acquire 恒失败；旧「纯 core」8/8 未覆盖 bridge。修法：busy 判定/置位/复位收敛到 core 内 RAII `BusyGuard`（任何 return 都复位），bridge 只装配槽位后调用 core（`table.cpp:246-261,465-477`）。
- **P1-B（缺表/元数据分类须先于状态变更）**：修前「合法重激活」与「游戏换表清状态」排在分类之前，非法 probe 可经「已有 buffer」捷径被判 OK 且拒绝路径会改写模块状态。修法（实际顺序，`table.cpp:246-343`）：读取 probe → 分类（NotReady/Unsupported 直接返回、不改模块状态）→ 合法重激活 → 换表清理（旧缓冲不 free）→ 抓组位快照 → **书名册字节不变式（在新注入前校验）** → 分配 → 注入。不得读成「所有永久错误发生前没有任何模块状态/RAII 变化」——快照式校验位于快照之后。
- **启动期 ownership 定向恢复（含 G1A-02 修法）**：仅 `ownership × fail-soft × 明确 kNotReady × Free` 武装一次；SaveEnter 只锁存 token，Host 主帧在 reconciler 之后单 claim 恢复 → Live → `on_resource_ready(ownership, nullptr)` 只重算业务门禁；不重装 12 hook、不改 desired/Store、不重复 install/on_enabled（`host/plugin_registry.{h,cpp}`、`host/assembly.cpp:349-352,359,386`、`contracts/plugin.h:198`、`plugin.cpp:258-270`）。信号只在**事务终态、准入通过、已选出目标 claim**后消费；同 token 初始安装在途时保留信号（`host/plugin_registry.cpp:3022-3122`），逐帧 `step → retry` 测试覆盖「末步前到达不丢、恢复恰好一次、hooks 不重装」。
- **本批范围**：MEMORYTEXT 域分离、文本按压轮廓校准、合成启动恢复、表 typed 状态域为**可交付子集 A**；无关项仍未实现：双 TEXT 工厂与 Debug 并列展示、mainmenu 精确字号 20→30、高度 ×1.5、容器 clip。
- **证据与状态（`validation/status-r2.txt`）**：跨 lane 生产链 10 目标 CTest **10/10**；整树 **143 目标全部编译通过**、全量 CTest **141/143**（红项仍是 `core_api_lifecycle_tests` 旧 `kHidden` 断言与未改动的 `help_page_tests` 布局断言，源/测试相对 HEAD 未改；先前 `config-native-owner` mkdtemp 失败已消除）；`check_http_routes` 147 条、缺失/额外 0 无漂移；`check_arch` **全部通过**。原 13 条 `resource_unknown_symbol` 是扫描器把 `recipe_ptr_slot`/`mixture_ptr_slot` 业务字段名当机制标识，已按机制完整标识符/调用形态边界修扫描器（非改名/豁免/freeze），12 正反例单测通过。
- **Gate 1A**：attempt1 暂不通过（G1A-01 P1 production `set_context` 遗漏、G1A-02 P2 installing 期信号提前消费），两项已修并补证据；**attempt2 已正式 PASS**（G1A-01/02/P3 关闭、无新材料阻断），**VM 仍 NOT_ACCEPTED**。
- **状态**：本批 Debug 构建成功（`output/inotia4_qol_lsposed_debug_2610151556_ed9081c4c54e.apk`，SHA-256 `ed9081c4c54e4bd5e0829ae8749d58d6a70e5101a0428f131cb299e953d29fd2`，`.tmp/ui-craft-regression-20261015/validation/build-debug.log:272-276`）；**未集成打包/未安装、无新运行 SHA** → **NOT_ACCEPTED**。

## 4. 启动、失败回滚与停用顺序（已实现事实）

`plugin_install`（`plugin.cpp:122-210`）在启用业务门禁前完成：`sdk`/`game`、两条 port 的 ABI/函数、commands 句柄、required capabilities（`item.facts` 含 size 校验）、text 订阅、两个 typed handler、`ui::bind`。任一步失败按逆序幂等清理本次注册（退订文本、注销 handler、`ui::clear()`），不依赖 Host 后帧清理。install 不打开业务门禁；`plugin_on_enabled` 只在 `ownership_ready()` 且 `ui::core_backups_ready()` 时开门禁。

停用：`plugin_on_disabled` 先 `set_enabled(false)`、作废 pending token，随后 Host drain 并按资源逆序释放（ownership 先 release 恢复 RECIPEBASE 记录数与组位，再释放 UI hooks）；`plugin_deactivate` 撤销事件/text subscriptions、typed handlers、UI context/backups 与 facts/ports。残留表缓冲不 free，维持 stale panel buffer 安全性约束（`table.cpp:374-395`）。

## 5. 验证目标与当前证据状态

受影响 targets 与合并后应覆盖范围（本批已在受影响的 17 个目标上执行并通过；下表为范围口径，个别目标的具体断言见 `verification-log.md`）：

| Target | 合并后应覆盖范围 |
|---|---|
| `custom_recipe_tests` | catalog/table/rules：8 条静态三格规则、动态规则生成、table bytes/组位 restore。 |
| `custom_recipe_handle_chain_tests` | 生产 feature provider：place/prepare/execute/make-item typed handle 路径；CR01 各阶段失败注入、CR10 need/write 顺序与 CR02/CR03 失败语义。 |
| `craft_ui_guard_tests`、`craft_ui_handle_identity_tests` | UI callback → 内部结果 API；合并后不再 require 外部 `craft_result`，typed command 入队时序保持。 |
| `plugin_registry_tests`、`plugin_lifecycle_tests`、`capability_lifecycle_tests`、`feature_reconciler_tests` | 单 descriptor dependency graph、feature off/on、资源 fail-closed/fail-soft、partial install cleanup 与 table rollback。 |
| `hooks_transaction_tests` | required 资源部分安装/逆序释放、optional fail-soft 与在途/隔离路径。 |
| `host_adapter_tests` | 两 write ports 的 owner/assembly wiring 与 Game Adapter port 边界。 |

当前证据状态（详细登记见 [`verification-log.md`](verification-log.md)）：

- L0：契约/端口头与 adapter 语法证据通过；`host_adapter_tests` 链接因缺共享随机 stub 而受阻（`fn_math_get_random` 未定义），未在本 lane 修 stub/CMake。
- L1：`rules`/`catalog` 机械检查（随机签名与错误传播、动态表代际）通过 —— `.tmp/custom-recipe-implementation/l1/mech_check`。
- L2：`plugin.cpp`、`ui/feature.cpp`、`ui/runtime.cpp` 语法检查通过。
- L3：Host 资源/装配 lane 的手工构建目标运行（`hooks_transaction` 439、`plugin_registry` 635、`plugin_lifecycle` 162、`capability_lifecycle` 191、`feature_reconciler` 全节通过），使用 `.tmp` 手工构建，不改仓库。
- L4 集成/CMake：仓库 CMake 增量构建 PASS（`.tmp/custom-recipe-implementation/final-host-build.log`）、受影响 CTest **17/17 PASS**（`final-affected-ctest.log`，含新增 `custom_recipe_owner_fixture_tests`）；全量 CTest **141/142**，唯一失败 `core_api_lifecycle_tests` 为与本合并无关的旧断言（测试与 `core.api` 源与 HEAD 一致，`unrelated-test-baseline.txt`），未修改无关源码。
- 静态校验：`check_arch` 全通过、`check_http_routes` 147 routes 无重复/遗漏漂移、`git diff --check` 通过（`final-check-arch.log`、`final-check-http-routes.log`、`final-diff-check.log`）。
- Debug 构建：`scripts/build-debug.sh --offline` BUILD SUCCESSFUL，产物 `output/inotia4_qol_lsposed_debug_2610142037_ff39e1c3502f.apk`，SHA-256 `ff39e1c3502f5e4402cfd26ede3e21767fcfb695cdc8c355367ade667802dd68`（`final-build-debug.log`）。
- oracle 最终只读复核（`ses_ec6e834faffeHZgxHmdF07dgtj`）：F1–F3、typed handler gate 与 ready-failure ownership 风险静态可关闭，无新增阻断；为静态复核，不是 VM 验收。

以上 Host/构建/静态证据仍不等于 VM-CR-MERGE-01 通过；不得用历史 PASS 行作为本轮合并证据。

## 6. VM-CR-MERGE-01（候选卡）

**状态：NOT_ACCEPTED（部署身份已建立；功能行为未验）**。`deployment identity established ≠ functional VM`。本节记录 2026-10-14 集成包与协议页阻断过程；更晚（2026-10-15）的 canonical 部署与运行身份见 §3.3 与 `verification-log.md` §3，本节不覆盖上述历史过程。

已完成（部署/身份口径）：

- 安装集成包 `output/custom_recipe_261014/Inotia4_v1.3.2_monster_v31_custom_recipe_ff39e1c3502f_lspatched.apk` → `Success`（APK SHA-256 `52a5b61363a92f10d706fb161838e43339d8717d2b7aa697c8752e53898fb828`；该路径为历史记录，包与目录已按清理要求删除，不作为当前交付入口）；内嵌 module `ff39e1c3502f5e4402cfd26ede3e21767fcfb695cdc8c355367ade667802dd68`，与 Debug 构建产物一致；package `com.com2us.inotia4…common` 不变，未卸载、未清 data。
- 游戏启动后运行身份核对一致：`versionCode=192`、`versionName=0.7.13`、`sha256=ff39e1c3502f…`（`device-api/running-module.log:33`）。
- `/api/health` `ready=true`、`monster/v31`（`after-health.json`）；config projection 仅注册 `custom_recipe`（`custom_recipe.recipes`），`craft_ui` 不在 projection（`after-config-list.json`）。
- 安装前已导出 slot 0 备份 `20261014-203427_s0_c97d845ba2b1.qol_save`（checksum `c97d845ba2b1`）并保留。

尚未完成（阻断功能验证）：

- 启动停在 Android 协议页 `AgreementUIActivity`（选项 `id=ok`/`同意`），**必须由用户手动确认**；本任务未点击（`startup-ui.json`、`startup-ui-dialog.json`）。
- 未 `enter_slot`、未发放材料、未执行任何配方；`startup-system-info.json` 的 `save_slots=false` 属启动未完成观察，**不能断言存档被删除**。
- 观察项（不本轮扩修）：`running-module.log:17` optional `attribute.quality` 边被降级以防循环；`running-module.log:59` `recipe table not ready recipe_size=0 mixture_size=0 count=0` 为协议页启动态，尚无 loaded-world 表资源证据，**不能宣称合成功能已在真机 ready**。

进入世界态后需核对 owner/resource 状态（唯一 `custom_recipe.recipes` feature、descriptor/资源/依赖），必要时按既有 off/on 重试 ownership；不新增自动 retry 机制。行为验收项：3-slot place/prepare/YesNo/execute 与 8 条静态及动态 recipes；jewel tier-up 与 quality 失败提示区分；text scopes；required hook rollback/optional degradation；table restore/count 与残留 UI 安全；关闭/恢复后的唯一可见开关行为；CR01 各阶段失败提示。另确认 `core.craft` `craft.facts` 及 `mix-direct` 501/path/method/response 未变化。
