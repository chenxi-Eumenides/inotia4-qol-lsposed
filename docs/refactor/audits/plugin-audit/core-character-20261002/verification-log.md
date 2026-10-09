# core.character 插件审计批（2026-10-02）验收登记

> 本登记记录本批验收范围与限制；不表示 core.character 全量验收通过。完整问题定义以 `docs/refactor/issues.md` 为准，验收矩阵以 `docs/refactor/verification.md` 为准。设备为 monster v31。
> 提交链：`867b0974`（INF-04 Host SDK 快照 API minor 15）、`a225ebcc`（5 插件迁移统一快照）、`e743e6a1`（CHR-04/05/06/07/09/10/A5 修复批）。

## §1 本批登记问题

问题正文与状态见 [`docs/refactor/issues.md`](../../../refactor/issues.md)，本节只登记编号与本批关联：

- 本批登记：CHR-01（已修复并真机专项 PASS，见 §3）、CHR-03、CHR-11（仍开放，不因本批其它专项 PASS 而关闭）。
- INF-04 是跨插件快照并发缺陷，已由 `867b0974`/`a225ebcc` 修复；character 纳入统一快照实现。
- CHR-04/05/06/07/09/10/A5 修复批提交为 `e743e6a1`；本登记只确认下列真机专项，不扩张为全量角色接口验收。

## §2 无法验证项

| ID | 项 | 原因与范围 |
|---|---|---|
| CHR-03 | 6 条 Kotlin 501 占位路由的 op 门禁 | 门禁失效仍开放；本批未验证其 op 开/关两态。 |
| CHR-11 | 写后读己写的 15 帧窗口 | 仍开放；本批未做逐帧刷新时序验证。 |
| 快照冻结 + 告警 | 设备批制造快照读取失败的告警路径 | 本次设备条件无法制造读取失败，故不能验证冻结旧快照及告警行为；不据此宣告通过。 |

## §3 已完成验收登记

| 验收项 | 结果 | 证据 |
|---|---|---|
| `set_exp` INT32 边界 | **PASS（本专项）**：INT32_MAX 请求受理且 ticket `done`；溢出与负值均 HTTP 400 `exp required`，不产生票据。 | `evidence/set-exp-*`、`evidence/ticket-9-set-exp-max.json`；来源 `.tmp/triple-device-verify/` |
| `add_stat` 能力点守卫与合法路径 | **PASS（本专项）**：超剩余能力点 HTTP 400 `no status point`；合法加点 ticket `done`，属性与能力点差分正确。 | `evidence/add-stat-*`、`evidence/ticket-11-add-stat.json` |
| 保护备份与恢复 | **PASS（角色状态逐字节一致）**：备份导入成功；恢复前 `party.json` 与恢复后 `final-party.json` 相同。 | `evidence/backup-export.json`、`evidence/backup-import-final.json`、`evidence/party.json`、`evidence/final-party.json`、`evidence/ticket-10-final-restore.json` |
| CHR-01 `set_hp`/`set_mp` 权威变更链 | **PASS（专项）**：改按 `目标−当前` 调 `CHAR_AddLife`(0xEA184)/`CHAR_AddMana`(0xE0958)；真机写后不再被拉满、regen 正常、上限钳制正确。 | 提交 `98a63af1`；`.tmp/chr01-verify/` |
| core.character 审计批整体 | **NOT_ACCEPTED**：CHR-03/11 仍开放，且快照冻结失败路径未能验证；本表专项 PASS 不等于插件全量验收通过。 | 本节 §2；`docs/refactor/issues.md` CHR-01/03/11 |
