# 阶段 5 批 5-4：save 三条写路由迁 save_backup owner（VM-K5-06d）

日期：2026-09-23 ｜ 前置：用户裁决「补内核能力，但先用更窄修复」+ 三项边界；窄修复暴露响应保真问题后，按用户预批准的「补内核能力」方向实现**有界延迟结算**。

## 1. 关键发现（决定实现形态）

- 窄修复（逻辑帧内联执行转换）解决了 `transition timeout`，但票据 `result.state` 为 `{"error":"not in game"}`。
- 核对 `transition_dispatch.cpp`：`transition_run` 的「完成」**只是 fn 执行完一帧**，并不等转场管线。
- 迁移前能取到数值型 Player 态的真正原因：Kotlin 链路 `nativeOpEnterSlot` → **`ModuleSaveStore.ensureSlot`（模块容器文件 I/O）** → `attachPlayer`，正是这段阻塞 I/O 期间游戏推进到 in-world 态。
- 迁移前证据（落盘）：`docs/history/stage5-b3-20260922/enter-slot.json`、`docs/history/stage5-b4-1-20260922/{world-enable-enter,enter-final2}.json` 三份均为
  `{"ok":true,"state":{"money":0,"map_id":4294967295,"x":-1,"y":-1,"active_quest":0,"leader_slot":0,"party_count":0}}`。
- 结论：保真目标 = **结算时世界态已提交**；用确定性条件（世界态）替代偶然的 I/O 时序。

## 2. 实现

### 内核能力：有界延迟结算
- `infra/command_queue.{h,cpp}`：`Command.defer_limit_frames`、`request_defer_current_command()`、
  `current_command_attempt()`（线程局部 execution 上下文）；`drain` 收到延迟请求时**保持票据 pending、
  下一帧重试**（重试窗口内回到可取消态），超上限以 `{"ok":false,"error":"settlement timeout"}` 显式失败。
- `contracts/commands.h`（尾部，`kHostSdkMinor` 8→9）：`QolTicketResultApi` 增
  `defer_current_result` 与 `current_attempt`（旧字段与 handler 签名不变）。
- `infra/commands.{h,cpp}` 与 `host/host_sdk.cpp`：透出并装配；typed 结果处理器默认 `kDefaultDeferFrames=120`。

### save 写面两阶段结算
- `contracts/game_ports/save_write_port.h`：写函数增 `attempt` 参数；就绪前返回 `kQolErrNotReady`。
- `game/state/save_http.h` + `game_system.cpp`：`build_save_write_result_json` 返回
  `kSaveWriteOk/kSaveWriteFailed/kSaveWriteNotReady`；`attempt==0` 发起游戏侧操作（`save` / `enter_slot` / `create_slot`
  的内联转换体），`enter/create` 在世界态提交前返回 NotReady，就绪后执行 sidecar 槽位生命周期
  （`kUnavailable` 视为不适用，其余非 `kOk` 显式失败为 `sidecar: <status>`）并以
  `{"ok":true,"state":<Player>}` 结算。
- `plugins/save_backup/plugin.cpp`：结果处理器透传 `current_attempt()`；收到 `kQolErrNotReady` 时请求延迟。
- `routes.inc`：3 条 owner `api.save` → `save_backup`（143 不变；`api.save` 归零）。
- Kotlin：`SaveActions` 重写为「平台门禁 + `NativeHttpCommand.post`」；删除已下沉的
  `ModuleSaveStore` 副作用与死代码 `ActionSupport.afterSave/afterNativeSuccess/currentSaveSlot`。

## 3. 验证

| 项 | 结果 |
|---|---|
| Host CTest | 65/65（新增延迟结算覆盖；quest 两测试同步 port 1.1 / 尾部结果面 / 第 5 条路由） |
| `check_arch` / `check_http_routes` | 全部通过；143/143（owner `save_backup=7`、`api.save=0`） |
| 构建 | 普通 `output/inotia4_qol_lsposed_debug_2609232012_686d41dc25ff.apk`；LSPatch 3/3 |
| 真机 `enter_slot` | 200 `{"ok":true,"ticket":7}` → 约 200ms 后终态 `state:"done"`、`result` **与三份迁移前证据逐字一致**、`result_retained:true`；随后快照 `screen:"world"` |
| 真机 `create_slot` | 200 票据 → 终态 `done`、`result:{"ok":true,"state":{...x:2048,y:2048,party_count:1}}`（同构，无独立迁移前样本可对照） |
| 延迟语义 | 结算发生在世界态提交之后（非提前 done）；票据在结算前保持 pending |
| API smoke | `total=143 pass=121 warn=22 fail=0`，与基线 verdict/status **diff=0**；`/api/system/save` 现返回票据（基线体已按契约更新） |
| logcat | FATAL/SIGSEGV/SIGABRT = 0 |

## 4. 副作用（验收真实操作）
- `create_slot` 验收覆盖了**槽 2 原存档**（槽 2 原为 hero_level 1）。
- 早期批 5-3 验收真实放弃了存档任务 180。

## 5. 四元组

| 触及文件 | R/B 编号 | VM 卡 | 原因 |
|---|---|---|---|
| `infra/command_queue.{h,cpp}`、`infra/commands.{h,cpp}`、`contracts/commands.h`、`contracts/host_sdk.h`、`host/host_sdk.cpp`、`contracts/game_ports/{ports.h,save_write_port.h}`、`game/adapter_ports/save_write_port.{h,cpp}`、`game/state/save_http.h`、`game_system.cpp`、`api/native/game_save.{h,cpp}`、`infra/frame/transition_dispatch.{h,cpp}`、`host/plugin_registry.cpp`、`plugins/core.api/routes.inc`、`plugins/save_backup/plugin.cpp`、`SaveActions.kt`、`ActionSupport.kt`、tests、`CMakeLists.txt`、docs | 无扩展背包 R-xx/B-xx | `VM-K5-06d` | 全部已执行：host/静态/构建/LSPatch/真机（保真逐字、延迟结算、sidecar、smoke 零差异、logcat） |

## 6. 遗留
- 帧暂停时 pending、逻辑帧执行顺序的直接证据：设备侧受限于同意页不可复现；由构造（drain 仅在逻辑帧）与延迟结算实测行为支撑，后续可在 party/character 批次一并补。
- 深度行为卡（VM-43/46/P4-07/D2）与 EB 成功路径仍待人工配合。
