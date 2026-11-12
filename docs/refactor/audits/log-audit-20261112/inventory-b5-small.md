# 日志审计清单：批次 5（小型插件）

依据 `docs/reference/logging-standards.md` 与 `docs/reference/logging.md`。范围：`module/app/src/main/cpp/plugins/{core.craft,craft_lottery,world_tuning,special_equip,character_panel,attribute_range_display}/**`。对范围逐项检索 `log_message(`、`log_info(`、`log_warn(`、`log_error(`、`log_debug(`、`QOL_LOG_`、`log.write(`；包装器定义和通用写出口不计作事件。`attribute_range_display` 未检出日志，明确为 0 条。

## 现有日志

| file:line | domain | level_now | message 摘要 | event_class | level_target | disposition | rationale |
|---|---|---:|---|---|---:|---|---|
| `core.craft/plugin.cpp:162` | core | I | install provider | 插件安装 | I | keep | 插件安装完成，有限生命周期事件。 |
| `core.craft/plugin.cpp:167` | core | I | deactivate provider | 插件停用 | I | keep | 插件停用状态变化。 |
| `core.craft/plugin.cpp:172` | core | I | enable provider | 功能启用 | I | keep | 功能启用状态变化。 |
| `core.craft/plugin.cpp:176` | core | I | disable provider | 功能停用 | I | keep | 功能停用状态变化。 |
| `craft_lottery/feature.cpp:49` | custom_recipe | W | degraded craft lottery operation | 单次奖池操作降级 | W | keep | 奖池操作降级，仍可运行；保留 operation 字段。 |
| `craft_lottery/feature.cpp:145` | custom_recipe | I | enabled | 功能启停 | I | keep | 有限状态变化。 |
| `craft_lottery/feature.cpp:157` | custom_recipe | W | unknown resource id | hook/resource 查找异常 | W | keep | 未知资源导致本次资源处理异常，带资源标识。 |
| `world_tuning/feature.cpp:119` | world_tuning | W | faction dependencies unavailable | 依赖能力缺失 | W | keep | 阵营判断失败时本次不应用倍率；一次性门控。 |
| `world_tuning/feature.cpp:311` | world_tuning | D | damage trace: attacker/victim、输入输出数值 | 逐次伤害应用细节 | D | keep | 已改为 level 0；逐次 hook 诊断仅在 debug 开启时格式化。 |
| `world_tuning/feature.cpp:345` | world_tuning | W | char not in pool; max-hp scaling skipped | 倍率应用单次失败 | W | keep | 无法定位池槽时跳过生命倍率，且一次性门控。 |
| `world_tuning/feature.cpp:551` | world_tuning | W | drop quantity above threshold | 倍率配置异常提示 | W | keep | 大倍率不被硬限制且触发掉落处理，单次告警门控。 |
| `world_tuning/feature.cpp:562` | world_tuning | D | drop settlement begin; side/rounds/reroll/pool | 掉落结算逐次明细 | D | keep | 已改为 level 0；逐次结算信息仅在 debug 开启时格式化。 |
| `world_tuning/feature.cpp:566` | world_tuning | D | drop settlement begin; pool 未知 | 掉落结算逐次明细 | D | keep | 已改为 level 0；仅池信息不可用的平行分支。 |
| `world_tuning/feature.cpp:578` | world_tuning | D | drop settlement end; 调用/空结果/池统计 | 掉落结算逐次明细 | D | keep | 已改为 level 0；逐次结算计数仅在 debug 开启时格式化。 |
| `world_tuning/feature.cpp:601` | world_tuning | D | make_item slot; sequence/category/lookup/pool | 逐物品处理细节 | D | keep | 已改为 level 0；每个 maker 调用的槽详情只用于调试。 |
| `world_tuning/feature.cpp:607` | world_tuning | W | make_item pool exhausted | 单次造物失败风险 | W | keep | 池满时原始创建可能空指针崩溃，告警带池使用量。 |
| `world_tuning/feature.cpp:619` | world_tuning | W | make_item returned null | 单次造物失败 | W | keep | 原始造物返回空，带分类、查找键及池状态。 |
| `world_tuning/feature.cpp:633` | world_tuning | W | drop reroll above threshold; path=drop | 掉落重抽倍率异常 | W | augment | 保留调用点，正文末尾追加 `path=drop` 与装备造物路径区分。 |
| `world_tuning/feature.cpp:679` | world_tuning | W | drop reroll above threshold; path=equip | 装备造物重抽倍率异常 | W | augment | 独立装备造物路径，保留调用点并追加 `path=equip`。 |
| `world_tuning/feature.cpp:755` | world_tuning | D | skill cooldown base/scaled/percent | 逐次冷却应用细节 | D | keep | 已改为 level 0；热路径数值仅在 debug 开启时格式化。 |
| `world_tuning/feature.cpp:773` | world_tuning | I | item.use subscribed id | 订阅成功 | I | keep | 订阅成功状态变化，subscription id 可追踪。 |
| `world_tuning/feature.cpp:788` | world_tuning | I | enabled | 功能启停 | I | keep | 有限状态变化。 |
| `world_tuning/feature.cpp:823` | world_tuning | W | stored rates invalid; keeping current values | 配置解析失败 | W | keep | 解析失败回退保留当前配置；游戏继续。 |
| `world_tuning/feature.cpp:827` | world_tuning | I | feature=world_tuning section=rates result=applied | 配置成功生效 | I | keep | 成功解析后调用 `apply_rates`，每次配置应用记录最终结果。 |
| `world_tuning/feature.cpp:896` | world_tuning | I | skill cooldown hook bound backup | hook 安装成功 | I | keep | hook 成功绑定，有限生命周期事件。 |
| `world_tuning/feature.cpp:911` | world_tuning | W | unknown resource id | 资源标识异常 | W | keep | 未识别 resource id，包含实际 id。 |
| `special_equip/feature.cpp:193` | special_equip | W | symbols unresolved; not installed | 特殊装备能力缺失 | W | keep | 已改为实际 level 2；依赖未解析使该功能未安装，但宿主游戏继续运行。 |
| `special_equip/feature.cpp:205` | special_equip | I | enabled | 功能启停 | I | keep | 有限状态变化。 |
| `special_equip/feature.cpp:223` | special_equip | W | unknown resource id | 资源标识异常 | W | keep | 本次资源处理异常，带 resource id。 |
| `special_equip/feature.cpp:331` | special_equip | W | sell rejected; menu char cannot change equip | 单次装备特殊逻辑拒绝 | W | keep | 已改为实际 level 2；单次出售门禁拒绝，不是生命周期变化。 |
| `special_equip/plugin.cpp:179` | special_equip | W | optional dependency unavailable | 能力缺失 | W | keep | 可选角色事实能力缺失；插件降级运行。 |
| `special_equip/plugin.cpp:195` | special_equip | W | sell gate unavailable; register rejected | 特殊装备逻辑单次失败 | W | keep | 已改为实际 level 2；gate 注册失败时游戏仍可运行。 |
| `character_panel/plugin.cpp:199` | core | W | open task registration failed reason=open_unavailable entry=open_task | 面板打开调度失败 | W | keep | 已改为实际 level 2；追加 `reason` 与 `entry` 区分失败类型和入口。 |
| `character_panel/plugin.cpp:233` | core | W | panel resource registration failed | 面板资源注册失败 | W | keep | 插件能力注册失败但游戏可继续；明确使用 level 2。 |
| `character_panel/plugin.cpp:235` | core | W | open task registration failed reason=open_unavailable entry=install | 面板启用阶段调度失败 | W | augment | 与安装阶段不同，保留此调用点并追加 `entry=install`。 |
| `character_panel/charinfo_watch.cpp:140` | core | W | character info popup restore failed reason=restore_rejected | 面板资源恢复失败 | W | keep | 已改为实际 level 2；失败原因明确。 |
| `character_panel/charinfo_watch.cpp:191` | core | W | overview open rejected reason=<branch> | 面板打开失败 | W | augment | 已改为实际 level 2；按实际分支写 `capability_missing`、`open_unavailable` 或 `open_rejected`。 |

## 缺失日志建议

| file + 函数 | 事件 | 目标等级 | 必填字段 | 理由 |
|---|---|---|---|---|
| `craft_lottery/feature.cpp` + `process_recipe_wrapper` | 不新增：无不可恢复的奖池解析失败分支 | — | — | 只读取宿主 `pool_read` 的结果；失败或容量越界时回退原版并记 W，无独立解析器或不可恢复解析错误。 |
| `special_equip/rules.cpp` + 规则判定 | 不新增：无规则数据解析/加载分支 | — | — | 该文件实现纯规则判定；插件依赖/符号缺失已是 W，没有不可恢复规则解析错误分支。 |

## 统计

- 事件日志：37 条（原有 36 条，新增配置生效 INFO 1 条）；`attribute_range_display`：0 条日志。
- 插件条数：`core.craft` 4，`craft_lottery` 3，`world_tuning` 19，`special_equip` 6，`character_panel` 5，`attribute_range_display` 0。
- 最终处置：keep 33，demote 0，promote 0，delete 0，merge 0，augment 4。
- 缺失日志建议：2 项批准并已实现，2 项检查后不适用并跳过。

## 审核裁决

### 批准并已执行

- demote 12 条均已执行：`world_tuning/feature.cpp` 的伤害 trace、掉落结算起止、maker 槽、冷却细节改为 level 0；`special_equip/feature.cpp:193,331`、`special_equip/plugin.cpp:195` 改为实际 level 2；`character_panel/plugin.cpp:199` 与 `charinfo_watch.cpp` 的恢复/打开失败改为实际 level 2。
- `world_tuning/feature.cpp` 的两条重抽告警保留独立调用点：掉落路径追加 `path=drop`，装备造物路径追加 `path=equip`。`character_panel/plugin.cpp:199,235` 保留安装阶段及启用阶段日志，分别追加 `entry=open_task`、`entry=install`。
- 配置成功生效日志已添加，正文为 `feature=world_tuning section=rates result=applied`，等级 I，在成功解析并应用 rates 后每次记录一次。
- 面板打开失败日志补入实际 `reason=`：缺 capability 为 `capability_missing`，打开函数缺失为 `open_unavailable`，调用拒绝为 `open_rejected`；未增加独立日志点。

### 检查后跳过

- `craft_lottery` 无不可恢复的奖池解析分支：仅 `pool_read` 失败或 `pool_count` 越界时回退原版并记录可恢复降级；没有加载/解析器，未新增 E。
- `special_equip` 无规则数据解析/加载分支；`rules.cpp` 为输入值判定，依赖缺失已有 W，未新增 E。

### 验证

- `cmake --build build -j`：失败。并行工作区既有修改 `plugins/settings/plugin.cpp:182` 的 `private_log` 未声明；本批涉及插件目标已通过构建。
- `ctest --test-dir build --output-on-failure`：通过，164/164。
- `uv run python scripts/verification/check_log_policy.py --warn-only`：通过；0 errors、37 warnings，告警均在本批插件范围外。
- `git diff --check`：通过。

## Top 发现

1. `world_tuning` 的 6 处逐次伤害/掉落/冷却细节已由 INFO 改为 D。
2. `special_equip` 的符号缺失、sell gate 注册失败与出售拒绝均为单次可恢复能力/操作失败，已统一写 level 2。
3. `character_panel` 的面板调度、popup 恢复和 overview 打开失败现写 level 2，并记录具体原因/入口。
4. rates 配置成功应用现记一条 INFO：`feature=world_tuning section=rates result=applied`。
5. `craft_lottery` 与 `special_equip` 未发现不可恢复的数据解析失败分支，因此没有新增 E 日志。
