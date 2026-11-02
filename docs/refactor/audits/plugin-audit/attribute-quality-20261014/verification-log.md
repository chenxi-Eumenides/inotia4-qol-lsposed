# `attribute_quality` 插件审计登记（2026-10-14）

> 范围：`attribute_quality` v0.1.0 / `attribute.quality` v2.0、ItemFacts 与 Game Host 读边、三个消费者。设计及 AQ01–AQ08 范围见同目录 [`design-card.md`](design-card.md)。本登记区分 Host/构建证据与 VM 证据，不宣告 ACCEPTED/CLOSED。

## 1. 原始 Oracle Findings AQ01–AQ08 整改映射

以下保留原 Oracle finding 编号及含义；这里登记代码落点和 Host 证据，不把 AQ 条目标为 ACCEPTED/CLOSED，也不把 Host 结果当作 VM 结论。

| ID | 原始 finding 与整改映射 | Host 证据 | 当前状态 |
|---|---|---|---|
| AQ01 | signed overflow：range width、乘法和结果加法须用宽算术并在窄化前检查溢出；`rules.cpp` 统一用 int64 中间值并拒绝不可表示结果。 | `attribute_quality_tests`：114 assertions（极值、百分位、fixed range overflow）；最终集成 CTest。 | 静态整改已实现；AQ 未销项，VM 待验。 |
| AQ02 | fixed option category/uses/pair 查询错误不能被忽略或 fallback；non-applicable 保留捕获范围，NoMatch/NotReady 逐项失败；R02 增加 GOT 中间引用空值守卫。 | provider fixed-table cases、`attribute_quality_adapter_tests`；R02 focused 3/3 与 final integration。 | 静态整改已实现；AQ 未销项，VM 待验。 |
| AQ03 | Host `QolResult` 不得折成 bool 丢失 Blocked/stale 等错误；range、facts、fixed-table 错误原样保留，batch 错误写到 item status。 | provider opaque-error/wrong-thread/stale-handle cases；最终集成 CTest。 | 静态整改已实现；AQ 未销项，VM 待验。 |
| AQ04 | 嵌套 capture 覆盖 TLS 状态；以独立 active/armed 状态与 RAII scope 拒绝嵌套、退出清理状态。 | provider nested-before/after-capture cases 返回 Busy；最终集成 CTest。 | 静态整改已实现；AQ 未销项，VM 待验。 |
| AQ05 | recipe 候选 evaluate 错误不能降级为 Grey 后继续；改为 QolResult 传播，失败时不调用 candidate random、不写 item、不更新材料表。 | `custom_recipe_tests`、`custom_recipe_handle_chain_tests` 2/2；最终集成 CTest。 | 静态整改已实现；AQ 未销项，VM 待验。 |
| AQ06 | rules/fake lifecycle/disabled 用例不足以覆盖真实 provider path；补真实 provider、ItemFacts/Item Port adapter、consumer 与 recipe 受影响目标。 | final integration CTest 14/14；含 `attribute_quality_provider_tests`、`attribute_quality_adapter_tests`、ItemFacts、Host adapter、recipe 与两个消费者。 | 静态整改已实现；AQ 未销项，VM 待验。 |
| AQ07 | 空 backup 曾可被 ready 接受、wrapper 无 backup 曾透传 `min`；ready 现拒绝空 backup，query 要求 active/live backup。wrapper 当前 `INT_MIN` sentinel 不是游戏可识别错误，不宣称游戏侧安全。 | provider backup/readiness cases；`l1-r01` focused 2/2、provider 135 checks；最终集成 CTest。 | 静态整改已实现；sentinel 仍仅有 Host 语义证据，AQ 未销项。 |
| AQ08 | 公共 contract 注释错误引用 ARCH §7.4；现行 public contract 注释已改为只读 provider职责与消费者呈现边界。 | `contracts/attribute_quality.h` 当前注释、provider/consumer 编译及最终集成 CTest。 | 注释已修；非运行时行为结论，AQ 未销项。 |

## 2. Oracle 跟进项与实现边界

| 编号 | 发现/处理 | 当前证据与限制 |
|---|---|---|
| R01 | stored-option 批量 query 曾用 `flag=0`，而 option getter 对非正 flag 不调用 `MATH_GetRandom`；现已改为 `flag=1`。Provider fixture 明确模拟 `flag<=0` 成功返回但不触发 capture；`feature.h` 对空 backup 的 callback 注释已与拒绝语义同步。 | `l1-r01/relative-to-snapshot.patch`；focused rules/provider CTest 2/2，provider `pass=135 fail=0`（`l1-r01/ctest.log`、`attribute_quality_provider.log`）。 |
| R02 | ITEMCLASSBASE/ITEMSTATICOPTBASE GOT data slot 的中间引用为空时，现先判空并返回 NotReady，不再二次解引用 null；输出参数保持原值。 | `game/access/attr_range_ui.cpp:31-40,62-73`；adapter test 覆盖两个 null intermediate GOT case，R02 build 中相关 3 targets 均通过（`.tmp/attribute-quality-implementation/r02/build/Testing/Temporary/LastTest.log`）。 |
| Wrapper sentinel | `math_get_random_wrapper` 在 backup 为空时返回 `INT_MIN`，它不是游戏定义的错误响应，不能宣称实际游戏调用可安全识别该 sentinel。正常防线是 resource-ready 对空 backup 返回失败、资源 required/fail-closed 与 query active/backup readiness 门禁。 | `plugins/attribute_quality/feature.cpp:225-230,255-264,389-398`；真实游戏如何处理意外触发的 sentinel 没有 VM 证据。 |

### 超出 AQ 范围的独立观察

| 编号 | 观察 | 处置范围 |
|---|---|---|
| G01 | Host capability lookup 与 revoke 期间 API pointer/lease 并发安全 | 不属于本批修复；provider 文档只记录同步调用约束，不宣称后台 require/lease 并发安全。 |
| G02 | hook 事务安装/撤销计数及资源机制通用行为 | 不属于本批 provider 目标；由 hook/Host 机制测试与所有者继续负责。 |
| G03 | plugin registration API 结果/注册状态转换 | 不属于本批 provider 目标；由 registry/lifecycle 所有者负责。 |
| G04 | ability-level 符号缺失读面 | 作为必要输入语义保留：`attr_ability_level` 返回 `-1`，Item Extras port 转为 NotReady；L4 adapter test 覆盖，非待修缺陷。 |

## 3. Host、静态与构建证据

| 范围 | 记录结果 | 证据与口径 |
|---|---|---|
| L1 quality rules/provider | rules 114 assertions；R01 后 provider 135 checks；focused targets 2/2 | `.tmp/attribute-quality-implementation/l1-r01/attribute_quality_rules.log`、`attribute_quality_provider.log`、`ctest.log`。 |
| L2 `custom_recipe` | `custom_recipe_tests`、`custom_recipe_handle_chain_tests` 2/2 | `.tmp/attribute-quality-implementation/l2/host-build/Testing/Temporary/LastTest.log`。 |
| L3 consumers | `autosell_view_diagnostic_tests`、`attribute_range_display_tests` 2/2 | `.tmp/attribute-quality-implementation/l3/build/Testing/Temporary/LastTest.log`。 |
| L4 ItemFacts/adapter 基线 | `item_facts_plugin_tests`、`host_adapter_tests`、`attribute_quality_adapter_tests` 3/3；当时 adapter 输出 18 assertions | `.tmp/attribute-quality-implementation/l4/build/Testing/Temporary/LastTest.log`；其后 R02 增加 GOT null guard cases。 |
| R02 focused adapters/consumers | `autosell_view_diagnostic_tests`、`attribute_range_display_tests`、`attribute_quality_adapter_tests` 3/3；更新后 adapter 输出 24 assertions | `.tmp/attribute-quality-implementation/r02/build/Testing/Temporary/LastTest.log`；两个 intermediate GOT null cases 保留 `not ready` 与输出不变。 |
| 最终集成 CTest | **14/14 通过**，包含 quality rules/provider/adapter、ItemFacts、Host adapter、recipe、autosell/display、registry/lifecycle/capability/reconciler/module health | `.tmp/attribute-quality-implementation/final-ctest.log`；该记录位于 R01/R02 修复后。 |
| 最终 Debug 构建 | **BUILD SUCCESSFUL**；APK `output/inotia4_qol_lsposed_debug_2610141000_10092b3392bd.apk`，SHA-256 `10092b3392bdbc6319144f0095698a32990e72da7b386d1bc9820bf93be4ad4d` | `.tmp/attribute-quality-implementation/final-build-debug.log`；APK 构建身份已核对，不代表设备安装或模块运行身份。 |
| 静态检查 | `check_arch` 无违规；route 检查 147 条、重复与差异为 0；scoped diff check exit 0 | `.tmp/attribute-quality-implementation/check-arch.log`、`check-http-routes.log`、`diff-check.log`。这些检查只证明相应静态面，不等同于 VM 行为验收。 |

## 4. VM 与整体结论

**当前结论：静态整改已实现，Host/Debug 证据成立，VM-AQ-01 `NOT_ACCEPTED`。** Oracle 已完成本范围最终复核，确认 R01/R02 修复且无新增阻断；contract/feature comment/success-code 观察为非阻断。没有设备 serial、模块 identity SHA 与运行时嵌入 APK SHA 的现场核对记录，也没有真机安装、游戏画面或行为日志。最终 14/14 与 Debug build 不等于真机验收。

`docs/refactor/verification.md` 中既有 VM-K2-02 的历史 PASS 行保留原样；其历史 item.facts/provider 验证不替代当前 `attribute.quality` v2.0 的 VM-AQ-01。VM-AQ-01 的运行条件、目标观测与失败判据见 [`design-card.md`](design-card.md)。
