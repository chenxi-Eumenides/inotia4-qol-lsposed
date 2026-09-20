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
| `feature/extension_bag` | `plugins/extension_bag` | 32 | entry hook、call patch、ptr slot | 已清除（符号头引用与 feature/patch 双向依赖随 D2c-2 消除） | ✅ D2c-2 完成（feature/ 目录已空；余 D2d 收尾） |
| `feature/patch` | 已删除（D2c-2b-2）：inventory 业务 → `plugins/inventory_hooks`；move_merge/X1 → `plugins/extension_bag`；IAP → `game/access`；机制 → `patch/`/`host/` | 12 | entry hook、call patch | 已清除（域删除） | ✅ D2c-2 完成 |
| `feature/autosell` | `plugins/autosell` | 10 | 帧任务消费者 | capability 消费（→ attribute.quality） | P4 |
| `feature/custom_recipe` | `plugins/custom_recipe` | 8 | entry hook（经 craft_ui） | 与 `feature/craft_ui` 双向 | P4 |
| `feature/attribute_range` | `plugins/attribute_quality`（provider）+ `plugins/attribute_range_display`（消费者）+ `game/access`（Host 原语） | 8 | entry hook ×3 | L3 拆分完成；provider/display 仅经 contracts 与 Host SDK/Game Host 协作 | L3 host-only 验证中 |
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

> 权威来源：`scripts/maintenance/arch_baseline.json`（2026-09-19 P0 实测，HEAD `6fc6098`）。
> **基线"通过"只代表无新增违规，不代表七目录合规**（审阅 §9.2）。

| 规则 | 基线条数 | 实测现存 | 说明 |
|---|---|---|---|
| `layer_direction` | 32 | 32 | **全部是 legacy 目录内部越层边**：`api→api` 5、`core→api` 4、`core→data` 4、`core→game_access.h` 4、`core→core` 3、`core→game_system.h` 2、`core→game_state.h` 2、`game_dialog.cpp→api` 3，以及 `data→core`、`game_access.cpp→core`、`game_access.cpp→game_system.h`、`game_state.cpp→api`、`game_state.cpp→core` 各 1。**不含插件的 legacy include**——因 `arch_common.allows()` 目前放行 `plugins→game/infra/hooks/patch/domains` |
| `offsets_symbols_header` | 11 | 9 | 6 条 `core/native/*` + 5 条插件（`extension_bag` 2、`custom_recipe` 2、`inventory_hooks` 1） |
| `resource_unknown_symbol` | 2 | 3（豁免 1） | 均为 `extension_bag` ptr slot 间接安装点（`extension_bag_equip.inc`、`game_ui_virtbag.cpp`） |
| **合计** | **45** | **44** | 另 1 条豁免（`plugins/core.ui/panels/settings.cpp` 运行时控件实例槽，§3）不计入基线 |

**基线未覆盖、但审阅已核验成立的债务与目标设计问题**（P0 新登记，详表见 `layers.md` §五）：

1. **插件 legacy 私有 include 19 文件**（`core/native/**`、`data/native/**`、根部裸名头）——因过渡规则被 `allows()` 放行，**当前不在任何违规计数内**（P3 清除）。
2. **`arch_common.py` 过渡规则**：`BASE_LAYERS` 含 `domains`；`allows()` 放行 `plugins→game/infra/hooks/patch/domains`；`_ROOT_OVERRIDES`/`_CORE_OVERRIDES`/`_TARGET_PREFIXES` 三张映射（P3 删除并硬失败）。
3. **单一 `gamebridge` target**：无独立插件 target，无法证明插件可独立编译/不链接宿主私有符号（P3）。
4. **`QolGameHostExtraApi` 过宽**（15 处 `extra.*` 注入）——目标设计问题（P4 拆分）。
5. **`contracts` 全局业务函数**（`autosell.h:44-46`）仍绕过 capability；`stack_limit.h` 的两个全局函数已在 P1-B5c 删除。**`host/core_patches` 把 S2 与 hub 混在同 owner/reason**已在 P1 收口。

P1 债务变化：S2 公共契约已删除，codec、策略常量和 999 有效态均归 `plugins/stack_limit` 私有实现；native bag marker 保持在 `plugins/extension_bag` 私有头。

完整清单与样例见 `docs/refactor/baseline.md` §3；核对命令：`uv run python scripts/maintenance/check_arch.py`。

## 5. 跨分支事项

| # | 事项 | 状态 | 说明 |
|---|---|---|---|
| 1 | 变体表补入 monster v27/v29 | ✅ 已在重构分支完成 | 修复 v27 用户 `known=0`（能力位误为 0）；主线仍需 cherry-pick（用户决定合并节奏） |
| 2 | 主线 `1d8bb97`（R-67/R-68 闪退修复）合入重构分支 | ⬜ 待办（用户指示暂不合并） | 合并前重构分支不含该修复 |
| 3 | 外部 review 缺陷修复批次（12 条 → 8 TRUE + 2 PARTIAL 已修，2 FALSE） | ✅ 完成（`e855850` 引擎 + `5727564` 插件；VM-FIX-01 真机通过） | 未闭合两项登记在 VM-FIX-01：① `in_flight_enter/leave` 生产 wrapper 未接线（级联在途等待暂无真实计数来源，建议调用点已归档）；② required consumer 反序 `blocked` 无重试路径（当前无 required 消费者，机制级隐患） |

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
| B2 | `feature/patch` 业务拆分 | 2,888 行 | A1、A4 | ✅ 完成（**B2a-1 ✅**：VM-P4-07 PASS——24/24 `live` + 用户冒烟正常；**B2a-2 ✅**（VM-P4-08：core 15/15 `live`、`stack_limit` 两态、启动守卫缺陷修复）；**B2c 经 D2c-2 完成**：inventory 业务 → `plugins/inventory_hooks`、move_merge/X1 → `plugins/extension_bag`、IAP → `game/access`，`feature/patch` 已删除） |
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
| D2 | `extension_bag` | 12,742 行 | A1、B2、B4、D1 | 🔄 进行中（冻结设计见 `docs/refactor/adr/0007-extension-bag-contract-and-mechanisms.md`；阶段：**D2a-1** 机制前置（popup_state 部分 wrap + `kBranchRedirect`，可并行）→ **D2a-2** stack 头归位 ✅（`stack_codec`→`contracts`、`stack_limit_port`→`contracts/`）；TU 拆分延后（棘轮按文件/行号记账，机械拆分需受控基线重定位，建议与 P5 检查器口径一并处理） → **D2a-3a** 契约 + 21 站点/符号登记（纯加法）✅ → **D2a-3b-1** 8 条声明（M1/M2 入口 hook、P1–P4 PopupState、S1 desc 槽命名空间、X1 move_merge 指针槽）+ 两桥 + 卸 raw + 门禁链路 ✅（M1/M2 按晚就绪语义校正为 required×failSoft；**C1–C21 raw call patch 与两条轮询线程保留，a3b-2 收口**） → **D2a-3b-2** 21 条 C1–C21 kCallPatch 声明化（功能型 required×failSoft / 装饰型 optional×failSoft，save callsite handler=`module_save_game`，机制 owner=extension_bag）+ 删 300ms 注入线程与自制 `allocate_draw_thunk` ✅（调用点 thunk 归宿主 `core/native/call_patch`；**150ms 生命周期线程保留，a3c 迁 scheduler**） → **D2a-3c** 150ms 生命周期线程迁 `infra::scheduler` 帧任务（kLogicPre / 节流 30 帧 / count=0 常驻；`set_virtual_bag_enabled(false)`/deactivate 移除）+ provides `extension_bag` v1.0（`ExtensionBagApi` 37 函数经桥交 EB TU `extension_bag_port.cpp` 逐条转发既有 port；宿主 enabled 注册 / revoking 注销）✅ → **D2b** 消费者切换/删反向引用（D2b-1 native / D2b-2 api/native POD）✅ → **D2c-1** `feature/extension_bag/**` 整体物理迁移到 `plugins/extension_bag/**`（34 文件，纯机械；include/CMake/基线路径同步）✅ → **D2c-2a** `feature/patch` 的 inventory 业务物理迁移到 `plugins/inventory_hooks`（`native_inventory_hook.{h,cpp}` + `inventory_hook_stage4.{h,cpp}`，4 文件）+ 删跨层 claims 桥（改建 `plugins/inventory_hooks/claims.h`）+ 被迁 TU 的 EB 调用切 capability（24 项经 `extension_bag_capability()` 现取；弹窗 2 项消费者内联）✅ → **D2c-2b-1** `inventory_refresh` v1.0 capability（refresh trampoline 转能力，EB/game_patch 消费者现取）+ 出售门禁转 `hooks/dispatch` 布尔门 Intercept（special_equip 登记、inventory_hooks 查询）+ `inventory_find_item_poc` 归位 `plugins/inventory_hooks`、删两处前置声明权宜 ✅ → **D2c-2b-2** `game_patch` 残壳收口：move_merge/X1 wrapper 迁 `plugins/extension_bag`、IAP 恢复迁 `game/access/hive_iap`、3 个 stage4 EB 谓词迁 EB 本地规则头、删除 `feature/patch` ✅（**`feature/` 目录已空**）→ **D2c-2 ✅ 完成** → **D2c-2c**（残留）：`native_hook_func`/`native_unhook_func` 机制原语与 `core/native/extension_bag_port.h` 过渡 port 收口（P5 前）→ **D2d** 收尾/冻结） |

> **D2 VM 命名空间**（D2 冻结设计验收；防与 `verification.md` 现有 VM-43..46 撞号）：
> - `VM-D2-01`：开关两态 + 25 资源 + 存读档。
> - `VM-D2-02`：消费者/API/JNI/autosell 联动。
> - `VM-D2-03`：deactivate 往返 + 出售链。
> - `VM-D2-04`：投影/拖拽/收编规则回归。
> 附注：归档规则册 VM 编号（VM-43..46/49/50 等）与 `docs/refactor` 编号冲突，引用以 `archive/docs/development/features/extension-bag/verification-matrix.md` 为准。

> **D1 延后项登记**（D1 未实现，已指定目标阶段；对应 `map.md` 过渡依赖区）：
> - 设置项数据化与配置源（Kotlin JSON → native config）：**P5**（决策 D；`ARCHITECTURE.md` §7.3 跨栈边界）。
> - U11-1 ui-kit adapter 契约与 host 注入面：**✅ 完成**；动态 UI 绑定管理器仍待 U11-3，覆盖 `plugins/core.ui/panels/settings.cpp` 运行时 ptr-slot 的临时豁免（`arch_exemptions.json`）。
> - U11-2 UI 组件层实施（ADR-0006）：第一阶段组件抽取（Panel/Button/Label/Toggle/Select/ScrollView）+ `contracts/ui.h` 标准组件接口已冻结；`core.ui` 通用组件实现**已完成**，宿主面板/调度器/PopupState 调用经 Host SDK 转发。
> - U11-3 面板消费者迁移：`settings`、`autosell`、`save_backup` **待办**，迁移后普通插件不再直接处理底层 UI。
> - `core.ui` 直依 `game/access/ui*`（过渡依赖）：U11-2 已经由 `contracts/ui.h` + Host SDK UI adapter 收口；动态 UI 绑定器仍留给 U11-3。

### U11 UI 收口状态

| 子任务 | 状态 | 当前交付 / 待办 |
|---|---|---|
| U11-1 | ✅ 完成 | `contracts/ui.h` 标准组件契约；`host/ui_adapter.cpp` 逐成员接入现有 ui-kit 原语；`QolHostSdk::ui` 在 host 装配期注入 |
| U11-2 | ✅ 完成 | `core.ui` 实现 Panel/Button/Label/Toggle/Select/ScrollView 组件，复制描述符字符串并保持主线程约束；5 处宿主私有 include 已切换至 Host SDK |
| U11-3 | ⬜ 待办 | 迁移 `settings`、`autosell`、`save_backup` 面板消费者 |

U11 未覆盖项：扩展背包控件树与拖放、齿轮贴图 raw、`save_backup` 多行/两步确认、`settings` ExecuteProc 槽绑定、`native_choice` PopupState 链。

### 迁移边界（包含什么 / 不包含什么）

- **包含**（目标架构强制，属迁移本身）：拆层与拆分（机制/业务、`ui_kit` 基础层）、接口替换（plugins 不触符号头 → `game/access`）、依赖方向修正（消除 feature↔feature）、hook/patch 声明式化（资源声明 + 事务）。
- **不包含**（推迟或明确不做）：业务语义重写、算法与性能优化（性能阈值走 §10 / U1 回填）、顺手重构无关代码（AGENTS：只移动职责）。
- 迁移中发现的缺陷单独提交，不与迁移混在一起（例：P2 的 v27 变体表缺口）。

## 7. 插件边界迁移施工阶段（P1–P4 范围；阶段编号 C1–C7）

> **编号口径（勿混用）**：架构分层只有 **L0–L5**（`ARCHITECTURE.md` §2.2–§2.4、`docs/refactor/layers.md`）。
> 本节使用 **C1–C7** 表示插件边界迁移的**施工阶段**（C = 施工阶段序号），是工作分解顺序，**不是架构层级**。

该计划依据 ADR-0008 执行。用户裁决：插件边界迁移属于 **P1–P4 范围**；设计在 P4 后修改，
不受 P5 签署门禁约束。C1 只冻结 Host SDK / Game Host API 的 ABI 骨架与 `PluginContext`
接缝，具体表成员可在后续 lane 演进；P6 才冻结对外兼容边界。

| 阶段 | 范围 | 状态 |
|---|---|---|
| C1 | ABI 骨架：`contracts/host_sdk.h`、`contracts/game_host.h`、`PluginContext` 注入 | ✅ 本批冻结骨架 |
| C2 | 宿主 adapter：Host SDK / Game Host API 的函数体与注入实现 | ✅ 已落地 adapter 与 host 测试；UI primitive 及动态 capability availability 仍按契约返回 unsupported |
| C3 | `domains` 拆分：`attribute_quality` provider + `attribute_range_display` 消费者 | ✅ 实现与 host-only 验证完成；真机 smoke 已过（VM-BOUNDARY） |
| C4 | 小中型插件边界化与 Host SDK/Game Host API 迁移 | ✅ 完成（9 插件迁移 + U11-1 契约/adapter + U11-2 `core.ui` 实现 `QolUiComponents` 并收口；`core.ui` 私有 include = 0） |
| C5 | `inventory_hooks` + `extension_bag` 边界化 | ✅ 完成（含 `save_backup`/`autosell`；`stack_codec` 迁 `contracts/`、`QolStoreApi` 容器级 ABI、SDK `hooks.intercept_blocks`、Host SDK PopupState ABI） |
| C6 | `core.api` 路由内核与 API provider 拆分 | ⬜ 待办 |
| C7 | native config 自读、脚本切七目录与边界违规硬失败 | ⬜ 待办 |

### C8：每插件 CMake 片段与父级自动发现

| 项 | 状态 | 当前事实 / 证据 |
|---|---|---|
| 构建片段 | ✅ 已落地 | `plugins/<id>/CMakeLists.txt` 定义各自 `qol_plugin_<id>`；本批仅 `simple_mode` 建立片段，源文件未再列入 `gamebridge` |
| 自动发现 | ✅ 已落地 | `plugins/CMakeLists.txt` 通过 `file(GLOB ... CONFIGURE_DEPENDS)` 自动发现同名子目录片段，集中设置 PIC、C++17、`-Wall -Wextra`、`contracts/` + 自身目录 include，并链接进 `gamebridge` |
| 边界与重复编译 | ✅ 已验证 | `simple_mode` 的宿主头负例编译失败；构建目标源清单确认 3 个插件源只属于 `qol_plugin_simple_mode`，未重复进入 `gamebridge` |
| 行为验收 | 不适用 | 本批只调整构建结构，行为零变化；不新增受影响 VM-B 卡，不安装真机 |

**插件边界合规现状（P0 修正）**：口径必须分开陈述——**目录前缀口径**（`game/ infra/ hooks/ patch/ host/` 与跨插件）为 **0**；**legacy 口径**（`core/native/**`、`data/native/**`、根部裸名头）仍有 **19 个文件**未清（逐文件见 `layers.md` §1.6）。因此此前"17/17 合规"的表述**不成立**，属审阅 P3（≈C7）清除目标。本批提交：`e7bccdf`(autosell) `8bf097f`(store 容器级 ABI) `cb63edd`(save_backup) `5ac1503`(extension_bag) `6bceda5`(inventory_hooks) `ca1430c`(基线行号受控重定位) `b5bd605`(EB `UiRect` 别名 / Android 构建修复) `130a4b1`(assembly provider 顺序 / autosell blocked 回归)。**真机 smoke 通过**（VM-BOUNDARY：18 项全绿、0 异常、无 FATAL；EB/craft 门禁两态、quest 生命周期往返均正常）。C1–C5 的**完整行为面**（各插件功能细则）仍按各自 VM 卡待补；本轮只覆盖启动状态与门禁/生命周期面。

**插件边界合规现状（P0 修正）**：目录前缀口径 **0**；legacy 口径 **19 文件未清**（同上）。故"plugins → game/infra/hooks 迁移债已清偿"**不成立**——基线内仍有层方向 32 条、符号/裸地址 9 条、资源 3 条。剩余债务＝legacy include 清偿（P3）+ 引擎级未闭合项（`verification.md` VM-FIX-01）+ C6/C7 范围。

### P0 事实与词汇冻结（2026-09-19 审阅）

- **三态标记**（与 `layers.md` 一致）：当前事实 / 迁移债 / 目标设计问题。
- **审阅记录**：`docs/refactor/architecture-review-2026-09-19.md`（结论 `NOT_ACCEPTED`）；6 条阻断事实已逐条核验成立——插件 legacy include 19 文件、脚本过渡规则（`arch_common.py:36,67-72,95,110,129`）、校验器"通过"≠合规（32/9/3）、单一 CMake target、`QolGameHostExtraApi` 过宽、contracts 全局函数绕 capability + `core_patches` 混 S2/hub。
- **审阅裁决（目标，非当前事实）**：`core.*` 保持 2 个（`core.api` + `core.ui`，插件总数 17→18）；S2 终归 `stack_limit`；hub 兼容补丁单独归属；`core.game` / `core.inventory` / `core.save` / `core.diagnostics` / `core.compat` / `core.config` / `core.log` / `core.registry` 一律不创建。
- **收口计划 P0–P6**：见 `layers.md` §六（P0 事实 → P1 契约纯度/S2 seam → P2 ABI 补齐 → P3 include 清零+独立构建+脚本硬失败 → P4 provider DAG+extra 拆分 → P5 core.api/core.ui → P6 S2 residency+签署门禁）。
- **口径纪律**：施工阶段 C1–C7 与收口阶段 P0–P6 不混用；`check_arch.py` 通过 ≠ 合规。

### P1 裁决冻结（2026-09-19，用户）

**裁决 1：`item_count` facade 采读+写（方案 B）**

- 成员：`canonical(item,out)`、`effective(item,out)`、`is_encoded(item,out)`、`write_effective(item,new)`、`writeback_needed(item,new,out)`；位段/常量/写回门控的**知识全部留在 `stack_limit` 实现**。
- **native bag marker 不进 facade**（审阅 §8.4：marker 属 `extension_bag`）。
- 公共 `contracts/` 不再暴露 `stack_codec.h` 的 codec 语义。
- 语义约束：读写在**同一调用窗内同步完成**（R-47/R-49 不可退化为跨帧命令）。

**裁决 2：S2 布局不做 boot-fatal，按"开关不改变存读方式"处理**

- S2 布局的目的是：**开启/关闭不改变数据的存读方式**；关闭态不使用额外数据位。
- **不变量（P1 验收硬约束）**：
  1. **开启时写必须走 S2 布局**（否则启用态写会破坏 S2 数据）；
  2. **关闭 / provider 失败时读走原始方式**（低 7 位/native 视图），写同样回落原始语义；
  3. **写侧与布局补丁同生共死**：布局补丁未就绪 ⇒ 999 功能视为不可用（`degraded`）⇒ 读写均回落原始语义，**不允许出现"启用态用 S2 写、布局补丁缺失"**。
- 因此：S2 布局补丁与 6 条 clamp、getter/写路径同属 `stack_limit` 的 **999 功能组**（gate 开则装、关/失败则回落），**不需要**新的 host "boot-required 资源"通道，也**不要求**失败致命。
- 上述取代审阅 §8.2 的"host 绝不在启用/停用时改函数表"的字面约束：布局补丁仍由宿主机制安装（`patch/` 资源通道），但**归属与生命周期**归 `stack_limit` 的 999 功能组。

### C4-0 Host SDK / Game Host ABI 冻结

C4-0 追加冻结 `QolGameHost` minor `1`。既有字段顺序与签名保持不变，新增成员统一位于
`QolGameHost::extra`，由 `host/game_host.cpp` 实现；U11-1 已建立 `QolHostSdk::ui` 的
UI adapter 注入面，U11-2 已完成组件实现，消费者迁移留给 U11-3。

`extra` 当前包含以下最小成员组：

- 角色：阵营链、等级、佣兵槽、装备权限、主控角色、池槽、HP 读写；
- 物品与库存：品质、真装备/装备等级、禁售/骰子/堆叠数、位段读取，以及合成产物创建、
  扣料、入库和释放；
- 合成：填入槽控件/目标物品/配方状态读取，以及初始化、重置、刷新、选空槽、类型、音效、
  文本和描述关闭调用；
- 世界：地图记录/当前地图/地图名/朝向/金币读取，以及扣钱、切图、关闭弹窗、HUD 恢复、
  横幅调用；
- 存档与任务：槽位元数据、文件加载/密钥/加解密/释放/脏标记，任务槽读取、状态判定与状态写入。

成员返回 `QolResult`，指针和文件缓冲区所有权在契约中明确；未解析、越界或非法参数统一
fail-closed。副作用成员只允许 owner 插件主线程命令处理器调用。

本批仍未覆盖：

- U11-3：`core.ui`、`autosell` 面板和 `save_backup` 游戏内面板的完整消费者迁移；
- U11 未覆盖：扩展背包控件树与拖放、齿轮贴图 raw、`save_backup` 多行/两步确认、
  `settings` ExecuteProc 槽绑定、`native_choice` PopupState 链；
- `extension_bag` / `inventory_hooks` 的库存遍历、控件树、指针槽、hook/patch 资源迁移；
- `custom_recipe` 的配方表直读；
- `game_symbols.h`、`game/access/patches`、`ptr_slots`、hook/patch 安装机制等内核私有面。

### P1 收口：B1/B2a/B2b/B3/B4/B5-0/B5a/B5b/B5c

| 项 | 状态 | 当前事实 / 证据 |
|---|---|---|
| P1 总体代码出口 | ✅ 完成 | B1、B2a、B2b、B3、B4、B5-0、B5a、B5b、B5c 均已落地；本批为 P1 最后一步代码 |
| B5c 公共 codec 清除 | ✅ 完成 | `contracts/stack_codec.h`、`contracts/stack_limit.h` 与 `core/native/stack_limit_state.cpp` 已删除；`CMakeLists.txt` 已移除状态实现；`plugins/stack_limit/stack_codec_impl.h` 现为唯一 S2 实现 |
| 999 有效态 | ✅ 完成 | `plugins/stack_limit/plugin.cpp` 内部原子量持有有效态；`QolItemCountApi` facade 与插件状态 API 仍是对外可观测路径 |
| marker | ✅ 完成 | `write_native_bag_object_marker` 继续只存在于 `plugins/extension_bag/extension_bag_port.h`，stack_limit 不再依赖 marker |
| 行为验收 | ⬜ 待真机 | `VM-P1-S2-01..05` 仍是行为面证据要求；本任务按要求不装真机，不以 host/build 替代真机证据 |

C4 插件源码未在 C4-0 修改；后续插件边界迁移必须只依赖本节追加契约或另行登记的 U11/C5
契约缺口。

| C4-原型 | `simple_mode` 独立 target 与 include 边界 | ✅ 已落地 | `qol_plugin_simple_mode` 仅编译插件自身 3 个源文件，仅 include `contracts/` 与自身目录；链接进 `gamebridge`，ABI 清单与复现命令见 ADR-0010「模块侧插件原型（simple_mode）」 |

### P2-A：日志与存档事件迁移（ADR-0010）

| 项 | 状态 | 当前事实 / 证据 |
|---|---|---|
| Host SDK log | ⚠️ 部分落地 | `autosell`、`save_backup`、`core.ui` 的日志使用者改经插件私有薄封装调用 `sdk->log.write`；`inventory_hooks` 与 `extension_bag` 仍受本任务禁止改动范围/缺少 SDK accessor 约束，未宣称清零 |
| Save events | ✅ host-only | `contracts/events.h` 新增 `kEventSaveEnter`/`kEventSaveExit` 与 `int32_t slot` POD；legacy save_enter/save_exit 发布点发布事件；autosell 与 custom_recipe 改经 SDK events 订阅；旧注册 API 保留 |
| 迁移证据 | ⬜ 行为面待补 | `infra_events_tests` 覆盖事件类型隔离与 slot 载荷；真机不执行，受影响 VM 卡不能标 PASS |
| 设计冻结 | ✅ 完成 | `docs/refactor/adr/0010-game-host-face-split.md`：extra 拆分、五项拆除、缺面、lane 与风险 |

### P1-B1：`stack_limit` S2 所有者迁移

| 项 | 状态 | 当前事实 / 证据 |
|---|---|---|
| 私有 codec 入口 | ✅ 已落地 | `plugins/stack_limit/stack_codec_impl.h` 已实体化 S2 codec 与 99/999 策略；不再转发公共头 |
| 999 原子组 | ✅ 已落地 | `stack_limit` 6 条 clamp + 12 条 S2 claim，共 18 条，统一 `required + fail-closed + resumable + atomic + plugin.stack_limit.enabled` |
| facade provider | ✅ 已落地（host-only） | provider 在 `plugin_install` 成功后驻留、`plugin_deactivate` 注销；`on_disabled` 只清 999 有效态；gate 关闭保留原始语义，gate 开且原子组未满足时 effective/写路径返回 `kQolErrNotReady`；未知类别与未就绪路径不写默认 out |
| `core_patches` | ✅ 已收窄 | 仅保留 `P_HUB_LOGIN_SKIP` 与 monster 特征扫描；S2 由 assembly 期 `stack_limit` 安装 |
| getter/写路径 | ⚠️ 已登记缺口 | ADR-0009 §999 清单中的 `ITEM_GetCumulateCount`、`INVEN_ConsumeItem`、`INVEN_SaveItem`、`INVEN_MoveItem`、`INVEN_SaveItemDirect`、`ITEMSYSTEM_Divide`、`INVEN_RemoveItemData` 尚未全部切到 facade/999 声明式资源；B2 与读侧同批迁移 |
| 消费者 | ⬜ B2 | 本批按约束不修改 `api/native`、`core/native`、`data/native`、`extension_bag`、`inventory_hooks`；其写 wrapper 缺口见 B2 |
| 验收 | ⬜ 真机待 B4 | host 断言已补；行为面仍需 VM-B 真机证据，未因 host/build 通过而宣称接受 |

### P1-B3/B4：extension_bag model 与 host 测试去 codec

| 项 | 状态 | 当前事实 / 证据 |
|---|---|---|
| model 边界 | ✅ 已落地 | `virtual_bag_state.h`、`virtual_bag_state_json.inc`、`virtual_bag_transaction_rules.inc` 不再 include 或调用 S2 codec；canonical/view/clamp 由 `CountPolicy` 入参提供 |
| marker | ✅ 已落地 | native bag marker 私有化到 `plugins/extension_bag/extension_bag_port.h`；未进入 `item_count` facade |
| host 测试 | ✅ 已落地 | `test_host.cpp`、`test_infra_data.cpp`、`test_inventory_hook_stage4.cpp` 删除公共 codec 直测，保留模型整数边界、三态 facade 与 fail-closed 断言 |
| 验证 | ⚠️ 行为面未闭合 | host tests、debug build、架构/资源/基线/map check 已通过；真机已完成启动、资源态与进世界 smoke（`VM-P1-S2-01` 部分证据），但 S2 开关往返/写拆堆/持久化卡未完成，故行为面仍不宣称接受 |

### C5 ABI freeze：四插件迁移前置契约

本批冻结四个插件后续迁移所需的公共面，不修改插件业务体：

- `contracts/stack_codec.h`：P1-B5c 已删除；纯位编解码现由 `plugins/stack_limit` 私有实现持有，
  `extension_bag` 与 `inventory_hooks` 经 facade/自身私有规则访问。
- `QolHostSdk::hooks.intercept_blocks`：暴露共享 Intercept 门查询，返回值为 `1=阻断`、
  `0=放行`，非法资源 id 返回负错误码。
- `QolUiAdapter` minor `1`：补齐控件树查询、PopupState/UIDesc 调度、分辨率、字体/贴图
  原语、原版按钮/开关/物品图标和自定义按钮绘制；实现统一经 `host/ui_adapter.cpp`。
- `QolHostSdk` minor `2`：`QolStoreApi` 增加 caller-owned 的容器级
  `read_container`、`import_container`、`delete_slot`；容量不足返回错误并报告所需长度。
  未增加 `ensure_slot`/`reset_slot`，因为 save_backup 当前迁移面不调用这两个操作。
- `QolGameHost::extra` 的既有角色、物品、库存、存档、世界、任务与选择框子表继续作为
  四插件迁移入口；未新增 game 内部类型、地址或 STL。

> 协作纪律：委托代理一律禁止使用 question 工具；遇阻返回报告，由编排者裁决。

### C8-1：并行迁移前置——插件源自动摘除

| 项 | 状态 | 当前事实 / 证据 |
|---|---|---|
| 集中源清单 | ✅ 已落地 | 根 `CMakeLists.txt` 只维护一次 `QOL_BRIDGE_PLUGIN_SOURCES`；现有插件源均由该清单提供给 `gamebridge` 的过渡直接源集合 |
| 自动摘除 | ✅ 已落地 | `plugins/CMakeLists.txt` 在 `file(GLOB ... CONFIGURE_DEPENDS)` 发现片段后，按 `plugins/<id>/` 路径前缀过滤已下沉插件源，再回写父级变量 |
| 试点 | ✅ 已落地 | `push_all` 片段定义 `qol_plugin_push_all`；三个插件源从 `gamebridge` 直接源移除并由插件静态 target 链接 |
| 新增插件操作 | ✅ 已冻结 | 仅执行“建片段 → 修越界”两步；不再编辑根 CMakeLists 的插件源条目 |
| 配置期语义 | ✅ 已登记 | 新增/删除片段依赖 CMake 重新配置；`CONFIGURE_DEPENDS` 负责触发重新配置，非运行期动态发现 |
| 行为验收 | NOT_ACCEPTED | 本任务不安装真机；构建与 host/静态检查只能证明编译归属，不能替代受影响 VM-P4-01 的真机日志 |

### P2-ABI-1：autosell 按需只读面增补

| 项 | 状态 | 当前事实 / 证据 |
|---|---|---|
| ABI | ✅ 已落地 | `QolGameHost::read_only.item` 追加宝石属性值/类型，`world` 追加 `game_state_gate`，新增 `inventory.item_ref_at` 与 `inventory.for_each_bag_slot`；既有成员 append-only；`kGameHostMinor=4`。 |
| game/access | ✅ 已落地 | `item_class` 收口 `item+I_COUNT` 宝石位域，`world` 收口 `g_gamestate` 读面，`inventory` 转发原库存引用/聚合遍历语义；插件不获得 symbols/base/裸偏移。 |
| host 测试 | ✅ 已补 | `test_host_adapters.cpp` 覆盖新增转发表、正常值、空参数、空槽、未就绪；库存 fake 使用 contracts 引用，不依赖真实游戏内存。 |
| autosell 迁移 | ⬜ 后续 lane | 本批不修改 `plugins/autosell` 消费者；出售事务与 UI 控件身份面仍未覆盖。行为证据受影响卡为 VM-44、VM-AS4/5/8/12/13，按要求不装真机，不能宣称 PASS。 |

### EB G1–G5 分组收口

| 组 | 状态 | 当前事实 / 证据 |
|---|---|---|
| G1 日志/事件/配置/调度/存储/能力 | NOT_ACCEPTED（无真机） | 扩展背包日志改经 Host SDK 薄封装；存档参与者与保存触发追加到 `QolStoreApi`；配置事件、scheduler、能力查找沿用既有接口。Host 50/50、Debug 构建通过；行为证据按任务要求不执行真机。 |
| G2 只读事实 | NOT_ACCEPTED（无真机） | `QolGameHost::read_only` 追加 Character/Item/Save/Inventory 只读面，`kGameHostMinor=8`；EB 已切换 world gate、物品类别/数量/装备判定、佣兵徽章、可用物品、出售单价和当前存档槽查询。Host 50/50、Debug 构建、架构/资源/基线/map 检查通过；投影、事务、回滚和 marker 写路径未改变。受影响规则 R-47/R-50/R-55/R-63，VM-43/VM-P4-07 真机证据未执行，不能宣称 PASS。 |
| G3 UI | NOT_ACCEPTED（无真机） | `QolUiAdapter` minor 追加控件身份/item/slot、touch state、rect fill、原版物品绘制、parent/root 查询；EB 的控件定位、树遍历、命中、页签/格子绘制、物品数据与释放坐标读取已切换到 `ctx.sdk->ui` 薄封装。Host 编译与 Debug 构建通过；Popup enter alias、Host fake/独立 include、架构/资源/基线/map 检查及 VM-B 真机证据尚未完成。受影响规则 R-12/R-13/R-18/R-47/R-48/R-49，行为证据按要求不能以构建替代。 |
| G4 副作用与所有权 | NOT_ACCEPTED（无真机） | `QolGameHost::extension_bag_write` 尾部追加，`kGameHostMinor=9`；按 inventory/equipment/ui 分组，保留原游戏整数返回值与 fail-closed 语义。EB 装备、跨袋入库、移动合并、镶嵌、使用、佣兵生成、金币、序列化/重建和释放路径已切换；原版刷新 trampoline、Popup/PtrHook 安装和控件树写仍为 G5/S1 缺口。Host 50/50 通过；受影响 R-12/R-18/R-34/R-38/R-40/R-41/R-47/R-48/R-49/R-50/R-55/R-63，对应 VM-B01..B05、VM-34、VM-40、VM-41，未执行真机。 |
| G5 收口 | ⬜ 未开始 | 待插件片段、独立编译、边界负例与零遗留 grep。 |
