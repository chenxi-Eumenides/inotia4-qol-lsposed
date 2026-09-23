# 阶段 5 批 5-S1：save_backup owner 接管备份四端点（command/query dispatch）

日期：2026-09-23 ｜ 对应卡：VM-K5-05c ｜ 分支：refactor/arch-v2

## 1. 范围与 owner

| 路由 | method | owner（迁移后） |
|---|---|---|
| `/api/system/backup/export`、`/import`、`/delete` | POST | `save_backup` |
| `/api/system/backup/list` | GET | `save_backup` |

`routes.inc` 4 条 owner `api.save` → `save_backup`；143 总数、method/path/op_gated/lease_required 不变。
`api.save` 余 3 条（`save`/`enter_slot`/`create_slot`）待 save 域归属确认后另行迁移。

**归属依据**：`save_backup_*_json` 的 bundle 组装/解析/导出/导入/删除逻辑全部在
`plugins/save_backup/bundle.cpp` + `feature.cpp`（插件自有实现），JNI 仅跨 TU 链接调用；
这批路由的真实 owner 就是 `save_backup`，非标签迁移。

## 2. 实现要点

- **声明式 http 路由**：`plugins/save_backup/plugin.cpp` 新增 4 条
  `HttpQueryHandlerDescriptor`（GET list + POST export/import/delete）挂到 descriptor 尾部
  `http_queries/http_query_count`；宿主按插件生命周期注册/撤销（复用 dispatch 的 query/command 双槽
  与路由墓碑语义），插件**不自注册**，因此不需要 `plugin_deactivate` 里显式 `unregister_owner`
  （这正是批 5-1 EB/core.ui 自注册模式暴露缺陷后的更稳做法）。
- **requires 扩展**：`save_backup` 的 requires 由 `ui_panels + save.facts` 扩为
  `+ http.registry + http.dispatch`（4 项）。
- **handler 语义**：`sb_http_handler` 按 ctx 分派四端点，直调
  `save_backup_list_json` / `save_backup_export_json` / `save_backup_import_json` /
  `save_backup_delete_json`；入参校验与迁移前 Kotlin 逐字节一致——
  `slot ∉ [0,2]` → 400 `{"ok":false,"error":"bad slot"}`；
  `checksum ∉ ^[0-9a-f]{12}$` → 400 `bad checksum`。
- **Kotlin 收窄**：`SaveController` 四条方法保留参数校验（400 语义冻结）后改
  `NativeHttpCommand.post`/`NativeHttpQuery.get` 透传（forward `JSONObject.toString()` 归一化 body）；
  `SaveBackupActions.kt` 删除，`ActionApiService`/`ActionApiServiceCore` 对应声明与实现删除。
  `LogFile.op` 端点日志随之消失（登记为行为面变更）。
- **编译细节**：`plugin.cpp` 需显式 `#include "bundle.h"`，且 `valid_checksum` 必须用
  `::save_backup::valid_checksum` 限定——`save_backup` 这个名字在
  `qol::features::save_backup` 内会先解析到外层命名空间自身。

## 3. 静态验证

| 命令 | 结果 |
|---|---|
| `git diff --check` | 通过 |
| `scripts/build-debug.sh` | `BUILD SUCCESSFUL`；`output/inotia4_qol_lsposed_debug_2609231621_79bcf7fd7eec.apk` |
| `scripts/build-debug.sh -PqolKernelOnly=true` | `BUILD SUCCESSFUL`；`output/inotia4_qol_lsposed_debug_2609231621_8ac69efa62d5.apk` |
| kernel-only strings | 批内符号 0（唯一 `save_backup` 命中为 `infra/log/qol_log.cpp` 的日志域标签，预存在）；普通构建 104 |
| `check_http_routes.py` | 143/143、三表一致、`save_backup=4`、`api.save=3` |
| `check_arch.py` | layers/offsets/jni/resources 全部通过 |

Host 基线不变（save_backup 插件源不在 host 测试编译面，同批 5-1 EB/core.ui 情形）；
`bundle.cpp` 既有 host 测试继续覆盖其纯逻辑。

## 4. 真机验收（monster v29 集成包，3/3 LSPatch，嵌入模块 `com.inotia4.qol.apk`）

进档流程：health `ready` → `POST /api/ui/dialog/select {"action":"ok"}` → `POST /api/system/enter_slot {"slot":0}` → `screen=world`。

| 检查点 | 结果 |
|---|---|
| routes owner | 4 条 `save_backup`；`api.save` 余 3 条（save/enter_slot/create_slot） |
| `GET backup/list` | 200，完整备份列表 JSON（含 map_name/class_name 等字段） |
| export 非法 slot | 400 `{"ok":false,"error":"bad slot"}`（slot=5 与空 body 同） |
| import 非法 checksum | 400 `{"ok":false,"error":"bad checksum"}` |
| delete 非法 checksum | 400 `{"ok":false,"error":"bad checksum"}` |
| export slot=0 | 200 `{"ok":true,"backup":{...}}`；list 随即含新 checksum |
| delete 有效 checksum | 200 `{"ok":true}`；再删同 checksum → 200 `{"ok":false,"error":"backup not found"}` |
| owner 停用 | `deactivate` → 4 条全 503 `{"ok":false,"error":"http owner inactive"}` |
| owner 恢复 | `activate` → 4 条全 200 逐字节恢复 |
| 全量 smoke | `total=143 pass=124 warn=19 fail=0`；与基线 verdict diff 仅 4 条已知 op 环境差异（party include/exclude/discharge/withdraw 405→400，与本批无关） |
| logcat | FATAL/SIGSEGV/UnsatisfiedLinkError = 0 |

**主菜单帧冻结语义复现**：本次验收首轮跳过进档直接 deactivate，`save_backup`/`quest` 均停在
`enabled`（ticket 正常返回但 gate apply 排队）；进档后排队事务立即落地（`save_backup` → inactive、路由 503）。
即 deactivate 失败是未进档所致，非本批缺陷。

## 5. 四元组

| 触及文件 | R/B 编号 | VM 卡 | 执行情况 |
|---|---|---|---|
| `plugins/save_backup/plugin.cpp`、`plugins/core.api/routes.inc`、Kotlin `SaveController`/`ActionApiService`/`ActionApiServiceCore`、`SaveBackupActions.kt`（删除）、`docs/refactor/{api-route-map,verification,ledger}.md` | 本批无扩展背包 R-xx/B-xx 编号 | VM-K5-05c | 全部已执行：静态/双构建/strings/routes/arch/LSPatch/真机（4 条冻结对比 + 生命周期 + smoke + logcat） |

## 6. 遗留与后续

- `save_backup` 现为 save 域 owner（含备份）；`save`/`enter_slot`/`create_slot` 是否并入待归属确认。
- `LogFile.op` 的 `POST /api/system/backup/*` 端点日志已随 Service 删除消失。
