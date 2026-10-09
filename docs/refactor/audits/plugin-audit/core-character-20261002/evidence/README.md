# 证据说明

证据从 `.tmp/triple-device-verify/` 选择性迁入，仅保留 character 写操作守卫/票据、恢复结果及备份记录；未复制 APK、整份 logcat 或完整运行目录。

| 文件 | 内容 |
|---|---|
| `set-exp-int32-max.json`、`set-exp-overflow.json`、`set-exp-negative.json` | 边界请求响应；溢出/负值分别返回 400 文案记录由 `verification-log.md` 明确限定。 |
| `ticket-9-set-exp-max.json` | INT32_MAX 成功票据终态。 |
| `add-stat-over-remaining.json`、`add-stat-legal.json`、`ticket-11-add-stat.json` | 能力点守卫响应与合法写票据终态。 |
| `backup-export.json`、`backup-import-final.json`、`party.json`、`final-party.json`、`ticket-10-final-restore.json` | 备份导出/导入、恢复前后角色状态及恢复票据。恢复前后 party body 逐字节一致。 |

源目录中没有为每个上述响应单独保存完整原始请求 body；本目录不补造请求体。其余无法验证项与范围见登记文档 §2。
