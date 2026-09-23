# 阶段 5 批 5-7a：对话写 2 条迁 core.choice（VM-K5-07a PASS）

日期：2026-09-23 ｜ 范围：`POST /api/ui/dialog/select`、`POST /api/ui/start_interact`

## 1. 改动

- **新私有端口** `QOL_PORT_CHOICE_WRITE = 20`（v1.0，`contracts/game_ports/choice_write_port.h`）：
  `write(kind, action, index, out, cap, size)`；kind `kQolChoiceWriteDialogSelect` / `kQolChoiceWriteNpcInteract`。
  仅 `core.choice` 可注入（`host/plugin_registry.cpp` 私有端口约束）。`QOL_PORT_UI_WRITE = 19` 同时预留（批 5-7b）。
- **拼装** `game/state/choice_http.h` + `game_system.cpp` 的 `build_choice_write_result_json`：调用既有
  `data_op_dialog_select` / `data_op_npc_interact`，输出**原始 op JSON**（失败携带原错误体）。
- **适配层** `game/adapter_ports/choice_write_port.{h,cpp}` + `CMakeLists.txt` 登记。
- **`core.choice` 插件**（`plugins/core.choice/plugin.cpp`）：requires 增 `http.registry` + `http.dispatch`；
  ports 增 CHOICE_WRITE（`port_count 1→2`）；结果处理器 id `0x4E430001`；声明式 POST 2 条
  （`kChoiceWriteRoutes`）+ `kChoiceWritePayload{kind,index,action[48]}`；install 校验 `port_count>=2`、
  `abi.size/minor>=9` 与尾部结果面并注册；deactivate 显式注销。
- **`routes.inc`**：2 条 owner `api.ui` → `core.choice`（143 不变；`api.ui` 5→3，`core.choice` 2）。
- **Kotlin**：`UiActions.npcInteract` → `NativeHttpCommand.post("/api/ui/start_interact","{}")`；
  `UiActions.dialogSelect` 的 **agreement（Java 窗口）分支保留在 Kotlin 传输层**（`UiActivityTracker`，JVM-only），
  非 agreement 时改为 `NativeHttpCommand.post("/api/ui/dialog/select", …)`。控制器校验不变
  （`action` 空且 `index<0` → 400 `action or index required`；非法 body → 400 `bad request`）。
- **测试**：`tests/test_game_port_stubs.cpp` 增 `choice_write_port()` 桩；`tests/test_native_choice.cpp`
  期望 `requires_count=2`、`port_count=2`（CHOICE + CHOICE_WRITE）。

## 2. 验证

| 项 | 结果 |
|---|---|
| Host CTest | **65/65** |
| `git diff --check` / `check_arch` / `check_http_routes` | 通过（143/143、exit=0） |
| 构建 / LSPatch | `output/inotia4_qol_lsposed_debug_2609232233_520a3129fc73.apk`；3/3；已安装 |
| 真机·owner | routes 中 `/api/ui/dialog/select`、`/api/ui/start_interact` owner = `core.choice` |
| 真机·校验前置 | `dialog/select {}` → 400 `action or index required`（未产生票据） |
| 真机·平台分支 | 同意页 `dialog/select {"action":"ok","index":0}` → `{"ok":true,"result":"close_dispatched"}`（Kotlin 分支，无票据） |
| 真机·票据（world） | `start_interact` → 200 ticket → `state:"done"` + `result":{"ok":true,"result":"dialog_shown"}`；`dialog/select {"action":"ok","index":0}` → `state:"failed"` + `result":{"ok":false,"error":"no dialog"}`（原 op JSON 逐字回取） |
| 真机·非 world | 两条均 `state:"failed"` + `result:{"ok":false,"error":"not in game"}`（原响应原样） |
| 真机·生命周期 | `core.choice` deactivate → 503 `http owner inactive`；activate → 恢复 200 |
| smoke | `total=143 pass=125 warn=18 fail=0`，与基线 diff 仅 4 条已知 party op 环境差异（零新增） |
| logcat | FATAL/SIGSEGV/SIGABRT = 0 |

## 3. 四元组

| 触及文件 | R/B 编号 | VM 卡 | 原因 |
|---|---|---|---|
| `contracts/game_ports/{ports.h,choice_write_port.h}`、`game/state/choice_http.h`、`game_system.cpp`、`game/adapter_ports/choice_write_port.{h,cpp}`、`CMakeLists.txt`、`host/plugin_registry.cpp`、`plugins/core.choice/plugin.cpp`、`plugins/core.api/routes.inc`、Kotlin `UiActions.kt`、`tests/{test_game_port_stubs.cpp,test_native_choice.cpp}`、文档 | 无扩展背包 R-xx/B-xx | `VM-K5-07a` | 全部已执行：host/静态/构建/LSPatch/真机（owner、校验、平台分支、票据与结果回取、非 world、生命周期）/smoke/logcat |

## 4. 遗留

- 批 5-7b：`/api/ui/open_panel`、`/api/ui/close_panel` → `core.ui`（`QOL_PORT_UI_WRITE=19`，结果附 `state=<gamestate>`）。
- 批 5-7c：`/api/ui/go_main_menu` → `core.save`（复用 `QOL_PORT_SAVE_WRITE` 新增 kind；需 `data_op_main_menu_direct`）。
