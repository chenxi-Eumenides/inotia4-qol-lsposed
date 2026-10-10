# core.settings plugin-object 配置迁移验收登记（2026-10-10）

> 本记录区分 Host/静态/Debug Gate 与整体设备行为验收。配置合同见 `ARCHITECTURE.md` §2.2/§7.1/§7.2/§7.3/§8.2、`docs/reference/api-reference.md` §7.6 和 `docs/reference/plugins/core.settings.md`。旧平铺配置只作为启动迁移输入，不代表当前 HTTP schema。

## 1. 本批问题登记与链接

| ID | 问题/收口结论 | 台账/规格 |
|---|---|---|
| SET-01 | 旧平铺字段、Kotlin 配置镜像与运行 gate 曾形成多份状态源；已收口为 native Loader-lifetime Store/config.json、registered plugin namespace 投影与 owner-token single-key 更新。 | `docs/refactor/issues.md` 附录 SET-01；当前契约 `docs/reference/api-reference.md` §7.6 |
| SET-02 | 配置开关与插件 lifecycle 的双通道语义未清楚区分；已明确 `enable` 默认不控制通用 lifecycle，只有七个旧映射维持既有 gates，`custom_recipe.enable` 映射到 `craft_ui` gate。 | `docs/refactor/issues.md` 附录 SET-02；`ARCHITECTURE.md` §8.2 |
| Owner/API/schema | `/api/config/list` 与 `/api/config/set` 均归 `core.settings`；Kotlin 仅 raw-body transport；registered active-set 控制读取可见集合，unknown root/未注册 namespace 保留但不投影。 | `docs/refactor/api-route-map.md`、`docs/reference/api-reference.md` §7.6 |

## 2. 无法验证项

| 项 | 状态/原因 |
|---|---|
| 整体设备验收 | **未执行**。本轮 Debug APK 未安装到设备；不得将 Host Gate 或 APK 构建成功表述为真机行为通过。 |
| 全量 CTest | **未执行**。仅执行定向非 EB Host CTest Gate。 |
| EB/扩展背包专项 | **未执行**，按本轮限制不扩展到 EB 范围。 |
| 新 config HTTP 与 runtime effects 的真机行为 | 未取得本轮设备日志；GET projection、raw-body 任意 JSON 写入/错误透传、`apiEnabled=false` 后 UI 恢复、revision 驱动的运行态效果不可称为真机验收通过。 |
| Store degraded/ReadyUnpersisted 实际运行场景 | 未在设备执行 I/O 故障注入。hard-I/O degraded 采用 Host Store 定义的默认读写语义，由状态/错误字段诊断；插件不得回退到 infra/Kotlin mirror 或旁路写文件。`ReadyUnpersisted` 表示进程内可用但 durable persistence 尚未确认，须区别于 hard failure，也不能报告为已持久提交。 |

## 3. 已完成验收登记

| 验收项 | 结果 | 证据/输出 |
|---|---|---|
| Phase1A/1B Store 与 owner API gates | **PASS（Host Gate）** | Gate 输出：PASS。此结果仅覆盖 Store/API gate。 |
| Phase2A batch seed gate | **PASS（Host Gate）** | Gate 输出：PASS。覆盖默认 seed/迁移 Gate，不扩展为设备验收。 |
| Phase2B 最终 Oracle Gate | **PASS（代码 Gate，attempt 3）** | Gate 输出：PASS；此前两个 findings 已修复。审阅流程不作为实现归属依据。 |
| 定向非 EB Host CTest | **PASS** | 定向批次 17 个测试通过；`config_platform_service_tests` 独立重复运行 5/5 通过。全量 CTest 未运行。 |
| `check_arch.py` | **PASS** | 输出：零违规。 |
| `check_http_routes.py` | **PASS** | 输出：146/146。 |
| JNI external/native 对照 | **PASS** | 输出：152/152。 |
| tracked/untracked scoped `git diff --check` | **PASS** | 输出：无 whitespace/error 诊断。 |
| `scripts/build-debug.sh` | **PASS（Debug 构建）** | 构建成功；APK SHA-256：`013ff709eef42f7275bbeb6f9eea8c7c8a4ff9700f957a645ab765cea23ae198`。该 APK 未安装。 |

**总体结论**：Host/静态/Debug 构建 Gate 均通过；整体设备行为验收为 **NOT_ACCEPTED（未执行）**。未运行全量 CTest、EB 测试或设备验收。不得把 Gate PASS 延伸为完整真机 PASS。

## 4. 模块设置 feature / lifecycle 实现批（Stage1–3，2026-10-10）

Host 独占 `enable`/`features`/`config` 三层 schema、注册元数据不持久化、`want_run = enable && any(feature=true)`、依赖级联不改 desired 的已批准模型，落地于 Stage1（contracts/Host schema-Store/API）、Stage2（registry desired reconciler、非阻塞 owner drain、依赖恢复）、Stage3（生产接线 + 产品插件 descriptor 切换 + settings 降级为普通 UI 消费者）。事实来源 `.slim/deepwork/module-settings-features.md`。

### 已执行（Host/静态/Debug 证据）

| 验收项 | 结果 | 证据/输出 |
|---|---|---|
| Stage1 Host 定向 CTest | **PASS**：6/6（`host_feature_controller_tests`、`config_namespace_api_tests`、`core_settings_config_store_tests`、`core_settings_config_service_tests`、`core_settings_config_service_writeback_tests`、`core_settings_config_service_effect_failures_tests`） | `.tmp/module-settings-features/targeted-tests.log`、`gate1-remediation-tests.log` |
| Stage2 lifecycle 定向 CTest | **PASS**：14/14（含 `feature_reconciler_tests`、`plugin_registry_tests`、`plugin_lifecycle_tests`、`capability_lifecycle_tests`、`infra_store_tests`、sidecar/save_route 锁竞争） | `.tmp/module-settings-features/stage2-remediation-tests.log` |
| Stage3 Host 生产接线定向 CTest | **PASS**：4/4（`host_feature_controller_tests`、`production_feature_facade_tests`、`settings_catalog_abi_tests`、`hooks_popup_state_tests`） | `.tmp/module-settings-features/stage3/tests.log`、`ui-bindings-removal/tests.log` |
| 产品插件 descriptor/旧 gate 清理定向 CTest | **PASS**：非 EB 17/17 产品 focused targets | fix-3 交付（见 `.slim/deepwork/module-settings-features.md`） |
| Stage1/2 Debug 构建 | **PASS（Debug 构建）**：SHA `af1f045f…`（Stage1）、`6dab3d19…`（Stage2）、`7c0185e9…`（Stage2 修复后） | `.slim/deepwork/module-settings-features.md` |
| `check_arch.py` / `check_http_routes.py` / JNI 对照 | **PASS**：arch 零违规、routes 146/146、JNI 152/152 | `.slim/deepwork/module-settings-features.md` |
| scoped `git diff --check` | **PASS**：无 whitespace/error 诊断 | `.tmp/module-settings-features/ui-bindings-removal/diff-check.log` |

### 未执行（NOT_ACCEPTED）

| 项 | 状态/原因 |
|---|---|
| 设备/EB 真机行为验收 | **NOT_ACCEPTED（未执行）**：Debug APK 未安装到设备；不得以 Host Gate 或构建成功替代真机行为证据。 |
| 全量 CTest | **未执行**：仅执行上述定向非 EB Host CTest。 |
| `/api/config/list|set|op/features/lifecycle|config/receipt` 真机行为 | **NOT_ACCEPTED**：本轮无设备日志；receipt 异步语义、Debug hidden 可改/Release 隐藏、settings 撤页重建均未真机取证。 |
| EB/扩展背包专项 | **未执行**：本轮限制不扩展到 EB 范围。 |

**总体结论**：Stage1–3 的 Host/静态/Debug Gate 通过；整体设备行为验收 **NOT_ACCEPTED（未执行）**。不把静态编译当运行验收。
