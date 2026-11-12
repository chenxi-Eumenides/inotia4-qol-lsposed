# 日志审计清单：批次 5（custom_recipe）

依据 `docs/reference/logging-standards.md` 与 `docs/reference/logging.md`。范围为 `module/app/src/main/cpp/plugins/custom_recipe/**`。已同时检索 `log_message(`、`log_info(`、`log_warn(`、`log_error(`、`log_debug(`、`QOL_LOG_`、`log.write(`。本范围没有 `log_info` 等独立封装或 `QOL_LOG_*`；`log_message(level, ...)` 将 level 原样传给宿主，0=D、1=I、2=W、3=E。日志出口定义/转发（`ui/feature.cpp:43,54`、`result/feature.cpp:161,172`、`table/table_runtime.cpp:27,34`）不是事件，不另计；`log_execute_stage` 的真实级别由其参数决定，结果细节使用 I 的分支应降为 D。

## 现有日志

| file:line | domain | level_now | message 摘要 | event_class | level_target | disposition | rationale |
|---|---|---:|---|---|---:|---|---|
| `ui/runtime.cpp:198` | custom_recipe | D | craft popup index/memoryTextId | 弹窗定位细节 | D | keep | 仅在有界预算内输出，用于查弹窗索引到文案映射。 |
| `ui/runtime.cpp:244` | custom_recipe | D | three slot confirm cancelled | 确认取消细节 | D | augment | 确认回调作废 pending；补 `op=craft`、`result=cancelled`，避免静态文案不可关联。 |
| `ui/runtime.cpp:256` | custom_recipe | W | confirm aborted；stuff read failed | 合成确认失败 | W | augment | 取消 pending 并拒绝执行；补 `reason=`/`code=` 及对应槽/配方上下文，当前只有自然语言原因。 |
| `ui/runtime.cpp:298` | custom_recipe | W | delegate start mix failed；mixType/slots/handles | 委托失败 | W | augment | 单次委托失败且材料保留；补结构化 `reason=`/`code=`，已有配方和物品身份字段不重复补。 |
| `ui/runtime.cpp:320` | custom_recipe | D/W | delegate 前后 mixType/槽/handle/count/cleared | 委托后状态核验 | D/W | augment | clear 时 D、未 clear 时 W 的等级选择正确；读回失败目前也被折算为空槽，补 `read_result=` 区分读失败与未清空。 |
| `ui/runtime.cpp:338` | custom_recipe | D | delegated to original start mix mixType | 查询/委托细节 | D | delete | 同一成功委托的更完整前后状态已由 320 记录；该行只重复 mixType。 |
| `ui/runtime.cpp:417` | custom_recipe | W | three slot place blocked；stuff write failed | 放料写失败 | W | augment | 单次写入失败，保留已填格；补 `index=`、`code=` 或 `reason=` 以定位失败槽和错误。 |
| `ui/runtime.cpp:481` | custom_recipe | D | place accepted tier-up target mixType/cost | 放料处理细节 | D | keep | 每次放料会触发，预算门控且仅为诊断；无 INFO 热路径。 |
| `ui/runtime.cpp:490` | custom_recipe | D | tier-up confirm cancelled | 确认取消细节 | D | augment | 清空待确认目标是状态结果；补 `mixType=` 和 `result=cancelled`。 |
| `ui/runtime.cpp:514` | custom_recipe | W | tier-up delegate start mix failed mixType | 委托失败 | W | augment | 确认后单次委托失败，补 `reason=`/`code=`；该调用本身已将失败提示给用户。 |
| `ui/runtime.cpp:518` | custom_recipe | D | tier-up delegated to original start mix mixType | 委托成功细节 | D | delete | 320 已提供原版委托后完整结果，本行重复记录成功委托。 |
| `ui/runtime.cpp:537` | custom_recipe | W | three slot prepare skipped；stuff read failed | 配方准备失败 | W | augment | 本次查询因输入不可读而中止；补 `code=` 与 `mixType=`，明确具体失败。 |
| `ui/runtime.cpp:544` | custom_recipe | D | three slot miss slots | 规则未匹配细节 | D | keep | 逐次配方匹配细节按规则应为 D；类别数组足以复现未命中输入。 |
| `ui/runtime.cpp:557` | custom_recipe | W | confirm unavailable；no_popup | 弹窗创建失败 | W | augment | 单次准备操作失败；补 `mixType=`/`recipe=` 关联当前配方。 |
| `ui/runtime.cpp:562` | custom_recipe | W | prepare unavailable slots | 配方准备失败 | W | augment | 匹配后上下文不可用，当前只有槽类别；补 `reason=`/`code=` 区分身份/类别读取失败。 |
| `ui/runtime.cpp:577` | custom_recipe | W | craft blocked；disabled/mixType | 停用配方拒绝 | W | keep | fail-closed 拒绝当前操作，带原因与配方类型。 |
| `ui/runtime.cpp:585` | custom_recipe | W | tier-up target slot read failed code | 目标槽读取失败 | W | augment | 单次查询失败；补 `mixType=`/目标槽上下文。 |
| `ui/runtime.cpp:587` | custom_recipe | I | tier-up target slot empty | 每次合成查询空目标 | D | demote | 合成按钮每次查询都会运行，空槽是正常可选状态而非生命周期变化；改 D 或删去。 |
| `ui/runtime.cpp:621` | custom_recipe | W | tier-up confirm unavailable；no_popup | 弹窗创建失败 | W | augment | 单次确认失败，补 `mixType=`/目标 handle；原因字段已有。 |
| `ui/runtime.cpp:679` | custom_recipe | D | custom recipe picked mixType/type | 配方点击换型细节 | D | keep | 逐次点击只做诊断，受预算限制；实际换型由行为可见。 |
| `ui/runtime.cpp:700` | custom_recipe | D | desc replaced mixType | 文案替换细节 | D | keep | UI 文本 hook 高频路径使用预算门控 D，无高等级日志。 |
| `ui/feature.cpp:201` | custom_recipe | W | craft stuff read failed index/result | 配方输入读取失败 | W | keep | 一次扫描失败即返回，index/result 使失败可区分。 |
| `ui/feature.cpp:211` | custom_recipe | W | malformed handle index/slot/generation | 配方输入损坏 | W | keep | 单个输入身份畸形后拒绝本次解析；槽与身份字段齐全。 |
| `ui/feature.cpp:216` | custom_recipe | W | item category facts unavailable index | 配方输入服务缺失 | W | augment | 单次读取失败，补 `reason=`/`code=`，并说明当前 item/槽身份。 |
| `ui/feature.cpp:222` | custom_recipe | W | item category read failed index/result | 配方输入读取失败 | W | augment | 有 index/result，但缺物品身份或类别上下文；补 handle/category。 |
| `ui/feature.cpp:228` | custom_recipe | W | item category out of range index/category | 配方输入非法类别 | W | keep | 拒绝本次查询且实际值/槽位已记录。 |
| `ui/feature.cpp:302` | custom_recipe | W | yesno unavailable text/reason | 确认弹窗服务缺失 | W | keep | 单次弹窗失败，text 与 reason 字段齐全。 |
| `ui/feature.cpp:306` | custom_recipe | W | yesno rejected text/null_callback/ok/cancel | 确认回调参数非法 | W | keep | 输入拒绝原因和两个回调状态明确。 |
| `ui/feature.cpp:312` | custom_recipe | W | yesno failed text/result | 确认弹窗失败 | W | augment | 错误码字段已有；补 `reason=` 并关联对应操作/配方。 |
| `table/table_runtime.cpp:69` | custom_recipe | W | wrong thread entry | 线程契约违反 | W | augment | 一次性告警正确；将动态 entry 纳入 `op=` 或 `entry=` 结构化字段，静态内容本身不够定位。 |
| `table/table.cpp:286` | custom_recipe | W | recipe table not ready recipe_size/mixture_size/count | 表未就绪 | W | keep | 有界去重仅记录探测状态变化，表未加载可重试且字段齐备。 |
| `table/table.cpp:298` | custom_recipe | I | recipe table reactivated records | 配方表恢复 | I | keep | 仅记录记录数发生变化的恢复状态，适合 INFO。 |
| `table/table.cpp:314` | custom_recipe | W | recipe table replaced by game/re-inject count | 游戏重载后重注入 | I | promote | 已检测并启动重新注入属于状态/恢复事件，非失败；应记录一次注入最终结果为 I，当前此行是重注入开始。 |
| `table/table.cpp:335` | custom_recipe | E | recipe book byte size would change/injection refused | 表布局不兼容拒绝注入 | W | demote | 仅此插件注入被拒，游戏仍可运行；`Unsupported` 非全模块不可用，按单次结构不兼容失败记 W。 |
| `table/table.cpp:364` | custom_recipe | E | table alloc failed bytes | 配方表注入分配失败 | W | demote | 单次注入失败且游戏可继续；应作为 W，字节数已提供输入规模，建议补 `reason=alloc_failed`。 |
| `table/table.cpp:380` | custom_recipe | I | recipe table injected base/records/materials/book_bytes | 配方表注入成功 | I | keep | 注入成功是有限状态变化，记录前后规模及书本字节。 |
| `result/feature.cpp:519` | custom_recipe | W | tier-up material need write failed index/result | 材料需求写入失败 | W | augment | 本次配方操作失败，补 `recipe=`/`item=` 关联配方；index/result 已有。 |
| `result/feature.cpp:525` | custom_recipe | D | material needs quality/attribute tier/level/count/first | 材料需求计算细节 | D | keep | 有界验证输出，所有计算输入与摘要字段齐全。 |
| `result/feature.cpp:640` | custom_recipe | W | dynamic recipe rebuild skipped/save facts unavailable | 动态配方恢复跳过 | W | augment | SaveEnter 时缺服务导致此次恢复失败；补 `reason=`、当前 slot/恢复触发上下文。 |
| `result/feature.cpp:648` | custom_recipe | W | dynamic recipe rebuild skipped/current slot invalid | 动态配方恢复失败 | W | keep | 保存槽读取失败/范围非法，本次操作可继续；result 与 slot 已有。 |
| `result/feature.cpp:660` | custom_recipe | W | dynamic recipe rebuild skipped/creation time unavailable | 动态配方恢复失败 | W | keep | slot、result、失败事件明确，单次恢复失败。 |
| `result/feature.cpp:674` | custom_recipe | W | dynamic recipe rebuild failed/stage/result | 动态配方缓存重建失败 | W | keep | 单次重建失败，stage/result 充分；游戏仍可运行。 |
| `result/feature.cpp:679` | custom_recipe | I | dynamic special recipes refreshed count/slot/cache/rerolls | 动态配方恢复完成 | I | keep | 保存数据驱动的配方恢复成功，记录规模、槽、缓存命中和重掷数。 |
| `result/feature.cpp:882` | custom_recipe | D/I/W | execute stage/index/result/before/after | 配方执行阶段观测 | D/W | demote | helper 按参数可输出 D、I、W；每次执行的 precheck/提交细节不是状态变化，I 分支降 D；失败保留 W。 |
| `result/feature.cpp:891` | custom_recipe | D | material remove/index/result/before/after/path | 逐项扣料细节 | D | keep | 每格执行细节有界预算控制，字段覆盖扣料路径与计数变化。 |
| `result/feature.cpp:901` | custom_recipe | W | product cleanup unavailable/index/result/stage/ownership | 产物清理失败 | W | augment | 单次清理失败后所有权未知；补 `reason=`/`product=` 以确定未释放对象。 |
| `result/feature.cpp:909` | custom_recipe | W | product cleanup failed/index/result/stage/ownership | 产物清理失败 | W | augment | 单次失败且仍可能有 detached 产物；补 `reason=`/`product=` 或 handle。 |
| `result/feature.cpp:913` | custom_recipe | D | product cleanup released/index/stage | 产物清理成功细节 | D | keep | 有界预算下记录临时对象已释放的诊断事实。 |
| `result/feature.cpp:951` | custom_recipe | W | tier-up gate blocked stage/code/handle | 升阶规则拒绝/服务失败 | W | keep | gate 每次点击受预算限制；真实服务错误以 W 记录，stage/code/handle 足以定位。 |
| `result/feature.cpp:982` | custom_recipe | W | jewel word mismatch quality/word/handle | 配方目标校验失败 | W | keep | 单次升阶被拒，品质事实与物品原始字及句柄完整。 |
| `result/feature.cpp:995` | custom_recipe | D | tier-up gate pass jewel/category/level | 升阶匹配通过细节 | D | keep | 每次目标查询的规则匹配细节，预算限制且使用 D。 |
| `result/feature.cpp:1009` | custom_recipe | W | gate blocked not_equipment/item_type/handle | 目标规则拒绝 | W | demote | 非装备是正常不匹配输入而非操作失败；逐条匹配细节目标为 D。 |
| `result/feature.cpp:1030` | custom_recipe | W | gate blocked view_flags/fields/options/handle | 目标数据格式不支持 | W | keep | 当前操作因结构信息不足被拒，字段位与句柄明确。 |
| `result/feature.cpp:1041` | custom_recipe | W | gate blocked no_native_option/handle | 目标没有可升阶词条 | D | demote | 正常规则不匹配，单次查询仍可继续，属于匹配细节。 |
| `result/feature.cpp:1071` | custom_recipe | W | gate blocked no_upgradable_affix/options/handle | 目标无可升阶词条 | D | demote | 正常规则未匹配而非失败；detail 目标等级为 D。 |
| `result/feature.cpp:1078` | custom_recipe | D | gate pass equipment/category/options/upgradable | 升阶匹配通过细节 | D | keep | 逐次查询细节且带选项计数，已有预算限制。 |
| `result/feature.cpp:1092` | custom_recipe | W | wrong thread entry | 主线程契约违反 | W | augment | 一次性告警正确；`entry=` 字段已有但文案需去掉括号自然语言，结构化表示契约违规。 |
| `result/feature.cpp:1126` | custom_recipe | D | three slot place index/category | 放料查询细节 | D | keep | 放料每次发生，属于调用细节；预算门控，字段能定位槽和类别。 |
| `result/feature.cpp:1146` | custom_recipe | D | three slot miss categories | 规则未匹配细节 | D | keep | 每次规则查询的未命中输入，按要求为 D 且受验证预算限制。 |
| `result/feature.cpp:1152` | custom_recipe | W | prepare failed identity/not ready | 配方身份读取失败 | W | augment | 单次匹配准备失败，补 `reason=` 和 mixType/slot context。 |
| `result/feature.cpp:1163` | custom_recipe | W | prepare failed identity/index/result | 配方身份读取失败 | W | augment | index/result 已有，补配方或 handle 身份。 |
| `result/feature.cpp:1170` | custom_recipe | W | empty identity conflict/index | 输入槽身份冲突 | W | augment | 单次输入校验失败；补 `reason=identity_mismatch` 和槽值。 |
| `result/feature.cpp:1179` | custom_recipe | W | identity category read failed/index/result | 配方类别读取失败 | W | augment | 单次规则准备失败；补实际/预期类别或 handle。 |
| `result/feature.cpp:1185` | custom_recipe | W | identity category mismatch/index/conflict | 配方类别不一致 | W | augment | 当前正文与 1179 同一摘要无法区分失败/不匹配；补 `reason=category_mismatch`、actual/expected。 |
| `result/feature.cpp:1666` | custom_recipe | W | product save failed/ownership detached | 产物保存失败 | W | keep | 单次保存失败，result 和 detached 状态齐备；index 固定 0。 |
| `result/feature.cpp:1673` | custom_recipe | D | crafted product/consumed | 合成成功摘要 | I | promote | 合成完成是有限用户操作的状态变化，不是逐项诊断；保留产品与消耗计数并升 I。 |
| `result/feature.cpp:1727` | custom_recipe | W | make item jewel quality query failed/code | 升阶输入读取失败 | W | augment | 单次产物生成失败，补 `item=`/`mixType=` 关联输入。 |
| `result/feature.cpp:1737` | custom_recipe | W | jewel quality facts mismatch | 宝石字段一致性失败 | W | augment | 单次升阶失败，当前无任何变量，补 item/quality 与 word 对比及 `reason=`。 |
| `result/feature.cpp:1743` | custom_recipe | W | item payload/replace unavailable | 升阶能力不可用 | W | augment | 单次合成失败但游戏可继续；静态失败文案须补 `mixType=`/`item=`/`reason=`。 |
| `result/feature.cpp:1754` | custom_recipe | W | material precheck failed/code | 材料需求校验失败 | W | augment | 单次操作失败，补 `mixType=`/`item=`。 |
| `result/feature.cpp:1770` | custom_recipe | W | tier evaluation failed/code | 升阶计算失败 | W | augment | 一次产物生成失败，补目标物品与配方标识。 |
| `result/feature.cpp:1790` | custom_recipe | W | payload update failed/result | 产物数据更新失败 | W | augment | 单次失败，补 item/mixType。 |
| `result/feature.cpp:1796` | custom_recipe | W | jewel replace failed/result/needs/category/count/payload | 宝石写回失败 | W | keep | 已含失败码、写入阶段及物品类别/数量/载荷信息。 |
| `result/feature.cpp:1804` | custom_recipe | D | jewel tier-up type/value/range/target | 升阶数值细节 | D | keep | 数值演算细节按 D；验证预算有限。 |
| `result/feature.cpp:1859` | custom_recipe | W | equipment affix random failed/code/pick/count | 词条随机失败 | W | augment | 单次生成失败，补 item/mixType；候选数和随机结果已有。 |
| `result/feature.cpp:1873` | custom_recipe | W | equipment tier evaluation failed/code/result | 装备升阶计算失败 | W | augment | 单次失败，补 `item=`/`mixType=`。 |
| `result/feature.cpp:1879` | custom_recipe | E | equipment value out of int16 range/value | 装备升阶输入越界 | W | demote | 单次升阶被拒，游戏可继续；不是不可恢复数据损坏或能力整体不可用。 |
| `result/feature.cpp:1893` | custom_recipe | W | item payload/replace unavailable | 升阶能力不可用 | W | augment | 与 1743 同一能力检查但不同装备执行路径；补 item/mixType/reason，避免静态失败文案。 |
| `result/feature.cpp:1900` | custom_recipe | W | material precheck failed/code | 材料需求校验失败 | W | augment | 装备路径单次操作失败，补 item/mixType。 |
| `result/feature.cpp:1913` | custom_recipe | W | payload update failed/result | 产物数据更新失败 | W | augment | 装备路径单次失败，补 item/mixType。 |
| `result/feature.cpp:1919` | custom_recipe | W | native option replace failed/result/needs/category/count/payload | 装备词条写回失败 | W | keep | 阶段、结果码、前置材料写入和对象载荷均已有。 |
| `result/feature.cpp:1926` | custom_recipe | D | equipment tier-up option/value/range/target | 装备升阶数值细节 | D | keep | 有界验证预算下的逐词条细节，符合 D。 |
| `result/feature.cpp:1975` | custom_recipe | W | table acquire failed/code | 配方表注入失败 | W | keep | ownership acquire 返回原始错误码并清理注入状态；单次失败可重试/降级。 |

## 缺失日志建议

| file + 函数 | 事件 | 目标等级 | 必填字段 | 理由 |
|---|---|---|---|---|
| `result/feature.cpp` + `own_acquire` / `refresh_dynamic_recipes` | 配方表注入或保存数据恢复的最终成功/失败结果（在所有分支收敛后唯一记录） | I（完成）/ W（单次失败）；不可恢复数据损坏 E | `op=inject|restore`、`result=`、`records=`/`slot=`、失败 `reason=`/`code=` | 当前注入失败由多个层级分别记录且失败入口可能没有最终状态；动态配方恢复失败有分支日志但缺统一最终结果。合并前需避免重复同一事实。 |
| `result/feature.cpp` + `api_prepare` / `api_craft_gate` | 规则匹配失败的具名原因（无匹配、数据上下文不可用需区分） | D（正常未匹配）/ W（规则数据读取失败） | `recipe=` 或 `slots=`、`result=miss|unavailable`、失败 `reason=`/`code=` | 当前 miss 有槽类别；prepare 的 unavailable 分支缺稳定的失败归因。只建议补规则匹配失败原因，不增加每次查询的 INFO。 |
| `result/feature.cpp` + 保存入口的自定义配方数据解析/恢复分支（若该路径解析持久化配方数据） | 存档中自定义配方数据损坏且不可恢复 | E | `op=restore`、`slot=`、`reason=`/`code=`、`record=`（可定位时） | 当前动态配方依赖存档时间字段并记录读取失败，但不能辨识持久化自定义配方内容的损坏/不可恢复分支；该分支应在唯一解析/恢复层记录。 |

## 统计

- 现有事件日志：83 条（含 table 核心经回调实际输出的 6 条）；通用 logger 出口不计。
- disposition：keep 34，demote 9，promote 1，delete 2，merge 0，augment 37。
- 缺失日志建议：3 条。

## Top 发现

1. `table/table.cpp:335,364` 把单次注入拒绝/分配失败记为 E；该插件失败不影响游戏继续，应为 W。
2. `result/feature.cpp:882` helper 可将每次配方执行细节输出 I；结果诊断分支应改 D，失败保留 W。
3. `result/feature.cpp:1009,1041,1071` 将正常的目标规则不匹配记 W；应按匹配细节降为 D。
4. `ui/runtime.cpp:587` 每次合成查询空目标记 I；这是常见正常查询，不是有限状态变化，应降 D 或删除。
5. 多条 `make item` 单次失败日志为静态文案或仅带 code；应补 `item=`、`mixType=` 与失败原因，增强失败可追踪性。

## 审核裁决

- 已执行 A：`:335`、`:364`、`:1879` 从 E 降为 W；`:1009`、`:1041`、`:1071` 从 W 降为 D；`:587` 从 I 降为 D；`result/feature.cpp` 的 `log_execute_stage` 将 I 参数映射为 D，W 分支不变。按清单 disposition 计为 demote 8 条；原 A 标注 9 条是把 `table/table.cpp:314` 也列入 demote，最终裁决将该项列入 promote。
- 已执行 B：`:314` 从 W 升为 I；`:1673` 从 D 升为 I，共 promote 2 条。
- 已执行 C：删除 `ui/runtime.cpp:338`、`:518` 两条委托成功重复日志，并清理对应空行，共 delete 2 条。
- 已执行 D：逐条补充 37 条 augment 结论指定的字段；现有 `code=`、`result=` 均未重复追加。未改变业务控制流或错误处理。
- 已检查保存入口：`result/feature.cpp` 仅在 SaveEnter 时按 slot 创建时间重建动态配方缓存，没有解析/恢复存档内持久化自定义配方数据的分支；仓库内 custom_recipe 范围也未发现 decode/deserialize/parse 持久化数据路径。因此跳过新增不可恢复损坏 E 日志。
- 驳回缺失日志建议 1、2：表注入成功/重激活和各失败分支已有结果日志；规则 miss 与 prepare 失败已有 D/W 日志。
- 最终处置计数：keep 34，demote 8，promote 2，delete 2，merge 0，augment 37；新增 E 日志 0 条。
