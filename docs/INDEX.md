# 文档导航

本目录按文档用途划分为四类：使用指南、开发维护、共享参考和历史归档。

## 阅读入口

| 任务 | 首先阅读 | 继续阅读 |
|---|---|---|
| 了解项目和交付物 | 根目录 `README.md` | 本索引、`guides/` |
| 使用已构建模块 | `guides/game-guide.md` | `reference/api-reference.md` |
| 从源码构建和部署 | `guides/build-and-deploy.md` | 根目录 `AGENTS.md`、`development/architecture.md` |
| 修改 Kotlin/Native 代码 | 根目录 `AGENTS.md` | `development/architecture.md`、相关功能文档 |
| 理解 API 契约 | `reference/api-reference.md` | `guides/game-guide.md` |
| 理解原版游戏机制 | `reference/game/` | 相关 `development/features/` |
| 继续重构工作 | `development/planning/refactor-plan.md` | `development/planning/backlog.md` |
| 查看历史过程 | `history/` | 仅用于追溯，不作为当前实现依据 |

## 目录职责

### `guides/`：使用指南

面向模块使用者、源码构建者和设备操作者，描述如何完成任务，不描述内部实现细节。

- `game-guide.md`：游戏与模块 API 的使用流程。
- `build-and-deploy.md`：环境、构建、设备连接、安装和验收。

### `development/`：开发维护

面向代码维护者和 AI 代理，描述如何修改、扩展和验证项目。

- `development/architecture.md`：当前代码结构、分层和工程约束。
- `planning/`：当前待办和重构计划。
- `features/`：模块新增或修改功能的设计、实现和验收。

扩展背包 P7 的阶段基线按以下四篇独立文档读取；后续实现不得回到聊天记录恢复状态：

- `development/features/extension-bag/stage-0-static-inventory-audit.md`
- `development/features/extension-bag/stage-1-physical-inventory-contract.md`
- `development/features/extension-bag/stage-2-static-support-matrix.md`
- `development/features/extension-bag/stage-3-bypass-audit.md`

### `reference/`：共享参考

面向使用者、开发者和 AI 代理，保存稳定、可复用的公共事实。

- `api-reference.md`：API 路由、参数、响应和状态码的唯一权威。
- `game/`：原版游戏系统和逆向结论。

### `history/`：历史归档

保存已结束的方案、交接、实验和旧版本记录。历史内容不得替代当前文档中的规则和状态。

## 权威关系

- 项目总览：根目录 `README.md`
- 代理规则：根目录及各源码目录的 `AGENTS.md`
- 当前代码规范：`development/architecture.md`
- API 契约：`reference/api-reference.md`
- 当前待办：`development/planning/backlog.md`
- 功能状态与验收：对应 `development/features/` 文档
- 原版游戏事实：`reference/game/`
- 历史资料：`history/`

## 临时文件约定

当前任务的日志、截图和可重建中间文件统一写入 `.tmp/<task-name>/`，任务完成后清理；需要长期保留的证据必须迁移到 `docs/history/` 或 `archive/`。现有文档中出现的 `.tmp/...` 路径属于历史证据引用，不构成新的存储位置规范。

普通 Markdown 文档默认面向人类，同时保证 AI 可读取；`AGENTS.md` 只用于代理执行规则，不复制普通文档内容。
