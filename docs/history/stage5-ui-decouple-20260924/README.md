# 阶段 5 / UI 文本域 — 设置页解耦（ADR 0001 第 1 批）

日期：2026-09-24 ｜ 依据：`docs/refactor/adr/0001-ui-text-domain-split.md`（D1–D8）

## 目的
按裁决次序第 1 步：**让设置页不再依赖框架私有头**（`core.ui/ui_components.h`、`panels/stack.h`），
为抽出 `core.settings` 做准备。行为保持不变。

## 改动
| 文件 | 改动 |
|---|---|
| `plugins/core.ui/panels/settings_ui_local.{h,cpp}`（新） | 设置页自持绘图/几何 helper（20 个）+ `ui_adapter()` + `ui_original::` + `ui_custom::`；依赖经 `settings_ui_bind(adapter, components)` 注入（抽出后改为经能力取得同一对指针，调用点不变） |
| `plugins/core.ui/ui_components.cpp` | 删除第 397–566 行（页面便利层整段）→ **568 → 398 行**，只留组件能力实现 + `bind/unbind/api` |
| `plugins/core.ui/CMakeLists.txt` | 登记新源文件 |
| `plugins/core.ui/panels/settings.cpp` | include 由框架私有头改为 `settings_ui_local.h` |
| `plugins/core.ui/feature.cpp` | `panels_bind_sdk` 注入 `settings_ui_bind(sdk->ui, ui_components_api())`；`panels_unregister` 调 `settings_ui_unbind()` |

行为等价说明：`ui_get_rect`/`ui_hit_test` 由「框架内部组件注册表」改为**适配表 `get_rect`/`hit_test`**，
使该模块不再引用框架私有状态（这是本批唯一实现路径变化，语义相同）。

## 验证
| 项 | 结果 |
|---|---|
| Host CTest | **66/66** |
| `git diff --check` / `check_arch` / `check_http_routes` | 通过（routes 143/143，exit=0） |
| 构建 | 普通 `2609241301_5a2d50bfda08`；kernel-only `2609241301_0e53d6e98e35` |
| LSPatch | 3/3，已安装 |
| 真机·owner | `core.ui` 5 / `core.ui_state` 8 / `api.ui` 1，合计 **143**（未变） |
| 真机·设置端点 | `settings-ui/status` 200；`inject` 200；`restore` 200 |
| 真机·能力撤销/恢复 | `deactivate core.ui`（ticket done，state `inactive`）→ **`inject`/`restore` 503 `{"ok":false,"error":"http owner inactive"}`**；`activate`（done，`enabled`）→ 均恢复 **200** |
| 真机·崩漏 | FATAL/SIGSEGV = 0（帧推进至 376） |

## 验收中发现并记录的事实
1. **`GET /api/debug/settings-ui/status` 不受插件停用约束**：停用 `core.ui` 后仍返回 200。
   原因是该 GET 不在插件的 dispatch 命令表内（4 条 POST 才是自注册命令），它走 Kotlin/JNI 路径。
   → 待第 2 批把设置路由整体迁入 `core.settings` 时一并收口。
2. **同意页/加载态帧冻结**：`screen=loading` 时帧停在 106，命令队列不排空 →
   lifecycle 票据长期 `pending`。**验收必须在主菜单且 `game_frame` 两次不同时进行**。
3. 设置调试端点中 `open-option`/`open-panel` **故意未调用**：二者会改写原版选项弹窗生命周期
   （smoke 亦跳过记 warn，历史上有崩溃记录），本次只验证 `status`/`inject`/`restore` 三条。

## 未覆盖（后续批次）
- 第 2 批：抽出 `core.settings`（含把 GET status 一并收口）
- 第 3 批：`ui_choice` 迁 `core.custom_ui`；`core.ui` 改名 `core.custom_ui`；文本机制并入；新建 `character_panel_i18n`
- 第 4 批：`go_main_menu` → `core.ui_state`（先逐条更新响应契约）
- 资源 `own:popup.settings` 的归还：本批未单独取证（停用/恢复路径已走通、无隔离与 FATAL），
  第 2 批搬迁时随 `core.settings` 一并取证。
