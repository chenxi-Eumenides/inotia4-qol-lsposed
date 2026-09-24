# UI/文本域 — core.ui 改名为 core.custom_ui（ADR 0001 第 3a 批）

日期：2026-09-24 ｜ 依据：`docs/refactor/adr/0001-ui-text-domain-split.md`（D1/D2）

## 目的
裁决 D1 的 A 档里，界面工具/框架插件正式命名为 **`core.custom_ui`**（`core.*` = 官方核心插件命名空间，非权限）。

## 改动
| 位置 | 改动 |
|---|---|
| 目录 | `plugins/core.ui/` → **`plugins/core.custom_ui/`**（git mv，含 `ui_components.{h,cpp}` + `panels/stack.{h,cpp}` + `plugin.{h,cpp}`） |
| 构建目标 | `qol_plugin_core.ui` → **`qol_plugin_core.custom_ui`** |
| 插件 id | `"core.ui"` → **`"core.custom_ui"`** |
| `core.settings/feature.cpp` | 3 处归属字符串 `"core.ui"` → `"core.settings"`（设置页 panel provider owner、动作项 owner、面板注销 owner） |
| `host/assembly.cpp` | include 路径随目录改名 |
| `module/app/src/main/cpp/CMakeLists.txt`、`tests/CMakeLists.txt`、`tests/test_core_ui_{components,panels}.cpp` | 源路径/include 随目录改名 |

**C++ 命名空间未改**（`qol::features::core_ui` 仍被框架文件使用）：裁决只要求插件命名，命名空间属内部实现，
如需一致可在后续单独收敛。能力 ID（`ui_components`/`ui_panels`/`ui_choice`/`text_layer`）与版本**保持原样**（D2）。

## 验证
| 项 | 结果 |
|---|---|
| Host CTest | **66/66** |
| `check_arch` / `check_http_routes` | 通过（143/143） |
| 构建 | 普通 `2609241334_05aed2f359ef`；kernel-only `2609241334_b39633a843a8` |
| LSPatch | 3/3，已安装 |
| 真机·插件状态 | `core.custom_ui` **enabled**、`core.settings` **enabled**、`core.ui_state` **enabled** |
| 真机·owner | `core.settings` 5 / `core.ui_state` 8 / `api.ui` 1 / `core.custom_ui` 0，合计 **143** |
| 真机·设置端点 | `status`/`inject`/`restore` 均 200 |
| 真机·崩漏 | FATAL/SIGSEGV = 0 |

## 过程要点
改名后需同步**四处**引用：插件目录/目标/id，以及 `module/app/src/main/cpp/CMakeLists.txt`（主库插件源清单）、
`tests/CMakeLists.txt`（测试源路径）、测试源码 include。漏掉主库清单会以
`Cannot find source file: plugins/core.ui/plugin.cpp` 形式在 CMake 配置阶段失败。

## 未覆盖
- 第 3b：`ui_choice` 从 `core.ui_state` 迁入 `core.custom_ui`
- 第 3c：文本机制并入 `core.custom_ui`；新建 `character_panel_i18n`
- 第 4：`go_main_menu` → `core.ui_state`
