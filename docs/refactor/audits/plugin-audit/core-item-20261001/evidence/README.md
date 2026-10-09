# core.item 审计证据目录

本目录保存本批纳入仓库的可直接检查材料：

- `guard-matrix.json`：28 项守卫请求的路径、输入、HTTP 状态、错误体及 ticket ack 标记。
- `verification-summary.json`：守卫结果、备份恢复状态和未能采集的 `bag6 slot11` 写前态。
- `write-ticket-7.json`、`write-ticket-8.json`：恢复流程中的代表性 ticket 快照；不是 6 项 item 写回归的完整票据集。
- `restored-money.json`、`restored-inventory.json`、`restored-party.json`：保护备份恢复后的对应读面快照。

6 项写回归的完整逐请求记录未纳入仓库，因此本目录不能独立重放这 6 项测试。验收范围和结论以同目录 `verification-log.md` 的明确登记为准；不得把摘要当作完整原始证据。保护备份、全量 logcat 和其他过程文件也未纳入仓库。
