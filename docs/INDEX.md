# 文档导航

本文说明 `docs/` 内文档的目录、职责和阅读入口。目标架构见根目录 `ARCHITECTURE.md`。

## 按任务查找

| 任务 | 首选文档 | 相关文档 |
|---|---|---|
| 了解模块、安装和基本使用 | 根目录 `README.md` | `guides/game-guide.md` |
| 使用 HTTP API | `reference/api-reference.md` | `guides/game-guide.md` |
| 理解 API 服务开关与 OP 权限边界 | `refactor/api-optional-op-design.md` | `reference/api-reference.md` §7.6/§8、`reference/plugins/core.api.md` |
| 构建、部署与设备验收 | `guides/build-and-deploy.md` | `reference/api-reference.md`（涉及 API 验收时） |
| 修改源码或设计边界 | 根目录 `ARCHITECTURE.md` | `refactor/`、相关 `reference/plugins/` |
| 插件审计 | `refactor/plugin-audit-playbook.md` | `refactor/audits/`、`refactor/issues.md` |
| 配置 `enable` 与插件生命周期方案评审 | `refactor/config-enable-lifecycle-proposal.md` | `ARCHITECTURE.md` §7.2/§8.2、`reference/api-reference.md` §7.6 |
| 查询验证标准和当前欠账 | `refactor/verification.md` | `refactor/verification/`、`refactor/slot-data-contract.md` |
| 理解原版游戏机制 | `reference/game/` | `archive/` 中相关历史记录仅用于追溯 |

## 目录职责

### `reference/`：稳定参考资料

保存当前可复用的事实和契约。

- `api-reference.md`：HTTP API 路径、方法、参数、响应和状态码的唯一权威。
- `game/`：原版游戏系统与逆向结论。
- `game/equipment.md`：群友推荐的可获得与不可获得装备清单。
- `plugins/`：官方插件当前职责与实现参考。

插件参考：[`core.world`](reference/plugins/core.world.md)、[`core.item`](reference/plugins/core.item.md)、[`core.character`](reference/plugins/core.character.md)、[`core.quest`](reference/plugins/core.quest.md)、[`core.system`](reference/plugins/core.system.md)、[`core.craft`](reference/plugins/core.craft.md)、[`core.save`](reference/plugins/core.save.md)、[`settings`](reference/plugins/settings.md)、[`core.api`](reference/plugins/core.api.md)、[`core.custom_ui`](reference/plugins/core.custom_ui.md)、[`core.data`](reference/plugins/core.data.md)、[`core.ui_state`](reference/plugins/core.ui_state.md)、[`module_health`](reference/plugins/module_health.md)、[`attribute_quality`](reference/plugins/attribute_quality.md)。

### `guides/`：操作指南

说明读者如何完成一项任务，重点是步骤和操作结果。

- `game-guide.md`：游戏及模块 API 的使用流程。
- `build-and-deploy.md`：开发环境、构建、设备部署和验收流程。

### `refactor/`：当前重构、审计与验证材料

这里是仍在使用和维护的执行材料，新审计记录和验证证据写入此目录，不写入 `archive/`。

- `verification.md`：验证规则、验收矩阵与活跃债务唯一登记处。
- `verification/`：具体阶段或验收批次的验证记录及证据。
- `audits/`：插件审计记录与对应证据；后续审计沿用此目录。
- `issues.md`：已确认问题台账。
- `plugin-audit-playbook.md`：审计、修复、测试闭环流程。
- `config-enable-lifecycle-proposal.md`：已被实现模型取代（superseded）；现行契约见 ARCHITECTURE §8.2 与 API 参考 §7.6。
- `baseline.md`：脚本生成的结构基线。
- `map.md`：脚本生成的当前源码结构统计；不定义目标架构。
- `api-route-map.md`：API 路由核对材料。
- `api-optional-op-design.md`：API 服务归属、可选 HTTP 接线和通用 OP 授权合同。
- `audits/infrastructure-20261013/inspection.md`：近址 thunk mmap 检查与验证证据状态。
- `audits/plugin-audit/attribute-quality-20261014/verification-log.md`、`audits/plugin-audit/attribute-quality-20261014/design-card.md`：attribute_quality v2.0 审计登记与 VM-AQ-01 验收卡。
- `slot-data-contract.md`：Slot data 当前契约与状态。
- `extension-bag/rulebook.md`、`extension-bag/verification-matrix.md`：扩展背包当前规则和操作验收卡。
- `adr/`：架构决策记录。

### `archive/`：只读历史归档

保存已退役的开发资料、阶段记录、实验、交接、日志和证据。归档材料用于追溯，不作为当前规则或状态依据；**不得在此新建审计、验证或执行记录**。

## 文档权威关系

- 项目介绍与玩家入口：根目录 `README.md`。
- 目标架构及依赖边界：根目录 `ARCHITECTURE.md`。
- API 契约：`reference/api-reference.md`。
- 原版游戏机制事实：`reference/game/`。
- 当前问题、审计和验收状态：`refactor/`。
- 已结束的过程与历史证据：`archive/`，不覆盖当前文档。
