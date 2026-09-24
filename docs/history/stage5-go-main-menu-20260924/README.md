# 阶段 5 / ADR 0001 第 4 批：`go_main_menu` 迁 owner 并转票据契约（VM-K5-08g）

日期：2026-09-24 ｜ 依据：`docs/refactor/adr/0001-ui-text-domain-split.md`（D5）
前置：3c 已完成并提交（`954680e`，VM-K5-08f）。

## 1. 范围与不变量
- 路由 `POST /api/ui/go_main_menu`：owner `api.ui` → **`core.ui_state`**；**URL 与 POST 方法不变**。
- `core.save` 继续负责进档（`enter_slot`）/建档（`create_slot`）等存档动作。
- 迁后 `api.ui` 标签归零（143 条路径/方法不变）。

## 2. 响应契约（先改文档，再实现）
- 迁移前：同步返回 `{"ok":true,"state":<Player 模型>}`（见 `api-reference.md` §7.10 与路由自身章节）。
- 迁移后：`POST` 立即返回 `{"ok":true,"ticket":N}`；原响应经 `GET /api/system/tickets/{id}` 的 `result` 回取；失败时 `result` 为原错误体且票据 `state=failed`。
- 文档更新：`api-reference.md` 该路由章节（返回格式改为票据 + 结果回取说明）、§7.10 新增「回主菜单」条目、`open_panel`/`close_panel` 章节的返回格式同步为票据契约（此前 5-8a 迁移后未更新的遗留）。
- smoke 基线：`/api/ui/go_main_menu` 条目 body 改为 `{"ok":true,"ticket":<id>}`、reason/note 标注票据契约（**verdict 仍按状态码判定**，条目数 143 不变）。

## 3. 实现
- **native**：`api/native/game_ui_operations.inc` 新增 `data_op_main_menu_direct()`——面板守卫（栈顶面板→官方 Pop 并等栈清空；非面板弹窗 fail-closed）、扩展背包 `prepare_main_menu` 清理、`GAMESTATE_SetState(4)` 切换全部与 `data_op_main_menu` 一致，**仅把跨帧同步的 `transition_run` 换成逻辑帧内联 `transition_run_inline`**（owner 命令本身在逻辑帧排空，再调同步版会自阻塞超时）；`api/native/game_ui.h` 加声明；旧入口与 JNI 保留。
- **端口**：`contracts/game_ports/ui_write_port.h` 增 kind `kQolUiWriteMainMenu = 2`（无参）；`game_system.cpp` 的 `build_ui_write_result_json` 增分支——面板写附 `state=<gamestate>`，回主菜单附 `state=<player>`（`build_player_json()`），失败原错误体透传。
- **插件**：`core.ui_state` 声明式写路由 `kWriteRoutes` 4 → **5**（新增 `/api/ui/go_main_menu`，复用 `ui_write_command`/`ui_write_result` 与既有 handler id `0x55490001`，由 kind 区分）；`routes.inc` owner 改 `core.ui_state`。
- **Kotlin**：`SaveActions.mainMenu` 改为 `NativeHttpCommand.post("/api/ui/go_main_menu", "{}")` 透传（import 已在）。

## 4. 验证
| 项 | 结果 |
|---|---|
| Host CTest（`.tmp/host-b4-build`） | **66/66 通过** |
| `git diff --check` / `check_arch` / `check_http_routes` | 通过（`api-reference`/native 双向 0） |
| 构建 | 普通 `output/inotia4_qol_lsposed_debug_2609241436_f0c5fd0d2e40.apk`；kernel-only `..._2609241437_29665e85a97a.apk`；LSPatch 3/3 |
| 真机·owner | `/api/ui/go_main_menu` owner=`core.ui_state`；`api.ui` 计数 **0**、`core.ui_state` 计数 **9**（143 不变） |
| 真机·票据与保真 | 世界态 `POST` → 200 `{"ok":true,"ticket":11}` → 轮询 `state:"done"`、`result={"ok":true,"state":{"money":28550,"map_id":20,"x":328,"y":136,"active_quest":0,"leader_slot":0,"party_count":3}}`（与迁移前 Player 模型同形）、`result_retained:true`；`snapshot` 随后 `screen:"main_menu"`（**真实切回主菜单**） |
| 真机·墓碑 | `core.ui_state` deactivate → `POST` 返回 **503** `{"ok":false,"error":"http owner inactive"}`；activate → 200 恢复 |
| smoke | `total=143 pass=121 warn=22 fail=0`，与更新后的基线 verdict/status **diff=0** |
| logcat | FATAL/SIGSEGV/SIGABRT = **0** |

## 5. 四元组
| 触及文件 | R/B 编号 | VM 卡 | 原因 |
|---|---|---|---|
| `api/native/game_ui_operations.inc`、`api/native/game_ui.h`、`contracts/game_ports/ui_write_port.h`、`game_system.cpp`、`plugins/core.ui_state/plugin.cpp`、`plugins/core.api/routes.inc`、`SaveActions.kt`、`docs/reference/api-reference.md`、`scripts/verification/smoke_baseline_v0.7.13.json` | 无扩展背包 R-xx/B-xx | `VM-K5-08g` | 全部已执行：host/静态/双构建/LSPatch/真机（owner、票据保真、真实切屏、墓碑）/smoke/logcat |

## 6. 备注
- 「逻辑帧内联 vs 跨帧同步」的取舍已写入 router 文档：owner 命令禁用同步 `transition_run`（自阻塞），这是 enter_slot（延迟结算）/teleport（内联）之后同一族问题的第三次应用。
- 陈旧文档缺陷（`open_panel`/`close_panel` 仍写同步响应）在本批一并修正。
