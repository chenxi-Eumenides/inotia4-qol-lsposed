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
| P4 | 批量迁移其余插件（内置静态轨） | 🔄 进行中（**阶段 A ✅**：A1–A4，证据 VM-P4-01..05） | 每插件：单测 + 验收卡；豁免清零 |
| P5 | `plugins/` 单层收口（ADR-0004）：`core.api` 传输内核、内置 API provider、`core.ui`、动态 capability 与注册口落地；JNI 分组、脚本收紧、删除旧路径（**P4-D0 已完成 `features/`→`plugins/` 更名；P5 只做 `core.api`/`core.ui`、动态能力、JNI 与脚本收紧**） | ⬜ 未开始 | 冻结 API 路径/响应兼容；路由 owner 表无重复/遗漏；动态能力发布/撤销与 route/capability lease 验收；`check_*` 全绿、豁免清零、单轨消失；U10 剩余 ABI 冻结 |
| P6（可选） | 外部插件轨（草案）：数据扩展 + 未签名全权 native | ⬜ 未开始 | schema 拒绝、用户显式启用、native 加载/禁用、托管资源回收、启动故障恢复与更新/回滚真机通过 |

## 2. 功能域迁移台账

规模为迁移期文件数（脚本统计口径见 `baseline.md` §6）。「已知债务」指该域在当前基线中的违规类型；
清除条件统一为：迁移到目标目录 + 通过 `check_arch.py` 且基线条目被移除。

| 域 | 目标归属 | 规模 | 机制占用 | 已知债务 | 迁移阶段 |
|---|---|---|---|---|---|
| `feature/ui` | `plugins/core.ui` + `plugins/text_layer` + 面板归各插件；基础层 `game/access`（`ui_kit`/`ui_components`） | 33 | entry hook ×4 安装者 | 已清除（符号头引用与域间依赖随迁移消除） | ✅ D1 完成（VM-D1 待真机） |
| `feature/extension_bag` | `plugins/extension_bag` | 32 | entry hook、call patch、ptr slot | 符号头引用；与 `feature/patch` 双向依赖 | P4 |
| `feature/patch` | `patch/` 机制 + `plugins/inventory_hooks` 业务 | 12 | entry hook、call patch | 符号头引用；被 7 个域依赖 | P3/P4 |
| `feature/autosell` | `plugins/autosell` | 10 | 帧任务消费者 | 域间依赖（→ attribute_range） | P4 |
| `feature/custom_recipe` | `plugins/custom_recipe` | 8 | entry hook（经 craft_ui） | 与 `feature/craft_ui` 双向 | P4 |
| `feature/attribute_range` | `plugins/attribute_range`（插件）+ `domains/attribute_range`（纯规则）+ `game/access`（界面接入） | 4 | entry hook ×3 | 已清除（符号头引用随迁移消除；feature→feature 依赖已消除） | ✅ P4-A3 完成（VM-P4-04） |
| `feature/craft_ui` | `plugins/craft_ui` | 4 | entry hook ×11（单一挂载处） | 与 `feature/custom_recipe` 双向 | P4 |
| `feature/save_backup` | `plugins/save_backup` | 4 | 无（纯逻辑 + 文件事务） | 被 `feature/ui` 依赖 | P4 |
| `feature/simple_mode` | `plugins/simple_mode` | 6 | entry hook ×2 | 已清除（符号头引用随迁移消除） | ✅ P2 完成（样板） |
| `feature/ui`（文本层子项） | `plugins/text_layer` | 4 | 共享资源 `hook:MEMORYTEXT_GetText`（owner）+ 能力 `text_layer` | 无（消费者改经 `hooks/dispatch` 分派，不再各自挂同址 hook） | ✅ P4-A4 完成（VM-P4-05） |
| `feature/world_teleport` | `plugins/world_teleport` | 4 | ptr slot、调用点 patch | 依赖 `feature/ui`（native_choice） | P4 |
| `feature/push_all` | `plugins/push_all` | 2 | call patch ×2 闸门 | 已清除（无已知域间依赖，干净样板） | ✅ P4-A1 完成（VM-P4-01） |
| `feature/quest` | `plugins/quest`（零资源常驻） | 2 | 帧节拍（进档回调暂借 legacy `save_enter` 设施） | 无已知域间依赖 | ✅ P4-A2 完成（VM-P4-03） |
| `feature/special_equip` | `plugins/special_equip` | 2 | entry hook ×2 | 无已知域间依赖 | P2 备选 |

> 说明：域间依赖与资源占用的完整清单见 `baseline.md` §2/§4；本表只做归属与阶段决策，不复制明细。

## 3. 临时豁免

当前 **1 条**（`scripts/maintenance/arch_exemptions.json`）：

- `resource_unknown_symbol` @ `plugins/core.ui/panels/settings.cpp`（主菜单「更多游戏」按钮运行时 ExecuteProc 槽）：
  该槽是运行时控件实例槽、非生命周期内稳定 GOT 槽，D1 暂以主线程帧任务管理绑定，`check_resources` 无法静态归属。
  清除阶段 **P5/U11**：动态 UI 绑定管理器建立后改申报 `own:ui.main_menu.more_games_entry`（kOwnership）＋运行期 lease。
  锚点：`ARCHITECTURE.md` §2.2 core.ui 自我约束 + §12.3 U11。

新增豁免必须包含：`规则名`、`路径/目标`、`理由`、`批准范围`、`清除阶段`、`到期条件`、`文档锚点`；
无文档锚点的豁免视为未登记（`check_layers` 会失败）。豁免与基线是两种机制：

- **基线**：既有债务，脚本自动维护，只允许收缩；
- **豁免**：有意保留的例外，人工登记，必须写清清除条件。

## 4. 已知债务摘要

| 规则 | 条数 | 说明 |
|---|---|---|
| `layer_direction` | 37 | 域间水平互调、feature↔feature、`game→infra/domains`、`infra→domains` |
| `offsets_symbols_header` | 12 | 非 `game/` 层直接 include 符号头 |
| `resource_unknown_symbol` | 2 | 间接安装点（符号在 `game_access` 全局解析），待人工归属 |

（本次 D1 冻结由 68 收缩至 **51**：D1-c0 72→68；D1-c 68→51，其中 `layer_direction` 43→37、
`offsets_symbols_header` 19→12、`resource_unknown_symbol` 6→2。另 1 条 `resource_unknown_symbol`
为 §3 登记的有意豁免，不计入基线债务。）

完整清单与样例见 `docs/refactor/baseline.md` §3；核对命令：`uv run python scripts/maintenance/check_arch.py`。

## 5. 跨分支事项

| # | 事项 | 状态 | 说明 |
|---|---|---|---|
| 1 | 变体表补入 monster v27/v29 | ✅ 已在重构分支完成 | 修复 v27 用户 `known=0`（能力位误为 0）；主线仍需 cherry-pick（用户决定合并节奏） |
| 2 | 主线 `1d8bb97`（R-67/R-68 闪退修复）合入重构分支 | ⬜ 待办（用户指示暂不合并） | 合并前重构分支不含该修复 |

## 6. P4 执行顺序与依赖

原则：**依赖驱动 + 机制先行**——每个新机制先用最小载体点亮并冻结接口，再迁大件；大件提前"开拆"冻结接口、实现放最后。
约束（ADR-0003，2026-09-17 修订）：**串行 lane 在集成分支执行**（单写者隔离；若需改核心，核心件先在集成分支单独提交冻结，再提交插件迁移）；**并行 lane 才开子分支 + worktree**；`contracts/`、`infra/`、`hooks/`、`patch/` 同一时间只允许一条 lane 修改。
排序说明：**B2 余项（B2a-3 `ptr_slot`+`move_merge`、B2b `extension_bag` 契约+出售链、B2c wrapper 业务搬迁）与 D2 同期**（全部与 extension_bag 强耦合，避免两次返工）；B3/B4/C 独立件先行。
规划载体约定：**每域不单独建"规划书"**——规划=本节 + §2 台账；实现规格=任务规格（临时，不落库）；证据=`verification.md` 验收卡。

### 阶段 A：机制铺路（核心串行）

（核心机制 + 最小验证载体：集成分支串行）

| # | lane | 载体（规模） | 点亮机制 | 状态 |
|---|---|---|---|---|
| A1 | `patch/` 机制 + `push_all` | push_all（85 行） | 指令 patch、占用声明、原字校验与回滚 | ✅ 完成（VM-P4-01 PASS：两 patch live、BL 重定向日志证据、物理还原；**用户真机确认推怪生效**） |
| A2 | 帧源/调度形式化 + `quest` | quest（79 行） | `infra/scheduler` + 帧源注入 | ✅ 完成（VM-P4-02/03 PASS：core 两条 `live`、命令排空经新 scheduler；quest 零资源插件 `enabled` + 进档 `quest resync: fixed=0` 日志。遗留：`save_enter` 回调设施未迁移，随存档域收口） |
| A3 | 能力契约 + `attribute_range` | attribute_range（396 行） | capability 提供/消费 + 撤销级联（§3.4.1） | ✅ 完成（VM-P4-04 PASS：3 条 hook `live`；deactivate → `inactive`+全 `free`（逆序归还+注销能力）；activate → `enabled`+全 `live`；feature→feature 依赖已消除） |
| A4 | `hooks/dispatch` + `ui` 文本层拆分 | `MEMORYTEXT_GetText` 共享资源 | before/original/after、Observe/Modify/Intercept | ✅ 完成（VM-P4-05 PASS：共享资源 `live`；deactivate → `free`、重复动作被拒（ticket failed）、activate → `live`；撤销期文本透传正常；设计 v0.3.8 阶段语义） |

### 阶段 B：依赖对与并行小件

| # | lane | 规模 | 依赖 | 状态 |
|---|---|---|---|---|
| B1 | `special_equip` | 272 行 | —（入口 hook 机制已由 P2 验证，可并行） | ✅ 完成（VM-P4-06 PASS：两 hook `live`；**用户真机确认功能正常**——普通队友/主角可穿脱专属位装备且可售，特殊 NPC 队友维持原版锁定） |
| B2 | `feature/patch` 业务拆分 | 2,888 行 | A1、A4 | 🔄 进行中（**B2a-1 ✅**：VM-P4-07 PASS——24/24 `live` + 用户冒烟正常；**B2a-2 ✅**（VM-P4-08：core 15/15 `live`、`stack_limit` 两态、启动守卫缺陷修复）；余：extension_bag 契约、wrapper 业务搬迁 B2c、`move_merge`/ptr_slot B2a-3） |
| B3 | `autosell` | 1,030 行 | A3 | ✅ 完成（VM-44——用户真机确认自动出售正确；证据：host 130+102 断言、基线 90→85） |
| B4 | `save_backup` + `infra/store`（`infra/store` 属核心件：先在集成分支落地并冻结，再开插件 lane） | 2,371 行 | A2 | ✅ 完成（B4-1 容器层 / B4-2 单写者 / B4-3 autosell 直连 / B4-4 save_backup 接 store / B4-5 本体迁移；用户回归通过：autosell 持久化、扩展背包存读档、存档导出/导入/删档；UI 面板留 D1） |
| W1 | 扩展背包：商店出售支持数量选择（部分出售） | 插单（用户请求；**非 B2 回归**——商店投影出售自 `0858eb2` 起即为模块接管整堆事务，无部分出售实现） | 受影响规则：R-12/R-13/R-47 决策 b/R-55（须全量读扩展背包规则书与 VM 卡） | ✅ 完成（VM-43——用户真机确认：部分出售/全量/取消/上限随开关均正常；证据归档 `docs/history/store-sell-disassembly.md`） |

### 阶段 C：中件

| # | lane | 规模 | 依赖 | 状态 |
|---|---|---|---|---|
| C1 | `world_teleport` | 525 行 | A4 | ✅ 完成（VM-45 用户真机确认；基线 83→81） |
| C2 | `craft_ui` + `custom_recipe`（同 lane，双向依赖） | 2,597 行 | A4 | ✅ 完成（VM-46 用户真机确认；含 3 处修复：停用 fail-closed、门禁 fail-soft、停用还原原版组位；基线 81→72） |

### 阶段 D：大件

| # | lane | 规模 | 依赖 | 状态 |
|---|---|---|---|---|
| D1 | `ui` 主体（拆 `ui_kit` / 文本 / 面板） | 5,320 行（实测 31 文件） | B3 ✅、C2 ✅ | ✅ 完成（D1-a1/a2/b0/b1/b2/b3/b4/b4a/c0；待真机卡 VM-D1）——共享 UI 原语下沉 `game/access`，文本层归 `text_layer`，面板归 `core.ui`/`autosell`/`save_backup`/`native_choice`；D1-c0 删 `custom` 与 `exp1-5` 实验面；D1-c 清 `UiPopupState*` 死代码、基线 68→51 |
| D2 | `extension_bag` | 12,834 行 | A1、B2、B4、D1 | ⬜ |

> **D1 延后项登记**（D1 未实现，已指定目标阶段；对应 `map.md` 过渡依赖区）：
> - 设置项数据化与配置源（Kotlin JSON → native config）：**P5**（决策 D；`ARCHITECTURE.md` §7.3 跨栈边界）。
> - U11 ui-kit adapter（动态 UI 绑定管理器）：**P5**；覆盖 `plugins/core.ui/panels/settings.cpp` 运行时 ptr-slot 的临时豁免（`arch_exemptions.json`）。
> - `core.ui` 直依 `game/access/ui*`（过渡依赖）：随 **P5** `core.ui` 收口清除。

### 迁移边界（包含什么 / 不包含什么）

- **包含**（目标架构强制，属迁移本身）：拆层与拆分（机制/业务、`ui_kit` 基础层）、接口替换（plugins 不触符号头 → `game/access`）、依赖方向修正（消除 feature↔feature）、hook/patch 声明式化（资源声明 + 事务）。
- **不包含**（推迟或明确不做）：业务语义重写、算法与性能优化（性能阈值走 §10 / U1 回填）、顺手重构无关代码（AGENTS：只移动职责）。
- 迁移中发现的缺陷单独提交，不与迁移混在一起（例：P2 的 v27 变体表缺口）。
