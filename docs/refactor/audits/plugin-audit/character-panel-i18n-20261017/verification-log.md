# `character_panel_i18n` 插件审阅登记（2026-10-17）

- 审阅对象：功能插件 `character_panel_i18n`（`module/app/src/main/cpp/plugins/character_panel_i18n/`，6 源文件 + CMakeLists）。
- 基线参考说明（阶段 0.75，本批新建）：`docs/reference/plugins/character_panel_i18n.md`（`docs/INDEX.md` 链接因同工作区并发会话正编辑该文件，未与本次一并提交，见文末说明）。
- 修复提交：`6cdc0ae2`（contracts 数值下沉）、`c909a476`（插件守卫/测试/注释）、`d051a1d7`（宿主 EntryHook 空 backup 语义）、`489e7efd`（参考说明）。
- 现场基线（阶段 0）：设备 `192.168.3.56:5555`，`/api/health` = `ready`、variant `monster v29`、API `:8088`、`/api/ui/screen` = `panel_settings`；`build-debug.sh` 成功（APK `2610170528_10b218e703f9`，SHA-256 `10b218e703f9…`）。

## §1 本批登记问题

Findings（阶段 1 @oracle 只读静态审计，A–G 七维；无 P0/P1）：

| 编号 | 严重度 | 文件:行 | 问题 | 处置 |
|---|---|---|---|---|
| I18N-01 | P2 | `plugins/character_panel_i18n/feature.cpp:27-32`、`feature.h:117-119` | 消费者把 `uint32_t text_id` 截成 `uint16_t`；通用 dispatch 收 `uint32_t`（`hooks/dispatch.h:26`），超 16 位 id 可能折叠命中 35185–35196。真实路径经 `core.custom_ui` 的 `uint16_t` 入口（`plugins/core.custom_ui/text_mechanism.cpp:53`）不可达 | 已修复：`owner_table_modify` 在转换前拒绝 `text_id > 0xFFFF`（`c909a476`），并补越界用例 |
| I18N-02 | P2 | `charinfo_scope.cpp:25,32-34`；宿主 `hooks/install.cpp:37-47`、`transaction.cpp:341-360` | `on_resource_ready(nullptr)` 返回成功；且宿主在 hook 原语 `rc==0` 而未交还原函数入口时仍判安装成功 → wrapper 无 backup 会跳过原绘制。用户裁决「插件+宿主一并修」 | 已修复：插件拒绝空 backup；宿主撤销后失败（撤销成功 `resource_ready_failed`／撤销失败 tombstone+isolated）；4 处调用点传入 `unhook_fn`（`c909a476`、`d051a1d7`） |
| I18N-03 | P2 | `plugin.cpp:52-55`；`feature.cpp:35-58` | 重新启用时忽略 `register_consumers()` 失败返回（能力缺失/`subscribe` 返回 0/容量满）→ 可静默失效 | 已修复：`on_enabled` 暴露注册失败并写日志，`fill_status` 如实报告（`c909a476`），补无 SDK 用例 |
| I18N-04 | P2 | `feature.h:10-18,117-119` | 插件 header 重声明 `qol::hooks` 枚举并依赖 `qol::infra::active_text_scope()`，越层依赖私有实现（`ARCHITECTURE.md:110-112`） | 已修复：稳定数值下沉 `contracts/text_layer.h`（`6cdc0ae2`），`infra`/`hooks` 以 `static_assert` 对齐；插件改用 contracts 常量并移除私有 helper（`c909a476`） |
| I18N-05 | P2 | `tests/test_character_panel_i18n.cpp:81-91` | 缺 wrapper push/pop、嵌套恢复、push 失败、空 backup、注册失败、越界用例 | 已修复：补齐上述 host 用例（`c909a476`） |
| I18N-06 | P2 | `charinfo_scope.h:5-12,18`、`charinfo_scope.cpp:18`、`feature.h:20-28`、`infra/text_scope.h:7`、`contracts/text_layer.h:5` | 注释仍称文案/Hook 归 `plugins/text_layer`（该插件已不存在，机制归 `core.custom_ui`），并引用过期 ARCHITECTURE 章节号 | 已修复：代码注释按当前实现修正（`c909a476`）；`infra/text_scope.h:7` 的 `plugins/text_layer` 措辞仍待统一（见 §2） |

文件数核对：`baseline.md`/`map.md` 的「6」指 `.cpp/.h/.inc` 源文件数，目录实为 6 个此类文件 + `CMakeLists.txt`（共 7 个实体），口径一致，非缺陷。

审阅决议（阶段 2，用户裁决）：

- **D1**：跨边界数值（TextScope 0..4、DispatchPhase 0/1、ConsumerKind 0/1/2）下沉到 `contracts/` 层，插件不再依赖 `hooks`/`infra` 私有头。
- **D2**：EntryHook「成功但 backup 为空」由插件 + 宿主一并修（宿主侧须先撤销再失败）。
- **D3**：dispatch resource group / scope 栈容量策略本批不处理，仅登记观察（无饱和证据）。

## §2 无法验证项

本批无经用户确认后永久不再主动验收的 UNVER 项。

遗留待办（非 UNVER，属文档/并发状态）：

- `infra/text_scope.h:7` 注释仍写「文本层（plugins/text_layer）」，与实际（`core.custom_ui` 提供 `text_layer` 能力）不符；`docs/refactor/issues.md` §9 文档漂移统一更新时处理。
- `docs/INDEX.md` 未提交：该文件被同工作区**并发会话**改动（新增 `push_all`、`world_teleport` 参考链接），与本次 `character_panel_i18n` 链接同处一行，无法拆分，故本批未一并提交。

## §3 已完成验收登记

| 验收项 | 结果 | 证据 |
|---|---|---|
| Host CTest（全量） | **PASS（142/143）**：唯一失败 `core_settings_config_store_tests` 为既有环境项 TL-06（`test_core_settings_config_store.cpp:424` `mkdtemp` 返回空），与本批无关 | `.tmp/character-panel-i18n/host`；`ctest --test-dir .tmp/character-panel-i18n/host --output-on-failure` |
| Host 受影响 target | **PASS**：`character_panel_i18n_tests`、`module_text_tests`、`hooks_dispatch_tests`、`hooks_registry_tests`、`hooks_transaction_tests` 全绿（含新增空 backup / 越界 / push 失败 / 注册失败 / 事务语义用例） | 同上 |
| 架构校验 | **PASS**：`check_layers`/`check_offsets`/`check_jni`/`check_resources` 全部通过 | `uv run python scripts/maintenance/check_arch.py` |
| 路由漂移 | **PASS**：native frozen 147、baseline 147、duplicates 0、missing/extra 0 | `uv run python scripts/maintenance/check_http_routes.py` |
| Debug 构建 | **PASS**：`BUILD SUCCESSFUL`，APK `output/inotia4_qol_lsposed_debug_2610170528_10b218e703f9.apk`，SHA-256 `10b218e703f94b5af41dfadb91d5c1f247d53e790abd9d8c61a32a92de742e3d` | `scripts/build-debug.sh`（构建成功 ≠ 运行身份） |
| `git diff --check` | **PASS**（无输出） | `git diff --check` |
| 真机功能（阶段 4/5，用户执行） | **PASS（用户确认，2026-10-17）**：构建安装后真机测试，插件正确工作 | 用户确认；本轮由用户自行完成部署与测试，未留本目录证据副本 |

## 基础设施观察（G，非本插件缺陷）

- **EntryHook「成功但无 backup」宿主语义**：`hooks/install.cpp` 与 `transaction.cpp` 原在 `rc==0` 时无条件接受空 backup；本批已修（`d051a1d7`）。同时收紧了 `install_plugin_sync` 的 `handle_claim_failure`：资源非 `free` 时不再武装 retry（避免对 tombstone 资源重试）。
- **dispatch resource group 不回收**：全局仅 8 个 resource group（`hooks/dispatch.cpp:15,26-30`），`unsubscribe` 移除消费者后不清空空 group（`hooks/dispatch.cpp:135-147`），持续订阅不同 resource ID 可能耗尽 group 槽。本插件经固定 GetText 资源不受影响。已登记 `docs/refactor/issues.md` §7（INF-13）。
- **作用域栈与分派**：Host scope push/pop 为 thread_local 16 项栈（`host/host_sdk.cpp:1078-1094`），本插件正常路径配对；未发现撤销窗口缺陷。
- **`fill_status` 零调用点**：本插件 `fill_status` 已按真实状态实现（`c909a476`），但宿主无调用点属全局 ENG-04（`docs/refactor/issues.md:85`），不重复立案。

## 过程记录

- 阶段 3 曾派两个 @fixer 后台车道：其一（`fix-2`）在本会话 Code Mode 目录无文件工具，两次报告无法改文件；另一（`fix-1`）实际写入了部分改动后被取消（其产物已人工复核、修正并合并：删除重复的 contracts 常量定义、改用 `host_sdk()` 替代未导出的 `g_sdk`、移除多余模板重载、补齐测试）。宿主侧（车道 B）由主代理直接实施。
- 同工作区存在并发会话改动（`contracts/game_ports/ports.h`、`game/adapter_ports/character_extras_port.cpp`、`tests/test_host_adapters.cpp`、`tests/test_item_facts_plugin.cpp`、`docs/refactor/adr/0003-*.md`），均未纳入本批提交。
