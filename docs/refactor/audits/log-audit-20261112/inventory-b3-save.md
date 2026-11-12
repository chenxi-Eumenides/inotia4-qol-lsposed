# 日志审计清单：批次 3（core.save）

依据 `docs/reference/logging-standards.md` 与 `docs/reference/logging.md`。范围为 `module/app/src/main/cpp/plugins/core.save/**`。调用点域为 `save`；插件生命周期 SDK 日志域为 `core`。本次 grep 得到 76 项，其中 74 条业务/事件调用及 2 个通用 SDK 写出口（出口仅转发，不作为独立事件）。

`log.h` 仅定义 `CS_LOG_INFO`（1）与 `CS_LOG_WARN`（2），没有 `CS_LOG_ERROR`，范围内也没有 `CS_LOG_DEBUG`；因此存档损坏、无法恢复或事务状态不一致等不可恢复失败当前只能用 WARN 表达。建议在 seam 增加 `CS_LOG_ERROR`（3）并接到既有 SDK ERROR 通道，不另造域/等级。

## 现有日志

| file:line | domain | level_now | message 摘要 | event_class | level_target | disposition | rationale |
|---|---|---:|---|---|---:|---|---|
| `save_coordinator.cpp:142` | save | W | save abort；slot；健康阻断 | 保存拒绝 | W | keep | 槽健康阻断新保存，带 slot/reason。 |
| `save_coordinator.cpp:155` | save | W | save abort；tx id exhausted | 保存序号耗尽 | W | keep | 当前操作失败且可继续运行，slot/reason 完整。 |
| `save_coordinator.cpp:161` | save | W | capture begin；tx/code | 捕获启动失败 | W | keep | 单笔失败，标识 tx 与错误码。 |
| `save_coordinator.cpp:179` | save | W | capture；tx/code | 捕获失败 | W | keep | 单笔捕获失败并收敛取消。 |
| `save_coordinator.cpp:194` | save | W | prepared journal encode；tx | 准备日志编码失败 | W | keep | 单次事务失败，带 slot/tx/步骤。 |
| `save_coordinator.cpp:221` | save | W | confirmed candidate encode；tx | 候选日志编码失败 | W | keep | 单次事务失败，带 slot/tx/步骤。 |
| `save_coordinator.cpp:233` | save | W | operation acquire；tx/code | 操作租约获取失败 | W | keep | 单次保存失败，可重试。 |
| `save_coordinator.cpp:245` | save | W | store reserve；tx/code | 存储保留失败 | W | keep | 单次保存失败，可重试。 |
| `save_coordinator.cpp:260` | save | W | prepared journal write；tx/code | prepared 写失败 | W | keep | 单次写失败，slot/tx/code 齐备。 |
| `save_coordinator.cpp:279` | save | W | 原版未调用；journal preserved/degraded | 保存事务降级 | W | keep | 有 slot/tx/阶段/返回与降级结果；非重复上层事实。 |
| `save_coordinator.cpp:288` | save | W | 原版未调用；journal cleaned | 原版保存未执行 | W | keep | 单次失败且 journal 已清，slot/tx/code 完整。 |
| `save_coordinator.cpp:299` | save | W | 原版返回失败；journal preserved/degraded | 保存事务降级 | W | keep | 明示证据保留及槽降级。 |
| `save_coordinator.cpp:308` | save | W | 原版返回失败；journal cleaned | 原版保存失败 | W | keep | 单次原版失败，slot/tx/code 完整。 |
| `save_coordinator.cpp:322` | save | I | pending；evidence/code | evidence 待重试 | W | demote | 过渡中的可重试失败不是已发生成功状态；以 W 表达单次操作失败。 |
| `save_coordinator.cpp:332` | save | W | confirmed journal encode；degraded | 确认日志编码失败 | W | promote | 原版已成功但确认日志不可编码，进入不可自动恢复的降级状态；应 E，当前 seam 缺口。 |
| `save_coordinator.cpp:347` | save | I | pending；confirmed write/code | confirmed 写等待重试 | W | demote | 单次事务写失败/待重试，不是生命周期状态变化。 |
| `save_coordinator.cpp:360` | save | I | pending；final replace/code | 最终替换等待重试 | W | demote | 单次事务替换失败/待重试，改 W。 |
| `save_coordinator.cpp:368` | save | I | committed；slot/tx/sections | 保存提交成功 | I | keep | 有限的最终保存状态，字段可回放。 |
| `save_coordinator.cpp:459` | save | W | journal recovery read；degraded | 恢复读 journal 失败 | E | promote | journal 不可读导致槽降级并阻止确认，属于恢复能力不可用；缺 E seam。 |
| `save_coordinator.cpp:463` | save | W | journal decode；degraded | journal 损坏/解码失败 | E | promote | journal 无法解码且槽降级，是不可恢复数据损坏；缺 E seam。 |
| `save_coordinator.cpp:470` | save | W | prepared stage abort；degraded | 崩溃窗口 journal 无法自动恢复 | E | promote | prepared 状态被保留并降级阻止恢复，需人工处理；缺 E seam。 |
| `save_coordinator.cpp:487` | save | W | recovery acquire；degraded | 恢复租约失败 | W | keep | 当前恢复尝试失败，保留证据，可重试/处理。 |
| `save_coordinator.cpp:503` | save | W | recovery reserve；degraded | 恢复存储保留失败 | W | keep | 单次恢复协调失败，带槽与 code。 |
| `save_coordinator.cpp:517` | save | W | evidence mismatch；degraded | 恢复证据不匹配 | E | promote | 证据校验失败后保留 journal 并降级，无法自动恢复；缺 E seam。 |
| `save_coordinator.cpp:552` | save | W | retries exhausted；degraded | 自动收敛重试耗尽 | E | promote | retry window 耗尽且槽降级、捕获事务终止；属最终恢复失败，缺 E seam。 |
| `save_coordinator.cpp:572` | save | W | retry evidence；degraded | 重试读取证据失败 | W | keep | 仅在重试终态收敛时输出，含 tx/code/health。 |
| `save_coordinator.cpp:586` | save | W | retry confirmed encode；degraded | 重试编码失败 | E | promote | 降级且无法继续事务恢复，缺 E seam。 |
| `save_coordinator.cpp:607` | save | W | retry confirmed write；degraded | 重试 journal 写失败 | W | keep | 当前重试失败并终态降级；保留错误码。 |
| `save_coordinator.cpp:621` | save | W | retry confirmed encode；degraded | 重试编码失败 | E | merge | 与 586 同一恢复阶段/同一失败事实在两个阶段入口重复；保留一个带 tx 的终态日志。 |
| `save_coordinator.cpp:645` | save | W | retry replace；degraded | 重试最终替换失败 | W | keep | 最终收敛处有 tx/code/health，暂不重复记录中间重试。 |
| `save_coordinator.cpp:674` | save | W | save rejected bad slot | 非法槽输入 | W | keep | 输入拒绝带 slot/reason。 |
| `save_coordinator.cpp:690` | save | W | save in flight | 同槽保存冲突 | W | keep | 操作拒绝并带 slot/reason。 |
| `save_coordinator.cpp:712` | save | W | slot degraded | 降级槽拒绝保存 | W | keep | 当前请求被拒绝，带槽与原因。 |
| `save_coordinator.cpp:716` | save | W | no bound transaction | 无事务绑定请求 | W | keep | 异常调用序列被拒绝，字段齐备。 |
| `save_coordinator.cpp:744` | save | W | save rejected bad slot | 非法槽输入 | W | merge | 与 674 相同拒绝语义，但分属 settle 与 SAVE hook 入口；建议入口归一化后只记录一次。 |
| `save_coordinator.cpp:753` | save | W | coordinator not installed | 协调器未安装拒绝保存 | W | keep | 能力未就绪导致单次请求失败；slot/reason 明确。 |
| `save_coordinator.cpp:762` | save | W | save in flight | 同槽保存冲突 | W | merge | 与 690 同一“保存进行中”事实的另一个入口；统一到一个唯一拒绝日志点。 |
| `save_coordinator.cpp:768` | save | W | slot degraded | 降级槽拒绝保存 | W | merge | 与 712 相同事实另一个入口；合并记录，避免重复。 |
| `save_coordinator.cpp:772` | save | W | no bound transaction | 无事务绑定请求 | W | merge | 与 716 同一拒绝事实的另一个入口；合并。 |
| `plugin.cpp:174` | core | I | SDK 插件 lifecycle 写出口 | 通用日志出口 | — | keep | 仅传递插件生命周期消息，无固定事件内容；非独立日志调用点。 |
| `plugin.cpp:182` | save | 动态 | bridge sink 写出口 | 通用日志出口 | — | keep | `CS_LOG_*` 唯一转发出口；等级和正文由调用点决定，不计事件条数。 |
| `plugin.cpp:388` | save | I | catch-up transition begun；slot/token | catch-up 转换开始 | I | delete | 同次 `save_transition_begin` 已在 transition.cpp:285 记录 token/kind/slot；删掉上层重复事实。 |
| `plugin.cpp:394` | save | W | catch-up begin failed；slot/code | catch-up begin 失败 | W | merge | begin 内部各失败分支已记录具体 reason/slot；该外层只重复失败结果。 |
| `plugin.cpp:464` | save | W | confirm timeout；token/elapsed | 读档确认超时 | W | keep | 超时事件字段充分；应视为最终 timeout 收敛触发日志。 |
| `plugin.cpp:472` | save | W | timeout cancel failed；token/code | 超时取消失败 | W | keep | 独立取消失败结果，token/code 可定位。 |
| `plugin.cpp:558` | save | I | rejected new game；stale token cancelled | 新档拒绝并清理旧选择 | I | keep | 有限状态变化，包含 token。 |
| `plugin.cpp:587` | save | W | resume pending slot mismatch | 读档目标槽不匹配 | W | keep | 单次请求拒绝，含请求与 pending 槽。 |
| `plugin.cpp:604` | save | W | resume begin rejected；slot/code | 读档转换启动失败 | W | merge | `save_transition_begin` 内部已经按各失败原因记日志；入口仅汇总相同失败。 |
| `plugin.cpp:615` | save | W | bypass unavailable；slot | 读档旁路不可用 | W | augment | 在取消转换后拒绝读档；补 `token=` 与 `reason=bypass_unavailable` 以定位交易。 |
| `plugin.cpp:625` | save | W | original resume failed；slot | 原版读档入口失败 | W | augment | 单次原版失败，补 `token=`、`reason=original_resume_failed`。 |
| `slot_data_provider.cpp:533` | save | W | transition unavailable；状态/target | 转换开始被 provider 拒绝 | W | merge | 失败已由 transition begin 外层写出具体 begin 失败；保留唯一的外层事件，不重复 provider 低层日志。 |
| `slot_data_provider.cpp:546` | save | W | prepare_detach rejected；section/code | participant detach 失败 | W | keep | 单次参与者失败，section/code 有区分；上层 begin 日志只在失败汇总路径可能重复，应优先保留此细节。 |
| `slot_data_provider.cpp:593` | save | W | confirm unavailable；状态/current | provider 确认状态不可用 | W | keep | 单次确认失败，状态字段齐备。 |
| `slot_data_provider.cpp:601` | save | W | target mismatch；current/target | 确认目标不符 | W | merge | `save_transition_confirm` 对同一 confirm 失败另有带 token/expected 的日志；以 transition 层为唯一业务事实。 |
| `slot_data_provider.cpp:634` | save | W | activation failed；code | slot data 激活失败 | W | merge | transition confirm 外层以 step=activate/token/code 记录同一事实；保留外层关联 token 的日志。 |
| `save_transition.cpp:105` | save | W | journal read failed；slot/code | journal 读取失败 | E | promote | 阻断确认序且 journal 内容不可读；属于恢复能力不可用，缺 E 通道。 |
| `save_transition.cpp:115` | save | W | prepared journal pending；slot/tx | 未决 prepared journal | E | promote | prepared journal 不能自动安全恢复，当前阻断转换；属不可恢复事务状态，缺 E 通道。 |
| `save_transition.cpp:121` | save | W | journal unresolved；slot/decode | journal 无法解析 | E | promote | 数据损坏/未知格式并阻断读档恢复，缺 E 通道。 |
| `save_transition.cpp:176` | save | W | begin rejected not main thread | 转换线程不符 | W | keep | 单次 begin 被拒，reason/kind/slot 齐备。 |
| `save_transition.cpp:183` | save | W | begin rejected invalid argument | begin 参数非法 | W | keep | 单次非法请求，字段齐备。 |
| `save_transition.cpp:216` | save | W | begin rejected already pending | 转换已在途 | W | keep | 含当前请求和 pending token/阶段信息。 |
| `save_transition.cpp:225` | save | W | begin rejected slot health | 槽健康阻断转换 | W | keep | 单次请求被拒，reason/kind/slot 齐备。 |
| `save_transition.cpp:231` | save | W | begin rejected token exhausted | token 耗尽 | W | promote | 转换 token 空间耗尽使转换能力不可用；当前 seam 无 E。 |
| `save_transition.cpp:257` | save | W | begin rejected operation busy | 操作租约获取失败 | W | keep | 单次 begin 失败，带 mask/code/message。 |
| `save_transition.cpp:271` | save | W | begin rejected slot data | provider 准备失败 | W | merge | provider prepare_detach 的具体失败在 provider:546 已记录；上层只汇总同一失败。 |
| `save_transition.cpp:285` | save | I | transition begun；kind/slot/token/epoch | 转换开始状态 | I | keep | 有限状态迁移，字段充分。 |
| `save_transition.cpp:292` | save | W | confirm failed not main thread | 确认线程不符 | W | keep | 单次确认失败并带 token/target。 |
| `save_transition.cpp:320` | save | W | confirm lookup failed | token 不存在 | W | keep | 单次确认失败，code/message/token 明确。 |
| `save_transition.cpp:327` | save | W | confirm slot mismatch | 观测槽不匹配 | W | keep | 包含 token/observed/target/code。 |
| `save_transition.cpp:335` | save | W | confirm in flight | 确认已在途 | W | keep | 单次重复确认拒绝，带 token/target。 |
| `save_transition.cpp:352` | save | W | confirm ensure failed | 创建/读档数据准备失败 | W | keep | 当前失败保留 pending 可重试，含 token/target/code。 |
| `save_transition.cpp:365` | save | W | confirm journal failed | journal 检查阻断 | W | merge | journal_check 在 105/115/121 已记录具体根因；避免同一失败在 confirm 汇总重复。 |
| `save_transition.cpp:379` | save | W | confirm activation failed | activation 失败 | W | keep | 最终确认失败且 token/target/code 关联完整。 |
| `save_transition.cpp:401` | save | I | transition ready；token/slot/epoch | 转换确认完成 | I | keep | 有限状态迁移，含 token 与 epoch。 |
| `save_transition.cpp:433` | save | W | cancel lookup failed；token | 取消未知 token | W | keep | 单次取消失败，token 可定位。 |
| `save_transition.cpp:436` | save | I | transition cancelled；token/slot | 转换取消完成 | I | keep | 有限状态变更，字段充分。 |

## 缺失日志建议

| file + 函数 | 事件 | 目标等级 | 必填字段 | 理由 |
|---|---|---|---|---|
| `save_coordinator.cpp` + `save_execute_impl` / `save_coordinator_retry_frame` | 每笔存档最终结果（含 hook SAVE_Save 原版结果、协调器结果） | I（提交）/ W（失败） | `op=save`、`slot=`、`tx=`、`result=` 或 `reason=` | committed 有日志，但部分 pending 转终态/原版失败的业务结果不形成统一最终结果；缺失失败原因应由唯一协调器写。 |
| `save_transition.cpp` + `save_transition_confirm` / `journal_check` | 每笔读档最终加载结果 | I（完成）/ W（单次失败） | `op=load`、`slot=`、`token=`、`result=` 或 `reason=` | ready 有完成日志，但缺少与 token 对应的统一最终读档结果及失败/取消结局。 |
| `save_coordinator.cpp` + `save_coordinator_on_session_enter` | 存档损坏且不可恢复分支 | E | `op=recover`、`slot=`、`tx=`（可得时）、`reason=`、`decode=`/`code=` | journal 解码失败、prepared 未决或 evidence mismatch 会降级并保留证据；现仅能 WARN，且须在 E seam 增加后输出。 |
| `plugin.cpp` + `save_transition_timeout_frame` | 事务超时后的最终收敛结果 | W | `op=load`、`slot=`、`token=`、`elapsed=`、`result=cancelled|cancel_failed`、失败 `reason=`/`code=` | 已分别记 timeout 与 cancel 失败，但成功取消无最终 outcome，无法确定 pending 是否收敛。 |

## 统计

- 业务/事件日志：74 条；另有通用转发出口 2 条（`plugin.cpp:174,182`，列出但不计事件处置统计）。
- disposition（仅 74 条）：keep 45，demote 3，promote 11，delete 1，merge 12，augment 2。
- 缺失日志建议：4 条。

## Top 发现

1. `log.h` 没有 ERROR 宏：journal 损坏、恢复耗尽及状态不可恢复目前只能 WARN；建议 seam 提供 `CS_LOG_ERROR` 并映射宿主 level 3。
2. `save_coordinator.cpp:322,347,360` 将可重试的 pending 事务失败记为 INFO；应统一降为 WARN，成功提交仍保留 INFO。
3. `save_transition.cpp` 与 `save_coordinator.cpp` 对同一 journal/事务失败存在根因与上层汇总重复；应只留包含 slot/token/tx 的唯一事实日志。
4. provider 对 prepare_detach、确认目标和激活失败的日志可能与 transition 外层重复；优先保留能关联 token/事务的业务层事件，低层只留独有根因。
5. 事务 timeout 已记录超时与取消失败，但成功取消没有最终收敛结果；补一条带 token/slot/result 的最终 outcome。

## 审核裁决

### 批准并已执行

- `log.h` 增加 `CS_LOG_ERROR`，level 为 3；`core_save_log` 将 level 透传给 sink，`plugin.cpp` bridge 再原样传入 `sdk->log.write(level, ...)`，因此 3 到达 SDK ERROR 通道，无需修改 bridge。
- promote 到 E 共 10 条：`save_coordinator.cpp:332,459,463,470,517,552,586` 与 `save_transition.cpp:105,115,121`。原正文保持不变。
- demote 到 W 共 3 条：`save_coordinator.cpp:322,347,360`。原正文保持不变。
- 合并重复日志 1 条：删除 `save_coordinator.cpp:621` 的重复 retry confirmed encode 日志，保留 586 带 tx 的记录。
- 其余原判 merge 项全部改为 augment 并追加入口标识：`save_coordinator.cpp:744,762,768,772` 追加 `entry=settle`；`plugin.cpp:394` 追加 `trigger=catch_up`，`:604` 追加 `entry=resume`；`slot_data_provider.cpp:533,601,634` 追加 `entry=provider`；`save_transition.cpp:271` 追加 `entry=transition`，`:365` 追加 `entry=confirm`。
- 驳回 `plugin.cpp:388` 的 delete，保留并追加 `trigger=catch_up`。
- `plugin.cpp:615,625` 分别追加 token 与 `reason=bypass_unavailable` / `reason=original_resume_failed`。
- 采纳 timeout 成功取消结果日志：仅成功 cancel 分支输出一次 `op=load result=cancelled slot=... token=...`，等级 W。

### 驳回

- 驳回 `save_transition.cpp:231` promote：token 耗尽属防御分支，继续保持 W。
- 驳回其余三条缺失日志建议；现有 committed/ready/cancelled 与失败分支已覆盖。journal 损坏通过现有日志升 E，不新增日志。
