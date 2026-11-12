# 功能级日志覆盖审计：custom_recipe

依据 `docs/reference/logging-standards.md`（等级、正文、频率契约）与 `docs/reference/plugins/custom_recipe.md`（组件职责、数据和生命周期）。只读检查：插件 descriptor/装配（`plugin.cpp`）、静态及动态配方目录（`model/catalog.*`）、匹配/结果契约（`model/rules.*`、`model/result.h`）、RECIPEBASE/MIXTUREBASE ownership、动态存档就绪入口及现有日志调用点。状态仅描述功能事件的可观测覆盖，不评价业务正确性。

## 功能事件覆盖

| 功能事件 | 触发条件 | 目标等级 | 现有日志 file:line | 状态 | 建议含文案 |
|---|---|---|---|---|---|
| Descriptor 安装/依赖及资源事务失败 | 插件依赖、端口校验、核心 UI hook 或 ownership 安装失败 | W；若单 claim 可恢复，记录可重试状态 | `host/plugin_registry.cpp:2702-2709,2751-2761`（宿主通用 resource WARN）；组件装配 `plugins/custom_recipe/plugin.cpp:141-308` 本身无日志 | present | 不建议组件重复记宿主的失败细节；现有宿主行已有 `plugin/resource/code`。 |
| 插件/配方功能启用 | 开关开启且 ownership 与核心 hook 均就绪，业务门禁转为 enabled | I | `host/plugin_registry.cpp:2870-2872` 记录 `op=activate plugin=custom_recipe result=active`；组件门禁 `plugins/custom_recipe/plugin.cpp:340-346` 无专属日志 | weak | 门禁真实打开时可记 `start feature=recipes result=enabled ownership=ready`，与插件安装成功区分。 |
| 配方功能停用/门禁关闭 | 用户关闭 feature，或插件停用，作废 pending 并撤回 ownership | I | `plugins/custom_recipe/plugin.cpp:349-354,443-445`；`result/feature.cpp:2039-2045`；无组件级停用结果日志 | missing | 在实际状态变化处记 `stop feature=recipes result=disabled`；不要对幂等重复调用重复输出。 |
| 配方 UI 核心 hook 安装结果 | 5 条 required hook 事务完成或回滚 | 成功 I；单 hook 失败 W | 声明在 `ui/runtime.cpp:767-783`；宿主失败日志 `host/plugin_registry.cpp:2702-2709`，成功汇总 `host/plugin_registry.cpp:2870-2872` | present | 无需增加逐 hook 插件日志；宿主事务已覆盖成功激活与失败资源标识。 |
| RECIPEBASE/MIXTUREBASE 注入成功 | ownership acquire 注入静态配方记录 | I | `table/table.cpp:380` (`recipe table injected base/records/materials/book_bytes`) | present | 已覆盖注入结果与规模。 |
| 游戏重载配方表后的再注入 | 发现游戏将表指针换回原表，启动重注入 | I | `table/table.cpp:314` `recipe table replaced by game, re-inject count`；最终注入 `:380` | present | 已记录启动及成功结果。 |
| 表未就绪、布局拒绝或分配失败 | 首次/重试获取表时字段缺失、布局不兼容或内存分配失败 | W（单次失败、游戏继续） | `table/table.cpp:286,335,364`；ownership 汇总失败 `result/feature.cpp:2011-2013` | present | 失败原因和表尺寸/记录数已在底层记录；避免再加相同的上层失败行。 |
| Ownership 释放并恢复原版记录 | feature 关闭后还原组位与原始记录数 | I | 行为入口 `table/table.cpp:388-408`、`plugin.cpp:349-354`；无成功恢复日志 | missing | 当确实恢复了记录时记 `restore op=recipe_table result=restored records=<base_count>`；空操作/重复释放不记。 |
| 配方创建、编辑、删除 | 作者运行期增删改配方 | 不适用（当前没有该功能） | 静态目录只有 `model/catalog.cpp:56-66` 的两条入口与 `:89-114` 的编译期三格规则；无 CRUD API/UI | missing（功能不存在） | 无日志建议；若未来加入编辑器，再按具体提交/拒绝事件审计。 |
| 配方列表与分页 | 游戏展示注入后的列表、切换列表页/配方页 | 状态变化 I；分页/选择过程 D | 配方按钮选择调试日志 `ui/runtime.cpp:693`；菜单入口 `ui/runtime.cpp:670-673` 无 open/close 日志；注入规模见 `table/table.cpp:380` | weak | 若组件能识别真实视图切换，只记一次 `open panel=craft section=custom_recipe`；不按逐页绘制/每帧记录。配方点击细节维持 D。 |
| 放料材料校验与拒绝 | 重复放入、持有数量不足、槽无效或写槽失败 | 正常匹配细节 D；实际写入失败 W | 判定位于 `result/feature.cpp:1095-1130`；UI 只记录写失败 `ui/runtime.cpp:415-420`，重复/数量不足仅提示 | weak | 只在 debug 诊断时记 `reject op=place item=<handle> slot=<index> reason=already_placed|not_enough_held`；不把正常拒绝升为高频 INFO。 |
| 静态/动态三格规则未命中 | 合成触发查询而类别组合没有匹配项 | D | `result/feature.cpp:1144-1152`（`three slot miss slots=...`）；UI 提示分支 `ui/runtime.cpp:548-555` | present | 已有类别输入与 D 级匹配结果；避免再加 INFO。 |
| 三格规则命中并进入确认 | 配方命中、槽内 handle/category 复核成功，创建 Yes/No pending | 成功匹配细节 D | `result/feature.cpp:1137-1205` 成功路径不记录命中的配方/产品；弹窗索引诊断 `ui/runtime.cpp:198-201` 不含配方身份 | weak | 在一次成功 prepare 节点补 `match recipe=<id> slots=<...> result=confirm_pending`，仅 D；以一次调用表达命中，不逐个内部校验点记日志。 |
| 命中后身份/类别校验拒绝 | 配方已匹配但 live handle、槽位类别读取或输入身份不一致 | W | `result/feature.cpp:1157-1201`；UI 汇总 `ui/runtime.cpp:568-575` | present | 结果层已有 stage、slot、result/handle 或 actual/expected；避免 UI 重复记同一失败。 |
| 三格合成执行、扣料及保存失败 | 确认后预检、扣料、产物准备/保存任一步失败 | W；扣料/对象变化细节 D | 阶段日志 helper `result/feature.cpp:881-888`，调用覆盖 precheck 至 save `:1241-1678`；产物保存失败 `:1681-1684`；清理失败 `:904-915` | present | 阶段日志含 stage/index/result/before/after；继续保持一阶段一根因，不另加重复的上层失败汇总。 |
| 三格合成成功 | 产物已提交或原物品更新完成 | I | `result/feature.cpp:1688-1693` `three slot crafted product/consumed` | present | 已是有限玩家操作结果，包含产物与消耗数量。 |
| 三同档宝石委托原版合成 | 三格规则 3–6 命中并委托 StartMix，返回后核验三格清空状态 | 成功 D；未清空 W | `ui/runtime.cpp:321-339` 的委托后完整核验（级别依 cleared 而定）；委托失败 `:299-310` | present | 已有调用前后槽位、句柄、委托配方类型及 cleared 结果。 |
| 升阶目标 gate 拒绝/匹配 | 查询宝石/装备、质量事实、词条候选及可升阶状态 | 单次服务/事实读取失败 W；逐条规则拒绝或匹配细节 D | W `result/feature.cpp:955-989,1034-1039`；D `:999,1013,1045,1075,1082` | present | 已覆盖 gate 失败原因、目标身份与匹配结果；细节受 gate budget 限制。 |
| 宝石/装备升阶提交成功 | 新值及材料需求写入成功，原物品完成更新 | I（结果状态）；数值/区间过程 D | `result/feature.cpp:1830-1833,1962-1968` 仅输出限额 D 数值细节 | weak | 在成功提交后补一条 `apply op=tier_up item=<handle> mixType=<id> result=updated` I；详细数值继续留在现有 D。 |
| 升阶写入/计算失败 | quality、随机、材料需求、payload 或 replace 失败 | W | `result/feature.cpp:1742-1826,1885-1959` 多数失败有 W；部分 MakeItem 早退没有独立日志（如 `:1711-1721,1733,1811-1826` 周边） | weak | 优先确保提交/replace 失败有 `fail op=tier_up item=... reason=...`；不为每个纯 helper/早期 guard 各加一条日志。 |
| SaveReady 后动态配方生成/缓存恢复 | 取得当前 slot 创建时间，用 seed 生成或命中进程缓存并发布动态表 | 成功 I；单次恢复失败 W | `result/feature.cpp:641-681`（save facts/slot/time/cache 失败与 `dynamic special recipes refreshed` 成功） | present | 已覆盖失败原因、slot、缓存命中、目录数量与 reroll 数。此功能只读取 save facts，不是插件持久化。 |
| 自定义配方持久化保存/加载 | 将用户创建的配方数据序列化至存档并重载 | 不适用（当前不持久化配方数据） | 权威架构说明 `docs/reference/plugins/custom_recipe.md:47-50,121-132`；组件只有 `save.facts` seed 读取 `result/feature.cpp:641-681` | missing（功能不存在） | 无日志建议；不能把动态目录的 seed 重建称为配方数据持久化。 |
| 导入/导出配方 | 外部配方文件或文本导入、导出 | 不适用（当前没有该功能） | `plugin.cpp:435-467` descriptor 无 route/API；目录无导入导出入口 | missing（功能不存在） | 无日志建议。 |
| 自定义配方数据损坏修复/降级 | 解析持久化自定义配方数据并修复或放弃 | 不适用（没有持久化数据格式、解析器或修复分支） | `docs/reference/plugins/custom_recipe.md:47-50`；`model/catalog.cpp:56-66,89-114` 为静态常量；仅动态生成失败日志 `result/feature.cpp:641-676` | missing（功能不存在） | 无新增 E 建议；若未来引入持久化格式，再在不可恢复损坏的唯一收敛分支记 `E op=restore result=failed slot=<...> reason=<...>`。 |

## 优先补全项

1. **P1：业务门禁/feature 停用**——补一次性 I `stop feature=recipes result=disabled`；需要与宿主的 plugin activate 区分 feature gate 生命周期。
2. **P1：ownership 释放成功**——仅在原版组位/记录数实际恢复时补 I `restore op=recipe_table result=restored records=<base_count>`。
3. **P2：升阶提交成功**——在既有 D 数值细节之外补一条 I，确认宝石/装备确已更新。
4. **P2：规则命中进入确认**——补单条 D 的 `recipe/slots/result=confirm_pending`；未命中 D 已有，不重复覆盖。
5. **P3：常见放料拒绝细节**——只对 debug 门控的 already-placed / insufficient-held 记录 D，不增加每次点击 INFO。
6. 配方 CRUD、持久化、导入导出及损坏修复当前均不存在；除未来新增这些能力外，无需为不存在的生命周期补日志。
