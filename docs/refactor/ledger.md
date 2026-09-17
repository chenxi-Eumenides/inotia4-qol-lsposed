# 重构迁移台账

> 定位：**执行状态台账**。架构规则与理由见根目录 `ARCHITECTURE.md`；现状结构事实见 `baseline.md`（脚本生成）。
> 本文件只记录：阶段状态、功能域归属与债务、临时豁免与清除条件。**不重复解释架构理由。**

## 1. 阶段状态

| 阶段 | 内容 | 状态 | 出口验收 / 证据 |
|---|---|---|---|
| P0 | 校验脚本族 + 基线棘轮冻结 | ✅ 完成 | `check_arch.py` 可运行；基线冻结 124 条（提交 `050e93c`） |
| — | 目标架构设计书入库 | ✅ 完成 | 根目录 `ARCHITECTURE.md` v0.3（提交 `1fa9ef2`） |
| P1 | 骨架：`contracts/` + `infra/` + `game/`；最小诊断接口 | ✅ 完成 | host 20/20（`e472d52`）；`GET /api/system/features` 与 `/api/health`（加法扩展 ready/variant）真机通过（`18f0a91` + 后续提交，卡见 `verification.md` VM-P1-02/03） |
| P2 | 纵向样板插件（入口 hook + 配置 + 状态 + 命令端到端）：simple_mode | ✅ 完成 | 真机卡 VM-P2-01 PASS（两态翻转 + 状态一致 + 半血 A/B 135↔270 + 无崩溃）；host 23/23；基线 124→121 |
| P3 | 资源事务（异步步进/回滚/在途归零/帧预算）+ `disable/deactivate/tombstone` 全路径 | ✅ 完成 | 真机卡 VM-P3-01/VM-P3-03 PASS（deactivate→`free`、物理归还 A/B `hp=270`、activate 重挂载 `hp=135`，U6 闭合；10 态 API + tickets `pending→done`）；故障注入 host-only（VM-P3-02，277+115 断言）；host 25/25 |
| P4 | 批量迁移其余插件（内置静态轨） | ⬜ 未开始 | 每插件：单测 + 验收卡；豁免清零 |
| P5 | `products/` + `host/` 收口；脚本收紧；删除旧路径 | ⬜ 未开始 | `check_*` 全绿、双轨消失 |
| P6（可选） | 外部插件轨（草案） | ⬜ 未开始 | 导入/拒绝/禁用/崩溃兜底真机通过 |

## 2. 功能域迁移台账

规模为迁移期文件数（脚本统计口径见 `baseline.md` §6）。「已知债务」指该域在当前基线中的违规类型；
清除条件统一为：迁移到目标目录 + 通过 `check_arch.py` 且基线条目被移除。

| 域 | 目标归属 | 规模 | 机制占用 | 已知债务 | 迁移阶段 |
|---|---|---|---|---|---|
| `feature/ui` | `features/ui` + 基础层拆分（`ui_kit`） | 33 | entry hook ×4 安装者 | 符号头引用 ×10；域间依赖（→ autosell/patch/save_backup） | P2（样板候选）/P4 |
| `feature/extension_bag` | `features/extension_bag` | 32 | entry hook、call patch、ptr slot | 符号头引用；与 `feature/patch` 双向依赖 | P4 |
| `feature/patch` | `patch/` 机制 + `features/inventory_hooks` 业务 | 12 | entry hook、call patch | 符号头引用；被 7 个域依赖 | P3/P4 |
| `feature/autosell` | `features/autosell` | 10 | 帧任务消费者 | 域间依赖（→ attribute_range） | P4 |
| `feature/custom_recipe` | `features/custom_recipe` | 8 | entry hook（经 craft_ui） | 与 `feature/craft_ui` 双向 | P4 |
| `feature/attribute_range` | 纯规则 → `core`/`game`；界面 → `features/ui` | 4 | entry hook ×3 | 被 3 个域依赖；符号头引用 | P4 |
| `feature/craft_ui` | `features/craft_ui` | 4 | entry hook ×11（单一挂载处） | 与 `feature/custom_recipe` 双向 | P4 |
| `feature/save_backup` | `features/save_backup` | 4 | 无（纯逻辑 + 文件事务） | 被 `feature/ui` 依赖 | P4 |
| `feature/simple_mode` | `features/simple_mode` | 6 | entry hook ×2 | 已清除（符号头引用随迁移消除） | ✅ P2 完成（样板） |
| `feature/world_teleport` | `features/world_teleport` | 4 | ptr slot、调用点 patch | 依赖 `feature/ui`（native_choice） | P4 |
| `feature/push_all` | `features/push_all` | 2 | call patch ×2 闸门 | 无已知域间依赖（干净样板） | P2 备选 |
| `feature/quest` | `features/quest` | 2 | 进档回调 | 无已知域间依赖 | P4 |
| `feature/special_equip` | `features/special_equip` | 2 | entry hook ×2 | 无已知域间依赖 | P2 备选 |

> 说明：域间依赖与资源占用的完整清单见 `baseline.md` §2/§4；本表只做归属与阶段决策，不复制明细。

## 3. 临时豁免

当前 **0 条**（`scripts/maintenance/arch_exemptions.json` 为空）。

新增豁免必须包含：`规则名`、`路径/目标`、`理由`、`批准范围`、`清除阶段`、`到期条件`、`文档锚点`；
无文档锚点的豁免视为未登记（`check_layers` 会失败）。豁免与基线是两种机制：

- **基线**：既有债务，脚本自动维护，只允许收缩；
- **豁免**：有意保留的例外，人工登记，必须写清清除条件。

## 4. 已知债务摘要

| 规则 | 条数 | 说明 |
|---|---|---|
| `layer_direction` | 70 | 域间水平互调、feature↔feature、`game→infra/domains`、`infra→domains` |
| `offsets_symbols_header` | 33 | 非 `game/` 层直接 include 符号头 |
| `resource_unknown_symbol` | 18 | 间接安装点（符号在 `game_access` 全局解析），待人工归属 |

（P2 由 124 收缩至 121：simple_mode 迁移消除 3 条。）

完整清单与样例见 `docs/refactor/baseline.md` §3；核对命令：`uv run python scripts/maintenance/check_arch.py`。

## 5. 跨分支事项

| # | 事项 | 状态 | 说明 |
|---|---|---|---|
| 1 | 变体表补入 monster v27/v29 | ✅ 已在重构分支完成 | 修复 v27 用户 `known=0`（能力位误为 0）；主线仍需 cherry-pick（用户决定合并节奏） |
| 2 | 主线 `1d8bb97`（R-67/R-68 闪退修复）合入重构分支 | ⬜ 待办（用户指示暂不合并） | 合并前重构分支不含该修复 |

## 6. P4 执行顺序与依赖

原则：**依赖驱动 + 机制先行**——每个新机制先用最小载体点亮并冻结接口，再迁大件；大件提前"开拆"冻结接口、实现放最后。
约束（ADR-0003）：阶段 A（核心机制 + 最小验证载体）在**集成分支串行**；阶段 B 起独立插件迁移每 lane 一条子分支 + worktree；若某 lane 需改核心（如 B4 的 `infra/store`），**先在集成分支完成并冻结该核心件**，再开插件子分支；`contracts/`、`infra/`、`hooks/`、`patch/` 同一时间只允许一条 lane 修改。
规划载体约定：**每域不单独建"规划书"**——规划=本节 + §2 台账；实现规格=任务规格（临时，不落库）；证据=`verification.md` 验收卡。

### 阶段 A：机制铺路（核心串行）

（核心机制 + 最小验证载体：集成分支串行）

| # | lane | 载体（规模） | 点亮机制 | 状态 |
|---|---|---|---|---|
| A1 | `patch/` 机制 + `push_all` | push_all（85 行） | 指令 patch、占用声明、原字校验与回滚 | ✅ 完成（VM-P4-01 PASS：两 patch live、BL 重定向日志证据、物理还原；**用户真机确认推怪生效**） |
| A2 | 帧源/调度形式化 + `quest` | quest（79 行） | `infra/scheduler` + 帧源注入 | ✅ 完成（VM-P4-02/03 PASS：core 两条 `live`、命令排空经新 scheduler；quest 零资源插件 `enabled` + 进档 `quest resync: fixed=0` 日志。遗留：`save_enter` 回调设施未迁移，随存档域收口） |
| A3 | 能力契约 + `attribute_range` | attribute_range（396 行） | capability 提供/消费 + 撤销级联（§3.4.1） | ✅ 完成（VM-P4-04 PASS：3 条 hook `live`；deactivate → `inactive`+全 `free`（逆序归还+注销能力）；activate → `enabled`+全 `live`；feature→feature 依赖已消除） |
| A4 | `hooks/dispatch` + `ui` 文本层拆分 | `MEMORYTEXT_GetText` 共享资源 | before/original/after、Observe/Modify/Intercept | ⬜ |

### 阶段 B：依赖对与并行小件

| # | lane | 规模 | 依赖 | 状态 |
|---|---|---|---|---|
| B1 | `special_equip` | 272 行 | —（入口 hook 机制已由 P2 验证，可并行） | ⬜ |
| B2 | `feature/patch` 业务拆分 | 2,888 行 | A1、A4 | ⬜ |
| B3 | `autosell` | 1,030 行 | A3 | ⬜ |
| B4 | `save_backup` + `infra/store`（`infra/store` 属核心件：先在集成分支落地并冻结，再开插件 lane） | 2,371 行 | A2 | ⬜ |

### 阶段 C：中件

| # | lane | 规模 | 依赖 | 状态 |
|---|---|---|---|---|
| C1 | `world_teleport` | 525 行 | A4 | ⬜ |
| C2 | `craft_ui` + `custom_recipe`（同 lane，双向依赖） | 2,597 行 | A4 | ⬜ |

### 阶段 D：大件

| # | lane | 规模 | 依赖 | 状态 |
|---|---|---|---|---|
| D1 | `ui` 主体（拆 `ui_kit` / 文本 / 面板） | 5,534 行 | B3、C2 | ⬜ |
| D2 | `extension_bag` | 12,834 行 | A1、B2、B4、D1 | ⬜ |

### 迁移边界（包含什么 / 不包含什么）

- **包含**（目标架构强制，属迁移本身）：拆层与拆分（机制/业务、`ui_kit` 基础层）、接口替换（features 不触符号头 → `game/access`）、依赖方向修正（消除 feature↔feature）、hook/patch 声明式化（资源声明 + 事务）。
- **不包含**（推迟或明确不做）：业务语义重写、算法与性能优化（性能阈值走 §10 / U1 回填）、顺手重构无关代码（AGENTS：只移动职责）。
- 迁移中发现的缺陷单独提交，不与迁移混在一起（例：P2 的 v27 变体表缺口）。
