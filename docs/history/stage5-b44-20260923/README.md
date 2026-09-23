# 阶段 5 批 4-4：character 15 条 GET 查询 owner dispatch（character_facts）

日期：2026-09-23 ｜ 对应卡：VM-K5-04e ｜ 状态：待编排方真机验收

## 1. 范围与 owner

`routes.inc` 中 owner=`api.character` 的全部 GET 路由（15 条：party 12 + mercenary 3），
owner 改为 `character_facts`；143 总数、method/path/op_gated/lease_required 不变。
POST 路由（含 `/api/op/*`）仍留 `api.character`（批 5）。

## 2. 侦察结论：15 条 Kotlin 处理链与响应组装语义

Kotlin 侧入口 `PartyController` / `MercenaryController` → `ApiServices.info`（`InfoApiServiceCore`
+ `InfoMercenaryQueries`）→ `NativeBridge` → native JSON → `NameInjector.withItemNames` 等加工。

| # | 路由 | Kotlin 链 | 数据源 | 响应组装语义（迁移前，冻结口径） |
|---|---|---|---|---|
| 1 | GET `/api/character/party` | `party()` | `nativeGetPartyJson` | `withItemNames(array)`：每成员 injectTypeName + injectClassName + injectEquipmentNames(逐件 injectItemName) + restructureRole；成员为 `null` 保持 `null` |
| 2 | GET `/api/character/party/count` | `partyCount()` | `nativeGetPlayerJson` | `{"count": player.party_count}`（缺省 -1） |
| 3 | GET `/api/character/leader` | `partyLeader()` | player + party | player.leader_slot（缺省 0）→ party[slot]，`null`/越界 → 404；否则该成员 `withItemNames(single)`（class_idx 分支） |
| 4 | GET `/api/character/party/{slot}` | `partyMember` | party | party[slot]，越界/`null` → 404；否则成员 `withItemNames(single)` |
| 5 | GET `/api/character/party/{slot}/id` | `partyMemberId` | party | 原成员 class_idx；`<0` → 404；`{"id":class_idx}`，`className(class_idx)` 非空时追加 `"id_name"` |
| 6 | GET `/api/character/party/{slot}/name` | `partyMemberName` | party | 原成员无 `name` → 404；否则 `{"name":<原值>}` |
| 7 | GET `/api/character/party/{slot}/level` | `partyMemberLevel` | party | 原成员无 `level` → 404；否则 `{"level":<int>}`（缺省 -1） |
| 8 | GET `/api/character/party/{slot}/status` | `partyMemberStatus` | party member + skills | 取原成员 hp/max_hp/mp/max_mp/exp/exp_next/status_point 与 skills[slot].skill_points，各 `>=0` 才写入；`status_point` 键名为 `attribute_points`；输出 `{"status":{...}}` |
| 9 | GET `/api/character/party/{slot}/stats` | `partyMemberStats` | party | `{"stats":<原成员 stats 对象>}`（32 项属性；成员 `null` → 404） |
| 10 | GET `/api/character/party/{slot}/equipment` | `partyMemberEquipment` | party | 原成员 equipment 数组逐件 `injectItemName(item,true)`；`{"equipment":[...]}`；成员 `null` → 404 |
| 11 | GET `/api/character/party/{slot}/equipment/{equip_slot}` | `partyMemberEquip` | party | equipment[equipSlot] 逐件注入后 `item.toString()`；越界/`null` → 404 |
| 12 | GET `/api/character/party/{slot}/skills` | `partyMemberSkills` | `nativeGetSkillsJson` | skills[slot] `injectSkillNames`（action_id≥0 且 `skillName` 非空时追加 `skill_name`）后 `obj.toString()`；`null` → 404 |
| 13 | GET `/api/character/mercenary` | `InfoMercenaryQueries.all` | `nativeGetMercenariesJson` | 原样透传（含 `{"error":"not in game"}`） |
| 14 | GET `/api/character/mercenary/list` | `.slots` | mercenaries | `{"slots":[每项 slot>=0]}`（顺序保持；`error` → 原样返回） |
| 15 | GET `/api/character/mercenary/{slot}` | `.slot` | mercenaries | 首个 `slot==N` 的元素原样；无匹配 → 404；`error` → 原样返回 |

**共享加工语义**（`NameInjectorCore`）：
- `restructureRole` 输出字段序：`name/type_name/class_name/level/hp/max_hp/mp/max_mp/exp/exp_next/main_stats/status_point/equipment/type/class_idx/name_id/stats`；`main_stats` 为 5 项 `{stat_name,base_stat,additional_stat}`（力量/敏捷/体力/智力/精力，total=`char_stat_total`，base=`fn_get_stat_base`）；`base_stats` 被丢弃。
- `injectItemName(item,true)`：`name=rarityPrefix(raw_rarity)+itemName(category)`、`rarity_tier`、`static_options`、`option_names`/`options_detailed`、`socket_info`/`enchant_info`/`chaos_info`，随后 `restructureItem` 重排为 `slot/name/category/count/item_type/need_level/rarity/rarity_tier/base/bonus/gem/chaos/enchant`。
- `not in game`（非 world）时 15 条全部返回 `{"error":"not in game"}`（HTTP 200）。

## 3. 实现要点（设计）

- **owner 插件** `plugins/character_facts/`（沿用既有 id，新增 http.dispatch 面）：15 帧周期主线程帧任务发布 seqlock 快照（player/party/skills/mercenaries），HTTP handler 只读快照；未就绪 503 `snapshot not ready`；deactivate → 503 `http owner inactive`（引擎统一）。
- 新增 `QOL_PORT_CHARACTER_HTTP` adapter port：`body_json(kind)` 分发到 `game_system.cpp` 的原生拼装；party 成员富化与批 4-3 的 `append_party_member_enriched` 同族，但字段集/装备富化更全（party 成员含 exp/exp_next/status_point/equipment/name_id/stats）。
- 装备富化复用启动期静态名称表（`QOL_PORT_ITEM.static_text`，item_facts 同源）；技能名随同一行式静态表下发（新增 `S` 行），供 `party/{slot}/skills` 的 `skill_name` 注入。
- 多源组合（leader=player+party、status=member+skills）在插件 handler 内以私有轻量 JSON 切片实现，逐字节对齐 Kotlin `JSONObject` 序列化。
- Kotlin `InfoApiServiceCore` 对应方法收窄为 `NativeHttpQuery.get` 透传（新增 `InfoCharacterQueries`）；`InfoMercenaryQueries` 与本批 party 专用组合代码删除。

## 4. 静态验证

| 命令 | 结果 |
|---|---|
| `git diff --check` | 通过（无输出） |
| Host CTest（`.tmp/stage5-b44-20260923/host`） | `100% tests passed out of 64`（新增 `character_facts_dispatch_tests`；`character_facts_plugin_tests` 同步改为双 port） |
| `uv run python scripts/maintenance/check_http_routes.py` | 143/143、三表一致、`owner character_facts=15`、`api.character=27`、duplicates=0 |
| `uv run python scripts/maintenance/check_jni.py` | 149/149、违规 0（无新增 JNI） |
| `uv run python scripts/maintenance/check_arch.py` | 全部通过（layers/offsets/jni/resources） |
| `uv run python scripts/maintenance/gen_baseline_doc.py` | 层文件数更新（contracts 51→52、game 95→98、plugins/character_facts 2→5，合计 476→483），已提交 `docs/refactor/baseline.md` |
| `scripts/build-debug.sh` | `BUILD SUCCESSFUL`；`output/inotia4_qol_lsposed_debug_2609231159_05cef821212a.apk` |
| `scripts/build-debug.sh -PqolKernelOnly=true` | `BUILD SUCCESSFUL`；`output/inotia4_qol_lsposed_debug_2609231159_ea035097ba48.apk` |
| kernel-only `.so` strings | `character_facts` 命中 0、`/api/character` 命中 0、`kernel-only` 命中 1；普通构建 `character_facts`/`/api/character/party` 各 15 |

真机列为待编排方执行项（见 §5）。

## 5. 真机验收步骤（留给编排方）

1. `scripts/patch-apk.sh` 生成 monster v29 集成包并安装（`adb -s <序列号> install -r`）、force-stop + monkey 重启。
2. 轮询 `/api/health` 至 `ready=true`；若出现同意页 `POST /api/ui/dialog/select {id:"ok"}`；`POST /api/system/enter_slot {"slot":0}` 进档（首次可能 `state=2` 被拒，重发一次）至 `screen=world`。
3. `GET /api/system/routes` 核对 15 条 owner=`character_facts`、其余不变、`registered=143/expected=143/duplicates=0/omitted=0`。
4. 15 条查询逐条与 HEAD 基线对比（重点：`/api/character/party` 的成员注入与装备重排、`leader`、`party/{slot}/status`、`party/{slot}/skills` 的 `skill_name`）。帧号/指针类字段按既有口径归一化。
5. `POST /api/op/features/lifecycle`（ticket）对 `character_facts` deactivate → 15 条均 503 `{"ok":false,"error":"http owner inactive"}`；activate → 逐字节恢复。
6. `uv run python scripts/verification/smoke_all.py ...` 全量 smoke（`total=143`，与基线 verdict/status diff=0）+ clean logcat（FATAL/SIGSEGV/UnsatisfiedLinkError=0）。

## 6. 四元组

| 触及文件 | 影响的 R-xx/B-xx 编号 | 对应 VM-B/受影响 VM 卡 | 已执行/无法执行原因 |
|---|---|---|---|
| `plugins/character_facts/{plugin.cpp,json_slice.{h,cpp},snapshot.h,CMakeLists.txt}`、`contracts/game_ports/{character_http_port.h,ports.h}`、`game/adapter_ports/character_http_port.{h,cpp}`、`game/state/character_http.h`、`game_system.cpp`、`game/adapter_ports/item_port.cpp` + `contracts/game_ports/item_port.h`（技能名静态文本 kind）、`host/plugin_registry.cpp`、`plugins/core.api/routes.inc`、`plugins/CMakeLists.txt`（自动发现）、Kotlin `InfoApiServiceCore/InfoCharacterQueries/StaticData`、`InfoMercenaryQueries.kt`（删除）、tests、`docs/refactor/{baseline.md,api-route-map.md,verification.md,ledger.md}` | 本批无扩展背包 R-xx/B-xx | `VM-K5-04e`（真机列待编排方执行） | 静态验证全部已执行（见 §4）；真机回归按 §5 由编排方执行 |

## 7. 备注与遗留风险

- 装备/技能富化语义在 legacy `game_system.cpp` 内**复刻**（`append_character_equipment_item` / `append_role_json_enriched` / `build_character_skills_json`），与 `item_facts` 的 `enrich_item` 同源但各自实现（插件间不得互相 include）；后续若两处需要统一，应提炼到独立 legacy 共享 TU 而非跨插件依赖。
- 技能名复用了 item_facts 的启动期行式静态文本通道（新增 `S` 行 / `kSkillName` kind），避免为单条路由新增 JNI；语义上属跨域共用文本表，是刻意的取舍。
- `NameInjectorCore`/`NameInjector` 在本批后已无调用方（party/mercenary 组合全部下沉 native），本批未删除以缩小改动面；建议后续单独清理。
- 响应优先级为冻结：`party/{slot}/stats` 在成员为 `null` 时返回 `{"stats":null}`（200，非 404），已按迁移前 Kotlin 行为复刻并在 host 用例覆盖。
- 快照为 15 帧周期；HTTP handler 只读 seqlock 快照，未就绪 503 `snapshot not ready`。

## 8. 真机验收结果（编排方，2026-09-23）

环境：monster v29 集成包（构建 `2609231159_05cef821212a`，LSPatch 3/3，嵌入模块核验
`com.inotia4.qol.apk`）；health `ready=true`，同意页关闭后一次 `enter_slot` 进档 `world`。

| 检查点 | 结果 |
|---|---|
| routes owner | `character_facts=15`、`api.character=27`（POST 留批 5） |
| 冻结抽查 | `party`/`leader` 注入生效（name/type_name/class_name + restructureRole 全序）；`status` 含 `attribute_points` 映射与 `exp/exp_next`；`mercenary/list`=`{"slots":[0,1,2]}` |
| deactivate | 15 条全部 503 `{"ok":false,"error":"http owner inactive"}` |
| activate | 15 条恢复 200，帧号归一化后与停用前 diff=NONE |
| 全量 smoke | `total=143 pass=124 warn=19 fail=0`；与基线 verdict diff 仅 4 条已知 party op 门控环境差异（`include/exclude/discharge/withdraw` 405→400，批 4-1 同款定性，与本批无关） |
| logcat | FATAL/SIGSEGV/UnsatisfiedLinkError = 0 |

结论：**VM-K5-04e PASS**。
