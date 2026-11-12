# 小型插件功能级日志覆盖审计

依据 `docs/reference/logging-standards.md`。只核查功能生命周期与关键业务节点的日志覆盖；`present` 表示有可定位日志，`weak` 表示仅覆盖部分生命周期/失败或仅 DEBUG，`missing` 表示关键结果节点无日志。逐帧及逐行渲染事件不建议 INFO。

## world_tuning

功能与装配：`plugin.cpp:505-532` 定义 `world_tuning.panel`；插件安装/启停、rates 配置加载和 optional item.use 订阅分别在 `plugin.cpp:401-470`。倍率实际作用点位于 `feature.cpp` wrappers。

| 功能 / 节点 | 启停 / 打开关闭 | 触发 / 成功生效 | 拒绝 / 可降级失败 / 不可恢复失败 | 恢复原值 / 收敛 | 判定 |
|---|---|---|---|---|---|
| 总开关 | `feature.cpp:786-789` `enabled` INFO；install/deactivate/enable/disable 调用 `plugin.cpp:401-470` | 由开关控制倍率 wrappers | 安装失败通过 `g_install_error` 暴露（`plugin.cpp:411-444`），缺失败日志；未知资源有 W（`feature.cpp:911`） | disable 关 gate 有 INFO | weak |
| rates 配置加载/生效 | 无单独配置开关日志 | 成功 apply 有 I（`feature.cpp:812-828`）；插件安装/启用及面板打开时 reload（`plugin.cpp:443,464`、`panel.cpp:393-404`） | decode 失败有 W（`feature.cpp:822-824`）；read_config/API 不可用或读取失败直接返回，无日志（`feature.cpp:812-820`） | 配置 reload 后值替换 | weak |
| 面板打开、关闭、刷新 | 面板 provider 注册/注销及开关状态无业务日志（`plugin.cpp:291-339,478-503`）；callbacks `panel.cpp:500-521` | 面板打开成功、翻页及 `arrow_clicked` 配置更新成功均无日志（`panel.cpp:273-309,393-407`） | 打开失败返回错误结果但无日志（`plugin.cpp:478-502`）；配置请求失败/结果未知仅更新面板文案，未记日志（`panel.cpp:248-270,281-296`） | close/destroy 无日志（`panel.cpp:500-507,532-537`） | missing |
| 面板 rates 保存并应用 | 不适用 | 持久化成功后 `apply_rates`（`panel.cpp:258-270`）没有最终 I；此前 `reload_rates_from_config` 的 I 不覆盖这次用户提交 | receipt 查询失败、持久化失败均只显示文案（`panel.cpp:251-266,289-291`） | 失败回滚到 `g_confirmed`，无日志 | missing |
| damage in/out、经验、金币倍率 | 由总开关控制 | damage 有限次 D（`feature.cpp:290-311`）；经验/金币直接缩放后调原函数，无成功日志（`feature.cpp:385-415`） | 依赖缺失时 damage 分类仅记一次 W（`feature.cpp:116-120`）；部分分类失败作为 neutral 静默处理 | 动态恢复原版路径，无恢复事件日志 | weak |
| 敌方 max HP 倍率 | 由总开关控制 | 成功缩放及 hp clamp 无日志（`feature.cpp:312-383`） | 池槽无法定位有一次 W（`feature.cpp:332-339`）；读取/写入失败静默回退 | wrapper 通过 `HpMark` 将先前 applied 写回 raw（`feature.cpp:342-355`），无恢复日志 | weak |
| 掉落数量/重抽、道具/装备造物 | 由总开关控制 | 结算与逐造物信息现为 D（`feature.cpp:527-619`）；重抽倍率超阈值有 W（`feature.cpp:633,679`） | 池耗尽和 maker 返回 null 有 W（`feature.cpp:607-622`）；部分候选读取/释放失败静默跳过 | 丢弃候选/保留最佳项，无最终重抽结果日志（`feature.cpp:627-650,665-689`） | weak |
| 技能/药水冷却倍率 | 由总开关控制；item.use 订阅成功有 I（`feature.cpp:761-789`） | 技能 cooldown 有限次 D（`feature.cpp:735-758`）；药水 cooldown 改写无日志（`feature.cpp:227-248`） | capability 缺失/读写失败静默跳过 | 动态关闭后使用原始 duration/tick，无恢复日志 | weak |

## special_equip

功能与装配：`plugin.cpp:234-258` 定义 Hidden toggle `special_equip.equipment_rules`；装备/卸下判断 wrappers 在 `feature.cpp:231-310`，出售门禁在 `feature.cpp:313-332`。

| 功能 / 节点 | 启停 / 打开关闭 | 触发 / 成功生效 | 拒绝 / 可降级失败 / 不可恢复失败 | 恢复原值 / 收敛 | 判定 |
|---|---|---|---|---|---|
| 特殊装备规则总开关 | `feature.cpp:203-205` 写 enabled INFO；plugin lifecycle 切 gate（`plugin.cpp:142-225`） | gate 开启，无单独规则生效结果 | 依赖缺失/安装失败有 W（`plugin.cpp:179,185-188`）；sell gate 注册失败有 W（`plugin.cpp:191-197`） | deactivate/disable 关闭 gate 有 INFO；规则不改写装备数据 | present |
| 装备 / 卸下规则覆写 | 成功放行及拒绝诊断仅 DEBUG（`feature.cpp:231-310`；`log_reject` 经 debug gate，`feature.cpp:107-155`） | 原版拒绝后模块规则放行有 DEBUG（`feature.cpp:283,309`） | 原版拒绝及依赖读取失败走 DEBUG `log_reject`；无法定位时没有独立 W（大部分属于预期规则拒绝） | wrapper 返回判定值，不保存/修改原装备数据 | weak |
| 出售保护 | 随 gate 启停 | 没有单独“保护已应用”结果日志 | 实际拦截有 W（`feature.cpp:313-332`）；早期缺依赖/读取失败直接 block，无原因日志（`feature.cpp:317-329`） | deactivate 清除 intercept（`plugin.cpp:202-212`），无解除结果日志 | weak |

## character_panel

功能与装配：`plugin.cpp:260-275` 定义 `character_panel.i18n` 和 `character_panel.overview`；安装注册消费者/provider 与帧任务在 `plugin.cpp:136-200`，enable/disable 在 `229-242`。

| 功能 / 节点 | 启停 / 打开关闭 | 触发 / 成功生效 | 拒绝 / 可降级失败 / 不可恢复失败 | 恢复原值 / 收敛 | 判定 |
|---|---|---|---|---|---|
| 属性面板汉化 | Feature callback 切换 translation gate（`plugin.cpp:261-267`），无启停日志 | text consumer 注册成功无日志（`feature.cpp:45-68`） | enable 时注册失败有 W（`plugin.cpp:232-234`）；install 注册失败会返回失败但无日志（`plugin.cpp:179-188`） | disable/deactivate 注销 consumer，无结果日志（`feature.cpp:71-76`、`plugin.cpp:203-227`） | weak |
| 属性总览打开/关闭 | Feature callback 切换 enabled 无日志（`plugin.cpp:268-274`） | 原版内容区点击到 pending、overview 打开成功无 I（`charinfo_watch.cpp:147-193`）；panel enter 读取快照无日志（`overview.cpp:432-439`） | capability/open 失败有 W 和 reason（`charinfo_watch.cpp:69-90,187-193`）；panel/provider 创建失败返回 false 但无日志（`overview.cpp:245-279,415-429`） | close/deactivate 关闭面板无日志（`overview.cpp:419-422`、`plugin.cpp:203-227`）；popup restore 失败有 W（`charinfo_watch.cpp:137-143`） | weak |
| 属性数据刷新与逐行渲染 | 每次打开读取一次；process 每帧绘制（`overview.cpp:432-456`） | 成功刷新、面板开关无 I；逐行/逐列绘制没有日志（合规，勿加 INFO） | 快照/属性/derived 读取失败写入 `g_snapshot.ok=0`，无聚合失败日志（`overview.cpp:178-200`）；展示可退化但原因不可见 | 再次打开重新快照；关闭清控件，无状态日志 | weak |
| 插件能力注册/帧任务 | open task 注册失败有 W（`plugin.cpp:199,235`）；consumer registration failure 在 enable 有 W（`plugin.cpp:232-234`） | 安装、provider 注册成功无 I | 安装阶段所需 capability/provider 注册失败返回 1，无原因日志（`plugin.cpp:136-197`） | deactivate remove task/unregister，无成功/失败结果日志（`plugin.cpp:203-227`） | weak |

## craft_lottery

功能与装配：`plugin.cpp:96-104` 定义 `craft_lottery.guarantee`，hook `F_CHAR_PROCESS_RECIPE` 在 `plugin.cpp:23-38`。执行逻辑在 `feature.cpp:60-140`。

| 功能 / 节点 | 启停 / 打开关闭 | 触发 / 成功生效 | 拒绝 / 可降级失败 / 不可恢复失败 | 恢复原值 / 收敛 | 判定 |
|---|---|---|---|---|---|
| 保底开关 | `set_gate` 记录 enabled I（`feature.cpp:143-146`），plugin lifecycle 调用（`plugin.cpp:52-89`） | gate 启用后接管配方处理 | 安装 context/port 校验失败返回 1，无日志（`plugin.cpp:52-73`） | disable/deactivate 关闭 gate，有 I；无独立 hook 恢复结果 | weak |
| 抽奖/配方处理 | process recipe 入口 `feature.cpp:60-73` | 成功选中配方、加书并显示 popup，无 I（`feature.cpp:95-140`） | `pool_read`/popup text/popup 显示的部分降级有一次性 W（`feature.cpp:47-50,73-76,127-139`）；`recipe_learned`、random、add book 等失败直接回原版且不记日志（`feature.cpp:83-124`）；未见不可恢复解析错误 | fallback 调原版，未记录 fallback 成功结果 | weak |

## core.craft

功能与装配：descriptor 只定义隐藏 provider toggle `core.craft.provider`（`plugin.cpp:185-206`），提供 craft facts API（`plugin.cpp:107-119`）及 QolCraftPort 转发，不在本插件内执行合成。

| 功能 / 节点 | 启停 / 打开关闭 | 触发 / 成功生效 | 拒绝 / 可降级失败 / 不可恢复失败 | 恢复原值 / 收敛 | 判定 |
|---|---|---|---|---|---|
| Craft facts provider 生命周期 | install/deactivate/enable/disable 均有 I（`plugin.cpp:145-176`） | API provision/read-through 成功无事件日志（`plugin.cpp:20-119`） | install context/port/ABI 校验失败直接返回 1，无 W/E（`plugin.cpp:145-160`）；read API not-ready 以 QolResult 返回（`plugin.cpp:16-24`） | deactivate 清空 port，有生命周期 I | weak |
| 配方/合成执行 | 此组件无执行入口或合成 hook；消费方位于其它插件范围 | 不适用 | 不重复建议跨组件业务日志 | 不适用 | present（职责边界明确） |

## attribute_range_display

功能与装配：descriptor 定义 `attribute_range_display.ranges` toggle 和两个 optional hooks（`plugin.cpp:20-48,78-82`）；仅对装备/宝石原生词条按 quality 插入颜色范围提示（`feature.cpp:238-289`）。

| 功能 / 节点 | 启停 / 打开关闭 | 触发 / 成功生效 | 拒绝 / 可降级失败 / 不可恢复失败 | 恢复原值 / 收敛 | 判定 |
|---|---|---|---|---|---|
| 范围提示总开关 | install/enable 设置 active，disable/deactivate 清 active（`plugin.cpp:51-69`），无日志 | toggle callback 无日志；颜色 span 插入成功无日志（`feature.cpp:211-215,238-289`） | 两个资源为 optional；backup 缺失时关闭 active 并返回失败（`feature.cpp:217-235`），无日志；能力/item view 不完整时静默跳过（`feature.cpp:40-65,85-134`） | wrapper 保留原始 AddOption 调用，不写 item 数据；deactivate 无日志 | missing（本组件现有日志为 0 条） |

## 优先补全项

1. **world_tuning 面板持久化/应用终态**：`panel.cpp:248-270` 与 `281-296`，仅在配置成功写入并 `apply_rates` 后记录一次 I；receipt 失败以唯一 W 记录 `section=rates reason=`，避免逐帧 poll 日志。
2. **world_tuning 面板打开失败**：`plugin.cpp:478-502`，open callback 返回错误但不落日志；在最终拒绝处记录一次 W，包含 `panel=world_tuning reason=`，不要在每个依赖探测层重复。
3. **character_panel overview 刷新退化**：`overview.cpp:178-200`，聚合一次 snapshot/关键字段读取结果；仅当必要快照整体失败时 W，逐字段/逐行细节保持 D 或不记录。
4. **character_panel 打开/关闭成功状态**：`charinfo_watch.cpp:187-193` 与 `overview.cpp:432-439,419-422`；分别在面板真正进入 active、关闭状态变化时 I，失败仍保留现有 W，避免把 pending click 当作已打开。
5. **craft_lottery 配方处理终态**：`feature.cpp:60-140`，记录一次完成结果 I（`use_type`、选中 recipe、`result`）；失败转原版时只在唯一 fallback 收敛点 W/I，附 `reason`/`fallback=original`，不要给每个 pool row 加日志。
6. **core.craft install 失败**：`plugin.cpp:145-160`，provider 能力不可用但游戏仍可运行，补一条 W（`op=install reason=`）；read-through API 的单次 not-ready 可由调用方处理，不在每个 accessor 重复写日志。
7. **attribute_range_display hook 能力缺失**：`feature.cpp:217-235`，optional hook backup 缺失/解除时至少 D；若组件启用后因此整体不提供属性提示，单次 W，区分可选资源正常未安装与功能降级。
