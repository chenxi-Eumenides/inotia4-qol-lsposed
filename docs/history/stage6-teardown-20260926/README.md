# 阶段 6 真机独立测试批（T2b→T2n 累积行为面回归）

日期：2026-09-26 ｜ 设备：`192.168.3.54:5555` ｜ 变体：monster v29 ｜ 分支：`refactor/arch-v2` @ `09b8e816`

本轮仅构建、集成、部署、真机回归采证；**未修改任何源码/路由/文档，未 commit**。构建/集成/临时日志保留于 `.tmp/t2o-regression/`。

> **R1 复验已追加于本文末尾「R1 rerun」节**（HEAD `1a5c292d`，构建 `372bd83f`）。R1 修复了本文 §3 的依赖环回归，但复验新发现 item 读面/写票据的 `invalid extension inventory` 问题（见 R1 节 §R1-3）。

## 0. 本批构建标识（证明运行的是本批）

| 项 | 值 |
|---|---|
| 模块 Debug APK | `output/inotia4_qol_lsposed_debug_2609261824_7f76b38e52e0.apk` |
| 模块 APK SHA-256 | `7f76b38e52e0966f76a069d9f28962df3accb7bc0773abe638835308274afbae` |
| 构建时间戳 | `2026-09-26 18:24:50 +0800` |
| 集成包嵌入模块 SHA-256 | 三个集成包内 `assets/lspatch/modules/com.inotia4.qol.apk` 均 = `7f76b38e…`（逐包解包实测） |
| 运行态模块身份日志 | `inotia4-export.log`：`module identity: package=com.inotia4.qol versionCode=192 versionName=0.7.13 apk=/data/user/0/<游戏包>/cache/lspatch/com.inotia4.qol/1792633852.apk sha256=7f76b38e…` |

三处 SHA-256 完全一致，**证明本次运行的确为本批 `7f76b38e` 构建**（非 LSPatch 旧缓存模块）。

集成包（`scripts/patch-apk.sh output/inotia4_qol_lsposed_debug_2609261824_7f76b38e52e0.apk`，成功 3/失败 0）：

| 集成包 | SHA-256 |
|---|---|
| `output/Inotia4_v1.3.2_monster_v29_lspatched.apk`（部署用） | `291cbbe74c27e34cfbd614d205f332408bbb978492bc2048f2e1045dcc395f7e` |
| `output/Inotia4_v1.3.2_original_lspatched.apk` | `c88f17f8328350936e7f96ace10b75e4e3049a7f63e2c3e948b98f1b2452e09a` |
| `output/Inotia4_v1.3.2_overhaul_lspatched.apk` | `d6f1141e13c8e2ef2810cf1fbfb75b619986b52ed9b8745ddd10288217479fbf` |

## 1. 设备与在装变体判定

- 设备 `192.168.3.54:5555`，型号 `Phh_Treble_with_GApps`，adb 在线。
- 部署前拉取在装 `base.apk`（`/data/app/…/base.apk`），提取 `lib/arm64-v8a/libgame.so` 计算 md5 = `4c332e9b508b35fe1e97f1f26e56f04d`，与 `apk/game-apk/Inotia4_v1.3.2_monster_v29.apk` 完全一致，与 `game_variant_table.inc` 的 `kMonster v29` 条目一致 → **在装变体 = monster v29**，故部署 monster 集成包。
- 启动后 `/api/health`：`variant={"series":"monster","version":"v29","known":true,"capabilities":3}`，与 md5 判定互证。

## 2. 步骤表

| 步骤 | 状态 | 关键输出 |
|---|---|---|
| 0 准备（临时目录/备份/设备） | ✅ | `.tmp/t2o-regression/`；备份 smoke 基线（sha256 `8d386bba…`）与 `output/*lspatched_v0.7.14.apk` 4 份至 `.tmp/t2o-regression/{backup,output-backup}/`；adb 在线 |
| 1 构建 Debug | ✅ | `build-debug.log`；APK `…debug_2609261824_7f76b38e52e0.apk` |
| 2 LSPatch 集成 | ✅ 3/3 | `patch-lspatch.log`；3 包嵌入模块哈希 = 本批 |
| 3 部署/重启/就绪 | ✅ | `install.log`、`health-ready.json`；同意页 `dialog-agreement.json` → `agreement-close.json` → `screen-after-agreement.json`（`main_menu`）；`enter-slot-*.json` → `world` |
| 4 143 smoke + diff | ✅（含回归） | `smoke.log`、`smoke_current.json`、`smoke-verdict-diff.json`；基线无漂移 |
| 5 行为抽检 | ⚠️ 部分受阻 | 见 §4；EB/inventory/shop 因特性回归不可达 |
| 6 logcat | ✅ | `logcat.txt`（13,716 行）；崩溃匹配 0 |
| 7 证据归档 | ✅ | 本目录 |
| 8 清洁 | ✅ | 见 §8 |

## 3. 关键发现（阻断级回归）：插件依赖环，9 个特性 failed

`features-main-menu.json` 显示以下特性 `state=failed`、`error="dependency cycle: <id>"`：

`core.item`、`extension_bag`、`inventory_hooks`、`autosell`、`attribute_quality`、`attribute_range_display`、`craft_ui`、`custom_recipe`、`special_equip`（9 个）。

**根因（源码事实）**：`core.item` 的 `kOptionalRequires` 含扩展背包能力（`module/app/src/main/cpp/plugins/core.item/plugin.cpp:225`，blame = `d90918c2` 2026-09-25 16:35「G-item-#10」），而 `extension_bag` 的 `kRequires` 含 `item.facts`（`plugins/extension_bag/plugin.cpp:93`），`item.facts` 由 `core.item` 提供 → 组成 `core.item ↔ extension_bag` 二元环。`host/plugin_registry.cpp:442-465` 对 optional provider 同样建 DAG 边（注释明示「required 和已注册的 optional provider 都进入 DAG」），于是成环；`:491-493` 将未拓扑发射者标记 `dependency cycle`。`core.item` 未发射后，所有直接/间接消费 `item.facts` 的特性（attribute_quality/autosell/craft_ui/custom_recipe/extension_bag/inventory_hooks/special_equip/attribute_range_display）级联失败。

**运行态影响**：
- `GET /api/item/inventory`、`/money`、`/items`、`/bag/{bag}/info`、`/bag/{bag}/{slot}`、`/api/item/shop/items`、`POST /api/item/inventory/{accept,reject}_dice`、`POST /api/debug/extension_bag/exit_view` 全部 `404 {"ok":false,"error":"http handler not found"}`。
- `extension_bag` 29 条资源全部 `free`，`/api/debug/extension_bag/status` 返回 `enabled:false,injected:false`。
- `stack_limit`、`core.character`、`core.world`、`core.quest`、`core.save`、`core.ui_state`、`push_all`、`world_teleport`、`save_backup` 等仍 `enabled`。

此为仅真机能暴露的行为回归（前序 T2 批次为 Host/静态证据，未加运行时特性装配门禁）。

## 4. 143 路由 smoke 对比

| 项 | 基线 `smoke_baseline_v0.7.13.json` | 本批 `smoke_current.json` |
|---|---|---|
| total | 143 | 143 |
| pass | 120 | **116** |
| warn | 23 | **27** |
| fail | 0 | 0 |
| 路由自检 | — | registered/expected 143/143、duplicates/omitted 0、accepting true |

逐路由 diff 共 13 条（`smoke-verdict-diff.json`）：

**回归（9 条，200/pass → 404/warn，均由 §3 特性失败导致）**：
`GET /api/item/inventory`、`GET /api/item/inventory/money`、`GET /api/item/inventory/items`、`GET /api/item/inventory/bag/{bag}/info`、`GET /api/item/inventory/bag/{bag}/{slot}`、`GET /api/item/shop/items`、`POST /api/item/inventory/accept_dice`、`POST /api/item/inventory/reject_dice`、`POST /api/debug/extension_bag/exit_view`。

**改善（4 条，405/warn → 400/pass，与阶段 5 party 遮蔽修复一致）**：
`POST /api/character/party/{include,exclude,discharge,withdraw}` → `400 {"ok":false,"error":"bad request"}`。

> 注：`POST /api/item/inventory/*`（带 `{bad` 或 `{}` body）在 smoke 中仍为 `400 bad request`/`bag/slot required`，系 Kotlin 层在到达 native 前拒绝；**不能据此认为 core.item 写面存活**。

## 5. 行为抽检逐项结论

### 5a 票据机理（PASS）
`POST /api/op/character/0/hp {"hp":8280}`（当前满血等值）→ HTTP 200 `{"ok":true,"ticket":18}`（0.084s）；轮询 `/api/system/tickets/18` → `state:"done"`、`result:{"ok":true}`、`result_retained:true`。与规格「kSetHp 成功不附 state」逐字一致。证据 `set-hp-submit.http`、`set-hp-ticket.json`。

### 5b 写后即时读（PASS，等值口径）
写后立即 `GET /api/character/leader`：`hp=8280`、`max_hp=8280`，与写前一致（`char-hp-after-write.txt`、`char0-stats-before.json`）。因采用等值写，**无法证明非平凡值的 cache/force_refresh 可见性**（局限见 §7）。

### 5c 校验文案 400（PASS，逐字匹配规格）
`negatives.log`：

| 请求 | 实测 HTTP | 实测 body | 规格（route-spec / 源码） |
|---|---|---|---|
| `POST /api/item/inventory/use_item {}` | 400 | `{"ok":false,"error":"bag/slot required"}` | ✅ I01 |
| `POST /api/character/grow/add_skill {}` | 400 | `{"ok":false,"error":"action_id required"}` | ✅ C01 |
| `POST /api/character/grow/0/add_stat {}` | 400 | `{"ok":false,"error":"attrs required (object: strength/agility/vitality/intelligence/spirit 或 0-4 → 数量)"}` | ✅ C02 逐字 |
| `POST /api/world/movement/move_to {}` | 400 | `{"ok":false,"error":"x/y required"}` | ✅ `core.world/plugin.cpp:557` |
| `POST /api/world/movement/move_to {bad` | 400 | `{"ok":false,"error":"bad request"}` | ✅ 非法 JSON |
| `POST /api/quest/quit_quest {}` | 400 | `{"ok":false,"error":"questId required"}` | ✅ `core.quest/plugin.cpp:306` |
| `POST /api/op/character/0/hp {}` | 400 | `{"ok":false,"error":"hp required"}` | ✅ C12 |

### 5d EB 面（BLOCKED / 回归失败）
`extension_bag` `state=failed`、`enabled=false`、29 资源全 `free`；`/api/debug/extension_bag/status` → `200 {"enabled":false,"injected":false,…}`；`GET /api/item/inventory` → `404`；EB 内 `POST /api/item/inventory/move_item {bag:6,slot:0,count:1,to_bag:6,to_slot:1}` → `404 http handler not found`。**扩展袋容量 `16/16/12/12/12` 无法观测；未做任何 EB 移动（无副作用）。** 证据 `eb-probe.log`。

### 5e 读面样本（部分 PASS）
| 端点 | 结果 | 证据 |
|---|---|---|
| `GET /api/world/map/units` | 200，20 单位 JSON | `read-world-units.json` |
| `GET /api/quest` | 200，active 任务列表 | `read-quest.json` |
| `GET /api/ui/dialog` | 200，`{"type":"npc","active":false,…}` | `read-dialog.json` |
| `GET /api/debug/path?tx=0&ty=0` | 200，16 步路径 + nearest | `read-debug-path.json` |
| `GET /api/debug/item/raw?bag=0&slot=0` | 200，`category:10,count:3`（`legacy.debug` owner 存活） | `read-debug-raw.json` |
| `GET /api/system/info` | 200，`save_slots`[0..2] `exists:true`、`current_save_slot:0`、`version:0.7.13` | `read-system-info.json` |
| `GET /api/item/inventory` | **404**（§3 回归） | `smoke_current.json` |
| `GET /api/item/shop/items` | **404**（§3 回归） | `smoke_current.json` |

### 5f 保存/进入往返（PASS）
- `POST /api/system/save` → ticket 19 `done`，result 含 `state{money:28550,map_id:20,x:328,y:136,party_count:3}`（`save-submit.http`/`save-ticket.json`）。
- 世界内 `enter_slot{slot:0}` → ticket 20 `failed`，`result {"ok":false,"error":"already in game"}`（preflight 生效，无副作用）。
- `POST /api/ui/go_main_menu` → ticket 21，screen `main_menu`；再 `enter_slot{slot:0}` → ticket 22 `done`，screen `world`（`save-reenter-*.http|json`）。**进出钩子功能级往返成功。**

### 6 logcat（PASS）
`logcat.txt` 13,716 行；`grep -E 'FATAL EXCEPTION|Fatal signal|SIGSEGV|SIGABRT|UnsatisfiedLinkError'` **0 命中**（`logcat-crash-matches.txt` 为空）。仅有 `E/Inotia4Qol` 的占位端点 `ApiException: not implemented`（`OpController` 501 占位，预期行为），非崩溃。

## 7. VM 卡四元组映射（样本覆盖，不宣称卡闭环）

| 触及文件/证据 | 影响 R-xx/B-xx | 对应 VM 卡 | 已执行/原因 |
|---|---|---|---|
| 本目录 `health-ready.json`、`features-main-menu.json`、`inotia4-export.log`、`apk-sha256.txt`、`module-apk-sha256.txt`、`patch-lspatch.log` | 无 | 构建/装配门禁（新发现，未登记卡） | ✅ 构建、集成、运行态身份三处哈希一致；**发现依赖环回归（§3）** |
| `negatives.log`、`set-hp-submit.http`、`set-hp-ticket.json`、`char-hp-after-write.txt`、`save-*.http/json`、`read-*.json` | 无 | VM-T2H-03、VM-T2L-01、VM-T2N-01 | ✅ 已迁写/读路径真机样本；save/enter 功能级往返 |
| `smoke_current.json`、`smoke-verdict-diff.json` | 无 | 受影响 VM-K5-04b/04c/06d/06i/07a/08h、VM-D1 | ⚠️ 部分样本：world/quest/save/ui/dialog PASS；item/shop/EB 因 §3 回归 404，未能覆盖 |
| `eb-probe.log` | 无 | VM-D2D-01/02、VM-T2J-01、VM-P1-S2（EB/clamp 面） | ❌ **受阻**：`extension_bag`/`core.item` failed；EB 能力与 `16/16/12/12/12` 不可测 |
| `read-system-info.json`、`features-main-menu.json`（`stack_limit` enabled） | 无 | VM-P1-S2 | ⚠️ 仅确认 `stack_limit` enabled；canonical>99 / 999 clamp 实机 A/B 未覆盖 |

## 8. 局限与建议

**未覆盖（本批明确未采证）**：
1. EB 容量 `16/16/12/12/12`、EB 内移动/镶嵌往返（`extension_bag` 因依赖环 failed）。
2. `inventory`/`shop items` 读面字节与 `item` 写票据链路（`core.item` failed）——item 写面仅测到 Kotlin 层 400。
3. `core.item`/EB 相关的 VM-D2D-01/02、VM-T2J-01 真机行为卡。
4. 非平凡值的写后 cache 刷新（5b 采用等值写）。
5. 世界/任务读面的逐字节 frozen 对比（仅功能级截取）。
6. `stack_limit` canonical>99 写读 A/B、999 clamp、原子组失败注入（VM-P1-S2-02..05）。

**建议**：① 优先修复 `core.item ↔ extension_bag` 依赖环（例如从 `core.item` 的 `kOptionalRequires` 移除扩展背包能力，改为运行时 `capabilities.require` 软取，或让 DAG 对 optional 回边不建成环）；② 修复前，所有依赖 `item.facts`/`extension_bag` 的 VM 卡无法采证，应保持 `NOT_ACCEPTED`；③ 修复后重跑本批步骤 4/5d/5e，并补 `stack_limit` 深度卡。

## 9. 恢复合规声明

- **smoke 基线**：本批以 `.tmp/t2o-regression/smoke_current.json` 为输出，`scripts/verification/smoke_baseline_v0.7.13.json` 未被覆写；`git status` 确认无漂移（sha256 `8d386bbafef1aefde90742cae5ef03b9f588f4ce0ccc43a8eebd886618ffd8ba`）。
- **output/**：`patch-apk.sh` 按设计清理了 `output/*lspatch*.apk` 后生成 3 个本批集成包；运行前已将 4 份 `*_lspatched_v0.7.14.apk`＋`release_v0.7.14.apk` 备份至 `.tmp/t2o-regression/output-backup/`，事后按研判**保留本批集成包**（`.tmp/` 备份留存；如需恢复 v0.7.14 集成包可从备份取回，但旧包模块为 v0.7.14、与本批模块不同，恢复会使设备回退）。
- **存档**：全批仅 1 次等值写（`set_hp=当前值`），无批量改数值；`/api/system/save` 与 `enter_slot` 为 smoke/往返步骤固有动作，写入的是当前未变更状态；测试结束游戏 `force-stop` 回干净存档态。
- 设备状态：游戏已 `force-stop`，前台回到桌面；未改任何源码、未 commit。

## 10. 原始文件索引

- 身份/构建：`build-debug.log`、`module-apk-sha256.txt`、`apk-sha256.txt`、`patch-lspatch.log`、`install.log`、`inotia4-export.log`
- 启动：`health-ready.json`、`features-initial.json`、`features-main-menu.json`、`screen-initial.json`、`dialog-agreement.json`、`agreement-close.json`、`screen-after-agreement.json`、`enter-slot-submit.json`、`enter-slot-ticket.json`
- smoke：`smoke.log`、`smoke_current.json`、`smoke-baseline-head.json`、`smoke-verdict-diff.json`
- 行为：`negatives.log`、`set-hp-submit.http`、`set-hp-ticket.json`、`char0-stats-before.json`、`char-hp-after-write.txt`、`eb-probe.log`、`read-*.json`、`save-*.http|json`
- 日志：`logcat.txt`、`logcat-crash-matches.txt`（空）

---

# R1 rerun（依赖环修复复验）

日期：2026-09-26（约 18:37–18:5x）｜ HEAD：`1a5c292d`（fix(host): break optional-requires cycle with two-phase topology）｜ 设备/变体同前（monster v29）。**未改源码，未 commit。** 所有 `r1-*` 文件为本节证据，位于本目录。

## R1-0. 新构建身份（证明运行 = 本批 `372bd83f`，非旧模块 `7f76b38e`）

| 项 | 值 |
|---|---|
| 模块 Debug APK | `output/inotia4_qol_lsposed_debug_2609261837_372bd83fecc8.apk` |
| 模块 APK SHA-256 | `372bd83fecc84b998368ae9538d50e97a562311497de5ebdc123b6930469c112`（构建 2026-09-26 18:37:31） |
| 集成包嵌入模块 SHA-256 | monster/original/overhaul 三包内 `assets/lspatch/modules/com.inotia4.qol.apk` 均 = `372bd83f…` |
| 运行态身份日志 | `r1-inotia4-export.log`：`module identity … sha256=372bd83fecc84b998368ae9538d50e97a562311497de5ebdc123b6930469c112` |

三处一致；与上批 `7f76b38e…` 不同 → 确认运行新构建、**非旧模块**。集成包 SHA-256（`r1-apk-sha256.txt`）：monster `617ac316…`、original `99619786…`、overhaul `ba5eda57…`。

## R1-1. 断言表（逐项）

| 项 | 断言 | 结果 | 原文摘录 / 证据 |
|---|---|---|---|
| a | 9 特性全 enabled：core.item、extension_bag、inventory_hooks、autosell、attribute_quality、attribute_range_display、craft_ui、custom_recipe、special_equip | **PASS** | `r1-features-main-menu.json`：9/9 `state=enabled,enabled=true,error=null`；全局 `failed=[]` |
| a | 日志出现一次 optional 降级 WARN（provider=core.item consumer=extension_bag） | **PASS** | `r1-inotia4-export.log` 共 5 行 `W core plugin_registry.cpp:496 optional edge downgraded to avoid cycle:`，含 `provider=extension_bag consumer=core.item capability=extension_bag`（另有 attribute_quality→attribute_range_display、extension_bag→autosell、custom_recipe→craft_ui、attribute_quality→custom_recipe） |
| b | smoke 143 恢复 | **PASS（路由级）** | `r1-smoke.log`/`r1-smoke_current.json`：total=143 **pass=125 warn=18 fail=0**；vs 基线 routes 实际 121/22 仅 4 条 diff（party include/exclude/discharge/withdraw 405/warn→400/pass，改善保留）；vs 上批 116/27 有 9 条恢复 404→200（列表见下） |
| b | 上批 9 条 404 回归全部恢复 200/pass | **PASS** | `r1-vs-prevbatch-diff.json`：item 5 读 + accept/reject_dice + shop/items + EB exit_view 全 404/warn→200/pass |
| c | EB enabled=true、资源 live、容量 16/16/12/12/12 | **PASS（debug 端点）** | `r1-eb-status-before.json`：`enabled=true,injected=true`；features 29/29 `live`；`capacities [16,16,12,12,12]` |
| c | EB 内移动往返 | **PASS** | bag6 slot0(cat14×13)→slot12：ticket 23 `done`，state 显示 slot0 空/slot12 有；反向 slot12→slot0：ticket 24 `done`，payload 与初始逐字一致（`r1-eb-*.json`） |
| d | item/shop 读面 200 且返回数据 | **FAIL（数据面）** | 路由 200 但 body 恒为 `{"error":"not in game"}`（`r1-smoke_current.json`、复测见 R1-3），与基线 `{"bags":[…],"money":28550}` 不符 |
| d | item 写票据链 | **FAIL（票据语义）** | bag0 slot5→slot6：ticket 21 `state:"failed" result {"ok":false,"error":"invalid extension inventory"}`，但游戏层实际已移动（after: slot5 empty / slot6 cat65），已反向恢复（ticket 22 `failed` 同错，实测 slot5 复原、slot6 空） |
| d | 等值写（character set_hp） | **PASS** | `set_hp{hp:8280}`（当前值）→ ticket 25 `done result {"ok":true}` |
| e | VM-P1-S2：stackLimitIncrease 两态 | **PARTIAL** | `r1-s2-on/off.json`：false→true→false 往返无崩溃，持久值回落 false；两态 EB `capacities [16,16,12,12,12]`、bag0 slot14（cat5）视图 223 不变（`r1-s2-*-view.txt`）；**canonical>99 写读 A/B 与 999-clamp 不可行**（无游戏内堆叠构造入口 + item 读面损坏） |
| f | save 往返 | **PASS** | save ticket 27 `done`（state money 28550/map20/party3）；`go_main_menu`→screen `main_menu`；`enter_slot{0}` ticket 29 `done`→screen `world` |

**smoke 三方对比**：基线 summary `120/23`（routes 实际 `121/22`，off-by-one 阶段 5 已记录）→ 上批 `116/27` → R1 `125/18`，fail 均 0。R1 vs 基线 diff=4（仅 party）；R1 vs 上批 diff=9（全部恢复）。

## R1-2. 9 特性恢复证据

`r1-features-main-menu.json` 中 9 目标特性全部 `enabled`，全局 `failed` 为空。`extension_bag` 资源 29 条全部 `live`（上批为全 `free`）。装配期 5 条 optional 边按 R1 设计降级为 WARN（诊断性），未再产生 `dependency cycle`。

## R1-3. 新发现（阻断 item 读面/原版袋写票据）：`invalid extension inventory`

**现象**
- 世界态下所有 core.item 快照读路由（inventory/money/items/bag info/bag slot/shop items）返回 HTTP 200 但 body 恒为 `{"error":"not in game"}`（复用上一次发布的 kNotInGame 快照；`r1-smoke_current.json` 及世界态复测）。
- 原版袋 `move_item`：游戏层实际执行成功，但票据 `state:"failed"` + `{"ok":false,"error":"invalid extension inventory"}`（EB 请求则返回 `done`，见 R1-1 c）。

**证据链（源码事实）**
1. `plugins/core.item/plugin.cpp:496 serialize_inventory_json` → `:550 append_extension_inventory_bags` 返回错误时，`update_snapshot`(`:593-596`) 早退且不发布，快照停留在菜单期发布的 kNotInGame；写结果 `:1079-1091` 同函数失败即把票据置 failed。
2. `append_extension_inventory_bags`(`:426-494`) → `for_each_logical_item` → 回调 `collect_extension_inventory_item`(`:413-424`)：当 `count <= 0` 时置 `items.invalid=true` → 返回 `invalid extension inventory`。
3. 回调的 count 来自 `extension_bag_public_runtime.inc:586-605 virtual_bag_for_each_logical_item`，其值为 `virtual_bag_effective_count(...)`。
4. `game_ui_virtbag.cpp:724-741 virtual_bag_effective_count`：当 `item_count_api()` 的 `effective` 不成功且 `read_encoded_field` 不成功（或 provider 未注册时 facade 返回 not_ready）时 **返回 0** → 命中 `count<=0`。
5. 装配日志 `W core plugin_registry.cpp:664 plugin core.item degraded on optional: missing optional capability: stack_limit.item_count`：R1 两阶段拓扑后 core.item 装配时 stack_limit 的 item_count 能力尚未注册（optional 边不再保证顺序）。`extension_bag` 的 `item_count_api()` = `g_game->item_count`（`game_ui_virtbag.cpp:638-640`），走 `host/assembly.cpp:329` 注入的 facade，依赖 stack_limit 的 `host_item_count_set_provider`（`plugins/stack_limit/plugin.cpp:263`）。
   → 即：**EB 的 effective-count 解析在所观察时点得到 0，导致 EB 序列化判非法**，进而 core.item 快照永不就绪、原版袋写票据误报失败。
6. 注：`/api/debug/extension_bag/status` 的 items 用 `virtual_bag_state_json.inc:26-27` 的裸 `item.count`，故显示非零，**不能作为 effective_count 正常的证据**。

**影响面**：item 全读面（快照）、原版袋写票据结果（`kMoveItem`/`kUseItem`/`kSellItem`/`kDiscardItem`/`kShopBuy` 等附 inventory state 的种类）、以及 inventory 响应中的扩展袋容量展示。EB 自身写已走独立分支正常。

**根因归属未定**：确切是哪一提交引入（T2h-B2 `d091afa3` 的 `append_extension_inventory_bags`/`effective_count` 依赖，或 R1 装配顺序使 stack_limit provider 在被调用时点缺失）需源码侧定位；本批仅给可复现证据与调用链。

## R1-4. logcat

`r1-logcat.txt` 15,241 行；`FATAL EXCEPTION|Fatal signal|SIGSEGV|SIGABRT|UnsatisfiedLinkError` **0 命中**（`r1-logcat-crash-matches.txt` 空）。无 `E/Inotia4Qol` 非占位错误行。

## R1-5. VM 卡四元组（更新）

| 触及文件/证据 | R/B | 对应 VM 卡 | 已执行/原因 |
|---|---|---|---|
| `r1-*` 构建/身份/集成哈希 | 无 | 装配门禁（本文 §3 发现） | ✅ R1 修复已验证：9 特性 enabled、无 dependency cycle |
| `r1-smoke_*`、`r1-vs-prevbatch-diff.json` | 无 | 受影响 VM-K5-*（全量 smoke 口径） | ✅ 125/18/0；9 条回归恢复（路由级） |
| `r1-item-*`、`r1-eb-*`、`r1-set-hp-*`、`r1-save-*` | 无 | VM-T2H-03、VM-T2L-01、VM-T2N-01、VM-D2D-01/02 | ⚠️ EB 移动往返 PASS；save 往返 PASS；item 原版袋写与读因 `invalid extension inventory` **FAIL** |
| `r1-s2-*` | 无 | VM-P1-S2 | ⚠️ 两态往返无崩溃；canonical>99/clamp 不可行 |

## R1-6. 局限与建议

1. **item 读面/原版袋写票据阻断**（R1-3）需源码侧修复（让 `append_extension_inventory_bags` 在 effective_count 缺失时回退描述符 count，或保证 stack_limit provider 在 core.item/EB 调用前注册）；修复前相关 VM 卡保持 `NOT_ACCEPTED`。
2. EB 容量 `16/16/12/12/12` 目前仅在 `/api/debug/extension_bag/status` 可见，未能在 `/api/item/inventory` 的扩展袋段观测。
3. VM-P1-S2 深度 A/B（canonical>99、999 clamp、原子组失败注入）本批仍未覆盖。
4. party 四条 400/pass 为善化，保留。

## R1-7. 恢复合规声明（R1）

- **smoke 基线**：以 `.tmp/t2o-regression/r1-smoke_current.json` 为输出，`scripts/verification/smoke_baseline_v0.7.13.json` 未覆写；`git status` 无漂移。
- **output/**：本批集成包 3 个（`r1-apk-sha256.txt`）；上批（依赖环构建）集成包已在 patch 前备份至 `.tmp/t2o-regression/output-backup/prev-cycle-build/`；v0.7.14 备份保留。
- **存档**：等值写（set_hp=当前值）；原版袋道具 slot5↔slot6 各移动一次并已复原（复读 raw 一致）；EB slot0↔slot12 各移动一次并已复原（payload 一致）；`stackLimitIncrease` 往返并回落 false；save/enter 为往返步骤固有动作；测试结束游戏 `force-stop` 回干净态。
- `git diff --check` clean；未改源码/文档正文（仅本目录证据与 README）；未 commit。

---

# R2 rerun（数据面降级修复复验，八项）

日期：2026-09-26（约 18:56–19:0x）｜ HEAD：`22f1b404`（fix(item,extension_bag,host): restore item data plane under missing EB count）｜ 设备/变体同前（monster v29）。**未改源码，未 commit。** 证据前缀 `r2-*`。

## R2-0. 新构建身份（必须 ≠ `372bd83f`）

| 项 | 值 |
|---|---|
| 模块 Debug APK | `output/inotia4_qol_lsposed_debug_2609261856_dd558f792ff8.apk` |
| 模块 APK SHA-256 | `dd558f792ff867d40ddcff49e6a64d391880bf592c50831836aa712a81ad2667`（构建 18:56:52） |
| 集成包嵌入模块 SHA-256 | monster/original/overhaul 均 = `dd558f79…` |
| 运行态身份日志 | `r2-inotia4-export.log`：`module identity … sha256=dd558f79…` |

三处一致且 ≠ 上批 `372bd83f` → 确认新构建。集成包（`r2-apk-sha256.txt`）：monster `7253a65a…`、original `1f342006…`、overhaul `28d05ab7…`。

## R2-1. 八项断言结果表

| # | 断言 | 结果 | 原文摘录 |
|---|---|---|---|
| 1 | item 六读 200 且非 `not in game` | **PASS** | `r2-read-*`：inventory `{"bags":[{"bag":0,…slot0 再生药水(小) count3…}`；money `{"money":28550}`；items 列表正常；bag/0/info `{"bag":0,"capacity":16,"slot_count":5}`；shop/items `{"items":[]}`。inventory bags 为 0..5 + EB 6(16,13)/7(16,3)/8(12,9)/9(12,4)/10(12,3)（`stackLimitIncrease=false` 下 EB 段仍存在，如实记录） |
| 2 | 原版袋 move 5→6 票据 done + 反向复原 | **PASS** | 前进 ticket 8 `state:"done" result.ok=true keys[ok,state]`，无 `invalid extension inventory`；after：slot5 empty / slot6 cat65；反向 ticket 9 `done`；restored 后 slot5 raw 与 before **逐字 IDENTICAL**，slot6 empty |
| 3 | EB enabled/29 live/容量 16/16/12/12/12 + 移动往返 | **PASS** | `r2-eb-status-before`：`enabled=true,injected=true`；features `extension_bag resources = live×29`；`capacities [16,16,12,12,12]`；EB bag6 slot0(cat14×13)→slot12 ticket 10 `done`，反向 ticket 11 `done`，payload 逐字 IDENTICAL |
| 4 | features core.item 无 degraded；日志无 `missing optional capability` | **PARTIAL** | features：`core.item state=enabled enabled=true degraded=False error=None`，`failed:[]`（**PASS**）；但启动日志仍有 1 行 `W core plugin_registry.cpp:665 plugin core.item degraded on optional: missing optional capability: stack_limit.item_count`（f=1，**日志条款 FAIL**；最终 degraded 态已恢复 False）。实际降级 WARN 如实记录：`optional edge downgraded`×5（预期诊断） |
| 5 | smoke 143 ≥125 pass / ≤18 warn / 0 fail，party 改善保留 | **PASS** | `r2-smoke_current.json`：**125/18/0**；vs 基线 diff=4（仅 party include/exclude/discharge/withdraw 405/warn→400/pass）；vs R1 diff=0（路由级一致；R2 修复的是 body 数据面） |
| 6 | VM-P1-S2 原版物理袋两态（ON>99/OFF≤99/再 ON 恢复） | **PARTIAL（无载体）** | 进入值 `stackLimitIncrease=false`；false→true→false 往返完成，终值恢复 false；物理袋（bag0-5）6 个物品三态计数**完全一致**（`OFF==ON==OFF2`，无 >99 载体）→ ON>99/OFF≤99 条款**构造入口仍缺，不造假** |
| 7 | EB 逻辑袋 >99 视图（R-48）重跑 | **PASS（含 1 处残留）** | `r2-s2-*.json` EB 段：bag6 slot14 cat5 **OFF=95 / ON=223 / OFF2=95**；ON 态 bag9 出现 228/770/129/321 等 >99，OFF 态无 >99 → 两态视图成立。**残留**：bag9 slot4 cat35（canonical 640）在 OFF/ON 均被跳过（模块日志 `extension inventory slot skipped: bag=9 slot=4 category=35 count=0`），未出现在 inventory EB 段 |
| 8 | logcat 崩溃 0 + `invalid extension inventory` 0 | **PASS** | `r2-logcat.txt` 12,784 行，崩溃关键字 **0**；`invalid extension inventory` 在 logcat 与模块日志均 **0**；用例 1（EB 三态回退）真机证据 = 断言 7 的 OFF=95/ON=223 与 slot-skip |

## R2-2. smoke 三方对比

| 批次 | pass | warn | fail | vs 基线 diff |
|---|---|---|---|---|
| 基线 `smoke_baseline_v0.7.13.json` | 120（routes 实际 121） | 23（routes 实际 22） | 0 | — |
| R1 `r1-smoke_current.json` | 125 | 18 | 0 | 4（party） |
| **R2 `r2-smoke_current.json`** | **125** | **18** | **0** | **4（party）** |

R2 vs R1 路由级 diff=0；差异仅在 item 读 body（R1 恒 `{"error":"not in game"}` → R2 真实数据，见 R2-1 #1）。

## R2-3. degraded / 日志结论

- **features 终态 PASS**：`core.item`/`extension_bag` 均 `enabled`、`degraded=False`、`error=None`；全局 `failed=[]`。
- **启动日志残留 FAIL**：1 行 `missing optional capability: stack_limit.item_count`（`plugin_registry.cpp:665`，f=1）。R2 把段 2 复查移到 enable 轮后，但首个 sync_gates 轮内 stack_limit 能力尚未就绪，仍补报一次；随后恢复（features 反映 False）。**如需彻底消除需再排除该首轮时序**。
- 预期诊断 WARN：`optional edge downgraded`×5；`extension inventory slot skipped`×1（bag9 slot4）。
- 无 `extension inventory omitted`。

## R2-4. 覆盖卡更新（R1+R2 真机终态）

| 卡 | 状态 |
|---|---|
| VM-T2H-03 / VM-T2L-01 / VM-T2N-01（item 读写/读面） | **R2 真机 PASS（样本）**：item 六读真实数据、原版袋写票据 done、save 往返 PASS |
| VM-D2D-01/02、VM-T2J-01（EB 面） | **R2 真机 PASS（功能级）**：EB enabled、29 live、容量 16/16/12/12/12、移动往返复原 |
| VM-P1-S2（stack_limit 两态） | **PARTIAL**：EB 逻辑袋 >99 两态视图 PASS（R-48）；原版物理袋 ON>99 无载体，条款不可观测 |
| 装配门禁（原 §3 依赖环） | **R1 PASS**（9 特性 enabled、无 dependency cycle） |
| 仍 **NOT_ACCEPTED** | ① 原版物理袋 ON>99/OFF≤99 需 >99 载体（构造入口缺）；② EB bag9 slot4 等 canonical&0x7F==0 且活体对象缺失的槽位投影（slot skipped 残留）；③ 999 clamp、原子组失败注入；④ ≥105 等级/大额数值等未抽检路径；⑤ 读面逐字节 frozen 对比 |

## R2-5. 恢复合规声明（R2）

- **smoke 基线**：以 `r2-smoke_current.json` 为输出，`smoke_baseline_v0.7.13.json` 未覆写（前 sha256 `8d386bba…`，`git status` 无漂移）。
- **output/**：R2 集成包 3 个（`r2-apk-sha256.txt`）；R1 构建集成包已备份 `.tmp/t2o-regression/output-backup/prev-r1-build/`；v0.7.14 备份保留。
- **存档**：原版袋 slot5↔slot6、EB bag6 slot0↔slot12 各移动一次并复原（复读逐字一致，零净变更）；`stackLimitIncrease` false→true→false 往返，终值 = 进入值 false；本批未主动调 `/api/system/save`（沿用 smoke 内置步骤 123 的 1 次 200 写盘，与既往各批同口径）；结束 force-stop 回干净态。
- `git diff --check` clean；未改源码（仅本目录证据与 README）；未 commit。

---

# R3 rerun（R2 两残项终验，三项）

日期：2026-09-26（约 19:05–19:1x）｜ HEAD：`bfbda2d5`（fix(extension_bag,host): keep canonical projection slots and defer first-round optional warn）｜ 设备/变体同前（monster v29）。**未改源码，未 commit。** 证据前缀 `r3-*`。

## R3-0. 新构建身份（必须 ≠ `dd558f79`）

| 项 | 值 |
|---|---|
| 模块 Debug APK | `output/inotia4_qol_lsposed_debug_2609261905_ea90eed85c4a.apk` |
| 模块 APK SHA-256 | `ea90eed85c4a51b7e3dc22e4521e142b4132f7f99e4f437ca51f98380804de77`（构建 19:05:10） |
| 集成包嵌入模块 SHA-256 | monster/original/overhaul 均 = `ea90eed8…` |
| 运行态身份日志 | `r3-inotia4-export.log`：`module identity … sha256=ea90eed8…` |

三处一致且 ≠ R2 `dd558f79`。集成包（`r3-apk-sha256.txt`）：monster `77ae716e…`、original `ba927a6c…`、overhaul `ab031a49…`。

## R3-1. 三项断言结果

| # | 断言 | 结果 | 原文摘录 |
|---|---|---|---|
| 1 | 启动日志 `missing optional capability: stack_limit.item_count` **0 次**；`optional edge downgraded` 5 次；features 无 degraded | **FAIL（1/3 子项）** | `optional edge downgraded` = **5**（PASS，`r3-log-evidence.txt:14-18`）；features `core.item`/`extension_bag` 均 `enabled,degraded=False,error=None`（PASS）；但 `missing optional capability` = **1** 次：`19:05:29.900 f=2 W core plugin_registry.cpp:698 plugin core.item degraded on optional: missing optional capability: stack_limit.item_count`（**FAIL**） |
| 2 | inventory EB 段含 bag9 slot4 category35（canonical 640），无 `slot skipped: bag=9 slot=4`；其余槽不回归 | **FAIL** | inventory EB 段 bag9 = `[(0,33,99),(1,57,2),(2,36,1),(3,41,65)]`，**不含 slot4**；模块日志与 logcat 各 **1** 次 `extension inventory slot skipped: bag=9 slot=4 category=35 count=0`（`r3-log-evidence.txt:120`）；其余 EB 段 bag6/7/8/10 与 R2 OFF **逐字一致**（好槽不回归 PASS） |
| 3 | smoke 无回归；item 六读真实；logcat 崩溃 0 + `invalid extension inventory` 0 | **PASS** | smoke **125/18/0**，vs 基线 diff=4（仅 party 405→400），vs R2 diff=0；item 六读 200 真实（money `28550`）；logcat 12,551 行崩溃 **0**；`invalid extension inventory` logcat 与模块日志均 **0** |

## R3-2. R3 修复未生效根因（源码事实）

**① 缺失能力 WARN 仍 1 次**：R3 把 `verify_optional_after_install` 改成 pending 状态机（首轮顺延、次轮补报，`plugin_registry.cpp:648-698`）。`sync_gates` 每帧调用，故第 2 帧即到「已顺延一轮」分支补报。stack_limit 的 capability 注册晚于首轮（其能力组在后续门禁轮才落地），单帧顺延不足 → WARN 仍在（f=2）。features 随后已恢复 `degraded=False`。

**② bag9 slot4 仍被跳过**：R3 只改了 `virtual_bag_effective_count` 的**失败/兜底分支**（`game_ui_virtbag.cpp:731-747` 改用 `view_from_canonical(canonical, extended=true)`），但该 item 走的是**成功早返回分支** `:727-730`：`api->effective(item,&count)` 返回 `ok` 且 `count=0`（stack_limit `provider_effective` 在 usable=false 时 `effective_view_count(640,false)=640&0x7F=0`，见 `stack_codec_impl.h:77-78`）→ 第 730 行 `if (succeeded) return count;` 直接返回 0，R3 新增的兜底永不执行 → core.item L1 仍判 count<=0 跳过该槽。

> 修法提示（非本批授权范围）：应在 `virtual_bag_effective_count` 对 `effective` 成功但结果为 0、且 descriptor canonical>0 的槽回退 canonical 视图；或让 EB 投影固定 `extended=true`（当前证据显示 OFF 态 `usable=false` 使 effective 走掩码）。

## R3-3. smoke 三批对比

| 批次 | pass | warn | fail |
|---|---|---|---|
| 上上批（依赖环构建） | 116 | 27 | 0 |
| R1 | 125 | 18 | 0 |
| R2 | 125 | 18 | 0 |
| **R3** | **125** | **18** | **0** |

R3 vs 基线 diff=4（party 改善保留）；R3 vs R2 diff=0。

## R3-4. R1+R2+R3 真机覆盖终态清单

| 项 | 状态 |
|---|---|
| 装配门禁（依赖环） | **R1 PASS** |
| item 数据面（六读真实、写票据 done、读面真实 28550） | **R2/R3 PASS** |
| EB 面（enabled/29 live/容量/移动往返） | **R2/R3 PASS** |
| VM-P1-S2：EB 逻辑袋 >99 两态视图（R-48） | **R2 PASS**（R3 好槽不回归） |
| optional 首轮 WARN 归零 | **R3 FAIL**（仍 1 次，见 R3-2①） |
| EB canonical 投影槽（bag9 slot4）恢复 | **R3 FAIL**（仍跳过，见 R3-2②） |

**仍 NOT_ACCEPTED（终态确认）**：
1. optional 首轮 `missing optional capability: stack_limit.item_count` WARN 归零（需覆盖 stack_limit capability 注册时点）。
2. EB canonical 投影槽兜底（`effective` 成功返回 0 的路径，bag9 slot4 类）。
3. 原版物理袋 ON>99/OFF≤99（当前存档无 >99 载体，构造入口缺）。
4. 999 clamp 实机行为、原子组失败注入。
5. 读面逐字节 frozen 对比（world/quest/inventory 冻结字节）。

## R3-5. 恢复合规声明（R3）

- **smoke 基线**：以 `r3-smoke_current.json` 为输出，`smoke_baseline_v0.7.13.json` 未覆写（sha256 `8d386bba…` 未变，`git status` 无漂移）。
- **output/**：R3 集成包 3 个（`r3-apk-sha256.txt`）；R2 集成包已备份 `.tmp/t2o-regression/output-backup/prev-r2-build/`；v0.7.14 与 R1 备份保留。
- **存档**：本批无任何数值写入（未调 move/EB move；`enter_slot` 仅为进档读取；smoke 内置步骤 123 的 1 次写盘，与既往各批同口径）；`stackLimitIncrease` 全程维持进入值 **false**（未切换）。
- 游戏 force-stop 回桌面；`git diff --check` clean；未改源码（仅本目录证据与 README）；未 commit。

---

# R4 rerun（R3 两残项终验，三项）

日期：2026-09-26（约 19:11–19:2x）｜ HEAD：`72241f4b`（fix(extension_bag,host): cover effective-success-zero path and per-frame pending）｜ 设备/变体同前（monster v29）。**未改源码，未 commit。** 证据前缀 `r4-*`。

## R4-0. 新构建身份（必须 ≠ `ea90eed8`）

| 项 | 值 |
|---|---|
| 模块 Debug APK | `output/inotia4_qol_lsposed_debug_2609261911_68e0c234a2a3.apk` |
| 模块 APK SHA-256 | `68e0c234a2a3b2b45807a5b67b6aba1effaa4f2da19c4c20fd7c278e3284d079`（构建 19:11） |
| 集成包嵌入模块 SHA-256 | monster/original/overhaul 均 = `68e0c234…` |
| 运行态身份日志 | `r4-inotia4-export.log`：`module identity … sha256=68e0c234…` |

三处一致且 ≠ R3 `ea90eed8`。集成包（`r4-apk-sha256.txt`）：monster `9b0f8f94…`、original `3047b9ff…`、overhaul `0cf56db9…`。

## R4-1. 三项断言结果

| # | 断言 | 结果 | 原文摘录 |
|---|---|---|---|
| 1 | `missing optional capability: stack_limit.item_count` **0 次**；`optional edge downgraded` 5 次；features 无 degraded | **PASS** | 模块日志 **0**、全量 logcat **0**；`optional edge downgraded` = **5**（`r4-log-evidence.txt:14-18`）；features `core.item`/`extension_bag` 均 `enabled,degraded=False,error=None`，`failed:[]` |
| 2 | inventory EB 段含 `bag9 slot4 cat35` 且 **count=640**（非缺失/非 0）；无 slot skipped；其余槽不回归 | **FAIL（count 子项）** | 槽**已出现**：`inventory bag9 = [(0,33,99),(1,57,2),(2,36,1),(3,41,65),(4,35,0)]`（R3 时 slot4 缺失）→ 不缺失；无 slot skipped（0）；其余槽与 R3 逐字一致；**但 slot4 count=0，非要求的 640**；`/debug/extension_bag/status` 对照：raw `[3][4] = (cat35, count640)`、capacities `[16,16,12,12,12]` |
| 3 | 无回归：smoke / party / 原版袋往返 / EB 往返 / 崩溃 / invalid / item 六读 | **PASS** | smoke **125/18/0**，vs 基线 diff=4（仅 party 405→400），vs R3 diff=0；原版袋 move 5→6 ticket8 `done`→反向 ticket9 `done`，restored slot5 **逐字 IDENTICAL**；EB bag6 slot0↔slot12 ticket10/11 `done`，payload IDENTICAL；logcat 12,533 行崩溃 **0**；`invalid extension inventory` **0**；item 六读真实（money `28550`） |

## R4-2. 断言 2 未闭合根因（源码事实，R4 覆盖不足）

R4 已修 `virtual_bag_effective_count` 的 **EB 侧**成功早退路径（`game_ui_virtbag.cpp:727-737`：`effective` 成功但 `count==0 && canonical>0` → `view_from_canonical(canonical, extended=true)=640`），因此 core.item 的 `collect_extension_inventory_item` 收到 count=640、不再判 `count<=0` → **槽不再被跳过**（R3 的 missing 已消除）。

但 core.item `append_extension_inventory_bags` 写出 JSON 的 count **并非回调值**：`plugins/core.item/plugin.cpp:461-480` 在 `native_item = extension_bag->item_at(internal_bag, slot)` 非空时，用 core.item **自己的** `effective_item_count(native_item, true, &effective, &count_present)` 覆盖回调 count（`:471-473`）。该函数（`:299-328`）走 `stack_limit_item_count_capability()->api->effective`，在 `usable=false` 时同样返回 `ok` 且 `effective=640&0x7F=0`，`count_present=true` → JSON count 被覆盖为 **0**。

→ R4 的 canonical 兜底只落在 EB 的 `virtual_bag_effective_count`，未落在 core.item 的 `effective_item_count` 覆盖路径，故投影 count 仍为 0。
**修法提示（非本批授权范围）**：core.item `effective_item_count` 成功但 `effective==0` 且回调 descriptor canonical>0 时，应回退 canonical 视图（或在 append 路径对 effective==0 保留回调 count）。

## R4-3. smoke 四批对比

| 批次 | pass | warn | fail |
|---|---|---|---|
| 依赖环构建 | 116 | 27 | 0 |
| R1 | 125 | 18 | 0 |
| R2 | 125 | 18 | 0 |
| R3 | 125 | 18 | 0 |
| **R4** | **125** | **18** | **0** |

R4 vs 基线 diff=4（party 改善保留）；R4 vs R3 diff=0。

## R4-4. R1→R4 真机覆盖终态矩阵

| 面 | R1 | R2 | R3 | R4 |
|---|---|---|---|---|
| 装配门禁（依赖环 9 特性） | PASS | PASS | PASS | PASS |
| item 数据面（六读真实/票据 done） | FAIL（404/假失败） | PASS | PASS | **PASS** |
| EB 面（enabled/29 live/容量/移动往返） | FAIL | PASS | PASS | **PASS** |
| EB 逻辑袋 >99 两态视图（R-48） | — | PASS | PASS（好槽） | PASS（好槽） |
| optional 首轮 WARN 归零 | — | — | FAIL（1 次） | **PASS（0 次）** |
| EB canonical 投影槽 bag9 slot4 | — | FAIL（缺失） | FAIL（缺失） | **PARTIAL**（出现但 count=0） |
| 票据语义（原版袋 move done） | FAIL（假失败） | PASS | PASS | **PASS** |

## R4-5. 仍 NOT_ACCEPTED（终清单）

1. **EB canonical 投影槽 count 值**：bag9 slot4 已出现但投影 count=0，未达 640（R4-2 根因，需修 core.item `effective_item_count` 覆盖路径）。
2. 原版物理袋 ON>99/OFF≤99：当前存档无 >99 载体，构造入口缺。
3. 999 clamp 实机行为、原子组失败注入。
4. 读面逐字节 frozen 对比（world/quest/inventory 冻结字节）。

## R4-6. 恢复合规声明（R4）

- **smoke 基线**：以 `r4-smoke_current.json` 为输出，`smoke_baseline_v0.7.13.json` 未覆写（sha256 `8d386bba…` 未变，`git status` 无漂移）。
- **output/**：R4 集成包 3 个（`r4-apk-sha256.txt`）；R3 集成包已备份 `.tmp/t2o-regression/output-backup/prev-r3-build/`；v0.7.14、R1、R2 备份保留。
- **存档**：原版袋 slot5↔slot6、EB bag6 slot0↔slot12 各移动一次并复原（复读逐字一致，零净变更）；`stackLimitIncrease` 全程维持进入值 **false**（未切换）；`enter_slot` 仅进档读取；smoke 内置步骤 123 的 1 次写盘与既往同口径。
- 游戏 force-stop 回桌面；`git diff --check` clean；未改源码（仅本目录证据与 README）；未 commit。
