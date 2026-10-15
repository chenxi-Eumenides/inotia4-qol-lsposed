# `custom_recipe` 插件参考

## 摘要

`custom_recipe` 是自定义合成配方与合成结果的唯一 owner：查询/判定配方结果，管理静态与动态三格配方，执行扣料、产物创建/保存与宝石升阶，并持有 RECIPEBASE/MIXTUREBASE 表注入的行为所有权。原独立 `craft_ui` 插件已并入本 owner 的内部 UI 子层（`plugins/custom_recipe/ui/`），不再作为独立 descriptor 注册。`core.craft` 仍是独立 `craft.facts` v1.0 provider。本插件不提供 HTTP 或 JNI，也不聚合通用 craft API。文中路径相对 `module/app/src/main/cpp/`。

## 身份、开关与 descriptor 面

插件 id/version 为 `custom_recipe` / `0.1.0`（`plugins/custom_recipe/plugin.cpp:33-34,311-312`）。唯一可见开关是 descriptor feature `custom_recipe.recipes`：默认关闭、Gameplay/toggle、priority `-1`（`plugin.cpp:297-331`）。原 `craft_ui` 的 hidden feature key `craft_ui.custom_crafting` 已从注册面删除；Store 中既有残留保持原样，既不迁移、不改写、不删除，也不参与注册投影或运行。

| Descriptor 面 | 当前声明 |
|---|---|
| `provides` | 无。结果 API `result::CraftResultApi` 内化为 UI 子层对 result 层的进程内调用，不经 capability registry 对外提供。见 `plugin.cpp:297-331`、`plugins/custom_recipe/result.h:92-105`、`plugins/custom_recipe/feature.h:34-35`。 |
| `requires` | `item.facts` v3.3、`craft.facts` v1.0、`text_layer` v1.0。见 `plugin.cpp:265-276`。 |
| `optional_requires` | `attribute.quality` v2.0。仅宝石升阶路径使用；缺失/失效只置 degraded，不阻断 owner 或三格配方。见 `plugin.cpp:259-263`。 |
| `ports` | 两条：`QOL_PORT_CUSTOM_RECIPE_WRITE` v1.2（`contracts/game_ports/ports.h:52`）与 `QOL_PORT_CRAFT_UI_WRITE` v1.1（`contracts/game_ports/ports.h:51`）。见 `plugin.cpp:278-285`。 |
| `resources` | 13 条：5 条核心 UI hook（required/fail-closed）+ 7 条装饰 UI hook（optional/fail-soft）+ 末尾 1 条 `own:RECIPEBASE_MIXTUREBASE` ownership（required/fail-soft）。顺序即声明顺序，ownership 最后。见 `plugins/custom_recipe/ui/runtime.cpp:526-566`、`plugin.cpp:55-90`。 |
| HTTP/JNI | JNI count 为 0，不声明 HTTP handler/route。合成直操作路由仍归 `core.craft`（见下文）。见 `plugin.cpp:297-331`。 |

## 层分工

| 层 | 文件 | 职责 |
|---|---|---|
| Descriptor / 生命周期 / typed 接线 | `plugin.cpp` | 依赖与端口校验、注册事务与失败清理、门禁、text 消费者、资源声明组装。 |
| UI 子层 | `ui/feature.{h,cpp}`、`ui/runtime.{h,cpp}` | UIMix 界面动作与文案、12 条 hook wrapper、本地 TLS text scope、UI 本地 backups 与 claims。 |
| 结果层 | `feature.cpp`、`feature.h` | 放料/准备/执行/产物/材料需求/宝石升阶、quality 消费、动态配方事件、ownership ops。 |
| 纯规则 | `rules.{h,cpp}` | 档位序数与区间搜索、材料数量、三格库存/token 判定、宝石数值缩放。 |
| 静态/动态目录 | `catalog.{h,cpp}` | 两条 RECIPEBASE 注入 entry、8 条静态三格规则、26 类特殊装备动态配方构造。 |
| 表注入 | `table.{h,cpp}`、`text_table.h` | 注入构造与组位快照/恢复、ensure/deactivate、配方呈现文案表。 |

## 配方目录与运行机制

`catalog.cpp` 有两条注入 RECIPEBASE 的 entry，均挂在宝石强化页（group 3 = `1<<3`）：`Kind::kThreeSlotCraft` 的「合成」与 `Kind::kJewelTierUp` 的「宝石强化」（`catalog.cpp:52-63`）。

静态三格匹配表共 8 条规则（表序即优先级；`catalog.cpp:84-111`，定义见 `catalog.h:97-116`）：

| 规则 | 匹配语义 |
|---|---|
| 皮革 + 空槽 + 魔法衣料 | 有序，产物为大背包。 |
| 小恢复药水×2 + 低级武器强化卷轴 | 无序，产物为顶级宝石。 |
| 低级宝石×3 | 无序，产物为中级宝石。 |
| 中级宝石×3 | 无序，产物为高级宝石。 |
| 高级宝石×3 | 无序，产物为顶级宝石。 |
| 顶级宝石×3 | 无序，产物为混沌宝石。 |
| 任意宝石 + 混沌武器卷轴 + 混沌防具卷轴 | 有序，产物为第一格宝石本身，数值按 1200 permille 缩放并保留源宝石随机等级与属性类型。 |
| 两件同类别特殊装备 + 元气恢复药水 | 有序，产物为同类别特殊装备新建，孔总数与剩余强化次数拉满。 |

动态配方：26 个特殊装备类别（`catalog.h:146-151`）各生成至多一条三格有序配方，材料从 14 条材料池（`catalog.h:138-141`，含空槽候选）随机抽取且三者互不相同；容量上限 64（`catalog.h:129`）。每次 `kEventSaveEnter` 重建并递增 generation（`catalog.cpp:280-294`、`feature.cpp:440-461`）。匹配顺序为**静态表优先，其后动态表**（`catalog.cpp:296-304`）。

材料需求数量（`CountRule::kJewelGradeAndLevel`）：`ceil(base_count × grade × (105 − level) / 105)`，纯整数运算，`level` 钳到 `[0,105]`，满级为 0（`rules.h:60-63`、`rules.cpp:169-185`）。宝石档位由类别 28..32 映射为 1..5（低→混沌）。

### 三格放料、prepare 与 execute

`ui/` 对当前 UIMix 状态构造输入，结果层只使用调用方传入的槽位类别、typed item handles 与持有数量，不读取 UIMix/control（`feature.h:16-22`）。

- `api_place`（`feature.cpp:600-635`）：无效槽/无效物品视为「无事发生」返回 `kPlaced`（CR06 现状，见审计）；重复不可堆叠对象 → `kAlreadyPlaced`；可堆叠类别的持有数不足 → `kNotEnoughHeld`；写入失败 → `kWriteFailed`。
- `api_prepare`（`feature.cpp:637-693`）：先使旧 pending 失效，再按静态/动态表匹配；**真正未命中** → `kNoMatch`；命中后复核每格 live identity 与类别，通过才写 pending（recipe、items、categories、generation、token）→ `kConfirmPending`；**已匹配但读取槽位身份/类别等上下文失败**（`stuff_item` 读取错误、类别读取错误、传入类别与槽内实际类别不一致、声称空槽却存在对象）→ 尾追加的 `PrepareOutcome::kUnavailable`（`result.h:20-26`），与 `kNoMatch` 区分（`feature.cpp:637-693`）。
- `api_execute`（`feature.cpp:695-1005`）分阶段，结果枚举见 `result.h:35-46`：
  1. **precheck**：pending 有效且 token 未失效；动态表 generation 未变；`in` 的类别与 handle 与 pending 一致；live handle/类别/可堆叠性/类别持有数复核；扣料入口就绪；重复对象身份检查。阶段 1 冲突 → `kStaleSlots`，阶段 2 就绪失败 → `kPrecheckFailed`（均未扣料）。类别持有数不足 → `kNotEnoughHeld`。
  2. **产物准备（未入包）**：`create_perfect_item`（否则 `create_item`）创建产物并按模式改写（缩放数值位 / 拉满孔位与强化段）。失败即丢弃未提交产物并返回 `kProductPrepareFailed`（此时尚未扣料）。
  3. **扣料**：仍经原版 inventory entry（可堆叠用 `remove_item_data`、不可堆叠用 `remove_item`），并读取类别数量前后差值观察。扣料调用失败 → `kMaterialRemoveFailed`；观察失败或差值与预期不符 → `kMaterialObservationFailed`。
  4. **保存**：`write_port.save_item`。成功且 `out_saved & 0xff != 0` 才 `kOk`，否则 `kProductSaveFailed`。
- 确认与取消（UI 子层，`ui/runtime.cpp:160-358`）：
  - `kConfirmPending` 时 `craft_show_yesno` 必须提供**非空 `ok` 与 `cancel`**（adapter 契约：任一为空即拒绝创建，历史 bug 即 cancel 传 `nullptr`）。
  - 槽位读取返回的 handle 只有 `slot` 与 `generation` **同时为 0** 才算合法空槽；**半无效 handle**（任一为 0、另一非 0）视为畸形身份，返回 `kQolErrInvalidArgument`("malformed item handle") 且不进入配方判定（`ui/feature.cpp:158-203`）。
  - 取消回调 `on_cancel_three_slot` 只调用内部 `cancel_pending_confirmation()` 作废 pending/token：**不写物品、不随机、不扣料、不保存、不清空已填格**（`ui/runtime.cpp:160-163`、`feature.cpp:1152`、`feature.h:51`）。
  - 弹窗创建失败（写端口缺失、回调为空、adapter 返回非 OK）不静默：作废 pending 并明确提示「合成暂不可用，材料未扣。」，保留已填格（`ui/runtime.cpp:343-351`、`ui/feature.cpp:265-284`）。
  - 提示文案按错误分类各走独立本地 scope：配方未命中见下节；已匹配但上下文不可用/弹窗创建失败用 scope 9；确认期间格子或代际变化（`kStaleSlots`/`kAborted`）用 scope 10；放料写入失败（`kWriteFailed`）用 scope 11。**真正的库存不足**仍是既有的材料不足提示（scope 无覆盖，popup 94 原文，`ui/runtime.cpp:256-258`），取消后保留已填格；扣料后的可能损失**不回滚**（CR01 口径不变）。
- CR01 口径：前置校验在任何扣料之前；产物在扣料前准备并在扣料前失败时丢弃；扣料后的失败**允许不回退**，提示区分阶段（不伪称已回滚）。扣料后阶段（remove / observation / save）统一提示「合成失败，材料可能已扣，请检查背包。」并清格——save 后产物所有权未知，不得提示空间不足或断言无产物入包；产物准备失败（扣料前）独立提示「产物准备失败，材料未扣。」并清格。`save_item` 之后 handle 所有权可能转移，代码不再 free 或回读该 handle，因此**不承诺 save 失败后可清理该 handle**（`feature.cpp:982-997`、`ui/runtime.cpp:167-221`）。
- CR10 口径（宝石升阶）：走 `api_make_item`，顺序为 quality 查询 → 读取并与 item word 交叉核对 → 材料需求**前置校验** → 计算升阶候选（随机）→ **先提交 need 写入** → **最后写 jewel word**；need 写入失败直接返回失败且不写 jewel（`feature.cpp:1026-1101`）。失败时已写入的展示值允许残留；该顺序不等价于 full atomic。

### Jewel tier-up

- 档位为 `AttributeQualityTier` 的 Grey..Gold（0..5，`contracts/attribute_quality.h:22-29`）。当前档来自 `attribute.quality` 查询结果的 `quality.level`。
- 候选值：以 `AttributeRange` 调 `evaluate`（经 `AttributeQualityApi::evaluate`）搜索当前档之上的首个有代表值档位区间，再在闭区间内调用随机源取值（`rules.cpp:60-168`、`feature.cpp:259-277,1056-1066`）。
- 边界：已是 Gold → `UpgradeResult::kAlreadyGold`；区间退化（`max <= min`）→ `kDegenerate`；两者都返回失败，与合成键 `kAlreadyGold` 前置拦截互为双保险（`rules.h:18-22`、`feature.cpp:1079-1081`）。
- 随机：`api_make_item` 使用调用方 `CraftMakeItemInput::rand_inclusive`，经 `make_item_rand_bridge` 传播错误并校验落点（`feature.cpp:425-436`）。UI 侧 `craft_rand_inclusive` 直接调用父命名空间 `random_inclusive`，即 write port ABI 1.2 的真实游戏随机（`ui/runtime.cpp:394-396`、`feature.cpp:62-75`）；不恒返回下界、不做静默 clamp。

### 宝石升阶 gate 与提示（CR02/CR03）

`api_craft_gate` 返回 `result::CraftGate`（`result.h:48-53`）：物品事实或 quality 查询失败、item word 与查询 facts 不一致 → `kUnavailable`；真正非宝石 → `kNotJewel`；已金档 → `kAlreadyGold`；其余 `kPass`（`feature.cpp:572-596`）。UI 将 `kUnavailable` 映射为「功能不可用」提示，与「只有宝石」分开，且只阻断升阶路径（`ui/runtime.cpp:380-383`）。

## 文本与作用域

- 呈现文案表在 `text_table.h`：35291 于本地 scope 2（配方按钮）/3（面板标题）替换为「宝石升阶」；scope 4（页签）**不登记**，保持页签名原文。本插件内部提示 scope 现为 5–11（均不推入 Host）：
  - 5 = 「功能不可用」（97，tier-up 质量服务不可用）；
  - 6 = 「合成失败，材料可能已扣，请检查背包。」（94，扣料后阶段）；
  - 7 = 「产物准备失败，材料未扣。」（94，扣料前产物准备失败）；
  - 8 = 「配方不匹配，请检查材料组合。」（94，真正未命中配方）；
  - 9 = 「合成暂不可用，材料未扣。」（94，已匹配但上下文不可用 / 弹窗创建失败 / 确认时读取失败）；
  - 10 = 「合成状态已变化，请重新填入材料。」（94，确认期间格子或代际变化）；
  - 11 = 「放入失败，请重试。」（99，放料写入失败）。
  见 `text_table.h:14-24,27-33,50-60`。同一 popup id（94）由不同本地 scope 提供互不相同的文案；无 scope 覆盖时弹出原文（例如真正的库存不足仍为材料不足提示）。
- scope 由 UI 子层本地 TLS 持有：`t_current_text_scope`（`ui/feature.cpp:20`）与 `TextScopeGuard`（`ui/feature.h:140-152`、`ui/feature.cpp:294-309`）在绘制回调内保存/恢复，并继续调用 Host SDK `text_scope.push/pop` 对准范围 2/3/4；不在表中登记的私有 scope 不推入 Host。查表签名是显式 `text_lookup(scope, id)`，删除旧的 infra 活动 scope 裸声明（`text_table.h:71-78`）。
- 消费者在 install 时订阅 after/modify，在 deactivate 时退订（`plugin.cpp:44-47,172-178,212-216`）。

## 生命周期、门禁与表所有权

**install**（`plugin.cpp:122-210`）：先校验 `sdk`/`game`、`port_count == 2` 与两条 port 的 ABI/函数齐备、`commands` 注册句柄；再 require `text_layer`/`item.facts`（含 size 校验）/`craft.facts`；随后按序登记文本订阅、两个 typed handler、`ui::bind`。任一步失败**立即幂等清理本次注册**（退订、注销 handler、`ui::clear()`），不依赖 Host 后帧清理。install 完成时**不**打开业务门禁。

**门禁**：`plugin_on_enabled` 仅在 `ownership_ready()`（表注入就绪）且 `ui::core_backups_ready()`（5 条核心 hook backup 齐备）时 `set_enabled(true)`（`plugin.cpp:233-240`）。feature toggle 打开时**不提前**开门禁；关闭时立即 `set_enabled(false)` 并作废在途确认 token（`plugin.cpp:301-309`、`feature.h:44-47`）。

**停用顺序**：`plugin_on_disabled` 先 `set_enabled(false)`（阻止新业务副作用、作废 pending），随后 Host 按资源声明逆序释放——ownership 先 release（`own_release` → `table_deactivate`），再释放 UI hooks（`plugin.cpp:242-248`）。`plugin_deactivate` 再清理订阅、typed handlers、事件订阅、UI context/backups 与 facts/ports（`plugin.cpp:212-231`、`ui/runtime.cpp:650-656`）。

**资源等级**：5 条核心 hook = required/fail-closed（任一失败整体回滚已装核心资源与内部注册）；7 条装饰 hook = optional/fail-soft（失败只 degraded，不拖垮核心）。`UIMix_ResetStuffItemControl` 保持 optional，且其 wrapper 不受 feature 门控，因为它同时服务原版成功链（`ui/runtime.cpp:508-512,472-510`）。

**表所有权**：`own:RECIPEBASE_MIXTUREBASE` 为 required + fail-soft：`own_acquire` 调 `custom_recipe_table_ensure` 并据结果置 `g_ownership_ready`，失败时回滚表状态并返回非 0（`feature.cpp:1131-1141`）；`own_release` 为 total。ensure 注入模块缓冲、清除模块占用组位并追加记录；deactivate 还原原版段组位并把 RECIPEBASE 记录数还原为 base，但**保持游戏指针指向模块缓冲且不 free**，使残留面板持有的注入下标仍在缓冲界内（`table.h:101-111`、`table.cpp:363-395`）。表未就绪时 `def_for_mix_type` 返回 `nullptr`，不把未注入状态伪装成可用配方。

**typed commands 时序**：UI 经 `commands.post_typed` 入队（`ui/feature.cpp:205-222`），Host 主线程 drain 后由 `plugin.cpp` 的 handler 转交结果层执行（`plugin.cpp:289-295`、`feature.cpp:1195-1214`）。未改成 UI callback 直接同步写；handler 在校验通过前先检查 `enabled()`，停用后 drain 中的命令直接失败（`feature.cpp:1195,1193`）。

## 消费者 API 边界与不迁移面

- 内部结果 API `result::CraftResultApi` 的 12 个成员（`enabled`/`three_slot_mode_active`/`is_jewel_tier_up_recipe`/`form_ui_type_for_mix_type`/`craft_gate`/`is_jewel_item`/`place`/`prepare`/`execute`/`material_needs`/`make_item`/`desc_should_replace`）由 `craft_result_api()` 返回（`feature.h:34-35`、`result.h:92-105`、`feature.cpp:1114-1128,1191`），只有本 owner 的 UI 子层调用；`result.h` 另保留未被当前 API 使用的 `CraftOutcome` 枚举。本轮确认回归**未增删该表成员、未新增 provider、未改 Host SDK/ABI**；`PrepareOutcome` 只做尾追加（`kUnavailable = 3`），`prepare` 槽内可返回该新枚举。
- 两写端口：`QOL_PORT_CUSTOM_RECIPE_WRITE`（写需求/槽位/描述/产物创建保存释放/宝石字/随机）与 `QOL_PORT_CRAFT_UI_WRITE`（UIMix 控件操作、提示与 YesNo、费用归零）。见 `contracts/game_ports/custom_recipe_write_port.h:12-26`、`contracts/game_ports/craft_ui_write_port.h:14-28`。
- `core.craft` 继续提供只读 `craft.facts` v1.0；`POST /api/op/craft/mix-direct` 仍由 `core.craft` 标记 owner，并保持 Kotlin `OpController` 的 501 `not implemented`；本插件不实现该 route，不新增 JNI。见 `plugins/core.craft/`、`docs/reference/api-reference.md` §8。

## 已知限制

- 三格流程继续调用受 inventory hooks 管理的原版入口，因此保留 H-03 类别总数（物理+扩展）、H-05 对象删除、H-13 保存与 H-21 类别补扣的扩展背包支持；支持以对应 hook `live`、EB capability 可用且 enabled 为条件，不限制为物理袋，也不默认拒绝 EB。
- 宝石升阶的 Game-owner 写入限制与三格流程分开核验，仍为待核项，不作为三格拒绝 EB 的理由（见 `static-audit.md`）。
- CR06（放料空/无效输入返回 `kPlaced` 并推进选格）行为尚未裁决，保持原行为。
- CR11（install 部分失败清理在 feature-mode 与 legacy `install_all` 路径的差异）为条件性基础设施观察，当前无确认的运行时泄漏证据。

## 本轮确认回归（2026-10-15）

- 修复内容：YesNo 确认补齐非空 `ok`/`cancel` 回调（历史 bug：`cancel = nullptr` 被 adapter 拒绝 → 正确配方无反应）；取消只作废内部 pending/token；`prepare` 区分「真正未命中」与「已匹配但上下文不可用」（新增 `PrepareOutcome::kUnavailable`）；槽位/类别读取失败不再伪装成空槽或「材料不足」；弹窗创建失败明确提示而非静默；提示文案按错误分类走 scope 8–11。
- Host 证据：7 个受影响 target 全部**重新构建后**运行 `custom_recipe_tests`、`custom_recipe_handle_chain_tests`、`custom_recipe_owner_fixture_tests`、`host_adapter_tests`、`craft_ui_guard_tests`、`craft_ui_handle_identity_tests`、`module_text_tests`（717 断言）共 **7/7 PASS**，确保最终 `ui/feature.cpp`（畸形 handle 分支）进入二进制（`.tmp/custom-recipe-fix-20261015/final-ctest.log`）。其中 owner fixture 用真实 wrapper 验证确认链；handle_chain 的 `wrapper_equivalent` 用构造输入覆盖结果层成功路径，属交叉覆盖而非真机。
- 构建、打包与部署（按脚本直接执行，未手工构建/搬迁旧包）：`scripts/build-debug.sh --offline` 成功并正常清理旧包，最新模块 APK `output/inotia4_qol_lsposed_debug_2610151038_6ef35a38129e.apk`（SHA-256 `6ef35a38129e2c8b0971a6c7629c578bcd278a0098a331b2931eb081523baa77`）；`scripts/patch-apk.sh <该模块>` 默认 3 variants 成功 3 / 失败 0，canonical 交付入口为 `output/Inotia4_v1.3.2_monster_v31_lspatched.apk`（SHA-256 `00dcbd36b16b27efe517dc99a374b6ccd1e797669d3683b04ca3b2921def84c2`）。`check_arch` 通过、`check_http_routes` 147/147。
- 部署与运行身份（`192.168.3.56:5555`）：`install -r` canonical → `Success`，force-stop + monkey 重启后 `/api/health` `ready=true`/`monster v31`、`screen=main_menu`；运行身份 `versionCode=192`/`versionName=0.7.13`/`sha256=6ef35a38129e…` 与模块**严格一致**，assembly `initialized=true registered=28 install_failed=0`（`running-module.log:24,33`）。
- 状态：部署身份已建立，**但本轮未执行合成、未做视觉手测、未自动进档**；部署身份不等于业务验收，**VM-CR-MERGE-01 仍 NOT_ACCEPTED**。回归登记见 [`verification-log.md`](../../refactor/audits/plugin-audit/custom-recipe-20261014/verification-log.md) 与 `docs/refactor/verification.md` §5.13。

## 文件地图

路径相对 `module/app/src/main/cpp/`。

| 子职责 | 文件 | 当前职责 |
|---|---|---|
| Descriptor、依赖、开关与生命周期 | `plugins/custom_recipe/plugin.cpp:33-331` | required/optional、两条 port、13 条资源组装、安装事务与清理、门禁、text 消费者、唯一 feature。 |
| UI 子层接口与实现 | `plugins/custom_recipe/ui/feature.h:26-156`、`ui/feature.cpp:11-351` | UIMix 控件/事实读写封装、local TLS text scope、放料处置映射、成功收尾与提示。 |
| UI runtime | `plugins/custom_recipe/ui/runtime.h:12-25`、`ui/runtime.cpp:34-658` | 12 条 hook wrapper、claims、backups、bind/clear、CR01/CR02/CR03 提示与真实随机适配。 |
| 结果层 | `plugins/custom_recipe/feature.cpp:64-1216`、`feature.h:24-60` | place/prepare/execute/make-item、quality 消费、动态配方事件、材料需求、table ownership ops。 |
| 内部结果契约 | `plugins/custom_recipe/result.h:10-105` | 结果枚举与 Input/Output POD 及 `CraftResultApi`。 |
| 纯规则 | `plugins/custom_recipe/rules.h:17-101`、`rules.cpp:42-224` | 档位/区间/候选、材料数量、三格库存与 token、宝石缩放。 |
| 静态/动态目录 | `plugins/custom_recipe/catalog.h:14-200`、`catalog.cpp:43-345` | 两条注入 entry、8 条静态规则、26 类动态构造与匹配。 |
| 表注入 | `plugins/custom_recipe/table.h:20-111`、`table.cpp:24-398` | 注入构造、组位快照/恢复、ensure/deactivate、模块缓冲生命周期。 |
| 配方呈现文案 | `plugins/custom_recipe/text_table.h:9-90` | 本地 scope 与 id 的替换映射。 |
| 设置兼容源 | `plugins/custom_recipe/settings_contribution.cpp:3-6` | 兼容入口不注册设置项；feature metadata 唯一来自 descriptor。 |
| Build composition | `plugins/custom_recipe/CMakeLists.txt:1-10` | catalog/rules/table/feature/plugin/settings contribution 与 `ui/feature.cpp`、`ui/runtime.cpp`。 |
