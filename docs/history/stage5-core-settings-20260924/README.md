# UI/文本域 — 抽出 core.settings（ADR 0001 第 2 批）

日期：2026-09-24 ｜ 依据：`docs/refactor/adr/0001-ui-text-domain-split.md`

## 目的
按 ADR 次序第 2 步：把**设置成品页**从 `core.ui` 抽出为独立插件 `core.settings`，
并让设置页**经能力**（而非框架实现）使用界面工具。

## 改动
| 位置 | 改动 |
|---|---|
| `plugins/core.settings/`（新插件，id `core.settings`） | 收纳：`feature.{h,cpp}`（页面 provider 注册 + 更多游戏按钮帧任务）、`log.h`、`panels/{rules.h, settings.{h,cpp}, settings_*.inc ×5, settings_ui_local.{h,cpp}}`、新 `plugin.{h,cpp}` |
| `core.settings/plugin.cpp` | 描述符：**requires** `ui_panels`+`ui_components`+`http.registry`+`http.dispatch`；**provides** 无；resources 1（`own:popup.settings`）；4 条 `/api/debug/settings-ui/*` POST 自注册；install/deactivate 调 `panels_bind_sdk`/`panels_register`/`panels_unregister`；deactivate 显式 `unregister_owner(kPluginId)` |
| `core.settings/feature.cpp` | `panels_bind_sdk` 改为**经能力**取得 `ui_panels`/`ui_components` 并注入 `settings_ui_bind(sdk->ui, components)`；新增 `settings_panels_open()` 薄入口（经 `ui_panels` 能力）；不再引用框架实现 |
| 设置页 3 处 `ui_panels_open` | 改为 `settings_panels_open`（经能力） |
| `plugins/core.ui/plugin.cpp` | 收敛为**纯框架插件**：provides `ui_components`+`ui_panels`；无 requires/resources/HTTP；install = `ui_components_bind`+`ui_panels_bind_sdk` |
| `plugins/core.ui/CMakeLists.txt` | 只余 `plugin.cpp` + `ui_components.cpp` + `panels/stack.cpp` |
| `plugins/core.api/routes.inc` | 5 条 `/api/debug/settings-ui/*` owner `core.ui` → **`core.settings`**（其他 138 条不变） |
| `host/assembly.cpp` | 注册 `features::core_settings::plugin_descriptor` |
| `bridge/native/gamebridge_internal.h` | settings 头路径随搬迁更新 |
| `tests/test_core_ui_panels.cpp` | `rules.h` include 路径随搬迁更新 |

## 验证
| 项 | 结果 |
|---|---|
| Host CTest | **66/66** |
| `git diff --check` / `check_arch` / `check_http_routes` | 通过（143/143，exit=0） |
| 构建 | 普通 `2609241327_e03dfddb17ed`；kernel-only `2609241325_bd7b4a29d006` |
| LSPatch | 3/3，已安装 |
| 真机·插件状态 | `core.settings` **enabled**、`core.ui` **enabled**（互相独立） |
| 真机·owner | `core.settings` **5** / `core.ui` **0** / `api.ui` 1，合计 **143** |
| 真机·设置端点 | `status` 200、`inject` 200、`restore` 200 |
| 真机·能力撤销/恢复 | `deactivate core.settings`（ticket `done`）→ `inject`/`restore` **503 `http owner inactive`**；`activate` → 恢复 **200**；期间 `core.ui` 保持 enabled（**依赖解耦已验证**） |
| 真机·崩漏 | FATAL/SIGSEGV = 0（帧推进 106→136） |

## 过程中的一个陷阱（已记录）
首次部署时 `core.settings` 呈现 **failed/404**：因为安装的 APK 是**更新 `routes.inc` 之前**构建的，
自注册命令的 owner（`core.settings`）与路由元数据 owner（`core.ui`）不一致 →
`register_command` 的 owner 校验失败 → 插件 install 失败。
**order matter**：owner 元数据与插件 id 必须同批一致，且需**重新构建后**再部署。

## 未覆盖（后续批次）
- `GET /api/debug/settings-ui/status` 停用后仍 200：该路由不在插件 dispatch 命令表内（走 Kotlin/JNI），
  如需收口需把它改为 dispatch query handler（本轮未做）。
- 第 3 批：`ui_choice` 迁 `core.custom_ui`；`core.ui` 改名 `core.custom_ui`；文本机制并入；新建 `character_panel_i18n`
- 第 4 批：`go_main_menu` → `core.ui_state`
- `own:popup.settings` 的归还取证：停用/恢复路径已走通且无隔离、无 FATAL；如需逐帧资源归还证据，随第 3 批一并补
