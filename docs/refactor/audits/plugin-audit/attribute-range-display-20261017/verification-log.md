# `attribute_range_display` 插件审阅登记（2026-10-17）

- 审阅对象：功能插件 `attribute_range_display`（`module/app/src/main/cpp/plugins/attribute_range_display/`，4 文件）。
- 基线参考说明（阶段 0.75，本批新建）：`docs/reference/plugins/attribute_range_display.md`，已加入 `docs/INDEX.md`。
- 修复提交：`cf09019d`（依赖改 required、删除未消费端口声明、入口 hook 空 backup fail-closed、删除未消费 `g_game`、补 Host 测试）。
- 现场基线（阶段 0）：设备 `192.168.3.56:5555`，`/api/health` = `ready`、variant `monster v29`、`/api/ui/screen` = `main_menu`；运行时 `attribute_range_display` = `enabled`、`degraded=false`、2 资源 `live`，provider `attribute_quality` = `enabled`、`MATH_GetRandom` `live`。

## §1 本批登记问题

Findings（阶段 1 @oracle 只读静态审计，A–G 七维）：

| 编号 | 严重度 | 文件:行 | 问题 | 处置 |
|---|---|---|---|---|
| ARD-01 | P1 | `plugins/attribute_range_display/feature.cpp:67-75,78-80,114-120` | `on_resource_ready` 对匹配入口 hook 的空 backup 仍返回 0；空 backup 时两个 wrapper 跳过原函数调用，与资源 `kFailClosed` 声明相悖（fail-open 破坏渲染/构造） | 已修复：空 backup 清对应指针、`set_active(false)`、返回非零（`cf09019d`）。镜像 `attribute_quality/feature.cpp:253-263` |
| ARD-02 | P2 | `feature.cpp:17-22,56-59,67-74,78-120` | backup 为普通全局指针，写入/读取无同步 | 关闭（非缺陷）：资源安装/还原经帧任务在游戏主线程推进（`ARCHITECTURE.md` §6.2；`host/plugin_registry.cpp:1288`），入口 hook 亦由游戏主线程调用，二者串行；真机可作附加确认 |
| ARD-03 | P2 | `feature.cpp:18,56-59,83-86` | `g_game`（QolGameHost）只保存并做 null 检查，未使用任何成员 | 已修复：删除该指针与门禁（`cf09019d`） |
| ARD-04 | P2 | `tests/test_attribute_range_display.cpp`、`tests/CMakeLists.txt:1086-1100` | 测试不编译 `plugin.cpp`，缺 descriptor/空 backup/负 level/全颜色码覆盖 | 已修复：target 纳入 `plugin.cpp` 并补用例（`cf09019d`） |
| ARD-05 | P2 | `docs/reference/plugins/attribute_range_display.md` | 文档「backup 始终先调用」不准确（实现仅非空时调用）；D1/D2 后依赖事实变化 | 已修复：基线文档按最终实现同步 |

审阅决议（阶段 2，用户裁决）：

- **D1**：删除本插件未消费的 `QOL_PORT_ITEM_EXTRAS` 端口声明及其 include。全局 `QOL_PORT_ITEM_EXTRAS` contract/adapter 是否退役属另行范围，不在本批。
- **D2**：`attribute.quality` 由 `optional_requires` 改为 `requires`（硬依赖）。required-only 图无环：`item.facts → attribute.quality → attribute_range_display`，display 不 `provides`。

关键判定（H1–H7）：

- H1 证实为缺陷 → ARD-01。
- H2 证实 `ctx.game` 未使用 → ARD-03。
- H3 竞态未证实，按主线程串行约束关闭 → ARD-02。
- H4 颜色码语义（`Grey→'G' … Gold→'Y'`）静态不可证实，留真机目视。
- H5 证实 D2 不引入 required-only 环。
- H6 每行重新 `require` 正确（避免跨撤销缓存能力指针），性能无证据、不为此改设计。
- H7 `on_toggle` no-op 正确（Host feature→enable 派生，`ARCHITECTURE.md` §8.2）。

基础设施观察（G，非本插件缺陷，已登记 `docs/refactor/issues.md` §7）：

- `host/plugin_registry.cpp:669-685` 的 optional-cycle 日志用「未发射」两端筛选，不能证明该边本身在环内（真实环是 `core.item ↔ core.character`）→ INF-10。
- `host/ui_adapter.cpp:74-85` 允许 `max_text_bytes=4096`，而 `game/symbols/game_symbols.h:606-607` 声明 UIDesc 文本缓冲 `0x200` → 越界读风险在 Adapter 边界 → INF-11。

## §2 无法验证项

本批无经用户确认后永久不再主动验收的 UNVER 项；真机待验项见 §3。

## §3 已完成验收登记

| 验收项 | 结果 | 证据 |
|---|---|---|
| Host 定向 CTest | **PASS（3/3）**：`attribute_range_display_tests`、`plugin_registry_tests`、`capability_lifecycle_tests` | `.tmp/phase3-attribute-range-display/host`；命令 `ctest --test-dir .tmp/phase3-attribute-range-display/host -R '^(attribute_range_display_tests|plugin_registry_tests|capability_lifecycle_tests)$' --output-on-failure` |
| 架构校验 | **PASS**：`check_layers`/`check_offsets`/`check_jni`/`check_resources` 全部通过 | `uv run python scripts/maintenance/check_arch.py` |
| 路由漂移 | **PASS**：native frozen 147、baseline 147、duplicates 0、missing/extra 0 | `uv run python scripts/maintenance/check_http_routes.py` |
| Debug 构建 | **PASS**：`BUILD SUCCESSFUL`，APK `output/inotia4_qol_lsposed_debug_2610170253_02f8fa02377a.apk`，SHA-256 `02f8fa02377ab8e3d48cddd2bf782bafb027cb59eb187cb1e698be6347a19f13` | `scripts/build-debug.sh`；构建成功不等于运行身份 |
| `git diff --check` | **PASS**（无输出） | `git diff --check` |
| 部署与运行身份（阶段 4） | **PASS**：集成包 `output/Inotia4_v1.3.2_monster_v29_lspatched.apk`（SHA-256 `4f4a298e91b6…`）安装成功；运行模块 sha256 `02f8fa02377ab8e3…` = 本次 Debug 构建模块 | `evidence/step-02-*.txt`、`evidence/step-03-install.txt`、`evidence/main-06-identity.txt` |
| D2 required 依赖（装配，阶段 4） | **PASS**：`attribute_range_display` `requires=['attribute.quality']`、`enabled`、`degraded=false`、2 资源 `live` | `evidence/main-07-features.json` |
| D2 required 级联（阶段 4） | **PASS**：provider `attribute_quality` deactivate → provider `inactive`（`MATH_GetRandom` `free`）且 **display `inactive`、两 hook `free`（required 级联停用，非 degraded）**；provider activate + display activate → 两者恢复 `enabled`、资源 `live` | `evidence/main-08a-deactivate.json`～`main-08e-final.json` |
| 日志（阶段 4） | **PASS**：当前会话不再出现 `downgraded … provider=attribute_quality consumer=attribute_range_display`（0 次）；其他 optional 环警告保留（`custom_recipe→attribute.quality`、`core.item↔core.character` 等）；FATAL/SIGSEGV = 0 | `evidence/main-09-session.txt` |
| VM-ARD-01：视觉（六档颜色目视） | **待用户目视（阶段 5）** | runbook 见本目录 `manual-runbook.md` |

> 进入游戏过程说明（阶段 4 记录）：安装后首次 `enter_slot` 被 `ui occupied: agreement` 阻断，`dialog/select {"action":"ok"}` 返回 `close_dispatched` 后仍未解除；随后重试 `dialog/select` 走 native 分支返回票据、`enter_slot` 成功、`screen=world`。属启动同意页异步关闭的时序问题，非本插件缺陷。
