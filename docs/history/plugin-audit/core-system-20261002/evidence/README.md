# 证据说明

证据从 `.tmp/triple-device-verify/` 选择性迁入，仅保留 owner 生命周期响应、实际 system 响应样本与 identity；未复制 APK 或大体积 logcat。

| 文件 | 内容 |
|---|---|
| `core-system-deactivate.json`、`core-system-activate.json`、`ticket-15-deactivate.json`、`ticket-16-activate.json` | lifecycle 请求 ACK 与票据结果记录。 |
| `system-events-active.json`、`system-events-inactive.json` 及对应 headers | events active/inactive 响应与 HTTP 状态头。 |
| `system-frame-inactive.json`、`system-game-inactive.json`、`system-info-inactive.json`、`system-snapshot-inactive.json` | 四类 system owner 路由的停用态错误体；`system-events-inactive.json` 另列。 |
| `system-frame-active.json`、`system-game-active.json`、`system-info-active.json`、`system-snapshot-active.json`、`info-after-activate.json` | 恢复态 system 响应与 identity 字段。 |
| `events-body-sha256.txt` | 本批记录的 events body SHA-256。 |

原始运行日志未整体归档；FATAL/SIGSEGV/SIGABRT/UnsatisfiedLinkError 计数按本批记录为 0。文档漂移的结项范围见登记文档 §2。
