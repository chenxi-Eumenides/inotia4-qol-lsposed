# 未覆盖插件日志覆盖审计（2026-11-12）

范围：`attribute_quality`、`better_move`、`custom_map`、`damage_widen`、`popup_blocker`、`push_all`、`stack_limit`、`module_health`。

判定口径：`present` = 关键事件已有日志且基本能说明结果；`weak` = 有相关日志但等级、字段、频率或事件覆盖不足；`missing` = 没有对应日志。表格按生命周期/功能节点归并，不把每个内部函数单列。四级语义按规范：能力整体不可用/不可恢复用 E；可降级失败用 W；有限状态变化用 I；只用于诊断的数值、分支、逐帧信息用 D。逐帧观测只建议 D，且需遵守 debug 零成本门控。

## attribute_quality

入口/描述符：`plugins/attribute_quality/plugin.cpp:35-65,67-113,115-131`。提供隐藏 feature `attribute_quality.provider`，依赖 `item.facts`，声明必需且 fail-closed 的 `MATH_GetRandom` hook；安装时校验 host 与依赖 API，启用/停用改变服务门禁，deactivate 清理运行态。API 查询装备/宝石属性区间和品质，固定表修正范围；调用失败返回错误结果，资源 backup 缺失则禁用服务。无用户 UI 操作，无插件自有数据文件加载或健康采样。

| 生命周期/关键节点 | 证据与覆盖 | 判定 | 建议 |
|---|---|---|---|
| 安装/依赖校验、服务可用、启停与 deactivate 清理 | `plugin.cpp:67-101`；全插件无日志 | missing | 在安装校验最终失败处记录一次 W（依赖/资源）；服务成功启动、停用状态变化各记 I。API 高频查询不逐次记 I。 |
| 资源 hook 安装/backup 就绪或失败 | `plugin.cpp:35-50`、`feature.cpp:308-317`；未记录 | missing | backup 为空导致能力不可用时 W，成功 hook 由统一资源生命周期日志覆盖；避免插件与 host 重复记同一安装事实。 |
| 属性范围捕获、品质查询成功/拒绝/可恢复失败 | `feature.cpp:79-145,320-399,402-482`；错误以 `QolResult` 返回，无日志 | missing | 不记录常规成功查询；仅对关键降级/失败路径按错误码和原因给 D（可重试/暂不可用）或 W（调用失败且继续），避免高频调用刷屏。 |
| 恢复/清理 | `feature.cpp:301-306`；没有日志 | missing | 生命周期 I 可包含运行态清理，无需单独重复记录每个字段归零。 |

**计数：present 0 / weak 0 / missing 4。**

## better_move

入口/描述符：`plugins/better_move/plugin.cpp:19-149,151-217,219-237`。两个 choice 开关：`better_move.direction`（关闭/八向/全向）及 `better_move.step`（步长档位）；声明 7 项必需私有资源（输入、移动、绘制、等待坐标、碰撞查询及两个 call patch），依赖移动输入 port。安装注册主循环 poll task、恢复默认步长；停用移除任务、关闭模式并恢复步长。用户通过 choice 切换模式/步长；hook wrapper 转发方向触摸、按键映射、八/全向移动、绘制指针和碰撞查询。端口/choice/scheduler 可用性失败可拒绝或保持原生步长，入口 wrapper 保留原函数回退。无数据文件及健康 API。

| 生命周期/关键节点 | 证据与覆盖 | 判定 | 建议 |
|---|---|---|---|
| 启停、模式/步长设置及恢复 | `feature.cpp:67-109,227-260`；只有 `movement step=%d`、默认步长日志（`feature.cpp:70,109`），缺少启停/模式状态；step 文案缺 feature/结果 | weak | 对实际模式或步长变化记 I（含 feature/value），停用恢复原生状态记 I；不重复记录未变化值。 |
| hook 路径生效 | `feature.cpp:146-159,188-224`；首次调用时记录 active（`155-157,191-193,203-207,218-221`），不是资源安装成功日志 | weak | 资源统一安装成功日志应作主证据；无需逐个 hook wrapper 再记成功。 |
| 安装依赖校验、poll task 注册/移除、choice 被拒 | `plugin.cpp:151-212`、`feature.cpp:87-108,126-143,227-260`；失败被返回/忽略，无日志 | missing | scheduler 注册最终失败及 port/choice 不支持导致功能不可用时 W，需包含 hook/feature 与 reason；避免记录可重试过程成功之外的每次 poll。 |
| hook 回退与资源卸载恢复 | descriptors 声明可物理还原；代码在 `plugin.cpp:186-212` 重置门禁/状态，未记恢复；内部 wrapper 缺 backup 时透传/短路，未记 | weak | 对停用的整体状态恢复使用上述单条 I；只有实际资源卸载失败才补 W，不在 wrapper 热路径重复记录。 |

**计数：present 0 / weak 3 / missing 1。**

## custom_map

入口/描述符：`plugins/custom_map/plugin.cpp:24-42,57-72,74-177,179-241`。提供 `custom_map.load` 开关和 `custom_map.open_panel` 操作；必需 UI/panel/confirmation 能力、可选 static-data 能力、custom-map resource port；声明 map loader hook。面板列出/解析自定义 `.dat` 与 `.dat.jpg`，允许选地图看概要/示意图；jpg 可解码并持久化为 dat。未注册到 assembly（`host/assembly.cpp:262-264`），因此当前 descriptor 不进入装配/DAG，源码描述的 hook 和面板并未实际接线。运行时自定义加载失败时回退原始 loader；面板错误多数只显示在 UI。无健康检查。

| 生命周期/关键节点 | 证据与覆盖 | 判定 | 建议 |
|---|---|---|---|
| 安装/面板注册、资源 hook、启停/卸载 | `plugin.cpp:74-177`；install 错误写入 status，但没有日志；enable/disable、provider 注销亦无日志；当前 assembly 未注册 | missing | 仅在完成装配后，于实际生命周期边界记录 install/启停结果和卸载；当前未注册状态不应伪报 hook 成功。 |
| 自定义 dat/jpg 加载成功、转换与诊断 | `feature.cpp:115-143,145-251`：记录成功 map load 与 jpg 转换状态；日志均用固定等级 1，正文自由文本含 id/size/reason/input/output，细节充分但成功/失败语义及等级未区分 | weak | 成功转换/加载 I；解码、持久化失败但回退成功 W，标明 fallback；可选源文件缺失不告警。统一动词与结构化字段。 |
| 加载拒绝/降级回退原始 loader | `feature.cpp:254-273,305-313`；绝大多数失败静默回退 original；转换记录不覆盖 `.dat` 读取/分配失败及最终 fallback 结果 | missing | 只在 custom file 被选中但加载失败并实际 fallback 时记 W（id、阶段、reason、fallback=original）；不要把正常无自定义文件升级为 W。 |
| 面板操作、扫描/解析错误及用户可见结果 | `ui/panel.cpp:184-215,274-353,481-530`；转换会记录；扫描失败、详情读取失败等只写 UI 状态（例如 `510,332-337`），选择/开关无日志 | weak | 只补面板打开/关闭的有限状态 I，以及最终扫描/详情失败 W；不为每个地图行或逐帧 process 记录。 |

**计数：present 0 / weak 2 / missing 2。**

## damage_widen

入口/描述符：`plugins/damage_widen/plugin.cpp:365-436,446-495`。`damage_widen.enabled` 开关。声明可选、fail-soft、可物理还原资源：普通/魔法/暴击/防御/物理防御 hook、怪物/攻击公式的指令 patch；安装时保存 SDK 与 character read API，启停切 gate，deactivate 清理。修复宽整数伤害计算并限制溢出；属性读取或物理防御计算失败时透传原函数。可选诊断 Act_DD 入口追踪。无用户操作、外部数据加载或健康采样。

| 生命周期/关键节点 | 证据与覆盖 | 判定 | 建议 |
|---|---|---|---|
| 属性读取失败并回退原计算 | `plugin.cpp:96-109`；一次性调用 `log_write(2,...)`，能辨认 fallback，但使用自然语言/无 `op`、标准 `reason`，资源索引不能直观辨识 hook | present | 保留 W 语义，结构化为 `fallback hook=... attr=... reason=... fallback=original`；现有一次性门控合适。 |
| 伤害改算结果/诊断轨迹 | `plugin.cpp:112-138,141-166,188-215,240-362`；大量 trace 用 level 1（如 `134-137`），在战斗热路径最多每阶段 12 次；message 含对象地址、数值，仍属于诊断而非状态变化 | weak | 改为受 debug=logging 门控的 D；参数在门控后求值，不得把逐次数值记 I。 |
| 安装、hook/指令 patch 成败、功能启停与卸载恢复 | `plugin.cpp:446-482`；无日志；资源为可选 fail-soft、失败后可用原版计算 | missing | 资源总体安装失败/跳过时 W（hook/resource + reason），开关变化及实际整体恢复记 I；不重复记录成功安装（统一资源日志已覆盖时）。 |

**计数：present 1 / weak 1 / missing 1。**

## popup_blocker

入口/描述符：`plugins/popup_blocker/plugin.cpp:30-56,58-94,96-113`。`popup_blocker.enabled` 开关；必需 fail-closed、可物理还原的 call patch 与 TutorialDescription hook。hook 短路教程文案返回 0，patch 保留教程状态但抑制 state=6 pause gate；安装校验 SDK/game，停用关闭 gate，deactivate 清理 SDK。无用户 UI/数据加载/健康检查。插件内完全没有日志。

| 生命周期/关键节点 | 证据与覆盖 | 判定 | 建议 |
|---|---|---|---|
| 安装/资源组/启停及还原 | `plugin.cpp:30-87`；无日志 | missing | 建议两处关键日志：资源组安装失败 W（功能 fail-closed）；启用/停用状态变化 I（包含 feature）。统一资源成功/卸载日志若已有则不重复。 |
| 教程抑制生效 | `plugin.cpp:22-28,44-55`；无日志 | missing | 不建议记录每次教程 hook；若需要验证，只在首次命中时记一次 D。 |
| 用户操作、数据与健康状态 | 无用户操作/数据加载/健康检查路径 | missing | 无需补日志。 |

**计数：present 0 / weak 0 / missing 3。**

## push_all

入口/描述符：`plugins/push_all/plugin.cpp:19-62,64-121,123-148`。`push_all.push` 开关；依赖 `character.facts` 和 character-extras port；两条必需 fail-closed 私有 call patch（party filter 与站立检查），插件 lifecycle 控制整体物理安装/还原。party filter 在启用时按角色类型/玩家组/队伍序号决策能否推动；接口/读取失败拒绝推动，关闭门禁时透传原行为。没有其他用户操作或数据加载。

| 生命周期/关键节点 | 证据与覆盖 | 判定 | 建议 |
|---|---|---|---|
| gate 启停 | `feature.cpp:34-37` 记录 enabled，但自然语言 `push_all enabled=%d` 缺 feature/状态动作，且无去重；resource 安装后也会调用 set_gate | weak | 只在状态实际变化时 I，结构化 `apply feature=push_all.push enabled=...`；重复同值不记。 |
| patch 路径首次生效 | `feature.cpp:43-50` 首次 filter 调用记录 active；是实际执行路径证据，不等于两条 patch 均安装成功 | weak | 保留为一次性 D 或改成明确首次 dispatch 诊断；资源成功/失败采用统一资源生命周期日志。 |
| 安装依赖、读取拒绝、资源失败/卸载恢复 | `plugin.cpp:64-116`、`feature.cpp:51-69,76-81`；安装失败、输入数据读取失败以及拒绝结果均无日志 | missing | 安装依赖/patch 组失败 W；仅当 filter 的一次决策因 facts 读取错误而 fail-closed 时 W（含 reason）；正常目标不符合推动条件是业务拒绝，可用 D 或不记。 |
| API/健康/文件加载 | 没有额外 API、健康采样或文件加载路径 | missing | 无需补日志。 |

**计数：present 0 / weak 2 / missing 2。**

## stack_limit

入口/描述符：`plugins/stack_limit/plugin.cpp:20-45,48-85,87-103`。`stack_limit.limit_999`（default false）单开关声明 16 项数量 patch 资源组，必须整组安装；资源失败 fail-closed，回退原始 99 数量视图。S2 编解码和数量策略已迁入 Host adapter，插件只描述资源所有权并由 lifecycle 更新 status gate。无插件自有 hook callback、用户操作、文件加载或健康检查；插件内完全没有日志。

| 生命周期/关键节点 | 证据与覆盖 | 判定 | 建议 |
|---|---|---|---|
| 资源组安装/原子回滚、开关变化及恢复原始数量视图 | `plugin.cpp:8-10,20-45,51-77`；无日志 | missing | 建议两处：整组安装失败/回滚 W（feature、资源组、reason、fallback=limit_99）；开关生效/停用 I。成功 patch 由统一资源日志覆盖，不逐 patch 重复记录。 |
| 数量策略行为及回退 | 当前实施在 host adapter，插件文件只声明 owner；未见插件日志 | missing | 不在每次数量读写热路径加日志；若 host 已有 owner 日志以单一调用点为准。 |
| 用户操作/数据/健康 | 无插件自有入口 | missing | 无需补日志。 |

**计数：present 0 / weak 0 / missing 3。**

## module_health

入口/描述符：`plugins/module_health/plugin.cpp:16-25,47-71,73-135,141-184,186-217`。隐藏 `module_health.enabled` 开关、提供 module-health snapshot capability；安装读取并校验初始 snapshot；启用每 30 帧采样 host facts，停用移除 task，deactivate 清理指针与缓存状态。无 hook、用户操作或文件加载；scheduler 不可用/注册/移除失败会保存至 `PluginStatusOut.error`，snapshot read 失败则保留旧快照。插件完全无日志。

| 生命周期/关键节点 | 证据与覆盖 | 判定 | 建议 |
|---|---|---|---|
| 初始 snapshot 校验/插件可用 | `plugin.cpp:141-159`；失败返回 install code，无日志 | missing | 初始化不可用时 W；若导致整体 health capability 不可用则按规范影响范围判 E。成功安装/首次可用 I。 |
| scheduler 开启、不可用/注册失败、移除失败与恢复 | `plugin.cpp:73-135,162-178`；失败仅写 status error，无日志 | missing | 仅对采样能力不可用/停止失败记 W（`op`、reason）；启停/恢复成功记 I。不要每 30 帧记录采样。 |
| snapshot 变化/读取失败 | `plugin.cpp:58-71`；只缓存有效且变化后的 snapshot，读取失败不记录 | missing | 如需跟踪仅在 snapshot 有意义变化时 D；采样循环的每帧细节禁止 I。 |

**计数：present 0 / weak 0 / missing 3。**

## 优先补全项（跨插件，按价值排序）

1. **先保证统一资源生命周期日志覆盖 install/uninstall 与原子组失败**：优先 `stack_limit`（16 资源原子组）、`popup_blocker`、`better_move`、`damage_widen`。成功 hook/patch 不要在插件 wrapper 再重复记录；失败至少有资源/feature、reason，降级说明 fallback。
2. **补齐无日志 provider 的可用性边界**：`attribute_quality` 的依赖/捕获 hook 失败、服务不可用和清理；避免对常规 API 查询逐次写 I。
3. **把 `damage_widen` 诊断轨迹从 Info 改为 debug-gated D**（`plugin.cpp:134-137,160-164,211-214,244-247,262-265,304-360`）；战斗热路径数值与对象地址不是状态事件，门控前不得格式化/读取额外属性。
4. **补足 custom_map 实际加载失败后的 fallback 结果**（`feature.cpp:254-273,305-313`）：一次失败记录 W 并说明 `fallback=original`；正常缺失资源保持静默。前提是先将插件纳入 assembly，当前 `host/assembly.cpp:262-264` 明确未注册。
5. **规范 custom_map 转换事件等级与正文**（`feature.cpp:115-143`、`ui/panel.cpp:184-215`）：成功转换 I；写盘失败但内存加载成功记录 W + fallback；单次解码失败 W；区分用户面板触发与资源 hook 热路径，避免同一转换双记。
6. **补 `module_health` scheduler 最终失败日志**：失败会让 health 采样能力不可用，但已有 status error 可查询；一次最终失败 W 足够，采样过程保持静默，不记录逐帧日志。
7. **为 `push_all` 和 `better_move` 增加状态变化而非调用日志**：只对启停/模式/步长实际变化记 I；读取拒绝或 choice 不支持记 W，首个 hook 命中仅保留一次 D 诊断。
