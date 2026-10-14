# `custom_recipe` 静态审计（2026-10-14）

> 基线 HEAD：`ac0dfc73`。A–G 结论与 CR01–CR11 finding 的**代码证据仍是该基线的只读核对结果**，含义与编号保持不变；本文件另加「本轮实施对应」列，记录合并实施（L0–L4）对每条 finding 的落地位置或仍保留的观察，不重编号、不把条件观察冒充新功能或已确认事故。获批目标见同目录 [`design-card.md`](design-card.md)，验收登记见 [`verification-log.md`](verification-log.md)。
>
> 基线路径提示：finding 证据中的 `plugins/craft_ui/*` 与 `contracts/craft_result.h` 在合并实施后已删除（UI 迁入 `plugins/custom_recipe/ui/`，结果类型迁入 `plugins/custom_recipe/result.h`）；这些引用保留为基线证据，不代表当前文件存在。

## A–G 结论（基线证据）

| 维度 | 静态结论 |
|---|---|
| A. 契约符合性 | `custom_recipe` 当前 id/version 为 `custom_recipe`/`0.1.0`，声明 required `item.facts` 3.3、`craft.facts` 1.0、`text_layer` 1.0，optional `attribute.quality` 2.0，provides `craft_result` 1.1，port 为 `QOL_PORT_CUSTOM_RECIPE_WRITE` 1.1；JNI count 为 0。`core.craft` 另行提供只读 `craft.facts` 1.0。`/api/op/craft/mix-direct` owner 是 `core.craft`，Kotlin 当前返回 501，不能据 owner 元数据推断有 native 合成 handler。 |
| B. 依赖方向/边界 | consumer/provider edges 均在 descriptor 中声明；当前 UI owner 是独立 `craft_ui`，其 required `craft_result` consumer 调用 `custom_recipe`。发现 CR04：配方文本表直接声明/调用 `qol::infra::active_text_scope()`；获批合并目标应以内部 UI scope 处理，不扩展 Host SDK。两个 typed write ports 仍分属两个 descriptors。 |
| C. 行为与错误语义 | 当前实现存在 CR01/02/03/06/10 所列条件风险或语义错配。CR01 的分阶段失败处理目标已裁决但尚未实施，不承诺强原子事务。CR07 是源码注释与 literal 长度不符，不影响当前 `sizeof` 文案长度门控。CR06 的预期行为待核，不能按 finding 编号推定应改成某一结果。 |
| D. 生命周期/并发 | 三格 pending recipe/token 由 UI 路径保存，代码注释按游戏 UI 主线程使用且未加锁；SaveEnter 会原地更新 dynamic recipe 缓冲。CR08 是 SaveEnter 与待确认窗口交错时的条件风险，尚无证据证明可达。CR11 是 plugin install 部分副作用清理路径差异，未证实泄漏。 |
| E. 测试覆盖 | CR05：当前纯规则/目录 target 与 handle-chain target 未覆盖 execute 扣料、产物创建/保存各失败点的矩阵，也没有覆盖完整 hook wrapper 安装/运行链。该结论只描述测试结构，不声称当前测试已失败。 |
| F. 文档事实 | CR07：`catalog.h` 注释声称配方描述为 36 UTF-8 字节 + NUL；当前 literal `用3个材料合成` 为 19 字节 + NUL。`sizeof(kModuleRecipeDesc)` 仍使用实际 literal 长度，相关门控不依赖注释数字。 |
| G. 基础设施/运行边界 | 当前注册表先构建完整 capability DAG，再按拓扑顺序安装；plugin 注册顺序不构成 load-order finding（`host/plugin_registry.cpp:2013-2021,2037-2049`）。CR11 需分别记录 feature-mode 异步 release 与 legacy `install_all` 路径，不能笼统宣告 Host cleanup 已统一。 |

## Findings CR01–CR11

| ID / 级 | 基线代码证据与影响（ac0dfc73） | 边界/处置 | 本轮实施对应 |
|---|---|---|---|
| CR01 / P1 | `custom_recipe::api_execute` 扣料前复核 held count，但 `remove_item_data` / `remove_item` 的 `QolResult` 被忽略，随后继续创建产物；创建、write 或 save 失败返回 `kNoBagSpace`，已扣材料没有回滚路径。Game Host adapter 可返回 NotReady、Blocked、NotFound 等错误（`plugins/custom_recipe/feature.cpp:496-523,525-584`；`game/adapter_ports/game_host_port.cpp:543-565`；`contracts/craft_result.h:42-58`）。这是条件风险，不是当前已真机复现。 | 用户裁决：不加新插件依赖、inventory txn/journal 或公共 transaction 接口；先完整确认配方可合成（不是 reservation/锁定），再在当前 owner 内尽量保持原子；非原子可接受，扣料后失败允许不回退，不承诺强原子。必须沿用原版入口对 EB 的支持，不得限制为物理袋或默认拒绝 EB。H-03/H-05/H-13/H-21 接线见 `plugins/inventory_hooks/native_inventory_hook.cpp:521-533,686-710,713-747,925-968`、`plugins/inventory_hooks/inventory_hook_stage4.h:147-153`。tier-up 的 Game owner/handle 限制另待核，不作为 three-slot 拒绝 EB 的理由。 | **已实施**：`api_execute` 分 precheck → 产物准备（未入包）→ 原版扣料+数量观察 → save，阶段结果用 `CraftExecuteResult` 区分（`feature.cpp:695-1005`、`result.h:35-46`）；扣料后阶段（remove / observation / save）统一提示材料可能已扣，产物准备失败独立提示材料未扣，且不再断言「无产物入包」；save 失败不称无消耗、不承诺清理 handle（`ui/runtime.cpp:167-221`）。无阶段失败 Host 证据，VM 未验。 |
| CR02 / P2 | `api_craft_gate` 把任意 `query_jewel_quality` error 映射为 `CraftGate::kNotJewel`；`craft_ui` 对该枚举显示“只有宝石”。因此质量 provider 不可用与真实非宝石在 UI 上混同（`plugins/custom_recipe/feature.cpp:380-391`；`plugins/craft_ui/plugin.cpp:295-303`）。 | 目标：质量不可用提示与非宝石提示区分；只阻断 tier-up，不停用整插件或三格配方。 | **已实施**：`CraftGate::kUnavailable`（`result.h:48-53`）由 quality/fact 失败与 word 不一致返回（`feature.cpp:572-596`），UI 显示「功能不可用」并与「只有宝石」分开（`ui/runtime.cpp:380-383`）。 |
| CR03 / P2 | `craft_rand64_adapter(lo, hi)` 在 `lo <= hi` 时恒返回 `lo`，而 `CraftMakeItemInput::rand_inclusive` 契约要求闭区间随机源（`plugins/craft_ui/plugin.cpp:311-327`；`contracts/craft_result.h:104-109`；`plugins/custom_recipe/rules.h:39-43`）。 | 目标明确为使用游戏真实闭区间随机源；当前常量下界只是实现事实，不是目标语义或验收结论。 | **已实施**：write port ABI 1.2 增加 `random_inclusive`（`contracts/game_ports/custom_recipe_write_port.h:25`、`feature.cpp:62-75`）；UI 适配直接调用父 `random_inclusive` 并传播错误（`ui/runtime.cpp:394-396`）；rules 随机桥校验落点并传播错误，不 clamp（`feature.cpp:425-436`）。真实随机边界尚无 Host/设备证据。 |
| CR04 / P3 | `custom_recipe/text_table.h` 前置声明并直接调用 `qol::infra::active_text_scope()`（`:17-20,66-69`）；共享 Host SDK text-scope ABI 当前只有 `push`/`pop`，没有 getter（`contracts/host_sdk.h:306-311`）。 | 获批合并目标：将当前 scope 保存在内部 UI 子层，用内部 RAII scope 查询文本表；不扩展 Host SDK。 | **已实施**：UI 子层本地 TLS `t_current_text_scope` + `TextScopeGuard`（`ui/feature.cpp:20,246-261`、`ui/feature.h:140-152`）；`text_lookup(scope, id)` 显式 scope（`text_table.h:71-78`）；继续调用 `text_scope.push/pop`，未新增 getter。 |
| CR05 / P2 | `custom_recipe_tests` 编译纯 rules/catalog/table；`custom_recipe_handle_chain_tests` 链接生产 feature，但当前链路只覆盖 execute 成功及若干 place/tier-up/quality 错误，没有 execute 的扣料返回错误、create/save 失败矩阵或完整 UI hook wrapper 集成（`tests/CMakeLists.txt:245-270`；`tests/test_custom_recipe_handle_chain.cpp:304-411`）。 | 目标实施后须补受影响失败矩阵；本审计任务未新增或运行测试。 | **已补覆盖（L4）**：失败矩阵与 UI wrapper 测试已落在生产测试中（含 `custom_recipe_handle_chain_tests` 阶段失败、`craft_ui_guard_tests`/`craft_ui_handle_identity_tests`、以及新增 `custom_recipe_owner_fixture_tests`）；受影响 17 目标 CTest 通过（`verification-log.md` §3）。仍为 Host 证据，不替代 VM。 |
| CR06 / P3 | `api_place` 对无有效 item 或无效槽位返回 `PlaceOutcome::kPlaced`（`plugins/custom_recipe/feature.cpp:395-401`）；`craft_ui` 将 `kPlaced` 映射为 `kAdvance` 并选下一空格（`plugins/craft_ui/feature.h:36-49`、`plugin.cpp:191-204`）。 | 期望行为尚未核实；不推定应改为报错、停留或其他结果。 | **保留原行为**：`api_place` 无效输入仍 `kPlaced`（`feature.cpp:600-607`），UI 映射不变（`ui/feature.h:36-50`）。未裁决，未改动。 |
| CR07 / P3 | `catalog.h` 注释称 `kModuleRecipeDesc` 为 36 bytes + NUL（`:49-52`）；当前 literal 为 19 UTF-8 bytes + NUL。代码用 `sizeof(kModuleRecipeDesc)` 做长度比较（`plugins/custom_recipe/feature.cpp:315-322,672-680`），该门控取实际字面量大小。 | 注释精确性观察；不据此扩展成配方描述文案/行为问题。 | 未变（`catalog.h:51-54`、`feature.cpp:1095-1104`）：门控仍取实际字面量长度，注释数字不影响行为。 |
| CR08 / P3 | `set_dynamic_three_slot_recipes` 原地覆盖固定 `g_dynamic_recipes` 数组（`plugins/custom_recipe/catalog.cpp:255-263`）；pending 只保存数组元素指针和 token（`feature.cpp:291-329`）；SaveEnter 重建没有更新 pending token（`feature.cpp:341-355`）；execute 通过重新匹配并比较结果地址验证（`feature.cpp:450-468`）。 | 若 SaveEnter 与未决 YesNo 确认交错，数组同址元素可能已换内容；目前仅条件风险，是否可达未验证，不写成设备已复现。 | **已实施防线**：动态表每次替换递增 generation，pending 记录 generation，execute 校验 generation 与 recipe 值相等（`catalog.cpp:280-294`、`feature.cpp:398-420,700-718`）；是否完全闭合仍待验证，未做设备复现。 |
| CR09 / P3 observation | `UIMix_ResetStuffItemControl` 声明 optional/fail-soft，但没有 feature gate；其 wrapper 调原版后选择第一个空格（`plugins/craft_ui/plugin.cpp:429-436,486-488`）。插件停用时 descriptor resource 仍会释放；这与 feature off 的 gate 状态不同。 | 保持 optional，不把该观察升级为 required；安装失败降级与插件停用释放语义分别记录。 | **保持**：ResetStuff 仍 optional/fail-soft 且 wrapper 不受 gate（`ui/runtime.cpp:508-512,507-509`）。 |
| CR10 / P2 | `apply_material_count` 在 character-level facts 失败时直接 return，`write_stuff_need` 的 QolResult 被忽略（`plugins/custom_recipe/feature.cpp:263-288`）。tier-up 在 jewel word 写成功后才调用该 void helper，并最终返回 0（`:661-669`）；若 need 更新失败，后续原版 UseStuff 可能读取旧的 stuffList 数量。通常放料阶段已写入正确 need（`plugins/craft_ui/plugin.cpp:232-245`），因此是条件风险，不代表普遍少扣。 | 用户批准的目标顺序：先取得 quality candidate 并完整校验所需材料；成功提交 need 后最后写 jewel word；所有步骤成功才返回 0 进入 UseStuff。失败时 need 显示值允许残留；该顺序不代表 full atomic。 | **已实施**：材料需求前置校验 → 计算候选 → 先写 need → 最后写 jewel；need 失败返回失败且不写 jewel（`feature.cpp:1044-1093`）。 |
| CR11 / P3 conditional observation | `plugin_install` 在完成所有验证前会订阅 text consumer、require facts 并注册 typed handlers（`plugins/custom_recipe/plugin.cpp:49-115`）。Feature-mode install failure 会调用 `feature_stop` 排入异步 release transaction；`deactivate` 到 drain/transaction terminal 才调用，至少跨一个 lifecycle step，不在 install call 内同步清理（`host/plugin_registry.cpp:2334-2345,1468-1486,2706-2714`）；`install_all` 的 `install_plugin_sync` 在 `plugin.install` 返回非零时只标失败并 return，不调用 `deactivate`/`feature_stop`（`:1728-1745`）。 | 存在路径/时序差异，但没有确认的 runtime leak 或设备可达性证据；保持条件观察，不升级为 P1 泄漏结论。 | **已实施**：`plugin_install` 在任何登记副作用前完成全部校验，登记失败时同步幂等清理本次注册（`plugin.cpp:122-210`）；typed handler 另加 `enabled()` 门禁（`feature.cpp:1195,1193`），已由测试覆盖；F3 增加 install 中止原语，required-ready 或 soft-ready 撤回失败会先 abort 在途 install，使后续步骤（尤其 trailing ownership acquire）无法运行（`hooks/transaction.h`）。Host 路径差异仍是条件观察，未宣称统一；oracle 静态复核无新增阻断。 |

## 独立复核修复项 F1–F3（不占用 CR 编号）

以下三项由独立源码复核提出并在本轮实施；它们是**新增复核项**，不覆盖、不重编号 CR01–CR11，也不改变已有 finding 的严重度与含义。

| 项 | 修复内容 | 实施位置与证据 |
|---|---|---|
| F1 | `read_stuff_item` 严格身份/就绪语义：主线程检查后控制不存在 → `NotReady`；控制存在但 `stuff_item` 读取缺函数 → 成功 + 无效 handle（正常空格）；对象非空仍走原 handle 注册/解析并保留其错误。 | `game/adapter_ports/craft_port.cpp`；`host_adapter_tests` 覆盖 OK / wrong-thread / NotReady 路径（`.tmp/custom-recipe-implementation/review-fixes/validation.txt`）。 |
| F2 | 三格合成失败提示语义：扣料后阶段（remove / observation / save）统一「合成失败，材料可能已扣，请检查背包。」+ 清格；产物准备失败独立「产物准备失败，材料未扣。」+ 清格；不再断言无产物入包/空间不足，保留 stage 日志。 | `ui/runtime.cpp:167-221`、`text_table.h:14-78`；文档同步见 `design-card.md` §3.1。 |
| F3 | 安装中止与安全撤回：`rollback_install` 跳过已安全撤回（`kFree` 且 runtime 为 false）的条目并清 `installed`，`kFree` 但 runtime 仍在的条目照常释放；新增 `hooks::tx_abort_install`，registry 在 required-ready 或 soft-ready 撤回失败时先 abort 在途 install，阻止后续步骤（尤其 trailing ownership acquire）继续。 | `hooks/transaction.{h,cpp}`、`host/plugin_registry.cpp`；`hooks_transaction_tests` / `plugin_registry_tests`（`review-fixes/validation.txt`）。oracle 只读复核确认静态可关闭，无新增阻断，非 VM 验收。 |

## 用户裁决与边界

- **CR01 当前裁决**：按 [`design-card.md` §3.1](design-card.md) 的分阶段失败处理执行；允许非原子及扣料后不回退，不新增事务依赖或公共接口。实现已落地但阶段失败 Host 测试与 VM 仍缺证，不能记为已验收或强原子保证。
- 原版 entry 已核：H-03 类别总数含物理+扩展，H-05 对象删除经扩展识别，H-13 SaveItem 可收编，H-21 RemoveItemData 按物理实扣差从扩展袋补扣。three-slot 继续使用这条原版入口链，保留扩展袋自动支持；不得只允许 physical bag 或默认拒绝 EB。
- tier-up 的 Game owner/handle 限制另待核；不能用该项未核事实扩大为 three-slot 拒绝 EB。
- CR10 批准顺序为「candidate/need 校验 → 成功写 need → 最后 jewel write → 全成功后返回 0」；允许失败后 need 显示值残留，不要求 full atomic。
- CR03 目标是游戏真实闭区间随机；CR02 质量服务不可用与非宝石需区分提示，并只阻断 tier-up；CR09 ResetStuff hook 保持 optional；CR06 未裁决，保留原行为。
- CR04 scope getter 由内部 UI RAII 解决，不增加 Host SDK getter。
- 合并目标和 API 例外见 `design-card.md`；其中 `/api/op/craft/mix-direct` 保留 `core.craft` owner 与现有 501，不迁入 custom_recipe，不新增 NativeBridge/JNI。
- 未登记“插件注册顺序导致 consumer blocked” finding：registry 在 install 前构建完整 capability DAG 并按拓扑顺序安装（`host/plugin_registry.cpp:2013-2021,2037-2049`）。
