# Native 源码范围

- Native 层职责、依赖方向、游戏适配边界和跨语言契约以根目录 `ARCHITECTURE.md` 为准。
- 新增或修改日志时，等级、正文结构、频率与去重遵循 `docs/reference/logging-standards.md`；行格式、文件与崩溃取证见 `docs/reference/logging.md`。
- 领域行为和锁约束以对应插件参考及验收卡为准；扩展背包规则见 `docs/refactor/extension-bag/`。
- 拆分代码时只移动职责，不顺带改变无关业务语义。
- Native 改动按根目录 `AGENTS.md` 和 `docs/refactor/verification.md` 执行适用检查及验收。
