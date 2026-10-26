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
| 插件重构架构指导 | `refactor/plugin-architecture-guide.md` | `refactor/extension-bag/final-architecture.md` |
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

插件参考：[`core.world`](reference/plugins/core.world.md)、[`core.item`](reference/plugins/core.item.md)、[`core.character`](reference/plugins/core.character.md)、[`core.quest`](reference/plugins/core.quest.md)、[`core.system`](reference/plugins/core.system.md)、[`core.craft`](reference/plugins/core.craft.md)、[`craft_lottery`](reference/plugins/craft_lottery.md)、[`core.save`](reference/plugins/core.save.md)、[`settings`](reference/plugins/settings.md)、[`core.api`](reference/plugins/core.api.md)、[`core.custom_ui`](reference/plugins/core.custom_ui.md)、[`core.data`](reference/plugins/core.data.md)、[`core.ui_state`](reference/plugins/core.ui_state.md)、[`module_health`](reference/plugins/module_health.md)、[`attribute_quality`](reference/plugins/attribute_quality.md)、[`autosell`](reference/plugins/autosell.md)、[`custom_recipe`](reference/plugins/custom_recipe.md)、[`attribute_range_display`](reference/plugins/attribute_range_display.md)、[`character_panel_i18n`](reference/plugins/character_panel_i18n.md)、[`push_all`](reference/plugins/push_all.md)、[`world_teleport`](reference/plugins/world_teleport.md)、[`custom_map`](reference/plugins/custom_map.md)、[`stack_limit`](reference/plugins/stack_limit.md)、[`extension_bag`](reference/plugins/extension_bag.md)、[`save_backup`](reference/plugins/save_backup.md)、[`special_equip`](reference/plugins/special_equip.md)、[`simple_mode`](reference/plugins/simple_mode.md)、[`popup_blocker`](reference/plugins/popup_blocker.md)。

模块平台兜底提示（非业务插件）：[`module_error_dialog`](reference/plugins/module_error_dialog.md)。

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
- `plugin-architecture-guide.md`：从 extension_bag 重构沉淀的通用分层/线程/数据/迁移准则，供其他插件参照。
- `custom-recipe/refactor-plan.md`：custom_recipe 整改方案（表槽位地址下沉到 game 端口、目录分层、主线程契约；含提交切分与验证矩阵）。
- `extension-bag/refactor-plan.md`：扩展背包插件分层与内部组件重构方案（执行契约、组件边界、分阶段迁移）。
- `audits/infrastructure-20261013/inspection.md`：近址 thunk mmap 检查与验证证据状态。
- `audits/ui-handle-lifecycle-20261021/delete-path-audit.md`：游戏控件删除路径审计（C 方案：`TouchHandle_DeleteControl` 不递归、推荐 `ControlObject_Delete` 入口；袋容器子树释放链 UNPROVEN）。
- `audits/plugin-audit/attribute-quality-20261014/verification-log.md`、`audits/plugin-audit/attribute-quality-20261014/design-card.md`：attribute_quality v2.0 审计登记与 VM-AQ-01 验收卡。
- `audits/plugin-audit/custom-recipe-20261014/`：custom_recipe 单 owner 合并的设计/实施记录（`design-card.md`）、静态 finding（`static-audit.md`）与审阅登记（`verification-log.md`）；合并验收卡 VM-CR-MERGE-01 仍 `NOT_ACCEPTED`。
- `audits/plugin-audit/device-probe-export-failure-20261016/verification-log.md`：Debug-only 探针 `device_probe_export_failure` 审阅与删除登记（零消费者、无独占断言覆盖面；探针能力保留在 host 侧 `probe_entry_export_resolution()`）；真机 probe 行为 `NOT_ACCEPTED`。
- `audits/plugin-audit/simple-mode-20261022/verification-log.md`：`simple_mode` 审阅登记（12 findings、5 项用户裁决、Host 153/153 与构建证据；设备面 `NOT_ACCEPTED`）。
- `slot-data-contract.md`：Slot data 当前契约与状态。
- `extension-bag/rulebook.md`、`extension-bag/verification-matrix.md`：扩展背包当前规则和操作验收卡。
- `extension-bag/display-hook.md`：装备页/商店/合成投影读取虚拟化（`ControlItem_SetItem/GetItem`、`INVEN_GetBagSize` hooks；不写 `INVEN_pItem` 或容量字）。
- `custom-ui/refactor-plan.md`：`core.custom_ui` 局部拆分方案（Debug 编译门、形状绘制抽离、确认框 overlay 可选拆分；不做 L0–L6 全量重构）。
- `core-character/architecture.md`：`core.character` 收敛方案（写面判定单点化、路由表 drift 防护、解析器合并、快照只读视图；不做 L0–L6 全量重构）。
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
