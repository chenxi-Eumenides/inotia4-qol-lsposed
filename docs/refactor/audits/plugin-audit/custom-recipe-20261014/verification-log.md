# `custom_recipe` 审阅登记（2026-10-14）

> 基线 HEAD `ac0dfc73`。本登记按 `docs/refactor/plugin-audit-playbook.md` 分为问题登记、无法验证项、已完成登记；finding 的代码证据见同目录 [`static-audit.md`](static-audit.md)。合并实施方案与未验证边界见 [`design-card.md`](design-card.md)。参考说明见 `docs/reference/plugins/custom_recipe.md`。

## §1 本批登记问题

本批不使用 AQ 编号；静态 findings 使用 CR01–CR11，含义与编号见 [`static-audit.md`](static-audit.md)，本轮实施对应位置亦在该文件的「本轮实施对应」列。CR09 为策略观察（ResetStuff optional 且不受 gate）、CR11 为条件性生命周期观察；二者不写成已确认的运行时事故。注册顺序不另立 finding：当前 Host registry 在安装前构建完整 capability DAG 并按拓扑顺序安装。

CR01 已裁决为分阶段、尽量保持原子且允许扣料后不回退的目标，并已在 `feature.cpp:687-997` 落地；CR02/CR03/CR04/CR10 的实现位置见 `design-card.md` §1/§3.1/§3.2。以上均为**实现事实**，不等于验收通过。

独立源码复核另提出 **F1–F3** 三项修复，按新编号登记（见 `static-audit.md` 的「独立复核修复项 F1–F3」），不覆盖、不重编号 CR01–CR11。

## §2 无法验证 / 待验证项

| 项 | 当前缺证 | 后续条件 |
|---|---|---|
| `VM-CR-MERGE-01` 合并目标行为 | 部署身份已建立但功能未验：集成包已安装且运行身份与构建一致，但停在协议页未由用户确认，未 `enter_slot`、未发放材料、无剪贴板/界面行为观测；因此无功能态证据。 | 用户手动确认协议并进入世界态后，执行 design-card §6 的 owner/resource 核对与 UI、两态、表/资源生命周期、失败路径、API 501 冻结面。 |
| CR01 分阶段失败处理 | 已实现阶段划分与阶段化提示（扣料后 remove/observation/save → 「合成失败，材料可能已扣，请检查背包。」+ 清格；扣料前产物准备失败 → 「产物准备失败，材料未扣。」+ 清格），Host 阶段断言已随 L4 落地（§3），但无游戏内观测证明各失败点的真实材料/产物状态。 | 游戏内逐阶段复核实际材料/产物状态；不得宣称强原子或所有失败均不扣料。 |
| CR02 quality 不可用提示 | 提示区分已在 UI 落地并有真实的阶段文案（F2），但无游戏内观测确认「功能不可用」与「只有宝石」在真实面板上分别出现。 | 质量 provider 可用/不可用两态下各触发一次并记录 popup 文本。 |
| CR03 真实闭区间随机 | port、适配与规则桥已接线，Host adapter RNG 边界/错误断言已落地（§3）；仍无设备抽样证明落点覆盖闭区间两端。 | 必要时设备抽样记录取值分布。 |
| CR06 放料空/无效输入结果 | 当前 `kPlaced` 会推进选格；该分支的产品期望尚未核准。 | 明确期望后再定规则和回归断言；在此之前保持原行为。 |
| CR08 pending 与动态表刷新 | 已加 generation 防线，但 SaveEnter 与未决确认的交错路径未证明可达或不可达；未做设备复现。 | 在 Host/生命周期 fixture 中控制 event/confirm 顺序，必要时做设备压力复核。 |
| CR10 材料需求写失败 | 顺序已实现（need 先、jewel 后、need 失败不写 jewel），Host 已覆盖顺序断言；未在游戏内核对该顺序与展示残留。 | 游戏内核对；实际展示/扣料结果仍需设备检查。 |
| CR11 install 失败清理时序 | 已做 install 内同步幂等清理与 F3 安装中止；oracle 静态复核无新增阻断，但 feature-mode terminal deactivate 与 legacy `install_all` 的路径差异未在设备上验证。 | 对生产 feature-mode 与 legacy install-all 分别做注册失败注入和 drain 时序验证，不将条件路径写成确认泄漏。 |
| AQ 延期联测 | `attribute_quality` 的真机行为按用户要求等待 `attribute_range_display` 审阅联测；本批不独立外推。 | display 审阅完成且设备/模块身份信息齐备后再补 VM。 |
| tier-up Game-owner 写入限制 | 与三格流程分开核验，尚未出结论。 | 单独核验后再决定是否需要相应限制；不得据此扩大为三格拒绝 EB。 |

## §3 本轮已完成登记

本轮登记合并实施的代码/机械/语法/定向 Host/集成构建/静态校验证据，以及设备**部署前状态**；不登记验收 PASS。

| 登记项 | 结果 | 证据/范围 |
|---|---|---|
| L0 契约/端口/适配（会话 `ses_ec9f65814ffeg1myu0kmaYXD2F`） | 头文件语法 PASS（`result.h`、`feature.h`、write port、`ports.h`，含 ABI `static_assert`）；`game/adapter_ports/craft_port.cpp` syntax-only PASS；Host CMake 配置 PASS。 | `.tmp/custom-recipe-implementation/l0/validation.txt`、`l0/host-adapter-link.log`、`l0/relative-to-snapshot.patch`。 |
| L1 结果层机械检查（会话 `ses_ec814caa3ffexHKBSPkq1UfAw4`） | `rules`/`catalog` 机械断言全部通过：随机源失败与越界原样传播不 clamp、动态配方失败不发布半成品表、成功路径递增动态代际。 | `.tmp/custom-recipe-implementation/l1/mech_check`（自建 harness，非仓库 target）、`l1/l1-custom-recipe.patch`。 |
| L2 UI 子层迁移与 descriptor（会话 `ses_ec8147572ffea0sxRhcp16mode`） | `plugins/custom_recipe/plugin.cpp`、`ui/feature.cpp`、`ui/runtime.cpp` 语法检查通过（仅既有 `requires` C++20 关键字告警）；`custom_recipe/{feature,table,catalog,rules}.cpp` syntax OK。 | `.tmp/custom-recipe-implementation/l2/lane.patch`、`l2/snapshot/`。 |
| L3 Host 资源/装配（会话 `ses_ec9f65814ffeg1myu0kmaYXD2F`） | 手工构建运行：`hooks_transaction_tests` pass=439、`plugin_registry_tests` pass=635、`plugin_lifecycle_tests` pass=162、`capability_lifecycle_tests` pass=191、`feature_reconciler_tests` 全节通过；`host/assembly.cpp` `-fsyntax-only` rc=0（craft_ui include/register 移除、custom_recipe 保留）。 | `.tmp/custom-recipe-implementation/l3/validation.txt`、`l3/manual-build/`、`l3/assembly-syntax.log`。 |
| F1–F3 独立复核修复 | F1 `read_stuff_item` 严格身份/就绪语义（正常空格 OK + 无效 handle，缺控制才 NotReady）；F3 安全撤回 + `tx_abort_install`（required-ready / soft-ready 撤回失败先 abort 在途 install，阻止 trailing ownership acquire），不重复 unhook、release 失败仍隔离。手工构建：`hooks_transaction_tests` pass=484、`plugin_registry_tests` pass=659、`plugin_lifecycle_tests` pass=162、`capability_lifecycle_tests` pass=191、`feature_reconciler_tests` 全节、`host_adapter_tests` PASS。 | `.tmp/custom-recipe-implementation/review-fixes/validation.txt`、`review-fixes/manual-build/`、`review-fixes/relative-to-snapshot.patch`。 |
| F2 阶段提示语义修复（限定源码 lane） | `ui/runtime.cpp` 与 `text_table.h`：扣料后阶段统一提示「合成失败，材料可能已扣，请检查背包。」并清格；产物准备失败独立提示「产物准备失败，材料未扣。」并清格，去除「无产物入包/空间不足」断言，stage 日志保留。 | 源码 diff 与文档同步；相应测试由 L4 落在受影响目标中（见下）。 |
| L4 集成构建与受影响 CTest | 仓库 CMake 增量构建 PASS（`final-host-build.log`：`ninja: no work to do.`）；受影响 CTest **17/17 PASS**（`final-affected-ctest.log`：item_facts/craft_facts/attribute_quality/custom_recipe/custom_recipe_handle_chain/module_text/module_health/plugin_registry/hooks_transaction/plugin_lifecycle/feature_reconciler/capability_lifecycle/craft_ui_guard/craft_ui_handle_identity/host_adapter/attribute_quality_provider/custom_recipe_owner_fixture）。 | `.tmp/custom-recipe-implementation/final-host-build.log`、`final-affected-ctest.log`。 |
| L4 merged-owner fixture 覆盖 | `custom_recipe_owner_fixture_tests` 覆盖 5 core / 7 optional hooks、ownership、gate、无 public capability、两条 port、feature off/on；断言见测试源码。 | `final-affected-ctest.log`、`tests/CMakeLists.txt`、`tests/` 内相应测试源。 |
| 全量 Host CTest | **141/142**：唯一失败 `core_api_lifecycle_tests`（旧 `kHidden` 断言）。该测试源与 `core.api` 实现与 HEAD 一致，属无关既有失败，本批不修改无关源码；不作为「142 全绿」。 | `.tmp/custom-recipe-implementation/final-full-ctest.log`、`unrelated-test-baseline.txt`。 |
| 静态/路由校验 | `check_arch` 全部通过；`check_http_routes` 147 routes、重复 0、controller/native/api-reference 无漂移；`git diff --check` 通过。 | `.tmp/custom-recipe-implementation/final-check-arch.log`、`final-check-http-routes.log`、`final-diff-check.log`。 |
| Debug 构建 | `scripts/build-debug.sh --offline` BUILD SUCCESSFUL；产物 `output/inotia4_qol_lsposed_debug_2610142037_ff39e1c3502f.apk`，SHA-256 `ff39e1c3502f5e4402cfd26ede3e21767fcfb695cdc8c355367ade667802dd68`。历史 Debug APK 已保留并做哈希核对。 | `.tmp/custom-recipe-implementation/final-build-debug.log`、`final-apk.json`、`final-apk-preservation.json`。 |
| oracle 最终静态复核（`ses_ec6e834faffeHZgxHmdF07dgtj`） | 只读确认 F1–F3、typed handler gate 与 ready-failure ownership 风险静态可关闭，无新增阻断。 | 静态复核结论；**不是** VM 验收，不代替设备行为证据。 |
| 设备部署与运行身份（deployment identity，非功能验收） | 集成包安装 `Success`（APK SHA-256 `52a5b61363a92f10d706fb161838e43339d8717d2b7aa697c8752e53898fb828`，内嵌 module 与 Debug 构建一致）；启动后运行身份核对一致（`versionCode=192`、`versionName=0.7.13`、`sha256=ff39e1c3502f…`）；`/api/health` `ready=true`/`monster v31`；config projection 仅 `custom_recipe`，无 `craft_ui`；package 不变，未卸载/未清 data。 | `device-api/install.log`、`launch.log`、`running-module.log:33`、`after-health.json`、`after-config-list.json`。仍**不是**功能验收。 |
| 设备阻断与未执行项 | 启动停在 Android 协议页 `AgreementUIActivity`（`id=ok`/`同意`），须用户手动确认；未点击、未 `enter_slot`、未发材料、未执行配方。`startup-system-info.json` 的 `save_slots=false` 为启动未完成观察，不能断言存档删除。观察：`running-module.log:17` optional `attribute.quality` 边降级防循环；`:59` `recipe table not ready … count=0` 为协议页启动态，无 loaded-world 表资源证据。 | `device-api/startup-ui.json`、`startup-ui-dialog.json`、`startup-system-info.json`、`running-module.log:17,59`。安装前 slot 0 备份 `c97d845ba2b1` 已导出并保留（`pre-deploy-backup.json`）。 |
| 合并验收 `VM-CR-MERGE-01` | **NOT_ACCEPTED（部署身份已建立；功能行为未验）**：`deployment identity established ≠ functional VM`；无截图/现象输出。 | `docs/refactor/verification.md` §5.11 与 design-card §6。 |
| 历史 PASS 复用 | **禁止**：VM-46 等历史卡为合并前分离双插件形态的证据，不作为本轮合并证据；历史 PASS 行不改写。 | 本登记不引用其作为合并验收。 |
| 整体状态 | **NOT_ACCEPTED（合并目标已实现；Host/静态/Debug 证据成立；部署身份已建立，设备功能行为未验）**。 | 待用户确认协议页并进入世界态后的适用 VM 验证。 |
