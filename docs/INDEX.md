# 文档导航

> 重构期内：**目标架构以根目录 `ARCHITECTURE.md` 为唯一权威**；旧开发文档已归档至
> `archive/docs/development/`，仅供历史参考与迁移取证，**不得作为落地依据**。

## 阅读入口

| 任务 | 首先阅读 | 继续阅读 |
|---|---|---|
| 了解项目和交付物 | 根目录 `README.md` | 本索引、`guides/` |
| 使用已构建模块 | `guides/game-guide.md` | `reference/api-reference.md` |
| 从源码构建和部署 | `guides/build-and-deploy.md` | 根目录 `AGENTS.md` |
| 修改 Kotlin/Native 代码 | 根目录 `ARCHITECTURE.md` | `AGENTS.md`、`docs/refactor/` |
| 继续重构工作 | `ARCHITECTURE.md` | `refactor/ledger.md`、`refactor/verification.md`、`refactor/baseline.md` |
| 理解 API 契约 | `reference/api-reference.md` | `guides/game-guide.md` |
| 理解原版游戏机制 | `reference/game/` | 归档的 `archive/docs/development/features/` |
| 查静态分析产物（解码 / 反编译 / 静态表） | `apk/decoded/`、`apk/decompiled/`、`apk/static-data/` | `guides/build-and-deploy.md` §1 |
| 查看历史过程 | `history/`、`archive/docs/development/` | 仅用于追溯，不作为当前实现依据 |

## 目录职责

### `refactor/`：重构执行文档

面向重构参与者与代理，只记录执行状态与证据，不重复架构理由（理由以 `ARCHITECTURE.md` 为准）。

- `baseline.md`：现状基线清单，**由脚本生成**（`uv run python scripts/maintenance/gen_baseline_doc.py`），禁止手工编辑。
- `ledger.md`：迁移台账（阶段/插件状态、目标归属、临时豁免与清除条件）。
- `verification.md`：验证矩阵与验收卡（PASS / NOT_ACCEPTED 定义与证据位置）。
- `adr/`：决策记录（只记录 `ARCHITECTURE.md` 未覆盖、实施中必须作出的长期决策）。

### `guides/`：使用指南

面向模块使用者、源码构建者和设备操作者，描述如何完成任务，不描述内部实现细节。

- `game-guide.md`：游戏与模块 API 的使用流程。
- `build-and-deploy.md`：环境、构建、设备连接、安装和验收。

### `reference/`：共享参考

面向使用者、开发者和 AI 代理，保存稳定、可复用的公共事实。

- `api-reference.md`：HTTP API 路由、参数、响应和状态码的唯一权威。
- `game/`：原版游戏系统和逆向结论。

### `history/`：历史归档

已结束的方案、交接、实验和旧版本记录；内容不得替代当前规则与状态。原 `development/` 全部内容已迁至 `archive/docs/development/`（含原 `architecture.md`、`logging.md`、`planning/`、功能设计文档与扩展背包七册）。

## 权威关系

- 项目总览：根目录 `README.md`
- **目标架构（重构期唯一权威）**：根目录 `ARCHITECTURE.md`
- 代理规则：根目录及各源码目录的 `AGENTS.md`
- 重构执行状态：`refactor/`（`baseline.md` 由脚本生成）
- API 契约：`reference/api-reference.md`
- 原版游戏事实：`reference/game/`
- 历史资料（仅供参考）：`history/`、`archive/docs/development/`

## 临时文件约定

当前任务的日志、截图和可重建中间文件统一写入 `.tmp/<task-name>/`，任务完成后清理；需要长期保留的证据必须迁移到 `docs/history/` 或 `archive/`。正式文档一律不得引用 `.tmp/...` 路径。

普通 Markdown 文档默认面向人类，同时保证 AI 可读取；`AGENTS.md` 只用于代理执行规则，不复制普通文档内容。
