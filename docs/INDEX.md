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
| 查静态分析产物（解码 / 反编译 / 静态表） | `apk/decoded/`、`apk/decompiled/`、`apk/static-data/` | `guides/build-and-deploy.md` §1 |
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
- `development/logging.md`：统一日志系统规范（级别、domain 词表、行格式、配置接线与合规检查的唯一权威）。
- `planning/`：当前待办和重构计划。
- `features/`：模块新增或修改功能的设计、实现和验收。

扩展背包按七册推荐顺序阅读：先读 Hub，再读规则册，最后按任务读取专题册；后续实现不得回到聊天记录恢复状态：

1. `development/features/extension-bag/control-plane.md`（Hub）
2. `development/features/extension-bag/rulebook.md`（规则册）
3. `development/features/extension-bag/runtime-architecture.md`（架构册）
4. `development/features/extension-bag/inventory-integration-decision-plan.md`（库存册）
5. `development/features/extension-bag/drag-protocol.md`（拖动册）
6. `development/features/extension-bag/module-save-store.md`（存档册）
7. `development/features/extension-bag/verification-matrix.md`（验收册）

存档管理器（save-backup）：`development/features/save-backup.md`（`.qol_save` 备份格式、导出/导入事务与跨槽/跨设备语义）。

属性显示范围（attribute-range-display）：`development/features/attribute-range-display.md`（宝石/装备详情按随机属性百分位着色的目标、结论与可行性测试记录；装备词缀范围与渲染注入已真机验证，未实现）。

自动出售（auto-sell）：`development/features/auto-sell.md`（背包页入口、品质阈值配置面板、扫描/保护/处置语义与风险验证计划；设计稿）。

合成器宝石合成操作优化（gem-craft-optimization）：`development/features/gem-craft-optimization.md`（宝石合成放料解绑配方、合成期同档校验与错误提示、自动选中格；设计已对齐，待实现）。

合成器自定义配方（custom-craft-recipe）：`development/features/custom-craft-recipe.md`（向混沌合成面板注入模块配方、选中时改写放料与合成按钮；配方 1 = 1 宝石 + 1 卓越灵药 → 宝石数值提升一档；表注入落位、档位定义、共存方案与 VM 验收卡；设计稿，未实现）。

简单模式（simple-mode）：`development/features/simple-mode.md`（怪物最大生命减半 + 玩家侧打敌人伤害 ×2 + 玩家侧受到伤害 ×0.5；双 Native Hook 落点、阵营判定原语、mod 跳板跟随与半血幂等账本；已实现并真机验证）。

帧派发宿主（frame-dispatch-host）：`development/features/frame-dispatch-host.md`（FrameTaskManager 是否可用 hook 重构、游戏帧周期 §2 逆向结论、锚点与相位模型、统一 `frame_tick` + `frame_host` 设计与分阶段实施、真机验证开放项；可行性调查与设计稿，未实现）。

进入存档回调（save-enter-callback）：`development/features/save-enter-callback.md`（存档生命周期回调：读档/新档加载完成进入 world 后触发一次；world→主菜单退出触发一次；call_patch 发起点 + frame_task world 就绪检测；切图返回不触发。进入回调已真机验证，退出回调未真机验证）。

原生选择框组件（native-choice）：`development/features/native-choice.md`（可复用 UICHOICE 选择框组件 `feature/ui/native_choice`：API、原版透传门禁、文本归属校验、单活动约束、世界传送集成与真机验证；并列后续可复用对话框组件候选 `native_popup` / `popup_state` / `native_dialog_router`）。

### `reference/`：共享参考

面向使用者、开发者和 AI 代理，保存稳定、可复用的公共事实。

- `api-reference.md`：API 路由、参数、响应和状态码的唯一权威。
- `game/`：原版游戏系统和逆向结论。

### `history/`：历史归档

保存已结束的方案、交接、实验和旧版本记录。历史内容不得替代当前文档中的规则和状态。

- `history/native-inventory-hook-development.md`
- `history/stage-0-static-inventory-audit.md`
- `history/stage-1-physical-inventory-contract.md`
- `history/stage-2-static-support-matrix.md`
- `history/stage-3-bypass-audit.md`
- `history/hp-clamp-offthread-incident.md`（HP 离线程钳制写回事故：经过、根因与同类排查；当前线程规则见 `development/architecture.md` §9.6）

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

当前任务的日志、截图和可重建中间文件统一写入 `.tmp/<task-name>/`，任务完成后清理；需要长期保留的证据必须迁移到 `docs/history/` 或 `archive/`。正式文档一律不得引用 `.tmp/...` 路径（扩展背包取证已迁至 `archive/extension-bag/`）。

普通 Markdown 文档默认面向人类，同时保证 AI 可读取；`AGENTS.md` 只用于代理执行规则，不复制普通文档内容。
