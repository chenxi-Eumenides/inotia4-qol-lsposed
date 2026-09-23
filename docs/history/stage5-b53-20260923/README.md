# 阶段 5 批 5-3：quit_quest 迁移 + 通配遮蔽修复（VM-K5-06c）

日期：2026-09-23 ｜ 范围：`POST /api/quest/quit_quest` 迁 owner `quest` 主线程命令 + 票据；顺带修复该路由不可达的既有传输缺陷。

## 1. 实现

- **契约**：`QolQuestWriteApi` 追加 `quit_result(quest_id, out, cap, size)`；`QolPortQuestVersion` 1.0→1.1；插件 port 需求同步 1.1。
- **拼装接缝**：新增 `game/state/quest_http.h` 声明 `build_quest_quit_result_json`；实现在 legacy 聚合 TU
  `game_system.cpp`（调 `data_op_quest_quit` + `build_player_json`），复刻迁移前 Kotlin
  `ActionSupport.attachPlayer(nativeOpQuestQuit)`：成功 → `{"ok":true,"state":<player>}`，失败 → 原错误体。
- **适配层**：`game/adapter_ports/quest_port.cpp` 接线 `write_quest_quit_result`。
- **插件**：`plugins/quest/plugin.cpp` descriptor 的 http 数组追加 POST 条目（声明式，宿主托管墓碑）；
  HTTP 线程 handler 只校验 + `post_typed` + 返回票据；结果处理器在主线程经 port 执行并回写结果；
  install 注册/`deactivate` 显式注销结果处理器；`routes.inc` 的 quit_quest owner 由 `api.quest` 改为 `quest`。
- **Kotlin**：quit 映射与校验移入 `QuestController`（见下），删除 `QuestActionController.kt` 与
  `QuestActions.questQuit`；`ActionApiService`/`ApiService` 相关声明删除。

## 2. 顺带修复的既有传输缺陷（重要）

`POST /api/quest/quit_quest` 此前**恒返回 405**（基线亦记录 405，即该文档化路由此前不可达）。
根因：AndServer 中同层 GET 通配 `/api/quest/{id}` 与精确 POST 路由分属**不同 adapter** 时，
通配把精确子路径吞掉（GET 命中通配→`invalid parameter`；POST 因通配仅 GET→405）。

修复：把 POST 映射移入持有通配的同一控制器（`QuestController`）并声明在通配之前。
验证：`POST {}` → 400 `questId required`（可达）；`GET /api/quest/5` → `not found`（通配仍正常）。

注：同形态的 `/api/character/party/{slot}` 与 `POST /api/character/party/{include,exclude,discharge,withdraw}`
在多次 smoke 中呈现 400/405 摇摆，属同一系统性缺陷，**未在本批处理**（需单独裁决，见 §5）。

## 3. 验证

| 项 | 结果 |
|---|---|
| Host CTest | 65/65 |
| `check_arch` / `check_http_routes` | 通过（143/143） |
| 构建 | 普通 `2609231718_e33c57fdf11c`；kernel-only 不受影响（quest 属业务插件） |
| LSPatch | 3/3 |
| 真机·成功路径 | `quit_quest{quest_id:180}` → 200 `{"ok":true,"ticket":7}`；票据 → `state:"done"`、`result:{"ok":true,"state":{"money":28550,"map_id":20,"x":328,"y":136,"active_quest":0,"leader_slot":0,"party_count":3}}`、`result_retained:true`；`/api/quest/active` 确认该任务已移除 |
| 真机·失败路径 | `{quest_id:9999}` → 票据 `state:"failed"`、`result:{"ok":false,"error":"quest not found"}`、`result_retained:true` |
| 真机·校验前置 | `{}` / `{quest_id:-1}` → 400 `questId required`（未产生票据） |
| 真机·生命周期 | quest deactivate → quit_quest 503 `http owner inactive`；activate → 200 票据 |
| API smoke | `total=143 pass=121 warn=22 fail=0`；与基线 diff 仅 quit_quest 一条（405→400，即本批刻意修复的路由可达性）；基线该条已按新行为更新 |
| logcat | FATAL/SIGSEGV=0 |

**副作用说明**：成功路径实际放弃了存档中的任务 180（验收必须真实执行写操作）；测试设备存档状态变化已记录。

## 4. 四元组

| 触及文件 | R/B 编号 | VM 卡 | 原因 |
|---|---|---|---|
| `contracts/game_ports/{quest_port.h,ports.h}`、`game/state/quest_http.h`、`game/adapter_ports/quest_port.cpp`、`game_system.cpp`、`plugins/quest/plugin.cpp`、`plugins/core.api/routes.inc`、Kotlin `QuestController/QuestActionController(删)/QuestActions(删)/ActionApiService/ActionApiServiceCore/ApiService`、`docs/reference/api-reference.md`、smoke 基线 | 无扩展背包 R-xx/B-xx | `VM-K5-06c` | 全部已执行：host/静态/构建/LSPatch/真机（成功·失败·校验·生命周期）/smoke/logcat |

## 5. 遗留（需裁决）

- **通配遮蔽（系统性）**：`/api/character/party/{slot}` 遮蔽同层 POST `include/exclude/discharge/withdraw`（smoke 呈 400/405 摇摆）。
  建议同法处理（把精确 POST 移入持有通配的控制器并前置声明），但涉及 party 写路由（批 5-7/5-8 范围），可在该批一并修复。
- 后续批次：save 3 / world 5 / ui 5 / item 13 / character 20。
