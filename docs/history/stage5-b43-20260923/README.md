# 阶段 5 批 4-3：system 4 条查询 owner dispatch（system_facts）

日期：2026-09-23 ｜ 对应卡：VM-K5-04d ｜ 实现主体来自失败会话的落地工作，由编排方审计、补验并收尾。

## 1. 范围与 owner

| 路由 | owner（迁移后） |
|---|---|
| GET `/api/system/snapshot`、`/game`、`/info`、`/game_frame` | `system_facts` |
| GET `/api/system/events` | 保持 `api.system`（Kotlin 直通 nativeGetEventsJson，无 enrichment） |
| GET `/api/ui`、`/screen`、`/panel`、`/dialog` | 保持 `api.ui`（依赖 UiActivityTracker 反射平台面与 agreement 覆盖，推送是行为变更，留阶段 6 评估） |

`routes.inc` 4 条 owner 字段更新；143 总数不变。

## 2. 实现要点

- **owner 插件** `plugins/system_facts/`（id `system_facts` v0.1.0，requires http.registry + http.dispatch，port QOL_PORT_SYSTEM v1.0）：15 帧周期主线程帧任务发布 seqlock 快照（snapshot 32KB / game 32KB / info 8KB / frame 64B），HTTP handler 只读快照，未就绪 503 `snapshot not ready`。
- **system_port**（contracts/game_ports/system_port.h + game/adapter_ports/system_port.{h,cpp}）：`body_json(kind)` 按类分发到 game_system.cpp 的 `build_system_*_json`；模块身份（version/package）启动期由 Kotlin 经新 JNI `nativeSetSystemIdentity` 下发（`system_port_set_identity`），未就绪时 info/game 返回 `system identity not ready`（快照/帧计数不受影响）。
- **响应冻结对照**（与 HEAD Kotlin 逐项核对）：
  - `/api/system/snapshot` = 原生快照 + NameInjector 注入复刻：party 成员按 restructureRole 序（name/type_name/class_name/level/hp/max_hp/mp/max_mp/type/class_idx/main_stats），main_stats 5 项（力量/敏捷/体力/智力/精力，base_stat=fn_get_stat_base、additional=total-base），class_name 中文表 6 项，type_name 主角/佣兵。
  - `/api/system/game` = `{"snapshot":<原生未注入>,"info":<info>}`（与 Kotlin game() 同构：game 用原生快照，注入只用于 snapshot 端点）。
  - `/api/system/info` = `{version, save_slots, current_save_slot, package_name, base}`，save_slots 从 data_save_slots_json() 提取数组。
  - `/api/system/game_frame` = `{"frame":N}`（批 4-0 试点口径；旧 bridge builtin handler 已删除）。
- **Kotlin 收窄**：`InfoApiServiceCore` 的 game/gameSnapshot/gameFrame/gameInfo 改 `InfoSystemQueries` 四方法 `NativeHttpQuery.get` 透传；`ApiServer` 启动期下发身份（失败仅记日志，对应端点 503）。

## 3. 静态验证

| 命令 | 结果 |
|---|---|
| `git diff --check` | 通过 |
| Host CTest（.tmp/stage5-b43/host） | `100% tests passed out of 63`（新增 system_facts_dispatch_tests） |
| `check_http_routes.py` | 143/143、三表一致、owner `system_facts=4` |
| `check_jni.py` + `gen_baseline_doc.py` | 149/149、违规 0（新增 nativeSetSystemIdentity） |
| `check_arch.py` | 全部通过、无新增基线 |
| `build-debug.sh` / `-PqolKernelOnly=true` | `2609231140_ff015d0a2431` / `2609231141_fcb8414cf34e` |
| kernel-only strings | `system_facts`/`/api/system/snapshot` 命中 0；普通构建命中 8 |

## 4. 真机验收（monster v29 集成包）

health `ready=true` → dialog select ok → 二次 enter_slot（首次 state=2 被拒，重发成功）→ `world`。

| 检查点 | 结果 |
|---|---|
| routes owner | system 4 条 `system_facts`；events=api.system、ui 9 条=api.ui |
| snapshot 注入 | party[0] 含 `name:"凯恩"` 等（注入生效）；game 的 party 为原生字段序（未注入）——两者与 Kotlin 旧实现口径一致 |
| deactivate | 4 条全部 503 `{"ok":false,"error":"http owner inactive"}` |
| activate | 4 条恢复 200，帧号归一化后与停用前逐字节一致 |
| 全量 smoke | `total=143 pass=120 warn=23 fail=0`，与基线 verdict/status diff=0 |
| logcat | FATAL/SIGSEGV/UnsatisfiedLinkError = 0 |

## 5. 四元组

| 触及文件 | R/B 编号 | VM 卡 | 原因 |
|---|---|---|---|
| `plugins/system_facts/*`、`contracts/game_ports/system_port.h`、`contracts/game_ports/ports.h`、`game/adapter_ports/system_port.{h,cpp}`、`game_system.{h,cpp}`、`game/state/system_snapshot.h`、`bridge/native/{gamebridge_data,gamebridge_dispatch,gamebridge_internal,gamebridge_static_data}.*`、`host/{assembly.cpp,plugin_registry.cpp}`、`plugins/core.api/routes.inc`、Kotlin `ApiServer/NativeBridge/InfoApiServiceCore/InfoSystemQueries`、tests、CMake、docs | 本批无扩展背包 R-xx/B-xx | VM-K5-04d | 全部已执行：host/静态/双构建/strings/LSPatch/真机 lifecycle/smoke/logcat |

## 6. 备注

- 派单返回 error（模型配置错误）不代表会话未执行：本批实现主体由失败会话在报错前落地，编排方接手时先审计工作区再决定续作。
- game_frame 的旧 bridge builtin 注册已移除，system 域查询面由单 owner 统一承载。
