# 阶段 5 批 4-2：item GET 6 条 + quest GET 4 条 owner dispatch

日期：2026-09-23 ｜ 对应卡：VM-K5-04c ｜ 范围：只实现与静态验证；真机验收由编排方执行。

## 1. 范围与 owner

| 路由 | owner（迁移后） |
|---|---|
| GET `/api/item/inventory`、`/money`、`/items`、`/bag/{bag}/info`、`/bag/{bag}/{slot}`、`/api/item/shop/items` | `item_facts` |
| GET `/api/quest`、`/api/quest/active`、`/api/quest/details`、`/api/quest/completed` | `quest` |
| GET `/api/quest/{id}` | 不变（`api.quest`，Kotlin `notFound` 占位） |

`routes.inc` 只改 owner 字段；method/path/op_gated/lease_required 不变，总路由 143。

## 2. 迁移前 Kotlin 拼装链

### item（6 条）

| 路由 | Service 方法 | native JSON | enrichment |
|---|---|---|---|
| inventory | `InfoApiServiceCore.inventory` | `nativeGetInventoryJson`（`core/native/game_cache.cpp` slot `inventory` → `build_inventory_json`） | `NameInjector.withItemNames` → `injectItemName`：名称前缀 + `restructureItem` 字段重排 |
| money | `inventoryMoney` | `nativeGetPlayerJson`（slot `player`） | 取 `money` → `{"money":N}`；非 world 转发 `{"error":"not in game"}` |
| items | `inventoryItems` | 同 inventory | 展平 + `put("bag",…)` + `injectItemName`；`restructureItem` 清空全部字段重建，故输出**不含 bag 字段** |
| bag info | `bagInfo` | 同 inventory | 选袋 → `{bag,capacity,slot_count}`；袋缺失 → 404 |
| bag slot | `bagSlot` | 同 inventory | 选槽 → `injectItemName`；袋存在空槽 → `null`；袋缺失 → 404 |
| shop items | `InfoShopQueries.items` | `nativeShopItems`（`api/native/game_shop.cpp`） | 仅追加 `name`（`StaticData.itemName`，非空才加） |

`restructureItem` 字段顺序：`slot,name,category,count,item_type,need_level,rarity,rarity_tier,base, bonus,gem,chaos,enchant`；
`name=(equip?rarityPrefix(raw_rarity):"")+itemName(category)`；`base.magic_rate=原生值/100`；`bonus/gem` 按 `option_types` 拆 `optionName`。

### quest（4 条）

| 路由 | Service 方法 | native JSON | enrichment |
|---|---|---|---|
| quest | `quest` | 复合 active/details/completed | `{"active","details","completed"}`；任一 native error 直接转发 |
| active | `questActive` | `nativeQuestActive`（`api/native/game_quest.cpp`） | 过滤 `questData` 缺失与 `hidden`；注入 `id_name` + `group_id,name,detail,is_side,is_mainline` |
| details | `questDetails` | `nativeQuestList` | 注入 `group_id,group_name,name,detail,accepted_dialog,delivered_dialog,class_req,reward_hint,is_side,is_mainline,hidden` + `rewards` |
| completed | `questCompleted` | `nativeQuestCompleted` | 注入 `name` + `group_id,detail,is_side,is_mainline` |

## 3. 实现方案

- **运行期事实**：item_facts / quest 各注册 kLogicPre 主线程帧任务（15 帧周期 ≈4Hz）发布插件私有 seqlock 快照；
  HTTP handler 只读快照（`plugins/item_facts/snapshot.h`、`plugins/quest/snapshot.h`），不在 HTTP 线程 live read。
- **静态 enrichment**：Kotlin `StaticData` 启动期构造并通过 JNI 下发（复刻批 4-1 map exits 模式）：
  - item：`N/T/O/E` 名称/类型/词缀/附魔名称表，存入 `game/adapter_ports/item_port`（kernel），经 port `static_text` 暴露；
    `item_facts` 内置私有 `json_lite` 解析原生库存 JSON 后按 `restructureItem` 重排。
  - quest：`A/D/C` 三端点按迁移前注入顺序预构造的 JSON 片段表，存入 `game/adapter_ports/quest_port`；
    运行期条目复用既有结构化 `QolQuestPort` 读（与 `data_quest_*_json` 同源同逻辑），handler 拼接片段。
  - 片段/名称文本由 Kotlin `JSONObject.quote` 转义后原样拼接 → 转义与字段顺序与迁移前 `JSONObject` 序列化一致。
- **静态就绪门控**：两个 port 暴露 `static_ready`；未下发时 owner 不发布快照（HTTP 503），避免空名称/缺字段响应。
- **Kotlin 收窄**：`InfoItemQueries` / `InfoQuestQueries` + `NativeHttpQuery.get` 解 nativeHttpDispatch 信封；
  owner 4xx/5xx 转 `ApiException` 交全局 Resolver 还原状态码。`InfoApiServiceCore` 10 个方法改为透传；
  `InfoShopQueries.kt` 删除（唯一消费者已迁）。
- **dispatch 缓冲区**：`nativeHttpDispatch` 输出缓冲由 64KB 提升到 256KB（线程常驻 `vector`），容纳任务完成列表等较大聚合响应。

## 4. 静态验证证据

| 命令 | 结果 |
|---|---|
| `git diff --check` | 通过（无空白错误） |
| `cmake -S module/app/src/main/cpp/tests -B .tmp/stage5-b42-20260923/host -G Ninja && cmake --build … && ctest …` | `100% tests passed out of 62`（基线 60 + 新增 `item_facts_dispatch_tests`、`quest_dispatch_tests`） |
| `uv run python scripts/maintenance/check_http_routes.py` | controller/api-reference/native 各 143、unique 143、duplicates 0、双向 missing/extra 0；owner `item_facts=6`、`quest=4` |
| `uv run python scripts/maintenance/check_jni.py` | Kotlin external 148 / native 导出 148，违规 0 |
| `uv run python scripts/maintenance/check_arch.py` | check_layers/offsets/jni/resources `全部通过`、无新增基线 |
| `scripts/build-debug.sh` | SUCCESS；`output/inotia4_qol_lsposed_debug_2609230152_e09bf2e7b1ee.apk`（sha 前缀 `e09bf2e7b1ee`） |
| `scripts/build-debug.sh -PqolKernelOnly=true` | SUCCESS；`output/inotia4_qol_lsposed_debug_2609230152_5c8556bf6364.apk` |
| kernel-only `.so` strings | `/api/item/inventory`=0、`/api/quest/active`=0、`/api/item/shop/items`=0、`[item_facts]`=0、`[quest]`=0；`kernel_only=` 标记在 |
| 普通 `.so` strings | `/api/item/inventory`=16、`/api/quest/active`=1、`/api/item/shop/items`=1、`[item_facts]`=5、`[quest]`=1 |

`scripts/verification/smoke_baseline_v0.7.13.json` 未改动（基线只读）；本批未执行真机 smoke。

## 5. 留给编排方的真机验收步骤

1. 安装最新 Debug：`adb -s <序列号> install -r output/inotia4_qol_lsposed_debug_2609230152_e09bf2e7b1ee.apk`
   （若测试机跑的是 LSPatch 集成包形态，须先 `scripts/patch-apk.sh` 重出集成包再安装）。
2. `am force-stop` + `monkey` 重启；轮询 `/api/health` 至 `ready`；同意页 `POST /api/ui/dialog/select {"action":"ok"}`。
3. `POST /api/system/enter_slot {"slot":0}` → `GET /api/ui/screen` = `world`。
4. `GET /api/system/routes`：10 条 owner 应为 `item_facts`/`quest`，`registered=143/expected=143/duplicates=0/omitted=0`。
5. 逐条 `GET` 10 条路由，与 `scripts/verification/smoke_baseline_v0.7.13.json` 对比：status/verdict/reason 不变；
   短 body（money、bag info）逐字节相等；长 body（inventory/items/bag slot/shop/quest）匹配基线截断前缀；
   帧号/指针/存档态时间性字段豁免（口径同 VM-K5-04b）。
6. `POST /api/op/features/lifecycle {"id":"item_facts","action":"deactivate"}`：6 条应返回 `{"ok":false,"error":"http owner inactive"}`（503）；
   `activate` 后与停用前逐字节一致。`quest` 同理 4 条。
7. 全量 `scripts/verification/smoke_all.py`：`total=143、pass=120、warn=23、fail=0`（与基线无差异）。
8. `adb logcat`：无 FATAL/SIGSEGV/UnsatisfiedLinkError。

## 6. 四元组

| 触及文件 | R/B 编号 | VM 卡 | 原因 |
|---|---|---|---|
| `plugins/core.api/routes.inc`、`bridge/native/gamebridge_static_data.cpp`、`game/adapter_ports/{item,quest}_port.{h,cpp}`、`contracts/game_ports/{item,quest}_port.h`、`plugins/item_facts/*`、`plugins/quest/plugin.cpp`、`host/...`（无）、Kotlin `service/info/*` + `StaticData.kt` + `ApiBridge`/`ApiServer`、tests/CMake/docs | 本批无扩展背包 R-xx/B-xx（物品/任务域不进扩展背包） | VM-K5-04c | 已执行：host/静态检查/两种构建；无法执行：真机证据（由编排方执行） |

## 7. 真机验收结果（编排方，2026-09-23）

环境：monster v29 LSPatch 集成包，构建 `2609230217_586d5b8434a4`（含编排方修复，见 §8）；
health `ready=true`，dialog-ok 进档至 `world`。

| 检查点 | 结果 |
|---|---|
| routes owner | `item_facts=6`、`quest=4`、registered=143 |
| quest deactivate | 4 条全部 503 `{"ok":false,"error":"http owner inactive"}` |
| quest activate | 4 条恢复 200，与停用前逐字节一致 |
| item_facts deactivate | 6 条全部 503 `http owner inactive` |
| item_facts activate | 恢复 200，紧邻窗口重放 diff=0（逐字节一致） |
| 全量 smoke | `total=143 pass=120 warn=23 fail=0`，与基线 verdict/status diff=0 |
| logcat | FATAL/SIGSEGV/UnsatisfiedLinkError = 0 |

## 8. 编排方修复：lifecycle deactivate 不摘 http handler

**现象**：首次 lifecycle 重放中 quest 停用后路由错误体不确定（`snapshot not ready` 与
`http owner inactive` 混合），且一次 activate 后出现墓碑遗留毛刺（重放未复现，判定为
门禁时序竞态，与 ledger §3 已登记的 activate 票据毛刺同类）。

**根因**：`plugin_registry.cpp` 两条 deactivate 路径都不调用 `unregister_http_queries`：
零资源快速路径（lifecycle_apply）依赖 `begin_revoke_provider`，而该函数对无 provides
插件第一行提前 return；kRelease 事务分支同样只调 `plugin.deactivate`。有 provides 的
owner（item_facts 等）经 `begin_revoke_provider` 正常摘除，无 provides 的 owner（quest）
的 handler 停用后仍挂着，请求落到空快照的 `snapshot not ready`。

**修复**：两条 deactivate 路径在 `plugin->deactivate` 之前补 `unregister_http_queries`。
`unregister_http_queries` 对 `http_query_count==0` 早退、对已 pending 槽幂等，影响面仅限
注册了 http query 的 owner；与 `cascade_stop_plugin` 的既有行为（无条件摘除）对齐。

**修复后 host**：62/62 通过。修复后真机：见 §7。

## 9. 运维教训

`scripts/patch-apk.sh` 的第一个位置参数是**模块 APK**（游戏包由脚本从 game_dir 自行
glob），误传游戏包会产出嵌入游戏包自身的无效集成包（LSPatch 报 `Unsupported or
non-module APK (targetApi=0)`）。安装前可用
`unzip -l <集成包> | grep modules/` 核验嵌入模块为 `com.inotia4.qol.apk`。
