# core.quest 插件审计批（2026-10-02）验收登记

> 本登记记录本批验收范围与限制；不表示 core.quest 全量验收通过。完整问题定义以 `docs/refactor/issues.md` 为准，验收矩阵以 `docs/refactor/verification.md` 为准。设备为 monster v31。
> 提交链：`867b0974`（INF-04 Host SDK 快照 API minor 15）、`a225ebcc`（5 插件迁移统一快照）、`95b2d004`（quest native 严格解析）、`b48d5ab6`（quest Kotlin 原样透传）。

## §1 本批登记问题

问题正文与状态见 [`docs/refactor/issues.md`](../../../refactor/issues.md)，本节只登记编号与本批关联：

- 本批登记：QST-03（quest/world/inventory 读面逐字节 frozen 对比）。QST-01 随 INF-04 修复；QST-02 由 `95b2d004` 与 `b48d5ab6` 修复，真机边界专项见 §3。
- INF-04 已由 `867b0974`/`a225ebcc` 修复；quest 纳入统一快照实现。

## §2 无法验证项

| ID | 项 | 原因与范围 |
|---|---|---|
| QST-03 | quest/world/inventory 读面逐字节 frozen 对比 | **NOT_ACCEPTED**：本批真机未完成跨读面逐字节 frozen 对照；真机写守卫与合法任务命令通过不能替代此欠账。后续仍按 `verification.md` §5 记录的欠账处理。 |

## §3 已完成验收登记

| 验收项 | 结果 | 证据 |
|---|---|---|
| `quit_quest` 非法 `quest_id` 守卫 | **PASS（本专项）**：畸形、小数、尾随字符、溢出、负值、非数字均 HTTP 400 `questId required`，未产生票据。 | `evidence/invalid-*.txt`（逐项响应）；`evidence/ticket-invalid-probe-8.json`（不存在/未产生的探测票据状态） |
| 合法 `quest_id` 命令 | **PASS（本专项）**：合法 ID 返回 ticket，终态 `done`；结果保留。 | `evidence/legal-quest-5.txt`、`evidence/legal-ticket-8-final.json` |
| 保护备份恢复 | **PASS（恢复字段核对）**：备份导出记录在案；恢复后 money、bag0、party、status_point/exp、quest 与写前状态均记录为 IDENTICAL，恢复进档票据 `done`。 | `evidence/backup-export.json`、`evidence/restore-fields-summary-final.json`、`evidence/ticket-restore-enter-10-final.json` |
| 本次 quest 验证窗口崩溃计数 | **PASS（日志窗口）**：FATAL、SIGSEGV、SIGABRT、UnsatisfiedLinkError 均为 0；仅覆盖记录文件标注的窗口。 | `evidence/crash-counts.txt` |
| core.quest 审计批整体 | **NOT_ACCEPTED**：QST-03 仍欠逐字节 frozen 对比；已通过的输入守卫和写票据专项不替代该读面验收。 | 本节 §2；`docs/refactor/issues.md` QST-03 |
