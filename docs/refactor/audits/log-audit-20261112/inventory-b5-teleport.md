# 日志审计清单：批次 5（world_teleport）

依据 `docs/reference/logging-standards.md` 与 `docs/reference/logging.md`。范围为 `module/app/src/main/cpp/plugins/world_teleport/**`。按要求检索 `log_message(`、`log_info(`、`log_warn(`、`log_error(`、`QOL_LOG_`、`log.write(`：发现 42 条业务日志调用，另有 `feature.cpp:61` 通用 `log_message` 实现和 `feature.cpp:74` SDK 写出口；后两者不是独立事件。`log_message` 的真实等级由数值参数决定：0=D、1=I、2=W、3=E；本范围 42 条均为字面量等级。`feature.cpp:61` 的 `log.write(12, ...)` 表明域号 12，对应 `QolDomain::kUi`，因此表中 domain 为 `ui`。

## 现有日志

| file:line | domain | level_now | message 摘要 | event_class | level_target | disposition | rationale |
|---|---|---:|---|---|---:|---|---|
| `feature.cpp:199` | ui | I | choice opened；current；top=-（SDK 缺失） | 传送选择 UI 打开 | I | keep | `open_choice_panel` 成功后诊断 UI 顶层；此分支只在能力接口缺失时输出一次，保留用户操作事件。 |
| `feature.cpp:209` | ui | I | choice opened；current；top=-（ui.state 不可用） | 传送选择 UI 打开 | I | keep | 面板已打开，`top=-` 表示可选诊断能力无效，不影响面板；事件频率为每次打开一次。 |
| `feature.cpp:214` | ui | I | choice opened；current；top=-（read_layers 缺失） | 传送选择 UI 打开 | I | keep | 面板已打开，分支仅改变 top-layer 诊断结果；保留打开状态记录。 |
| `feature.cpp:219` | ui | I | choice opened；current；top；type | 传送选择 UI 打开 | I | keep | 成功打开后记录当前地图及顶层 UI；符合每次用户操作一次。 |
| `feature.cpp:227` | ui | E | choice symbols not resolved | 地图符号缺失 | W | augment | 只影响本次选择面板，游戏继续；改 W 并补 `reason=map_facts_unresolved`。 |
| `feature.cpp:231` | ui | I | map records count；max_map_id；current | 地图列表构造诊断 | D | demote | 这是枚举目标前的过程数据，不是地图列表加载完成事件；不得用 I 记录过程细节。 |
| `feature.cpp:257` | ui | E | static map data missing；target | 目标静态地图数据缺失 | W | augment | 目标被置为不可用但游戏继续；改 W 并补 `reason=landing_data_unavailable`，保留 target。 |
| `feature.cpp:287` | ui | E | ui_choice capability unavailable | UI 能力缺失 | W | augment | UI 能力缺失使传送面板不可用、游戏可继续；改 W，并按静态无变量文案规则补 `reason=capability_unavailable`。 |
| `feature.cpp:291` | ui | E | choice open failed | UI 打开失败 | W | augment | 单次打开失败但游戏继续；改 W 并补 `reason=api_rejected`。 |
| `feature.cpp:308` | ui | E | confirmation symbols not resolved | 确认框符号缺失 | W | augment | 本次确认操作不可用但游戏继续；改 W 并补 `reason=confirmation_symbols_unresolved`。 |
| `feature.cpp:316` | ui | E | confirmation open failed；target | 确认框打开失败 | W | augment | 单次 UI 操作失败；改 W 并补 `reason=create_yesno_failed`。 |
| `feature.cpp:319` | ui | I | confirmation opened；target；cost | 传送确认框打开 | I | keep | 有限用户操作状态变化，带目标及费用。 |
| `feature.cpp:328` | ui | E | delayed choice registration failed | 延迟任务注册失败 | W | augment | 单次打开流程失败、游戏继续；改 W 并补 `reason=scheduler_rejected`。 |
| `feature.cpp:339` | ui | E | instant message symbol not resolved | 提示能力缺失 | W | augment | 提示未显示但游戏继续；改 W，并补 `reason=show_banner_unavailable` 以满足无变量失败文案要求。 |
| `feature.cpp:342` | ui | I | instant message shown；text | 提示显示成功 | I | keep | 调用方仅在 show_banner 成功时记录，属于有限单次操作结果。 |
| `feature.cpp:354` | ui | E | transfer symbols not resolved（基础传送接口） | 传送接口缺失 | W | augment | 本次传送不可执行而游戏继续；改 W 并补 `reason=transfer_symbols_unresolved`。 |
| `feature.cpp:359` | ui | E | transfer symbols not resolved（收费接口） | 收费传送接口缺失 | W | augment | 收费传送不可执行而游戏继续；改 W 并补 `reason=charge_symbols_unresolved`，区分基础接口分支。 |
| `feature.cpp:365` | ui | E | active player unavailable（方向读取失败） | 玩家数据读取失败 | W | augment | 单次操作失败但游戏继续；改 W 并补 `reason=direction_unavailable`。 |
| `feature.cpp:372` | ui | E | active player unavailable（金钱读取失败） | 玩家数据读取失败 | W | augment | 仅本次收费传送无法继续；改 W 并补 `reason=money_unavailable`，区分方向读取分支。 |
| `feature.cpp:377` | ui | I | insufficient money；target；cost；money | 传送请求因余额不足被拒 | W | augment | 这是操作失败而非状态变化；改 W，现有 target/cost/money 已足，补 `reason=insufficient_money`。 |
| `feature.cpp:383` | ui | E | charge failed；target；cost | 扣费失败 | W | augment | 单次扣费失败但游戏继续；改 W 并补 `reason=minus_money_failed`。 |
| `feature.cpp:393` | ui | E | transfer failed；target；cost=0 | 免费传送失败 | W | augment | 单次切图请求失败；改 W 并补 `reason=map_change_failed`。 |
| `feature.cpp:400` | ui | E | transfer failed；target；cost；refunded | 收费传送失败/退款结果 | W | augment | 单次切图失败且记录退款结果；改 W 并补 `reason=map_change_failed`。 |
| `feature.cpp:404` | ui | I | transferred；target；cost；landing | 传送成功 | I | keep | 传送成功的最终结果，目标、费用及落点齐备。 |
| `feature.cpp:414` | ui | E | choice close symbols not resolved | 关闭面板接口缺失 | W | augment | 本次关闭/传送链失败但游戏继续；改 W 并补 `reason=close_or_restore_unavailable`。 |
| `feature.cpp:420` | ui | E | transfer registration failed | 传送任务注册失败 | W | augment | 单次传送流程失败；改 W 并补 `reason=scheduler_rejected`。 |
| `feature.cpp:430` | ui | E | confirm symbols not resolved | 确认回调依赖缺失 | W | augment | 本次确认动作不可执行但游戏继续；改 W 并补 `reason=confirm_action_unavailable`。 |
| `feature.cpp:434` | ui | E | transfer registration failed（确认后） | 确认后传送任务注册失败 | W | augment | 单次操作失败；改 W 并补 `reason=scheduler_rejected`。 |
| `feature.cpp:446` | ui | E | choice reopen registration failed | 取消后重开面板任务失败 | W | augment | 取消后的重开失败但游戏继续；改 W 并补 `reason=scheduler_rejected`。 |
| `feature.cpp:448` | ui | I | confirmation cancelled | 用户取消确认 | I | keep | 用户操作使确认流程状态变化，一次操作记录一次。 |
| `feature.cpp:456` | ui | E | confirm symbols not resolved（选择入口） | 选择回调依赖缺失 | W | augment | 本次选择无效但游戏继续；改 W 并补 `reason=selection_context_unavailable`。 |
| `feature.cpp:463` | ui | W | invalid target；index；map_id | 不可用目标被选择 | W | augment | 目标不可达时拒绝本次请求，等级正确；补 `reason=target_unavailable`。 |
| `feature.cpp:474` | ui | E | transfer registration failed（免费路径） | 免费传送任务注册失败 | W | augment | 单次传送流程失败但游戏继续；改 W 并补 `reason=scheduler_rejected`。 |
| `feature.cpp:477` | ui | I | selected；index；target；cost=0 | 用户选择过程 | D | demote | 选择是过程细节，不是最终状态变化；后续成功/失败日志负责记录结果。 |
| `feature.cpp:483` | ui | E | confirm symbols not resolved（收费路径） | 收费确认依赖缺失 | W | augment | 本次操作失败但游戏继续；改 W 并补 `reason=create_yesno_unavailable`。 |
| `feature.cpp:493` | ui | E | confirmation registration failed | 确认框任务注册失败 | W | augment | 单次确认流程失败但游戏继续；改 W 并补 `reason=scheduler_rejected`。 |
| `feature.cpp:496` | ui | I | selected；index；target；cost | 用户选择过程 | D | demote | 选择是过程细节；确认框或之后的传送结果另有日志，故降为 D。 |
| `feature.cpp:512` | ui | E | banner map id unavailable（当前地图读取失败） | 地图横幅数据读取失败 | W | augment | 横幅失败不阻断游戏；改 W 并补 `reason=current_map_unavailable`。 |
| `feature.cpp:518` | ui | E | banner map id unavailable（地图名缺失） | 地图横幅数据读取失败 | W | augment | 横幅失败不阻断游戏；改 W 并补 `reason=map_name_unavailable`，与读取 map id 失败区分。 |
| `feature.cpp:523` | ui | E | state unavailable | 世界状态读取失败 | W | augment | 当前调用被中止但游戏继续；改 W 并补 `reason=world_state_unavailable`。 |
| `feature.cpp:526` | ui | I | CallMapName intercepted；state | hook 入口过程诊断 | D | demote | 每次原入口调用均会记录，不表示状态变化；应为 D。 |
| `feature.cpp:528` | ui | W | ignored CallMapName outside world；state | 非世界态可选路径跳过 | D | demote | 处于非世界态是正常可选路径，不是失败；仅作为诊断信息用 D。 |

## 缺失日志建议

| file + 函数 | 事件 | 目标等级 | 必填字段 | 理由 |
|---|---|---|---|---|
| `feature.cpp` + `delayed_transfer` / `teleport_on_select` | 传送执行最终结果（成功、失败或目标不可达） | I（成功）/ W（失败、不可达） | `op=transfer`、`target=`、`result=`；失败时 `reason=`；收费时 `cost=`、退款时 `refunded=` | 当前成功与失败分支各自输出，但没有一个统一覆盖最终结果的事件；不可达目标在选择时单独报错，建议统一结果语义以便确认请求结局。 |
| `feature.cpp` + `open_choice_panel` | UI 打开失败的具体原因 | W | `op=open_choice`、`reason=capability_unavailable|api_rejected` | 现有失败日志无可区分的具体原因；能力缺失与 open API 拒绝都导致本次 UI 不可用，游戏仍可继续。 |

## 统计

- 业务日志：42 条；通用 `log_message` 实现及 `sdk->log.write` 出口各 1 处，不作为独立事件计数。
- disposition：keep 8，demote 5，promote 0，delete 0，merge 0，augment 29。
- 缺失日志建议：2 条。

## Top 发现

1. 42 条日志当前均通过 `log_message` 的数值参数发出；其中 27 条 E 均对应游戏可继续的单次功能/操作失败，应降到 W。
2. `feature.cpp:477,496` 把用户选择过程记为 I；按规则应降为 D，最终传送结果由 `:404` 与失败分支记录。
3. `feature.cpp:231,526` 是地图枚举/入口诊断而非列表加载完成或状态变化，不应保留 I；应降为 D。
4. `feature.cpp:287` 的静态无变量能力缺失文案必须补 `reason=`，且因仅使传送 UI 不可用而应为 W。
5. `feature.cpp:257` 地图静态数据缺失令目标不可达但不影响游戏运行，应为 W；需要以最终结果明确不可达结局。

## 审核裁决

### 批准并已执行

- E→W 共 27 条：按清单所有 `level_now=E` 且 `level_target=W` 的行调整 `log_message(3, ...)` 为 `log_message(2, ...)`；正文前缀保留，augment 项按下述追加原因字段。
- 其他等级调整共 6 条：`feature.cpp:231,477,496,526` I→D；`:377` I→W；`:528` W→D。
- 执行 augment 共 29 条：在原正文末尾追加 `reason=` 字段，保留已有字段及正文前缀。
- 没有新增调用点，未改控制流、返回码或错误处理。

### 驳回

- 驳回两条缺失日志建议，不新增调用点。传送结果由 `feature.cpp:404`、`:393`、`:400`、`:377`、`:463` 覆盖成功、失败、不可达及余额不足；UI 打开失败原因由 `:287`、`:291` 补充 `reason=` 覆盖。
