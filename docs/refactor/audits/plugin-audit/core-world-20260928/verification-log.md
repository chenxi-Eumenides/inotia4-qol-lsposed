# core.world 插件审计批（2026-09-27/28）验收登记

> 结构：§1 本批新登记问题（详见 `docs/refactor/issues.md`）→ §2 无法验证项（用户确认登记，不再主动验收）→ §3 已完成验收登记。
> 流程规范见 `docs/refactor/plugin-audit-playbook.md`。代码提交链：`39b90e4e`（BFS 统一+守卫）、`9e2c2384`（money 迁移）、`d89d272c`/`c1e136bc`/`8cf061ef`（真机事故修复：弹窗自锁 / patch revert / wipeout OK 崩溃）。

## 1. 本批新登记问题（归属速览）

core.world：WLD-01~04；core.item：ITM-01~11；core.character：CHR-01~11（含 CHR-01 op 血量写无效，P1，真机探针证据 `evidence/result.json`、`evidence/hp-combat-trace.jsonl`）；core.ui_state：UIS-01；core.custom_ui：CUI-01；core.settings：SET-01/02（复测证据 `evidence/` 与真机记录）；引擎：ENG-01~08；infra/game：INF-01~09（含 INF-01 stack_limit 驻留致 slot corrupt 实测）；工具：TL-01~05；文档：DOC-01~04。全量条目、证据与状态见 `issues.md`。

## 2. 无法验证项（用户确认登记，不再主动验收）

| ID | 项 | 归属 | 原因与结论 |
|---|---|---|---|
| UNVER-01 | 教学态拦截（药水教学激活时 world 写面被 `tutorial_block_error` 拒绝） | core.world（写执行链 game/access/world.cpp） | 无教学存档可进入教学态；用户确认无法验证。不再主动验收；代码路径与拦截文案保持现状 |

## 3. 已完成验收登记

**设备与形态**：真机 192.168.3.54:5555（monster v29），集成包形式部署，运行时模块 sha256 与目标构建逐一比对（`8edcb179…`、`fdb94dee…`、`69d6a8af…`/`869c0714…`）。写测试前 slot0 备份导出+pull（`48c05d9ebe79`），备份导入闭环在团灭恢复中实测通过。

| 验收项 | 结果 | 证据 |
|---|---|---|
| 静态审计（A-G 七维） | 完成；P0=0；P1 修复或入账 | 审计记录（issues.md 对应 ID） |
| host：nav_bfs_tests / world_facts_{plugin,dispatch} / world_legacy_readers + 5 条 POST 写路径/守卫/停用 503 用例 | 全绿（含 `x=4294967296`/`-1`/溢出→400 不入队、size 拒绝、tombstone 503） | 提交 `39b90e4e` |
| money 归口迁移装配 | world.facts 2.0 / item.facts 1.1 协商成功；停 core.item→world_teleport fail-closed | 提交 `9e2c2384`；级联证据 `evidence/`（features 快照）+ 本批真机运行记录 |
| 读面 7 GET 完整体 | 与历史证据 schema/键序一致，动态差异已登记（drops 非空证伪「恒空」→DOC-01） | `evidence/world-map-*.json`、`evidence/batch-summary.md` |
| 写面 5 POST（票据/守卫/越界校验） | done/failed 语义与既有 400 文案逐项一致；越界 teleport=票据 failed `out of map bounds`（异步契约，符合 §7.10） | `evidence/batch-summary.md`、`evidence/guard-*.txt`、`evidence/teleport-oob.txt`、`evidence/move-to-ticket.json` |
| 级联停用/恢复 | deactivate→12 路由 503 + 5 消费者级联；activate+逐级恢复（消费者不自动恢复=既有设计，挂账 issues 关联 ENG-02） | `evidence/batch-summary.md` |
| 日志与 smoke | 459 帧无回退、限速 warn 零刷屏；smoke 124/19/0（基线 120/23/0，差异登记 TL-01/02） | `evidence/batch-summary.md` |
| 人工目视：move_to 可达 / walk_dir / stop_move / 出口切图（含跨图落点 (3,9) 与 exits 元数据一致） | PASS（用户确认画面、动作正常） | runbook 卡①②执行记录（会话） |
| 人工目视：移动三态其余子场景（被障碍隔开目标 / 越界目标） | 按用户确认「API 移动已完成画面确认」登记为完成；注明专项三态卡中 ②③ 未做独立定向测试（该行为 API 数据与 host 用例已验） | 本节说明即登记 |
| 人工：drops 产生（0→10 增长/跨图隔离）+ **拾取减少** | PASS（拾取闭环用户手动验证） | 增长轨迹证据 + 用户确认 |
| 人工：传送 UI 扣费（余额、扣减额、扣费经 item.facts 查钱） | PASS（用户确认扣费正常） | 会话记录（金币 28550→28250 对账） |
| 人工：UI 占据暂态 | PASS：票据 failed + `ui occupied: panel_character_info / dialog_npc`，画面无位移 | 会话票据 41/42 |
| 人工：级联停用游戏内表现 + **revert 正确性** | PASS：停用后=monster 大修原生行为（非原版）、启用后弹窗恢复、面板开合正常、无 crash | 会话三步验证（tombstone 零增量）+ `c1e136bc` 复验 |
| 真机事故：弹窗自锁 / wipeout OK 崩溃 | 已修复并真机复验（三步）；崩溃现场栈存档 `evidence/backtrace.txt`；tombstone_25/26/27 签名一致记录 | 提交 `d89d272c`、`8cf061ef` |

**VM 卡口径**：VM-K5-04b、VM-K5-06a/06b/06d/06e/06f/06g 的本轮复核以上表为证（原「真机延后」注记的 world 部分由本批补齐；卡片行更新指向本目录）。

**收口状态**：core.world 插件审计批——除 §2 无法验证项外**全部收口**。后续修复批（CHR-01 等）按 issues.md 排期，不属本卡范围。
