# 证据说明

证据从 `.tmp/quest-verify2/` 选择性迁入；未复制 APK、`.qol_save` 文件或大体积 logcat。

| 文件 | 内容 |
|---|---|
| `invalid-fractional.txt`、`invalid-malformed.txt`、`invalid-negative.txt`、`invalid-nonnumeric.txt`、`invalid-overflow.txt`、`invalid-trailing.txt` | 对应非法 quest_id 请求的逐项响应记录。 |
| `legal-quest-5.txt`、`legal-ticket-8-final.json` | 合法命令与成功票据终态。 |
| `ticket-invalid-probe-8.json` | 对非法请求票据号的查询，返回 `unknown`。 |
| `backup-export.json`、`restore-fields-summary-final.json`、`ticket-restore-enter-10-final.json` | 备份登记、恢复字段比较与恢复进档票据。 |

源记录没有将每个无效输入的原始请求 body 与响应封装在同一文件；保留了现有逐项响应文件，不从其外推未存档请求字节。读面逐字节 frozen 欠账见登记文档 §2。
