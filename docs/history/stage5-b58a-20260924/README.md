# 阶段 5 批 5-8a：core.choice → core.ui_state（UI 域 owner）+ 面板写并入

日期：2026-09-24 ｜ 对应卡：VM-K5-08a ｜ 依据：ARCHITECTURE §1.2/§5.2/§5.4/§7.2（本次同批修订）

## 1. 背景与范围

按 UI/文本划分定案（`core.custom_ui` 工具 / `core.ui_state` UI 域 owner / `core.settings` 成品页），本批执行第一步：

- **`core.choice` 撤销 → `core.ui_state`**：它从"选择框域"升为"游戏 UI 域 owner"，吸收面板开/关写路由（收尾批 5-7b）。
- `ui_choice` 能力**暂留** `core.ui_state`（后续 5-8b 随 `core.ui`→`core.custom_ui` 迁移到工具插件）。
- 路由 owner：`/api/ui` 4 条 GET + `open_panel`/`close_panel`/`dialog/select`/`start_interact` → `core.ui_state`（共 8 条）；`go_main_menu` 暂留 `api.ui`（5-8d 迁 `core.save`）。

## 2. 改动

- **改名**（10 文件）：插件目录 `plugins/core.choice` → `plugins/core.ui_state`；CMake 目标 `qol_plugin_core.ui_state`；C++ 命名空间 `qol::features::core_ui_state`；插件 id `core.ui_state`；日志前缀；`host/assembly.cpp`、`host/plugin_registry.cpp`（含私有约束）、`routes.inc`、测试接线；端口/接口注释里的 owner 名同步。
- **面板写并入**（收尾 5-7b）：`QOL_PORT_UI_WRITE`（19，私有于 `core.ui_state`）接入 registry（include + available_port + 私有约束）；`CMakeLists.txt` 登记 `game/adapter_ports/ui_write_port.cpp`；插件 `kPorts` 由 2 项增至 3 项、descriptor `port_count 3`；新增 `UiWritePayload{kind,panel[64]}`、`ui_write_command`（`open_panel` 时 panel 空 → 400 `panel required`）、`ui_write_result`（调端口，主线程）；**独立结果处理器 id `0x55490001`**（面板 kind 与对话 kind 各自从 0 起，共用一个 payload 会产生取值域歧义）；两个结果处理器在 install 注册、deactivate 显式注销；`kWriteRoutes[4]`（对话 2 + 面板 2）合并进 descriptor 的 `http_queries`。
- **结果形状**：`open_panel`/`close_panel` → `{"ok":true,"state":<gamestate JSON>}`（同迁移前 `attachUi` 键序，由 `game_system.cpp` 的 `build_ui_write_result_json` 拼装）；失败为原 op 错误体。
- **Kotlin**：`UiActions.panelOpen/panelClose` 改为 `NativeHttpCommand.post`（删除 `attachUi` 直调与 5 个已失效 import）；控制器校验不变。

## 3. 验证

| 项 | 结果 |
|---|---|
| Host CTest | **65/65**（含 `native_choice_tests` 期望 `port_count==3`） |
| `git diff --check` / `check_arch` / `check_http_routes` | 通过（routes 143/143、exit=0） |
| 构建 | 普通 `output/inotia4_qol_lsposed_debug_2609240043_c1c99fa2ca3c.apk`；kernel-only `..._2609240044_7262a4b2b405.apk`（`.so` 内 `/api/ui/*` 命中 0；唯一 `core.ui_state` 命中来自 registry 私有约束字符串，属内核预期） |
| LSPatch | 3/3；已安装 |
| 真机·owner | `/api/system/routes`：`core.ui_state` **8** 条、`api.ui` **1**（go_main_menu）、`core.choice` **0** |
| 真机·校验前置 | `open_panel {}` → 400 `{"ok":false,"error":"panel required"}`（无票据） |
| 真机·票据与保真 | `open_panel {panel:"inventory"}` → 200 `{"ok":true,"ticket":13}` → 终态 `state:"done"`、`result:{"ok":true,"state":{"screen":"world","frame":962,"dialog":{"type":"none","options":[]}}}`（gamestate 形状与迁移前一致）、`result_retained:true` |
| 真机·生命周期 | `core.ui_state` deactivate → `open_panel`（合法 body）**503** `http owner inactive`；activate → 恢复票据 |
| 真机·smoke | `total=143 pass=125 warn=18 fail=0`，与基线 verdict/status diff **仅 4 条已知 party op 环境差异**；FATAL/SIGSEGV/SIGABRT=0 |

## 4. 观察到的交互（重要，非缺陷）

**打开游戏面板会冻结帧计数**：`open_panel(inventory)` 成功后 `game_frame` 停止推进（实测 871→871），随之**命令队列不再排空**——表现为此后 `close_panel` 票据长期停留在 `pending`，且 lifecycle 停用事务也无法推进（插件仍显示 enabled）。面板关闭或重启游戏后恢复；实测中通过重启恢复干净状态并完成后续验收。

这与既有登记一致（主菜单帧冻结时 queued 命令不落地，需进档/恢复帧推进）。**含义**：以 API 打开游戏面板后，同进程的后续命令（含关闭面板）要等面板关闭或帧恢复才能执行；集成方对此应有预期。已记入本证据。

## 5. 四元组

| 触及文件 | R/B 编号 | VM 卡 | 原因 |
|---|---|---|---|
| `plugins/core.ui_state/*`（原 `core.choice`）、`contracts/game_ports/{ui_write_port.h,ports.h}`、`game/adapter_ports/ui_write_port.*`、`game/state/ui_http.h`、`game_system.cpp`、`host/{assembly.cpp,plugin_registry.cpp}`、`plugins/core.api/routes.inc`、`CMakeLists.txt`、Kotlin `UiActions.kt`、tests（`test_game_port_stubs.cpp`、`test_native_choice.cpp`）、`ARCHITECTURE.md`（规则先行）、docs | 无扩展背包 R-xx/B-xx | `VM-K5-08a` | 全部已执行：host/静态/双构建/kernel-only strings/LSPatch/真机 owner·校验·票据·生命周期/smoke/logcat |

## 6. 遗留与下一步

- `core.ui_state` 目前仍 provides `ui_choice`（工具能力）——**5-8b** 随 `core.ui`→`core.custom_ui` 一并迁移。
- 4 条 `/api/ui/*` GET 的处理器仍在 Kotlin（`ActivityThread` 平台检测，JVM-only），owner 标签已归 `core.ui_state`；deactivate 不使它们 503（无插件 handler 可摘），此点已在 api-reference「处理器仍在 Kotlin 的路由」小节登记。
- **5-8b**：`core.ui` 拆 `core.custom_ui`（组件+面板框架+ui_choice+文本机制）与 `core.settings`（设置页 + `/api/debug/settings-ui/*`）。
- **5-8c**：`text_layer` 机制并入 `core.custom_ui`；charinfo 内容与绘制钩子资源迁新汉化插件。
- **5-8d**：`go_main_menu` → `core.save`（内部名 `exit_slot`，URL 不变；新增 `data_op_main_menu_direct`）。
