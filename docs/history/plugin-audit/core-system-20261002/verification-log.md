# core.system 插件审计批（2026-10-02）验收登记

> 本登记记录本批验收范围与限制；不表示 core.system 全量文档一致性问题已关闭。完整问题定义以 `docs/refactor/issues.md` 为准，验收矩阵以 `docs/refactor/verification.md` 为准。设备为 monster v31。
> 提交链：`867b0974`（INF-04 Host SDK 快照 API minor 15）、`a225ebcc`（5 插件迁移统一快照）、`a30f699e`（SYS-02/03/04/06 修复批）。

## §1 本批登记问题

问题正文与状态见 [`docs/refactor/issues.md`](../../../refactor/issues.md)，本节只登记编号与本批关联：

- 本批登记：INF-04（跨插件快照并发缺陷，已修复）；SYS-01/02/03/04/06 已修复，修复批为 `a30f699e`（SYS-01 随 `867b0974`/`a225ebcc` 修复）。
- 文档漂移仍开放：SYS-05 对应 DOC-06，SYS-07 对应 DOC-07；本批不统一修改 API/health 文档。

## §2 无法验证项

| ID | 项 | 原因与范围 |
|---|---|---|
| SYS-05 / DOC-06 | `/api/system/info` 示例及 `game_frame` 描述漂移 | 本批只验证实际 identity 与路由生命周期，不对文档段落统一更新或结项。 |
| SYS-07 / DOC-07 | health `ready` 文档口径 | 本批不审改 health 文档，也未把 `ready` 扩大解释为游戏世界/插件就绪保证。 |

## §3 已完成验收登记

| 验收项 | 结果 | 证据 |
|---|---|---|
| `/api/system/events` 路由归属及 owner 停用/恢复 | **PASS（专项）**：路由归属 `core.system`；deactivate 后 5 条 system 路由返回 503 `http owner inactive`，activate 后恢复。 | `evidence/system-*-inactive.json`、`evidence/system-*-active.json`、`evidence/ticket-15-deactivate.json`、`evidence/ticket-16-activate.json`；事件响应 body SHA-256 见 `evidence/events-body-sha256.txt` |
| system identity | **PASS（专项）**：版本 `0.7.13`、package name `com.com2us.inotia4.normal.freefull.google.global.android.common`。 | `evidence/system-info-active.json`、`evidence/info-after-activate.json` |
| system snapshot/game 接口样本 | **PASS（本批抽查）**：有效响应留档；不等同所有 system 读面逐字节 frozen 全量对比。 | `evidence/system-game-active.json` |
| 快照并发修复及修复提交链 | **已登记**：INF-04 由 `867b0974`/`a225ebcc` 修复，SYS 修复批为 `a30f699e`。 | 提交编号与状态见 §1、`docs/refactor/issues.md` INF-04/SYS 条目；本表不替代提交自身 Host/TSan 记录。 |
| core.system 审计批整体 | **NOT_ACCEPTED（文档事项未收口）**：SYS-05/SYS-07（DOC-06/DOC-07）仍待统一更新。 | 本节 §2；`docs/refactor/issues.md` §9 DOC-06/DOC-07 |
