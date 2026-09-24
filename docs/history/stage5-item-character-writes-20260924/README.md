# 阶段 5：item/character 写路由验收（P4b）

日期：2026-09-24 ｜ 设备：`192.168.3.54:5555` ｜ 变体：monster v29

本轮仅构建集成包、部署、运行验收并归档证据；**未修改源码、未提交 Git**。临时构建/patch 日志保留于 `.tmp/item-char-writes/`。

## 1. 集成 APK

使用普通 Debug 模块作为 LSPatch 模块输入；`scripts/patch-apk.sh` 默认选择
`tools/lspatch/lspatch-v1.2-487-release.jar`，遍历 `apk/game-apk/` 的 3 个游戏包。

```bash
scripts/patch-apk.sh output/inotia4_qol_lsposed_debug_2609241844_ccc72c5e5ae7.apk
unzip -l output/Inotia4_v1.3.2_overhaul_lspatched.apk | grep modules/
unzip -l output/Inotia4_v1.3.2_original_lspatched.apk | grep modules/
unzip -l output/Inotia4_v1.3.2_monster_v29_lspatched.apk | grep modules/
```

| 集成包 | SHA-256 | 嵌入模块核验 |
|---|---|---|
| `output/Inotia4_v1.3.2_overhaul_lspatched.apk` | `e287b4f8c123cdd3b318d2289546b7edd7022c40185410dbe0963554c2a782e8` | `assets/lspatch/modules/com.inotia4.qol.apk`，12,398,535 bytes |
| `output/Inotia4_v1.3.2_original_lspatched.apk` | `b4ce5c0d3716fe6c99e3b7fa20e06b66160362571e31ecda8d9941ea9ab7e129` | `assets/lspatch/modules/com.inotia4.qol.apk`，12,398,535 bytes |
| `output/Inotia4_v1.3.2_monster_v29_lspatched.apk` | `92785053d38fd541502dbd57350cf4a4b817d8c0c0316e49bd4371a56d564e7b` | `assets/lspatch/modules/com.inotia4.qol.apk`，12,398,535 bytes |

模块 APK SHA-256：普通 `ccc72c5e5ae7f56874ca67d4ca11c6b0545cd0ea659abc671dfd153e0ce51837`；kernel-only `d003e107553f3e382d6ae8a5946419e0ce9e5fcb23f323de5ee1c2567793af7a`。两个待验模块均未重新构建。

生成与哈希证据：本目录 `apk-sha256.txt`；过程日志：`.tmp/item-char-writes/patch-lspatch.log`、`.tmp/item-char-writes/embed-check.log`。

## 2. 部署与进入游戏

```bash
adb -s 192.168.3.54:5555 install -r output/Inotia4_v1.3.2_monster_v29_lspatched.apk
adb -s 192.168.3.54:5555 shell am force-stop com.com2us.inotia4.normal.freefull.google.global.android.common
adb -s 192.168.3.54:5555 shell monkey -p com.com2us.inotia4.normal.freefull.google.global.android.common -c android.intent.category.LAUNCHER 1
curl -sS http://192.168.3.54:8088/api/health
curl -sS -X POST http://192.168.3.54:8088/api/ui/dialog/select -H 'Content-Type: application/json' -d '{"action":"ok","index":0}'
curl -sS http://192.168.3.54:8088/api/ui/screen
curl -sS http://192.168.3.54:8088/api/system/game_frame
sleep 1
curl -sS http://192.168.3.54:8088/api/system/game_frame
curl -sS -X POST http://192.168.3.54:8088/api/system/enter_slot -H 'Content-Type: application/json' -d '{"slot":0}'
curl -sS http://192.168.3.54:8088/api/system/tickets/7
curl -sS http://192.168.3.54:8088/api/ui/screen
```

结果：安装成功；health=`ready:true, variant=monster/v29`；同意页请求返回 `close_dispatched`，screen=`main_menu`；帧值从 `286` 增至 `316` 后才提交进档；`enter_slot` ticket `7` 为 `done`、`result_retained:true`，最终 screen=`world`。

原始证据：`install-monster.log`、`force-stop.log`、`monkey-start.log`、`health-ready.json`、`agreement-close-1.json`、`screen-after-agreement-1.json`、`frame-before.json`、`frame-after.json`、`enter-slot-submit.json`、`enter-slot-ticket.json`、`screen-after-enter.json`。

## 3. 验收结果矩阵

| 项目 | 结果与原始证据 |
|---|---|
| owner 归位 | PASS。`/api/system/routes` 自检 `143/143`、重复/遗漏 `0`、accepting=`true`；`api.item=0`、`api.character=0`；本批 `core.item` 13 条（item 域 11 + OP inventory 2），`core.character` 21 条（grow 4 + combat 6 + party 4 + OP character 6 + party swap 1）。汇总见 `owner-counts.json`/`owner-counts.log`；原始 routes/features 在 `routes.json`、`features.json`。本批计数不含早批次仍归 core owner 的 OP 占位路由。 |
| item 校验 400 | PASS。`POST /api/item/inventory/use_item {}` → HTTP 400，文案逐字为 `{"ok":false,"error":"bag/slot required"}`；无票据。见 `use-item-400.http`。 |
| 普通背包移动票据 | PASS。原版袋 bag 0 slot 0 的 1 个药水移动至 slot 3 → ticket `8` done、result 含 inventory state；反向合并恢复 → ticket `9` done。最终背包与初始槽位/数量一致。见 `move-item-submit.http`、`move-item-ticket.json`、`move-item-restore-*`。 |
| 装备 category 分支 | PASS。`equip_item {category:372}` → ticket `10` done，result 中 party 槽 4 装备 category 372；随后 `unequip_item {slot:4}` → ticket `13` done，并移回原版袋；`move_item` 恢复到初始槽位 4 → ticket `54` done。见 `equip-category-*`、`equip-category-restore-*`。 |
| character add_stat / set_attr | PASS。`add_stat {attrs:{strength:1,agility:1}}` → ticket `11` done，`result.applied=[{"attr":0},{"attr":1}]`；`set_attr {stats:{strength:82,agility:64}}` → ticket `12` done，`result.set` 回显两项。之后通过 ticket `14` 恢复 status-point、ticket `15` reset_stat；最终主角 base stat、已分配属性点和 status-point 与初始读取一致。见 `add-stat-*`、`set-attr-*`、`status-point-restore-*`、`add-stat-restore-*`、`party-final.json`。 |
| party 四条路由遮蔽修复 | PASS（路由可达）。include/exclude/discharge/withdraw 分别均为 HTTP 200 并取得 tickets `16–19`；以不存在的 `mercenary_slot=9999` 避免修改队伍/遣散数据，四个 owner 终态均为 `failed` + `mercenary not found`。另以当前有效的 `mercenary_slot=1` 抽验 include，HTTP 200 ticket `55`，终态 `already in party`（当前队伍已满/该佣兵已在队），无状态变更。结果证明请求已越过 AndServer 映射并进入 owner；不是 party 修改成功路径。见 `party-{include,exclude,discharge,withdraw}-{submit,ticket}.*`、`party-include-valid-slot-*`。 |
| core.item 生命周期 | PASS。deactivate ticket `20` done；写路由返回 HTTP 503 `http owner inactive`；activate ticket `21` done；恢复后的 reject_dice 返回 HTTP 200 ticket `52`，终态业务失败 `no dice result pending`。见 `core-item-*`。 |
| core.character 生命周期 | PASS。初次 deactivate/activate tickets `30/31` done；第一次 probe 在 activate 后才发出，故 HTTP 200 ticket `34`（业务终态 `mercenary not found`），不是停用态证据。随后重新 deactivate ticket `35`，party/include 返回 HTTP 503 `http owner inactive`；activate ticket `36` 后路由返回 HTTP 200 ticket `53`，终态业务失败 `mercenary not found`。见 `core-character-*`。 |
| smoke 与基线 | PASS（无新增路由回归）。沿用 `uv run python scripts/verification/smoke_all.py http://192.168.3.54:8088 scripts/verification/smoke_baseline_v0.7.13.json`，本次 `143` 条、`pass=125,warn=18,fail=0`。与原基线逐路由 status/verdict 比较只有四条 party POST 从 `405/warn` 变为 `400/pass`，符合可达性修复及 smoke 缺参请求预期；差异明细 `smoke-verdict-diff.json`。原基线文件的 summary 自身与其 routes 数量相差 1（summary `120/23`，routes 实际 `121/22`），本次 smoke 后已从 HEAD 还原该跟踪文件；HEAD 副本和本轮 smoke 快照分别为 `smoke-baseline-head.json`、`smoke-current.json`。 |
| crash/logcat | PASS。测试过程采集完整 logcat；搜索 `FATAL EXCEPTION|Fatal signal|SIGSEGV|SIGABRT|UnsatisfiedLinkError` 及 Inotia4Qol crash/FATAL 标签无命中。见过滤摘录 `logcat-qol-excerpt.txt`（qol 标签行）与空结果 `logcat-crash-matches.txt`；全量 logcat（438722 行）体积超出批次先例，未入仓，留存于 `.tmp/item-char-writes/logcat-full.txt` 供用户查看。 |

## 4. 生命周期级联和测试副作用

按测试要求停用 `core.item` 时，多个消费者进入 `cascade_stopped`。恢复顺序中 `extension_bag` 首次 activate ticket `28` 为 failed；记录状态后重试 ticket `29` done，随后特性恢复 enabled。对 `core.character` 的停用也级联停用了 `push_all`、`special_equip`，经 owner activate 后分别显式恢复（tickets `37/38`）。最终 `core.item`、`core.character`、`inventory_hooks`、`extension_bag`、`special_equip`、`push_all` 均 enabled；`simple_mode` 保持 disabled（其测试前即 disabled）。最终 features 快照显示 autosell/craft_ui/custom_recipe 的 `degraded=true`，其它对应插件状态见 `features-final2.json`；这项 lifecycle 后状态差异如实保留，没有修改代码或尝试修复。

测试写入已回滚：移动药水恢复原槽/数量，category 372 鞋装备后已脱下并移回原槽，主角属性点/reset 恢复到初始状态。party 操作用不存在的槽 ID，未改变队伍。初始/最终状态分别见 `inventory-before.json`、`party-before.json`、`inventory-final.json`、`party-final.json`。

## 5. R/B 与 VM 四元组

本批无新增或变更的扩展背包 R-xx/B-xx 编号。`docs/refactor/verification.md` 当前没有 item/character writes 的专属新卡；以下按 handoff W4 归档为 `VM-K5-W4` 验收证据，并引用已有同族卡 `VM-K5-06c`（通配路由）、`VM-K5-06e`（写票据）、`VM-K5-06b`（owner 票据/生命周期）作为流程参照，**不宣称这些旧卡替代新批次登记**。

| 触及文件/交付物 | 影响 R-xx/B-xx | 对应 VM 卡 | 已执行/无法执行原因 |
|---|---|---|---|
| `output/Inotia4_v1.3.2_{overhaul,original}_lspatched.apk`、`output/Inotia4_v1.3.2_monster_v29_lspatched.apk`；`.tmp/item-char-writes/patch-lspatch.log`、`embed-check.log`、`apk-sha256.txt` | 无 | VM-K5-W4（新卡未登记）；流程参考 VM-K5-06e | 三个 LSPatch 包生成 3/3；逐包确认嵌入 `com.inotia4.qol.apk`；记录路径及 SHA-256。 |
| 本目录 `install-*`、`health-*`、`agreement-*`、`frame-*`、`enter-slot-*`、`routes*.json`、`features*.json`、`owner-counts.*` | 无 | VM-K5-W4；流程参考 VM-K5-06c/06e | 主包安装、重启、health、agreement→main_menu、帧推进、进档 ticket、owner 计数均已执行。 |
| 本目录 `use-item-*`、`move-item-*`、`equip-category-*`、`add-stat-*`、`set-attr-*`、`party-*-submit.http`、对应 `*-ticket.json` | 无 | VM-K5-W4；流程参考 VM-K5-06e | 参数 400、票据结果回取、category 与批量写、party 4 条可达性已执行；party 四项是有效请求/路由验证，业务终态按测试槽号为 `mercenary not found`。 |
| 本目录 `core-item-*`、`core-character-*`、`reactivate-*`、`features-final2.json` | 无 | VM-K5-W4；流程参考 VM-K5-06b | 两 owner 停用期间 503 与恢复后 200 均已验证；消费者级联、extension_bag 首次恢复失败后重试成功和最终 degraded 状态均保留证据。 |
| 本目录 `smoke.log`、`smoke-current.json`、`smoke-baseline-head.json`、`smoke-verdict-diff.json`、`logcat-qol-excerpt.txt`、`logcat-crash-matches.txt` | 无 | VM-K5-W4；覆盖 VM-K5-* 的全量 smoke/logcat 口径 | smoke 143 条并完成 HEAD 基线逐路由 diff；logcat 检查无命中。baseline 跟踪文件已恢复到任务前版本。 |

## 6. 原始日志索引

所有 curl 请求/响应以 HTTP 文件或 JSON 文件保存于本目录；patch/build 中间输出放在 `.tmp/item-char-writes/`，依用户要求保留，不清理。安装、重启、health、ticket、路由/能力状态、生命周期、smoke 和 logcat 证据均位于本目录。
