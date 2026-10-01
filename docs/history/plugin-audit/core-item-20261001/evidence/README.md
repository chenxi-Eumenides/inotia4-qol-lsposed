# core.item 审计证据目录

本目录从 `.tmp/core-item-guard-verify3/` 选取关键证据迁入，未照搬任务目录：

- `guard-matrix.json`：28 项守卫请求的路径、输入、HTTP 状态、错误体及 ticket ack 标记。
- `verification-summary.json`：守卫总数、无副作用结果、备份恢复状态和未能采集的 `bag6 slot11` 写前态。
- `write-ticket-7.json`、`write-ticket-8.json`：本轮恢复流程中的代表性 ticket 快照（不是 6 项 item 写回归的完整票据集；其中 ticket 7 是 `not in game` 失败，ticket 8 是 enter-slot 恢复成功）。
- `restored-money.json`、`restored-inventory.json`、`restored-party.json`：保护备份恢复后的对应读面快照。

6 项写回归均为 `done` 的完整源记录位于原临时任务目录 `.tmp/core-item-guard-verify3/write-regression.json`，该大文件未整体复制；验收登记只记录用户提供的汇总事实。源保护备份 `.qol_save`、大体积 logcat 及其它过程性文件未迁入。早期过程材料另见 `.tmp/core-item-audit/`、`.tmp/core-item-guard-verify/` 和 `.tmp/core-item-guard-verify2/`；它们不作为本登记表中新增结论的替代证据。
