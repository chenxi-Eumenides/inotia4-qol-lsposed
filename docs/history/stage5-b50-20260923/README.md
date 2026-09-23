# 阶段 5 批 5-0：http.dispatch command 机制补全 + `/api/op/features/lifecycle` 试点迁移

日期：2026-09-23 ｜ 对应卡：VM-K5-05a ｜ 状态：待编排方真机验收

## 1. 范围

本批只做**机制补全 + 单条 POST 试点**，不做 64 条 POST 批量迁移：

1. `host/http_dispatch.cpp` 的 `register_command` 由 unsupported 占位改为真实实现（POST
   command handler 注册：模板匹配、owner 校验、墓碑语义、并发保护）；
2. `dispatch()` 对 POST 命中 command handler 的执行路径；
3. op 门控（`op_gated=1` 路由）在 dispatch 层对齐，拒绝体与 Kotlin 现 403 逐字节一致；
4. 试点迁移 `POST /api/op/features/lifecycle`（owner `core.api`）：handler 注册 + Kotlin 透传，
   成功/失败/op 拒绝三类响应与现网逐字节一致。

## 2. 侦察结论

### 2.1 现状：register_command 与 dispatch 路由分支

- `contracts/http.h`：`HttpQueryHandlerFn` 与 `HttpCommandHandlerFn` 签名相同（`QolResult(const HttpReq*, HttpRes*, void*)`）；
  `QolHttpDispatchApi` 已含 `register_command` 槽位。`HttpRoute` 含 `op_gated`/`lease_required`；
  `HttpReq` 已带 `body/body_len`，POST body 经 JNI 已可传递（`nativeHttpDispatch(method, path, query, body)`）。
- `host/http_dispatch.cpp`：`register_command` 现返回 `kQolErrUnsupported "command dispatch reserved"`；
  `HandlerSlot` 只存 `query`，dispatch 只调 `selected->query`。`register_query` 已实现模板校验、
  registry owner 比对、重复/墓碑处理与容量上限；`unregister_owner` 置 `unregister_pending` 并等待
  `inflight` 归零后把 `query/ctx` 置空（保留路由墓碑 → 请求得 503 `http owner inactive`）。
- `dispatch()` 现状：参数校验 → `host_http_registry_accepting()` → 加锁按 method+模板选槽 →
  墓碑 503 → `lease_enter` → handler → `lease_leave` → 失败兜底 500/无状态 500。
- **结论**：query 与 command 的契约差异仅为 method（GET vs POST）与语义（只读快照 vs 写命令）；响应形状统一由 `HttpRes{body, capacity, size, status}` 承载，无需异步票据化。command 机制应完全镜像 query。

### 2.2 试点路由现状链

`POST /api/op/features/lifecycle`（`routes.inc` L97：owner=`core.api`，op_gated=1，lease_required=1）：

```
AndServer → OpController.featureLifecycle(@RequestBody)
  → JsonUtil.parseBody（非法 → 400 "bad request"）
  → id/action 取空 → 400 "id and action required"
  → OpApiService.featureLifecycle
      → checkOpEnabled（ModuleConfig.opEnabled=false → ApiException 403 "op disabled"）
      → action 映射：deactivate→3 / activate→4 / 其他 → 200 {"ok":false,"error":"unknown action"}
      → LogFile.op(...) { NativeBridge.nativeFeatureLifecycle(id, code) }
          → bridge/native/gamebridge_lifecycle.cpp：id 截断进 64B 缓冲，
            构造 QolCommand{kCommandFeatureLifecycle, id, action} 经 infra::commands_port().post
          → ticket>0 → 200 {"ok":true,"ticket":N}
          → 否则 → 200 {"ok":false,"error":"lifecycle rejected"}
```

- native 落点：`infra/commands.cpp` 主线程 drain → 宿主 `lifecycle_apply`。
- op 门控位置：**service 入口**（controller 完成 body 解析之后）。
- `check_http_routes.py` 三表（Kotlin controller / api-reference / routes.inc）已含本路由，owner 已是真实 owner `core.api`，本批**不改 routes.inc**（owner 无需变更）。

### 2.3 op 门控语义（Kotlin）与 native 对齐点

- 22 条 `op_gated=1` 路由的 Kotlin 现有行为：`OpApiServiceImpl.checkOpEnabled()` 在**业务执行前**读
  `ModuleConfig.opEnabled`；未开启抛 `ApiException(StatusCode.SC_FORBIDDEN, "op disabled")`，
  经 `GlobalExceptionResolver` 输出 **HTTP 403 + `{"ok":false,"error":"op disabled"}`**。
- `ModuleConfig.opEnabled` 默认 `false`（安全基线），仅 Kotlin 侧持久化，此前未镜像到 native。
- 对齐方案：dispatch 在选中 op_gated 槽位后、进入 handler 前检查 native op 状态；未开启直接写
  **403 + `{"ok":false,"error":"op disabled"}`**（`write_error` 已产出同形状），返回 `kQolErrBlocked`。
- 状态来源：新增 host seam `http_dispatch_set_op_enabled(bool)`，由新增 JNI
  `nativeSetOpEnabled(bool)` 在 Kotlin 配置加载/变更时同步（默认 false，与 Kotlin 默认一致）。
  **选择独立 host seam 而非 `infra::config` 的原因**：op 门禁是 dispatch 层门禁，不是插件资源门禁；
  且 dispatch 被 10 个 host 测试目标直接编译，引入 `infra/config.cpp` 会扩散 9 个目标的链接面。
- 顺序差异（登记为风险）：Kotlin 在 body 解析后做 op 检查，故「op 关闭 + 非法 body」旧行为是 400；
  dispatch 层门控在 handler（含 body 解析）之前，该组合将先返回 403。三类冻结响应
  （成功 / 失败 / op 拒绝）在各自前置条件下逐字节一致。

### 2.4 owner 决策

`/api/op/features/lifecycle` owner 维持 `core.api`：本路由属插件生命周期控制域，core.api 同时是
HTTP provider，command handler 由 core.api 插件自注册（经 `ctx.sdk->http_dispatch.register_command`）。
未新增 PluginDescriptor 尾部 `http_commands`（批量迁移时再决定是否需要宿主驱动注册）。

## 3. 实现（本批）

### 3.1 机制：command 注册与分发（`host/http_dispatch.{h,cpp}`）

- `HandlerSlot` 新增 `op_gated`（注册时从路由表快照）与 `command` 槽；`register_handler` 为
  query/command 共用的注册核心（显式 `command_slot` 标志——两者 handler 函数指针类型相同，
  不能用类型区分），`register_query` 要求 kGet、`register_command` 要求 kPost。
- `register_command` 完整镜像 query：模板校验、registry owner 比对、重复冲突、墓碑复用
  （`unregister_pending` 同 owner 重注册）、容量上限、`g_dispatch_mutex` 并发保护。
- `dispatch`：GET 调 `query` 槽、POST 调 `command` 槽；`unregister_owner` 同时清空两槽并保留
  墓碑（停用态请求 503 `http owner inactive`）。
- op 门禁：新增 host seam `http_dispatch_set_op_enabled(bool)`；`dispatch` 选中 op_gated 槽后、
  进入 handler 前读取该状态，未开启即写 **403 + `{"ok":false,"error":"op disabled"}`** 并返回
  `kQolErrBlocked`。默认关闭，与 `ModuleConfig.DEFAULT_OP_ENABLED=false` 一致。

### 3.2 JNI / Kotlin

- `bridge/native/gamebridge_dispatch.cpp`：新增 JNI `nativeSetOpEnabled(bool)` → 调用 host seam。
  既有 `nativeHttpDispatch(method, path, query, body)` 已带 body 参数，POST 传输无需扩参。
- `NativeBridge.kt`：新增 `external fun nativeSetOpEnabled`。
- `ConfigApiService`：`applyToNative`/`applyOnChange` 下发 `ModuleConfig.opEnabled`（`applyOnChange`
  不改既有签名，幂等下发当前值）。
- 新增 `service/info/NativeHttpCommand.kt`：POST 透传 helper（解 `{"status","body"}` 信封，
  ≥400 抛 `ApiException`，与 `NativeHttpQuery.get` 同构）。

### 3.3 试点：`/api/op/features/lifecycle`（owner `core.api`）

- `plugins/core.api/plugin.cpp`：新增 `lifecycle_command`（owner handler），`plugin_install` 在路由
  注册后经 `ctx.sdk->http_dispatch.register_command` 注册，`plugin_deactivate` 先撤 handler 再冻结路由。
- 新增 `plugins/core.api/json_field.{h,cpp}`：插件自持的 JSON 对象字符串字段提取（含转义、嵌套值跳过、
  重复键后者覆盖），对齐 Kotlin `JsonUtil.parseBody` 失败语义。
- handler 语义：body 非法 → 400 `bad request`；id/action 缺失或空 → 400 `id and action required`；
  未知动作 → 200 `{"ok":false,"error":"unknown action"}`；deactivate→3 / activate→4 经
  `sdk.commands.post` 投递 `kCommandFeatureLifecycle`，ticket>0 → 200 `{"ok":true,"ticket":N}`，
  否则 → 200 `{"ok":false,"error":"lifecycle rejected"}`。
- `OpController.featureLifecycle` 改为 `ControllerGuard.guard { NativeHttpCommand.post(...) }`；
  `OpApiService.featureLifecycle` 接口/实现删除（无其他调用方）。

## 4. 静态验证

| 命令 | 结果 |
|---|---|
| `git diff --check` | 通过（无输出） |
| Host CTest（`.tmp/stage5-b50-20260923/host`） | `100% tests passed out of 65`（新增 `core_api_lifecycle_tests` 16 项；`http_dispatch_tests` 52 项含 command 机制与 op 门控） |
| `uv run python scripts/maintenance/check_http_routes.py` | 143/143、三表一致、owner 分布不变（`core.api=6`）、duplicates=0 |
| `uv run python scripts/maintenance/check_jni.py` | 150/150、违规 0（新增 1 个 JNI） |
| `uv run python scripts/maintenance/check_arch.py` | 全部通过（layers/offsets/jni/resources） |
| `uv run python scripts/maintenance/gen_baseline_doc.py` | `plugins/core.api` 3→5、合计 483→485、JNI 149→150，已更新 `docs/refactor/baseline.md` |
| `scripts/build-debug.sh` | `BUILD SUCCESSFUL`；`output/inotia4_qol_lsposed_debug_2609231225_e6876fa8cd8d.apk` |
| `scripts/build-debug.sh -PqolKernelOnly=true` | `BUILD SUCCESSFUL`；`output/inotia4_qol_lsposed_debug_2609231225_b4326fc8c91d.apk` |
| kernel-only `.so` strings | `world_facts`=0、`/api/op/features/lifecycle`=0；普通构建 `world_facts`=138、`/api/op/features/lifecycle`=1（`nativeSetOpEnabled`/`op disabled` 两变体均在，属 host dispatch 内核面） |

真机列为待编排方执行项（见 §5）。

## 5. 真机验收步骤（留给编排方）

1. `scripts/patch-apk.sh` 生成 monster v29 集成包并安装（`adb -s <序列号> install -r`）、force-stop + monkey 重启。
2. 轮询 `/api/health` 至 `ready=true`；出现同意页 `POST /api/ui/dialog/select {"id":"ok"}`；
   `POST /api/system/enter_slot {"slot":0}` 进档（首次可能 `state=2` 被拒，重发一次）至 `screen=world`。
3. `GET /api/system/routes` 核对总数 143/143、`duplicates=0`、`omitted=0`，`core.api` owner 仍为 6
   （本批不改 owner）。
4. **开关两态 op 门禁**：
   - `POST /api/config/set {"opEnabled":false}` →
     `POST /api/op/features/lifecycle {"id":"simple_mode","action":"deactivate"}` 期望
     **HTTP 403 + `{"ok":false,"error":"op disabled"}`**（与迁移前逐字节一致）。
   - `POST /api/config/set {"opEnabled":true}` → 同一请求期望 **HTTP 200 + `{"ok":true,"ticket":N}`**；
     `GET /api/system/tickets/N` 确认 `done`，`GET /api/system/features` 确认目标插件状态迁移。
5. **失败/未知动作形状**（opEnabled=true）：`{"id":"x","action":"bogus"}` → 200
   `{"ok":false,"error":"unknown action"}`；`{}` → 400 `{"ok":false,"error":"id and action required"}`；
   非 JSON body → 400 `{"ok":false,"error":"bad request"}`。
6. `activate` 回滚：`{"id":"simple_mode","action":"activate"}` → ticket 响应、features 恢复 `enabled`。
7. 全量 smoke（`total=143`，与基线 verdict/status diff=0）+ clean logcat（FATAL/SIGSEGV/UnsatisfiedLinkError=0）。

## 6. 四元组

| 触及文件 | 影响的 R-xx/B-xx 编号 | 对应 VM-B/受影响 VM 卡 | 已执行/无法执行原因 |
|---|---|---|---|
| `host/http_dispatch.{h,cpp}`、`bridge/native/gamebridge_dispatch.cpp`、`plugins/core.api/{plugin.h,plugin.cpp,json_field.h,json_field.cpp,CMakeLists.txt}`、`tests/{test_http_dispatch.cpp,test_core_api_lifecycle.cpp,CMakeLists.txt}`、Kotlin `NativeBridge.kt`/`ConfigApiService.kt`/`OpController.kt`/`OpApiService.kt`/`service/info/NativeHttpCommand.kt`、`docs/refactor/{baseline.md,api-route-map.md,verification.md,ledger.md}` | 本批无扩展背包 R-xx/B-xx | `VM-K5-05a`（真机列待编排方执行） | 静态验证全部已执行（见 §4）；真机回归按 §5 由编排方执行 |

## 7. 遗留风险与后续切片建议

- **op 门禁顺序差异**：Kotlin 在 body 解析后做 op 检查，「op 关闭 + 非法 body」旧行为为 400；
  dispatch 层门禁在 handler（含 body 解析）之前，该组合将先返回 403。三类冻结响应
  （成功/失败/op 拒绝）在各自前置条件下逐字节一致；如验收要求严格顺序，可把 op 检查下移到
  handler 内（牺牲机制集中性）。
- **非字符串字段**：native 解析把 `{"id":123}` 视为字段缺失（400），Kotlin `optString` 会
  stringify 成 `"123"`；契约 body 恒为字符串，该差异登记为已知边界。
- **Kotlin 侧日志**：迁移前 `OpApiService.featureLifecycle` 经 `LogFile.op` 记录 id/action；迁移后
  该记录消失（响应冻结不受影响），如需保留可在 owner handler 内经 `sdk.log` 补记。
- **host 与 infra 状态**：op 门禁用独立 host seam 而非 `infra::config`（避免 10 个 host 测试目标
  引入 `infra/config.cpp` 链接面）；如后续需要插件也能读 op 状态，再统一到 config 门禁单一写者。
- **批 5-1+ 切片建议**：按 owner 域分批（`api.character` 11 条、`api.item` 7 条、
  `api.ui` 5 条、`api.save` 5 条、`api.world` 4 条 + `api.quest`/`custom_recipe` 等），每批
  1 个 owner、复用本批 command 机制；op 路由直接经 dispatch 门禁，无需各插件复刻。
- kernel-only `.so` 含 `nativeSetOpEnabled`/`op disabled` 属宿主 dispatch 内核面，与业务插件无关。

## 7. 真机验收结果（编排方，2026-09-23）

环境：monster v29 集成包（构建 `2609231225_e6876fa8cd8d`，LSPatch 3/3）；health `ready=true`，
进档 `world`。

| 检查点 | 结果 |
|---|---|
| op 关闭门禁 | `POST lifecycle` → **403 `{"ok":false,"error":"op disabled"}`**（dispatch 层门禁，逐字节对齐 Kotlin 信封） |
| op 开启 + 合法 body | 200 `{"ok":true,"ticket":N}`；ticket 后 `/api/system/features` 中 quest `inactive`、activate 后 `enabled`（命令真实生效） |
| 未知动作 | 200 `{"ok":false,"error":"unknown action"}` |
| id/action 缺失 | 400 `{"ok":false,"error":"id and action required"}` |
| 全量 smoke | `total=143 pass=125 warn=18 fail=0`；与基线 verdict diff 仅 5 条已知 op 门控环境差异（party 4 条 + `quit_quest` 405→400，批 4-1 同款定性） |
| logcat | FATAL/SIGSEGV/UnsatisfiedLinkError = 0 |

过程记录：首轮 smoke 中 quest 4 条 GET 503 `http owner inactive` 系编排方验收脚本误发
deactivate（ticket 10）所致，非缺陷；activate（ticket 11）恢复后重跑 smoke 上述结果。

结论：**VM-K5-05a PASS**。
