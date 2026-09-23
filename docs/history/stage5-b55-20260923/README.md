# 阶段 5 批 5-5：世界写 5 条迁 world_teleport owner（VM-K5-06e）

日期：2026-09-23

## 1. 范围
`POST` 世界写 5 条：`/api/world/movement/{move_to,walk_dir,stop_move,interact_with}` +
`/api/op/movement/teleport`。owner `api.world` → `world_teleport`（143 总数不变；`api.world` 归零）。

## 2. 实现
- **新端口** `QOL_PORT_WORLD_WRITE`（v1.0，`contracts/game_ports/world_write_port.h`）：`write(kind,a,b,c,d,out,cap,size)`；
  kind 取 `kQolWorldWrite{MoveTo,WalkDir,StopMove,Interact,Teleport}`。**私有**：仅 `world_teleport` 可注入（同 save 写端口约束）。
- **拼装**（`game/state/world_http.h` + legacy `game_system.cpp` 的 `build_world_write_result_json`）：
  `move_to/walk_dir/interact_with` 迁移前经 Kotlin `ActionSupport.attachPlayer` 追加 state，故成功时输出
  `{"ok":true,"state":<player>}`；`stop_move` 与 `teleport` 输出原始 op JSON；失败输出原错误体。
- **传送**：新增 `data_op_teleport_direct`（`api/native/game_world_operations.inc`）——与既有 `data_op_teleport`
  同校验与同转换体，仅把跨帧派发的 `transition_run` 换成 `transition_run_inline`（owner 命令排空本身就在
  `kFramePointLogicPre` 相位，不能再阻塞等待下一帧）；旧同步入口与 JNI 签名保留。传送结果是原始 op JSON、
  不读转场后的世界态，故无需延迟结算。
- **插件**（`plugins/world_teleport/plugin.cpp`）：descriptor 追加 5 条**声明式** POST（宿主按生命周期注册/置墓碑）
  + 结果处理器自注册（`kWorldWriteHandlerId=0x57540001`，全局 id 空间）+ `plugin_deactivate` 显式注销；
  `kPorts` 增 WORLD_WRITE、port_count 1→2。HTTP handler 只做 body 字段提取与范围校验：
  `x/y required`、`direction 0-3 required`、`map_id/x/y required`（文本与迁移前一致），并把
  **瓦片坐标换算为像素**（`tile*16+8`，与迁移前 Kotlin 同口径）后投递票据。
- **Kotlin**：`MovementController` 与 `OpController.teleport` 改为「`parseBody`（400 `bad request`）+ `NativeHttpCommand.post`」；
  删除 `MovementActions.kt` 及 `ActionApiService`/`ActionApiServiceCore`/`OpApiService` 的对应声明与实现。
  op 门禁由 dispatch 层按 `op_gated` 统一处理（403 信封同旧）。

## 3. 验证
| 项 | 结果 |
|---|---|
| Host CTest | 65/65（补 `test_game_port_stubs` 的 `world_write_port()` 桩） |
| `check_arch` / `check_http_routes` | 全部通过（143/143，exit=0） |
| 构建 | 普通 `output/inotia4_qol_lsposed_debug_2609232026_ee14d786e321.apk`；LSPatch 3/3；已安装 |
| 真机·校验前置 | `move_to {}` → 400 `x/y required`；`walk_dir {direction:9}` → 400 `direction 0-3 required`；`teleport {}` → 400 `map_id/x/y required` |
| 真机·票据与保真 | `stop_move` → ticket → `done` + `{"ok":true}`（原始 op JSON）；`interact_with` → `done` + `{"ok":true,"state":{"money":28550,"map_id":20,"x":328,"y":136,"active_quest":0,"leader_slot":0,"party_count":3}}`（**与 smoke 基线逐字一致**）；`walk_dir` 同构；`move_to` 业务失败时 `state:"failed"` + `result:{"ok":false,"error":"no path"}`（原错误体原样回取） |
| 真机·传送 | `teleport {map_id:0,x:20,y:8}` → ticket → `done` + `{"ok":true}`；快照确认落点生效（x:20,y:8） |
| 真机·生命周期 | `world_teleport` deactivate → 5 条全 503 `http owner inactive`；activate → 恢复（带合法 body 的 3 条 200） |
| API smoke | `total=143 pass=121 warn=22 fail=0`，与基线 verdict/status **diff=0**；FATAL/SIGSEGV/SIGABRT=0 |

## 4. 四元组
| 触及文件 | R/B 编号 | VM 卡 | 原因 |
|---|---|---|---|
| `contracts/game_ports/{world_write_port.h,ports.h}`、`game/state/world_http.h`、`game/adapter_ports/world_write_port.{h,cpp}`、`game_system.cpp`、`api/native/game_world.{h}/*.inc`、`host/plugin_registry.cpp`、`plugins/world_teleport/plugin.cpp`、`plugins/core.api/routes.inc`、`CMakeLists.txt`、Kotlin `MovementController/OpController/OpApiService/ActionApiService/ActionApiServiceCore`、`tests/test_game_port_stubs.cpp` | 无扩展背包 R-xx/B-xx | `VM-K5-06e` | 全部已执行：host/静态/构建/LSPatch/真机（校验、票据、保真、传送、生命周期）/smoke/logcat |

## 5. 副作用
`teleport` 验收为真实传送（同图重入至当前瓦片，落点生效）。

## 6. 遗留
`move_to` 成功路径（真实寻路移动）需在可通行的落点下验证，属深度行为卡范畴（待人工配合）。
