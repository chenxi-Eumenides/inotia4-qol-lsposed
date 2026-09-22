# VM-K5-04b：阶段 5 批 4-1——`world_facts` HTTP owner 迁移

## 前任改动盘点

工作区原有批 4-1 的未提交实现，已包含 world Adapter Port 扩展、`world_facts` 七条 query handler、插件描述符尾部 handler 声明、生命周期接线、Kotlin `nativeHttpDispatch` 调用链和 Host dispatch 测试。

本轮保留上述迁移面，并修正两处可验证缺口：

- `world_facts` 在自身目录 `plugins/world_facts/snapshot.h` 提供领域快照 seqlock；插件不能 include `infra/` 私有实现（`check_arch.py` 的 `plugins/<id>` 依赖规则），并在 install/enable/disable 时清空快照，避免生命周期恢复前读取旧帧。
- HTTP handler owner 注销不再删除 `core.api` 的路由元数据；停用保留墓碑返回明确错误，恢复时可重新绑定 handler。新增 host 生命周期恢复断言。

`docs/history/` 下既有遗留修改与无关文档未作为本批实现修改。

## 七条路由与迁移方式

| 路由 | 迁移方式 |
|---|---|
| `GET /api/world/map/id` | `world_facts` 主线程帧快照读取 map id，并在 handler 内用 world port 地图名注入 `id_name` |
| `GET /api/world/map/exits` | world port 在主线程读取当前 map id，使用启动期下发的静态 exits 表生成快照 |
| `GET /api/world/map/units` | world port 主线程构造 units JSON，handler 只读快照 |
| `GET /api/world/map/enemies` | world port 主线程构造 enemies JSON，handler 只读快照 |
| `GET /api/world/map/interactives` | world port 主线程构造 interactives JSON，handler 只读快照 |
| `GET /api/world/map/drops` | world port 主线程构造 drops JSON，handler 只读快照 |
| `GET /api/world/map/distance` | handler 解析 `tx/ty`，对快照中的导航矩阵执行 BFS；不读游戏内存 |

Kotlin `CurrentMapController` 保留参数解析，`InfoMapQueries` 统一通过既有 `NativeBridge.nativeHttpDispatch`；路径、方法、成功响应和 JNI 签名未改。

## 线程模型与地图名

HTTP worker 线程不直接调用游戏访问器。`world_facts` 在逻辑帧主线程每 15 帧发布一次 `plugins/world_facts/snapshot.h` 的 `Snapshot<WorldSnapshot>`；handler 只执行 seqlock 读取，未发布或竞争失败返回 `503` dispatch 错误体。world port 的 units、enemies、interactives、drops、navigation 和 map id/name 读取均发生在该帧回调中。

地图名 native 可达：world port 的 `map_name` 读取器在游戏主线程返回名称，handler 将其复制进快照并输出 `id_name`。静态 exits 表由 `StaticData.buildMapExitsJson()` 在启动期通过新增 JNI 注入 native，快照按当前 map id 选取；未改变响应字段。

## 验证结果

- `git diff --check`：通过（`diff-check-final2.log` 无输出）。
- Host CTest：`100% tests passed out of 60`，含 `http_dispatch_tests`、`infra_snapshot_tests` 和新增 `world_facts_dispatch_tests`（`host-ctest-final2.log`）。
- `uv run python scripts/maintenance/check_arch.py`：全部通过，无新增违规；`plugins/world_facts` 仅依赖 `contracts/` 与自身目录（`check-arch-final2.log`）。
- `uv run python scripts/maintenance/check_http_routes.py`：143/143，重复 0，遗漏 0，`world_facts=7`（`check-http-routes-final2.log`）。
- `uv run python scripts/maintenance/check_jni.py`：146/146，违规 0（`check-jni-final2.log`）。
- `scripts/build-debug.sh`：通过，普通变体 APK `output/inotia4_qol_lsposed_debug_2609230057_f32c3f5e0644.apk`；`strings` 无 `kernel-only mode`。
- `scripts/build-debug.sh -PqolKernelOnly=true`：通过，kernel-only APK `output/inotia4_qol_lsposed_debug_2609230058_5374bb624265.apk`；`strings` 有 `kernel-only mode`、`world_facts` 为 0。
- `scripts/patch-apk.sh output/inotia4_qol_lsposed_debug_2609230057_f32c3f5e0644.apk`：LSPatch 集成 `成功 3 / 失败 0`；设备安装 monster v29 集成包（`patch-final2.log`、`install-monster-final2.log`）。
- 真机 `192.168.3.54:5555`：重启后 health `ok=true, ready=true, variant=monster/v29`；同意页 `dialog/select {"action":"ok"}` → `screen=main_menu`，`enter_slot` → `screen=world`；routes `registered=143, expected=143, duplicates=0, omitted=0, accepting=true`，七条 owner 均为 `world_facts`（`routes-final2.json`）。
- 七条真机响应：全部 HTTP 200。`id`、`enemies`、`interactives`、`drops`、`distance` 与 HEAD 基线逐字节相等；`exits`、`units` 与基线前 400 字节相同，基线 body 本身以 `...` 截断。证据 `world-compare-final2.json` 与 `final/`。
- lifecycle：`world_facts` deactivate 返回 ticket 7，ticket `done` 后 feature `inactive`；七条均返回 HTTP 200 `{"ok":false,"error":"http owner inactive"}`；activate 返回 ticket 8，feature 恢复 `enabled`，七条响应与停用前逐字节相同。
- 全量 smoke：`total=143, pass=120, warn=23, fail=0`。与 HEAD 基线比较：路由集合相同，verdict/status 无任何差异，summary 相同；仅 11 条含运行时/时间字段的 body 文本不同（`smoke-final2.json`、`smoke-final2.log`）。
- 清空 logcat 后采集：`FATAL=0`、`SIGSEGV=0`、`UnsatisfiedLinkError=0`（`logcat-final2.txt`）。

## R-xx / B-xx 与 VM 四元组

本批无新增或改变的 R-xx/B-xx 编号；受影响验收卡为 `VM-K5-04b`。

| 触及文件 | 影响编号 | 对应 VM 卡 | 已执行/无法执行原因 |
|---|---|---|---|
| `contracts/game_ports/ports.h`、`contracts/game_ports/world_port.h`、`game/access/world.h`、`game/adapter_ports/world_port.*` | 无 | VM-K5-04b | Host CTest、普通 Debug、真机七条查询均已执行；kernel-only 构建确认不含 world owner |
| `plugins/world_facts/plugin.cpp`、`plugins/world_facts/snapshot.h`、`plugins/core.api/routes.inc`、`contracts/http.h`、`contracts/plugin.h` | 无 | VM-K5-04b | handler 注册、快照未就绪、七条响应、停用错误体、恢复重绑均已执行 |
| `host/http_dispatch.*`、`host/plugin_registry.cpp` | 无 | VM-K5-04b | 原 dispatch 覆盖随全量 CTest 通过；新增 owner 注销/恢复测试已执行 |
| `bridge/native/gamebridge_operations.cpp`、`NativeBridge.kt`、`ApiServer.kt`、`StaticData.kt`、`InfoMapQueries.kt`、`CurrentMapController.kt` | 无 | VM-K5-04b | JNI 检查、Android 普通/kernel-only 构建、真机 API smoke 已执行 |
| `tests/CMakeLists.txt`、`tests/test_game_host_stubs.cpp`、`tests/test_world_facts_plugin.cpp`、`tests/test_world_facts_dispatch.cpp` | 无 | VM-K5-04b | 全量 host CTest `60/60` 通过 |

## 文件说明

- 权威证据为带 `-final2` 后缀的文件与 `final/` 目录，对应当前交付代码（`plugins/world_facts/snapshot.h`）构建的普通/kernel-only APK。
- 不带后缀的 `world-*.json`、`smoke.json`、`routes.json`、`features-*.json`、`logcat-clean.txt` 为本次实现修正前的首轮取证，行为结论相同，但其构建仍含被后续修正的 `infra/snapshot.h` 越层依赖；仅作过程留痕。

## 编排方复验记录

fixer 会话中断后，编排方以同一构建产物独立复验，结论与 `-final2` 证据一致：

- 重建普通 Debug APK（`25ee7891887e`）与 kernel-only APK（`3262a6f945df`）均成功；kernel-only `libgamebridge.so` 无 `world_facts`/`world.facts` 符号（0），普通包 15 处。
- 真机重走：health `ready=true`（monster v29）→ `dialog/select {"action":"ok"}` → `enter_slot slot 0` → world 态；7 条路由全通，`map-id.json` 含 `id_name`（黑暗教团礼堂），`routes.json` 中 7 条 owner=`world_facts`；lifecycle deactivate → `{"ok":false,"error":"http owner inactive"}`、activate → 恢复；smoke `143/143 pass=124 warn=19 fail=0`。
- smoke 相对基线 4 条 warn→pass（`/api/character/party/{include,exclude,discharge,withdraw}` 405→400 参数校验）：为真机 op 门禁开启状态差异所致（lifecycle 验证需 `opEnabled=true`），与本批 world 查询改动无关。
- 游戏进程（pid 级过滤）FATAL/SIGSEGV/UnsatisfiedLinkError 均为 0；logcat 中 2 条 SIGABRT 属其他系统进程。

### 首轮 SIGSEGV 定性（非本批缺陷）

复验前一轮曾发生一次 SIGSEGV：操作路径为 tap「开始游戏」进入存档选择画面后调用 `enter_slot` API。tombstone（tombstone_19）崩溃栈全部位于原版 `libgame.so`（`Scene_Process/Draw_POPUP_SC_SAVESLOT → GAMELOADER_DrawBackGround → GRPX_DrawPart`，fault addr 0x8），无任何 `libgamebridge` 帧；判定为 SAVESLOT 弹窗显示期间 API 进档打断原版 popup 的固有竞态。规避路径（`dialog/select ok` 直接进档，即既有验收口径）重复验证稳定。真机验收流程必须沿用 dialog-ok 进档，不得在 SAVESLOT 画面停留时调 `enter_slot`。

## 遗留风险

- `scripts/verification/smoke_baseline_v0.7.13.json` 在当前工作区已被此前一次 smoke 运行覆盖，改动仅限 `meta.timestamp` 与各路由 `elapsed`/`body` 等易变字段；HEAD 版本与本次 smoke 的 summary、verdict、status 完全一致。是否恢复该文件由编排方决定，本批不擅自丢弃。
- smoke 基线中的长 body 是生成器截断文本，因此 exits/units 只能证明截断前字段一致；完整真机响应已归档。
- 本目录仅归档证据；本批文档和源码由编排方审查后提交。
