# device_probe_export_failure 插件审阅与删除验收登记（2026-10-16）

> 流程依据 `docs/refactor/plugin-audit-playbook.md`。本插件为 Debug-only 探针，**不进 Release**（`QOL_ENABLE_DEVICE_PROBE` 在 Release 直接 `FATAL_ERROR`，`module/app/src/main/cpp/CMakeLists.txt:7-9`、`module/app/build.gradle.kts:35-36`）。本批性质是**删除**，不改变任何 Release 或正常 Debug 构建的行为。

## 1. 本批登记问题与链接

| ID | 问题/收口结论 | 台账/规格 |
|---|---|---|
| PRB-01 | `device_probe_export_failure` 探针插件零消费者、无独占断言覆盖面，作为预检回归哨兵的存在依据不成立；连同两个空目录 `device_probe_provider` / `device_probe_consumer` 一并删除，探针能力保留在 host 侧 `probe_entry_export_resolution()`。 | `docs/refactor/issues.md` 附录 PRB-01 |
| TL-06 | 工作树既有欠账导致三项 host 测试失败（`core_api_lifecycle_tests`、`debug_two_column_tests`、`core_settings_config_store_tests`），本批结构上不可能引入，见 §3。 | `docs/refactor/issues.md` §8 TL-06 |
| TL-07 | `docs/refactor/baseline.md` 与 `docs/refactor/map.md` 的 `--check` 已不一致（由无关改动造成的统计漂移），本批不重生成以免混入无关内容。 | `docs/refactor/issues.md` §8 TL-07 |

## 2. 无法验证项

| 项 | 状态/原因 |
|---|---|
| 真机 probe 构建行为 | **NOT_ACCEPTED（未执行）**。本批未执行 `./scripts/build-debug.sh --offline -PqolDeviceProbe=true`，未安装集成包，未取 logcat。删除的是「必然失败」的负例探针，无玩家可见行为，但 `probe_entry_export_resolution()` 的真机 `GRPX_Start=hit` / 缺失符号 `missing` 输出**本批未复验**，其历史证据仍为 VM-EXPORT-01。 |
| 缺失导出 `unsupported` 状态的真机产出 | **NOT_ACCEPTED（未执行）**。该字符串过去由本插件在真机上产出；删除后真机不再产出，负例断言改由 host CTest 与 host 侧探针承担。真机不再有该证据，不得声称真机负例已验收。 |
| 全量 host CTest 全绿 | **未达成**。143 项中 140 通过，3 项失败为工作树既有欠账（TL-06），非本批引入；测试构建不编译 `host/assembly.cpp`（`tests/CMakeLists.txt` 仅引用 `host/owner_identity.cpp`、`host/save_route.cpp`、`host/http_dispatch.cpp`；`build.ninja` 中 `assembly` 出现 0 次），探针插件目录亦从未进入测试构建。 |
| Release 构建 | **不适用**。本插件不进 Release，删除不影响 Release 产物；本批未构建 Release。 |

## 3. 已完成验收登记

| 验收项 | 结果 | 证据/输出 |
|---|---|---|
| 插件目录删除 | **PASS** | `plugins/device_probe_export_failure/`（3 文件）、`plugins/device_probe_provider/`、`plugins/device_probe_consumer/`（均空目录）已删除；`find` 确认源码树无残留 `device_probe*` 目录。 |
| 宿主接线清理 | **PASS** | `host/assembly.cpp` 删除三处：include 块、`{&features::device_probe_export_failure::plugin_descriptor},` 注册项、`infra::status_get("device_probe_export_failure", ...)` 的 WARN 日志块；连带删除已无引用的 `#include "infra/status.h"`。 |
| 探针能力保留 | **PASS** | `probe_entry_export_resolution()`（正例 `elf-export:GRPX_Start`、负例 `elf-export:QOL_Preflight_Missing_Export`）与其 `assembly_init` 调用点、`QOL_ENABLE_DEVICE_PROBE` option/Release 禁、`core.custom_ui` 的 probe 条件分支均**未改动**。 |
| 死代码清理 | **PASS** | `plugins/CMakeLists.txt` 的 `^device_probe_` 跳过规则在探针插件归零后条件恒假、`continue()` 不可达，已删除并在该处留下恢复条件注释；`core.custom_ui/CMakeLists.txt` 指向 `feature.cpp` 的过时注释更正为 `plugin.cpp`（分支实际在 `plugin.cpp:87`，`feature.cpp` 无 probe 分支）。 |
| 源码零残留引用 | **PASS** | 全仓 grep（排除 `module/app/build/` 与 `.tmp/` 等 git-ignored 产物）无任何源码命中。 |
| 引擎契约覆盖（替代依据） | **PASS（Host Gate）** | `tests/test_plugin_registry.cpp:1514-1530` `test_required_entries_preflight_before_plugin_install` 断言 `install_calls==0`、`hook_calls==0`、两资源 `kFree`、状态 `kUnsupported`；`:1532-1559` 另覆盖 preflight 失败对独立插件与消费者的隔离性。 |
| `check_arch.py` | **PASS** | 输出：`check_layers` / `check_offsets` / `check_jni` / `check_resources` 全部通过，零违规。 |
| `check_http_routes.py` | **PASS** | 输出：`plugin=140 resident=7 total=147 GET=68 POST=79 op_gated=21`，`duplicates: 0`，`controller -> native missing: 0`，`native -> api-reference extra: 0`。删除前后 owner 分布无 `device_probe_export_failure`（该插件零路由），147 基线未漂移。 |
| `scripts/build-debug.sh` | **PASS（Debug 构建）** | `BUILD SUCCESSFUL`；产物 `output/inotia4_qol_lsposed_debug_2610161056_2cf5f19dee6b.apk`，SHA-256 `2cf5f19dee6b695efe907491518f456d1d5d400e3c8b461bc18b89b0c19fec7e`。**该 APK 未安装到设备。** |
| host CTest | **PARTIAL** | 143 项中 140 通过。失败 3 项：`core_api_lifecycle_tests`（`test_core_api_lifecycle.cpp:141,166` 断言 `features[0].category == kHidden` 与实现不符）、`debug_two_column_tests`（`test_debug_two_column.cpp:381` 断言 `g_provider->active()`）、`core_settings_config_store_tests`（`test_core_settings_config_store.cpp:424` `mkdtemp` 返回空，环境项）。三项测试源文件与被测对象在本批均未改动。 |
| `git diff --check` | **PASS** | 输出：无 whitespace 诊断。 |
| 文档同步 | **PASS** | `docs/reference/plugins/settings.md` 删除探针 feature 例外句；`docs/refactor/verification.md` 的 VM-EXPORT-01 追加「插件审计批 2026-10-16 后注」。`api-reference.md` 无需改动（本插件零路由）。 |
| 生成文档重生成 | **未执行** | `gen_baseline_doc.py --check` 与 `gen_map_doc.py --check` 在本批动手前即已不一致（TL-07），重生成会混入工作树的无关批次内容。待该批次提交后统一重生成。 |

**总体结论**：Host Gate 与 Debug 构建 Gate 通过；真机行为验收为 **NOT_ACCEPTED（未执行）**。本批为纯删除，不改变 Release 与正常 Debug 构建的运行时行为；不得把构建 PASS 延伸为真机 PASS。

**后续待办**：无关批次（config platform service / UI 字体）落地后重跑 `gen_baseline_doc.py` 与 `gen_map_doc.py`，消除 TL-07。
