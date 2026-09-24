# 阶段 5 / ADR 0001 第 3b 批：`ui_choice` 迁入 `core.custom_ui`（VM-K5-08e）

日期：2026-09-24 ｜ 依据：`docs/refactor/adr/0001-ui-text-domain-split.md`（D1/D2/D3）

## 1. 改动

| 项 | 从 | 到 |
|---|---|---|
| 控件实现 | `plugins/core.ui_state/feature.{h,cpp}` | **`plugins/core.custom_ui/feature.{h,cpp}`**（命名空间随迁为 `qol::features::core_ui`） |
| 能力 `ui_choice` v1.0 | `core.ui_state` provides | **`core.custom_ui` provides**（能力 ID 与版本**不变**，消费者零改动） |
| 2 条私有资源 `hook:UIChoice_Init`、`ptrslot:g_uichoice_button_list_exe_got` | `core.ui_state` | **`core.custom_ui`**（owner 与 handler 随迁） |
| 端口 `QOL_PORT_CHOICE` 需求 | `core.ui_state` | **`core.custom_ui`**（port_count 1） |
| 私有端口约束主体 | `core.ui_state` + `world_teleport` | **`core.custom_ui`** + `world_teleport` |
| `core.ui_state` 描述符 | requires 2 / provides 1 / resources 2 / ports 3 / routes 4 | requires 2 / **provides 0** / **resources 0** / **ports 2**（CHOICE_WRITE + UI_WRITE）/ routes 4 |
| `core.custom_ui` 描述符 | provides 2 / resources 0 / ports 0 | **provides 3**（components/panels/choice）/ **resources 2** / **ports 1**，并接 `on_resource_ready` |

配套：`core.ui_state/CMakeLists.txt` 去掉 feature.cpp 与 device-probe 定义；`core.custom_ui/CMakeLists.txt` 加入 feature.cpp 与 device-probe 定义；根 `CMakeLists.txt` 的插件源路径同步；`core.custom_ui/plugin.h` 增 `choice_port()`/`host_sdk()` 声明与 `choice_port.h` include；测试接线（`native_choice_tests` 改编译 `core.custom_ui/{feature,plugin,ui_components}.cpp` + `panels/stack.cpp`，删去自备桩、断言改对 `core.custom_ui` 描述符；`plugin_registry_tests` 的 `QOL_PORT_CHOICE` 私有约束用例主体 id 改 `core.custom_ui`）。

## 2. 验证

| 项 | 结果 |
|---|---|
| Host CTest | **66/66** |
| `git diff --check` / `check_arch`（含 check_resources）/ `check_http_routes` | 全部通过 |
| 普通 Debug | `output/inotia4_qol_lsposed_debug_2609241351_d265fc7aa34a.apk` |
| kernel-only Debug | `output/inotia4_qol_lsposed_debug_2609241351_64627ed87cfb.apk` |
| LSPatch | 3/3（monster v29 已安装） |
| 真机·提供者与消费者 | `core.custom_ui`/`core.ui_state`/`core.settings`/`world_teleport` 均 `enabled` |
| 真机·资源归属 | `hook:UIChoice_Init` 与 `ptrslot:g_uichoice_button_list_exe_got` 均 **`live` 且 owner=`core.custom_ui`** |
| 真机·资源归还 | 停用 `core.custom_ui` → 两条资源 **`free`**；启用 → **`live`**（撤销/恢复闭环） |
| smoke | `total=143 pass=120 warn=23 fail=0`，与基线 **diff=0** |
| logcat | FATAL/SIGSEGV/SIGABRT = 0 |

**过程记录（帧冻结）**：首次停用命令未落地——同意页/加载态使帧停在第 106 帧，命令队列不排空（lifecycle 票据 pending）。`POST /api/ui/dialog/select {"action":"ok","index":0}` 关闭后帧恢复（196→211，main_menu），停用随即落地。此为既有登记行为，非本批引入。

## 3. 四元组

| 触及文件 | R/B 编号 | VM 卡 | 原因 |
|---|---|---|---|
| `plugins/core.custom_ui/{plugin.{h,cpp},feature.{h,cpp},CMakeLists.txt}`、`plugins/core.ui_state/{plugin.cpp,CMakeLists.txt}`、`host/plugin_registry.cpp`、根 `CMakeLists.txt`、`tests/{CMakeLists.txt,test_native_choice.cpp,test_plugin_registry.cpp}` | 无扩展背包 R-xx/B-xx | `VM-K5-08e`（含 `VM-D1`、`VM-P4-05` 口径：控件资源撤销/恢复） | 全部已执行：host/静态/双构建/LSPatch/真机（提供者、资源归属与归还、生命周期）/smoke/logcat |

## 4. 遗留

- 选择框**运行时行为**（弹出、选择回调）需游戏内触发，属 `VM-P4-05`/`VM-45` 深度行为卡，待人工配合。
- 下一批 3c：文本机制并入 `core.custom_ui`（撤销独立 `text_layer`）；新建 `character_panel_i18n`（保留 35188 `nullptr` 空占位）。
