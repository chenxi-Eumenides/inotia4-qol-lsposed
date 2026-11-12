# 日志审计清单：批次 2（extension_bag/ui）

依据 `docs/reference/logging-standards.md` 与 `docs/reference/logging.md`。`VIRTBAG_LOG` 在本目录定义为 INFO；审计范围只含 `extension_bag/ui/*.cpp`。

## 现有日志

| file:line | domain | level_now | message 摘要 | event_class | level_target | disposition | rationale |
|---|---|---|---|---|---|---|---|
| `detail_actions.cpp:91` | extension_bag | I | `extension sell custom confirmation unavailable code=%d token=%llu` | UI 操作/状态或诊断 | W | promote | 此处记录单次失败/拒绝且游戏仍可继续；按 W 并补 reason=，不是状态成功事件。 |
| `detail_actions.cpp:97` | extension_bag | I | `extension sell custom confirmation opened price=%u token=%llu` | UI 操作/状态或诊断 | D | demote | 过程/回调细节不是状态迁移；UI 操作过程按 D 记录，避免每次触发 INFO。 |
| `detail_actions.cpp:126` | extension_bag | I | `sell quote item=%p count=%d unit=%lld multiplier=%u/%u final=%lld` | UI 操作/状态或诊断 | D | demote | 调用位于查询/按钮执行/投影诊断路径，非有限状态迁移；应为 D 或删除高频明细。 |
| `detail_actions.cpp:195` | extension_bag | I | `extension sell confirmation entered` | UI 操作/状态或诊断 | — | delete | 无变量静态文本，不能提供对象/结果定位信息。 |
| `detail_actions.cpp:216` | extension_bag | W | `extension sell rejected stale detail bag=%d slot=%d item=%p` | UI 操作/状态或诊断 | W | augment | 详情操作拒绝属于可恢复单次失败；核对 bag/slot/item/reason，避免与 detail_hooks 同一 stale 失败双记。 |
| `detail_actions.cpp:226` | extension_bag | W | `extension sell rejected item handle unavailable bag=%d slot=%d` | UI 操作/状态或诊断 | W | augment | 单次 UI 操作失败，W 合适；建议结构化为 verb + 对象 + reason，补齐定位字段。 |
| `detail_actions.cpp:235` | extension_bag | W | `extension sell rejected symbols unavailable bag=%d slot=%d` | UI 操作/状态或诊断 | W | augment | 单次 UI 操作失败，W 合适；建议结构化为 verb + 对象 + reason，补齐定位字段。 |
| `detail_actions.cpp:249` | extension_bag | W | `extension sell rejected no-sell item bag=%d slot=%d category=%d` | UI 操作/状态或诊断 | W | augment | 单次 UI 操作失败，W 合适；建议结构化为 verb + 对象 + reason，补齐定位字段。 |
| `detail_actions.cpp:263` | extension_bag | W | `sell price reject reason=quote source=equip category=%d count=%d payload=%u gen=%llu` | UI 操作/状态或诊断 | W | augment | 单次 UI 操作失败，W 合适；建议结构化为 verb + 对象 + reason，补齐定位字段。 |
| `detail_actions.cpp:271` | extension_bag | I | `sell price source=equip category=%d count=%d unit=%lld multiplier=%u/%u final=%lld payload=%u gen=%llu` | UI 操作/状态或诊断 | D | demote | 调用位于查询/按钮执行/投影诊断路径，非有限状态迁移；应为 D 或删除高频明细。 |
| `detail_actions.cpp:297` | extension_bag | I | `extension sell action rejected bag=%d slot=%d count=%d status=%d money=%lld->%lld` | UI 操作/状态或诊断 | W | promote | 此处记录单次失败/拒绝且游戏仍可继续；按 W 并补 reason=，不是状态成功事件。 |
| `detail_actions.cpp:305` | extension_bag | I | `extension sell action ok bag=%d slot=%d count=%d action_price=%lld money=%lld->%lld` | UI 操作/状态或诊断 | I | keep | 有限的状态/操作结果变化；正文含操作对象或结果字段。 |
| `detail_actions.cpp:320` | extension_bag | I | `extension sell committed bag=%d slot=%d price=%lld` | UI 操作/状态或诊断 | I | keep | 有限的状态/操作结果变化；正文含操作对象或结果字段。 |
| `detail_actions.cpp:342` | extension_bag | W | `extension destroy rejected stale detail bag=%d slot=%d` | UI 操作/状态或诊断 | W | augment | 详情操作拒绝属于可恢复单次失败；核对 bag/slot/item/reason，避免与 detail_hooks 同一 stale 失败双记。 |
| `detail_actions.cpp:352` | extension_bag | I | `extension destroy action rejected bag=%d slot=%d count=%d status=%d` | UI 操作/状态或诊断 | W | promote | 此处记录单次失败/拒绝且游戏仍可继续；按 W 并补 reason=，不是状态成功事件。 |
| `detail_actions.cpp:365` | extension_bag | I | `extension destroy committed through inventory.action bag=%d slot=%d` | UI 操作/状态或诊断 | I | keep | 有限的状态/操作结果变化；正文含操作对象或结果字段。 |
| `detail_actions.cpp:391` | extension_bag | W | `extension consume rejected object release context=%s bag=%d slot=%d` | UI 操作/状态或诊断 | W | augment | 单次 UI 操作失败，W 合适；建议结构化为 verb + 对象 + reason，补齐定位字段。 |
| `detail_actions.cpp:399` | extension_bag | W | `extension consume rejected count encoding=%d context=%s bag=%d slot=%d` | UI 操作/状态或诊断 | W | augment | 单次 UI 操作失败，W 合适；建议结构化为 verb + 对象 + reason，补齐定位字段。 |
| `detail_actions.cpp:409` | extension_bag | W | `extension consume rejected count write context=%s bag=%d slot=%d` | UI 操作/状态或诊断 | W | augment | 单次 UI 操作失败，W 合适；建议结构化为 verb + 对象 + reason，补齐定位字段。 |
| `detail_actions.cpp:421` | extension_bag | W | `extension consume reserialize failed context=%s bag=%d slot=%d` | UI 操作/状态或诊断 | W | augment | 单次 UI 操作失败，W 合适；建议结构化为 verb + 对象 + reason，补齐定位字段。 |
| `detail_actions.cpp:457` | extension_bag | I | `extension confirm-use lock busy item=%p` | UI 操作/状态或诊断 | W | promote | 此处记录单次失败/拒绝且游戏仍可继续；按 W 并补 reason=，不是状态成功事件。 |
| `detail_actions.cpp:484` | extension_bag | W | `extension confirm-use action rejected bag=%d slot=%d status=%d` | UI 操作/状态或诊断 | W | augment | 单次 UI 操作失败，W 合适；建议结构化为 verb + 对象 + reason，补齐定位字段。 |
| `detail_actions.cpp:499` | extension_bag | I | `extension confirm-use inventory.action bag=%d slot=%d used=%d` | UI 操作/状态或诊断 | D | demote | 过程/回调细节不是状态迁移；UI 操作过程按 D 记录，避免每次触发 INFO。 |
| `detail_actions.cpp:516` | extension_bag | I | `extension mercenary-seal lock busy` | UI 操作/状态或诊断 | — | delete | 无变量静态文本，不能提供对象/结果定位信息。 |
| `detail_actions.cpp:541` | extension_bag | W | `extension mercenary-seal action rejected bag=%d slot=%d status=%d` | UI 操作/状态或诊断 | W | augment | 单次 UI 操作失败，W 合适；建议结构化为 verb + 对象 + reason，补齐定位字段。 |
| `detail_actions.cpp:559` | extension_bag | I | `extension mercenary-seal inventory.action bag=%d slot=%d used=%d` | UI 操作/状态或诊断 | D | demote | 过程/回调细节不是状态迁移；UI 操作过程按 D 记录，避免每次触发 INFO。 |
| `detail_actions.cpp:608` | extension_bag | W | `sell price reject reason=category source=equip category=%d count=%d` | UI 操作/状态或诊断 | W | augment | 单次 UI 操作失败，W 合适；建议结构化为 verb + 对象 + reason，补齐定位字段。 |
| `detail_actions.cpp:614` | extension_bag | I | `sell no-sell delegated to original category=%d` | UI 操作/状态或诊断 | D | demote | 过程/回调细节不是状态迁移；UI 操作过程按 D 记录，避免每次触发 INFO。 |
| `detail_actions.cpp:619` | extension_bag | I | `sell no-sell c18 zero-price category=%d count=%d` | UI 操作/状态或诊断 | D | demote | 过程/回调细节不是状态迁移；UI 操作过程按 D 记录，避免每次触发 INFO。 |
| `detail_actions.cpp:625` | extension_bag | I | `sell price source=equip category=%d count=%d unit=%lld multiplier=%u/%u final=%lld` | UI 操作/状态或诊断 | D | demote | 调用位于查询/按钮执行/投影诊断路径，非有限状态迁移；应为 D 或删除高频明细。 |
| `detail_actions.cpp:630` | extension_bag | W | `sell price reject reason=quote_or_no_sell source=equip category=%d count=%d` | UI 操作/状态或诊断 | W | augment | 单次 UI 操作失败，W 合适；建议结构化为 verb + 对象 + reason，补齐定位字段。 |
| `detail_actions.cpp:639` | extension_bag | I | `sell price reject reason=popup_unavailable source=equip valid=%d owned=%d` | UI 操作/状态或诊断 | W | promote | 此处记录单次失败/拒绝且游戏仍可继续；按 W 并补 reason=，不是状态成功事件。 |
| `detail_actions.cpp:658` | extension_bag | I | `extension sell confirmation created price=%lld custom=1` | UI 操作/状态或诊断 | — | delete | 无变量静态文本，不能提供对象/结果定位信息。 |
| `detail_actions.cpp:662` | extension_bag | I | `extension sell custom confirmation unavailable; original fallback disabled` | UI 操作/状态或诊断 | — | delete | 无变量静态文本，不能提供对象/结果定位信息。 |
| `detail_actions.cpp:682` | extension_bag | W | `extension desc hook stale index=%d id=%llu button=%p current_button=%p data=%p execute_slot=%p code=%d matches=%d reason=identity_validation_failed` | UI 操作/状态或诊断 | W | augment | 单次 UI 操作失败，W 合适；建议结构化为 verb + 对象 + reason，补齐定位字段。 |
| `detail_actions.cpp:700` | extension_bag | I | `extension desc execute index=%d original=%p bag=%d slot=%d` | UI 操作/状态或诊断 | D | demote | 调用位于查询/按钮执行/投影诊断路径，非有限状态迁移；应为 D 或删除高频明细。 |
| `detail_actions.cpp:719` | extension_bag | W | `extension use rejected stale detail bag=%d slot=%d` | UI 操作/状态或诊断 | W | augment | 详情操作拒绝属于可恢复单次失败；核对 bag/slot/item/reason，避免与 detail_hooks 同一 stale 失败双记。 |
| `detail_actions.cpp:734` | extension_bag | W | `extension use action rejected bag=%d slot=%d status=%d` | UI 操作/状态或诊断 | W | augment | 单次 UI 操作失败，W 合适；建议结构化为 verb + 对象 + reason，补齐定位字段。 |
| `detail_actions.cpp:749` | extension_bag | I | `extension use execute bag=%d slot=%d confirm=%d used=%d` | UI 操作/状态或诊断 | D | demote | 过程/回调细节不是状态迁移；UI 操作过程按 D 记录，避免每次触发 INFO。 |
| `detail_actions.cpp:784` | extension_bag | W | `extension dice use rejected stale detail bag=%d slot=%d` | UI 操作/状态或诊断 | W | augment | 详情操作拒绝属于可恢复单次失败；核对 bag/slot/item/reason，避免与 detail_hooks 同一 stale 失败双记。 |
| `detail_actions.cpp:806` | extension_bag | W | `extension release-sealed rejected stale detail bag=%d slot=%d` | UI 操作/状态或诊断 | W | augment | 详情操作拒绝属于可恢复单次失败；核对 bag/slot/item/reason，避免与 detail_hooks 同一 stale 失败双记。 |
| `detail_actions.cpp:827` | extension_bag | W | `extension shortcut rejected stale detail bag=%d slot=%d` | UI 操作/状态或诊断 | W | augment | 详情操作拒绝属于可恢复单次失败；核对 bag/slot/item/reason，避免与 detail_hooks 同一 stale 失败双记。 |
| `detail_actions.cpp:845` | extension_bag | I | `extension sell button matched source=%s bag=%d slot=%d item=%p` | UI 操作/状态或诊断 | D | demote | 调用位于查询/按钮执行/投影诊断路径，非有限状态迁移；应为 D 或删除高频明细。 |
| `detail_actions.cpp:855` | extension_bag | I | `extension desc execute rejected unsupported action index=%d original=%p bag=%d slot=%d` | UI 操作/状态或诊断 | W | promote | 此处记录单次失败/拒绝且游戏仍可继续；按 W 并补 reason=，不是状态成功事件。 |
| `detail_hooks.cpp:57` | extension_bag | W | `extension desc hook unbind failed index=%d id=%llu reason=host_unbind_error code=%d message=%.*s` | UI 操作/状态或诊断 | W | augment | 单次 UI 操作失败，W 合适；建议结构化为 verb + 对象 + reason，补齐定位字段。 |
| `detail_hooks.cpp:110` | extension_bag | W | `extension desc hook skipped reason=desc hook v2 unavailable` | UI 操作/状态或诊断 | W | augment | 单次 UI 操作失败，W 合适；建议结构化为 verb + 对象 + reason，补齐定位字段。 |
| `detail_hooks.cpp:148` | extension_bag | D | `extension desc hook skip index=%d reason=already_bound id=%llu` | UI 操作/状态或诊断 | D | keep | debug 定位字段/分支信息，按需开启。 |
| `detail_hooks.cpp:154` | extension_bag | D | `extension desc hook rebind index=%d id=%llu reason=button_changed resolve=%d validate=%d matches=%d` | UI 操作/状态或诊断 | D | keep | debug 定位字段/分支信息，按需开启。 |
| `detail_hooks.cpp:165` | extension_bag | W | `extension desc hook rebind unbind failed index=%d id=%llu code=%d` | UI 操作/状态或诊断 | W | augment | 单次 UI 操作失败，W 合适；建议结构化为 verb + 对象 + reason，补齐定位字段。 |
| `detail_hooks.cpp:180` | extension_bag | D | `extension desc hook resolve skip index=%d code=%d` | UI 操作/状态或诊断 | D | keep | debug 定位字段/分支信息，按需开启。 |
| `detail_hooks.cpp:186` | extension_bag | D | `extension desc hook resolve skip index=%d reason=null_execute_slot` | UI 操作/状态或诊断 | D | keep | debug 定位字段/分支信息，按需开启。 |
| `detail_hooks.cpp:200` | extension_bag | D | `extension desc hook resolve skip index=%d reason=duplicate_execute_slot execute=%p` | UI 操作/状态或诊断 | D | keep | debug 定位字段/分支信息，按需开启。 |
| `detail_hooks.cpp:210` | extension_bag | D | `extension desc hook bind skip index=%d code=%d id=%llu` | UI 操作/状态或诊断 | D | keep | debug 定位字段/分支信息，按需开启。 |
| `detail_hooks.cpp:227` | extension_bag | W | `extension desc hook classify failed index=%d id=%llu equip_code=%d unequip_code=%d reason=original_matches_error` | UI 操作/状态或诊断 | W | augment | 单次 UI 操作失败，W 合适；建议结构化为 verb + 对象 + reason，补齐定位字段。 |
| `detail_hooks.cpp:233` | extension_bag | W | `extension desc hook cleanup failed index=%d id=%llu reason=host_unbind_error code=%d` | UI 操作/状态或诊断 | W | augment | 单次 UI 操作失败，W 合适；建议结构化为 verb + 对象 + reason，补齐定位字段。 |
| `detail_hooks.cpp:249` | extension_bag | W | `extension desc hook equip/unequip unbind failed index=%d id=%llu equip=%d unequip=%d reason=host_unbind_error code=%d` | UI 操作/状态或诊断 | W | augment | 单次 UI 操作失败，W 合适；建议结构化为 verb + 对象 + reason，补齐定位字段。 |
| `detail_hooks.cpp:265` | extension_bag | D | `extension desc hook installed index=%d id=%llu button=%p data=%p execute_slot=%p original=%p` | UI 操作/状态或诊断 | D | keep | debug 定位字段/分支信息，按需开启。 |
| `equip.cpp:103` | extension_bag | W | `extension equip reject reason=payload_serialize_failed src=%d/%d` | UI 操作/状态或诊断 | W | augment | 单次 UI 操作失败，W 合适；建议结构化为 verb + 对象 + reason，补齐定位字段。 |
| `equip.cpp:123` | extension_bag | W | `extension equip rejected source release bag=%d slot=%d` | UI 操作/状态或诊断 | W | augment | 单次 UI 操作失败，W 合适；建议结构化为 verb + 对象 + reason，补齐定位字段。 |
| `equip.cpp:127` | extension_bag | I | `extension equip ok source=extension bag=%d category=%d src=%d/%d` | UI 操作/状态或诊断 | I | keep | 有限的状态/操作结果变化；正文含操作对象或结果字段。 |
| `equip.cpp:142` | extension_bag | W | `extension equip ledger exhausted src=%d/%d` | UI 操作/状态或诊断 | W | augment | 单次 UI 操作失败，W 合适；建议结构化为 verb + 对象 + reason，补齐定位字段。 |
| `equip.cpp:147` | extension_bag | W | `extension equip remove verify failed bag=%d slot=%d` | UI 操作/状态或诊断 | W | augment | 单次 UI 操作失败，W 合适；建议结构化为 verb + 对象 + reason，补齐定位字段。 |
| `equip.cpp:163` | extension_bag | I | `extension equip ok bag=%d category=%d src=%d/%d` | UI 操作/状态或诊断 | I | keep | 有限的状态/操作结果变化；正文含操作对象或结果字段。 |
| `equip.cpp:171` | extension_bag | W | `extension equip state reject bag=%d category=%d` | UI 操作/状态或诊断 | W | augment | 单次 UI 操作失败，W 合适；建议结构化为 verb + 对象 + reason，补齐定位字段。 |
| `equip.cpp:187` | extension_bag | W | `extension equip persist failed bag=%d restored=%d` | UI 操作/状态或诊断 | W | augment | 单次 UI 操作失败，W 合适；建议结构化为 verb + 对象 + reason，补齐定位字段。 |
| `equip.cpp:233` | extension_bag | W | `extension character equip action rejected role=%d bag=%d slot=%d equip_slot=%d status=%d` | UI 操作/状态或诊断 | W | augment | 单次 UI 操作失败，W 合适；建议结构化为 verb + 对象 + reason，补齐定位字段。 |
| `equip.cpp:279` | extension_bag | W | `extension tab drop equip result=%d index=%d` | UI 操作/状态或诊断 | W | augment | 单次 UI 操作失败，W 合适；建议结构化为 verb + 对象 + reason，补齐定位字段。 |
| `equip.cpp:284` | extension_bag | I | `extension tab drop equipped index=%d` | UI 操作/状态或诊断 | I | keep | 有限的状态/操作结果变化；正文含操作对象或结果字段。 |
| `equip.cpp:317` | extension_bag | W | `extension source tab equip result=%d target=%d source=%d/%d` | UI 操作/状态或诊断 | W | augment | 单次 UI 操作失败，W 合适；建议结构化为 verb + 对象 + reason，补齐定位字段。 |
| `equip.cpp:361` | extension_bag | I | `extension desc item captured bag=%d slot=%d item=%p` | UI 操作/状态或诊断 | D | demote | 过程/回调细节不是状态迁移；UI 操作过程按 D 记录，避免每次触发 INFO。 |
| `equip.cpp:375` | extension_bag | I | `extension bag desc keeps original unequip bag=%d slot=%d item=%p` | UI 操作/状态或诊断 | D | demote | 过程/回调细节不是状态迁移；UI 操作过程按 D 记录，避免每次触发 INFO。 |
| `equip_hooks.cpp:46` | extension_bag | E | `hook=%s backup missing` | UI 操作/状态或诊断 | W | demote | 单一 hook/调用路径失败且游戏可继续；保留 hook/失败原因字段。 |
| `equip_hooks.cpp:80` | extension_bag | W | `equip detail identity lock busy item=%p; blocked` | UI 操作/状态或诊断 | W | augment | 单次 UI 操作失败，W 合适；建议结构化为 verb + 对象 + reason，补齐定位字段。 |
| `equip_hooks.cpp:101` | extension_bag | W | `equip button blocked extension detail item=%p` | UI 操作/状态或诊断 | W | augment | 单次 UI 操作失败，W 合适；建议结构化为 verb + 对象 + reason，补齐定位字段。 |
| `equip_hooks.cpp:148` | extension_bag | E | `hook=%s backup missing` | UI 操作/状态或诊断 | W | demote | 单一 hook/调用路径失败且游戏可继续；保留 hook/失败原因字段。 |
| `equip_hooks.cpp:165` | extension_bag | E | `hook=%s backup missing` | UI 操作/状态或诊断 | W | demote | 单一 hook/调用路径失败且游戏可继续；保留 hook/失败原因字段。 |
| `equip_hooks.cpp:249` | extension_bag | I | `equip hook bound resource=%s backup=%p` | UI 操作/状态或诊断 | I | keep | 有限的状态/操作结果变化；正文含操作对象或结果字段。 |
| `equip_hooks.cpp:259` | extension_bag | E | `hook=%s backup missing` | UI 操作/状态或诊断 | W | demote | 单一 hook/调用路径失败且游戏可继续；保留 hook/失败原因字段。 |
| `mix.cpp:143` | extension_bag | I | `mix tab delete failed control=%p` | UI 操作/状态或诊断 | W | promote | 此处记录单次失败/拒绝且游戏仍可继续；按 W 并补 reason=，不是状态成功事件。 |
| `mix.cpp:205` | extension_bag | I | `mix tab create failed index=%d` | UI 操作/状态或诊断 | W | promote | 此处记录单次失败/拒绝且游戏仍可继续；按 W 并补 reason=，不是状态成功事件。 |
| `mix.cpp:210` | extension_bag | I | `mix tab install failed index=%d stage=%s` | UI 操作/状态或诊断 | W | promote | 此处记录单次失败/拒绝且游戏仍可继续；按 W 并补 reason=，不是状态成功事件。 |
| `mix.cpp:251` | extension_bag | I | `mix tabs re-anchored root=%p` | UI 操作/状态或诊断 | D | demote | 过程/回调细节不是状态迁移；UI 操作过程按 D 记录，避免每次触发 INFO。 |
| `mix.cpp:282` | extension_bag | I | `mix tabs installed root=%p generation=%llu complete=%d created=%d` | UI 操作/状态或诊断 | I | keep | 有限的状态/操作结果变化；正文含操作对象或结果字段。 |
| `mix.cpp:352` | extension_bag | I | `mix item refresh failed reason=missing-adapter` | UI 操作/状态或诊断 | — | delete | 无变量静态文本，不能提供对象/结果定位信息。 |
| `mix.cpp:366` | extension_bag | I | `mix item refresh failed code=%d` | UI 操作/状态或诊断 | W | promote | 此处记录单次失败/拒绝且游戏仍可继续；按 W 并补 reason=，不是状态成功事件。 |
| `mix.cpp:387` | extension_bag | I | `mix item refresh completed without item root; retry scheduled` | UI 操作/状态或诊断 | — | delete | 无变量静态文本，不能提供对象/结果定位信息。 |
| `mix.cpp:402` | extension_bag | I | `mix view rejected display hooks unavailable set=%d get=%d size=%d` | UI 操作/状态或诊断 | W | promote | 此处记录单次失败/拒绝且游戏仍可继续；按 W 并补 reason=，不是状态成功事件。 |
| `mix.cpp:414` | extension_bag | I | `mix view rejected host bag=%d` | UI 操作/状态或诊断 | W | promote | 此处记录单次失败/拒绝且游戏仍可继续；按 W 并补 reason=，不是状态成功事件。 |
| `mix.cpp:443` | extension_bag | I | `mix view installed bag=%d window=%d capacity=%d` | UI 操作/状态或诊断 | I | keep | 有限的状态/操作结果变化；正文含操作对象或结果字段。 |
| `mix.cpp:449` | extension_bag | I | `mix restore view bag=%d window=%d current=%d state=%d` | UI 操作/状态或诊断 | I | keep | 有限的状态/操作结果变化；正文含操作对象或结果字段。 |
| `mix.cpp:607` | extension_bag | I | `mix state edge state=%d installed=%d bag=%d window=%d current=%d` | UI 操作/状态或诊断 | D | demote | 过程/回调细节不是状态迁移；UI 操作过程按 D 记录，避免每次触发 INFO。 |
| `store_projection.cpp:78` | extension_bag | I | `store snapshot tag=%s root=%p count=%d counted=%d bag=%d generation=%llu window=%d window_word=%#x word_read=%d` | UI 操作/状态或诊断 | D | demote | 调用位于查询/按钮执行/投影诊断路径，非有限状态迁移；应为 D 或删除高频明细。 |
| `store_projection.cpp:91` | extension_bag | I | `store snapshot tag=%s slot=%d control=null` | UI 操作/状态或诊断 | D | demote | 调用位于查询/按钮执行/投影诊断路径，非有限状态迁移；应为 D 或删除高频明细。 |
| `store_projection.cpp:100` | extension_bag | I | `store snapshot tag=%s slot=%d control=%p parent=%p root_match=%d type=%d data=%p item=%p expected=%p` | UI 操作/状态或诊断 | D | demote | 调用位于查询/按钮执行/投影诊断路径，非有限状态迁移；应为 D 或删除高频明细。 |
| `store_projection.cpp:120` | extension_bag | I | `store item refresh failed reason=missing-adapter` | UI 操作/状态或诊断 | — | delete | 无变量静态文本，不能提供对象/结果定位信息。 |
| `store_projection.cpp:139` | extension_bag | I | `store item refresh failed code=%d` | UI 操作/状态或诊断 | W | promote | 此处记录单次失败/拒绝且游戏仍可继续；按 W 并补 reason=，不是状态成功事件。 |
| `store_projection.cpp:156` | extension_bag | I | `store item refresh completed without item root; retry scheduled` | UI 操作/状态或诊断 | — | delete | 无变量静态文本，不能提供对象/结果定位信息。 |
| `store_projection.cpp:218` | extension_bag | W | `store item bind failed slot=%d control=%p data=%p item=%p` | UI 操作/状态或诊断 | W | augment | 单次 UI 操作失败，W 合适；建议结构化为 verb + 对象 + reason，补齐定位字段。 |
| `store_projection.cpp:224` | extension_bag | I | `store item retarget slot=%d control=%p parent=%p prev_control=%p prev_parent=%p` | UI 操作/状态或诊断 | D | demote | 调用位于查询/按钮执行/投影诊断路径，非有限状态迁移；应为 D 或删除高频明细。 |
| `store_projection.cpp:239` | extension_bag | I | `store view rejected display hooks unavailable set=%d get=%d size=%d` | UI 操作/状态或诊断 | W | promote | 此处记录单次失败/拒绝且游戏仍可继续；按 W 并补 reason=，不是状态成功事件。 |
| `store_projection.cpp:247` | extension_bag | I | `store view rejected original bag=%d` | UI 操作/状态或诊断 | W | promote | 此处记录单次失败/拒绝且游戏仍可继续；按 W 并补 reason=，不是状态成功事件。 |
| `store_projection.cpp:253` | extension_bag | I | `store view rejected capacity read window=%d` | UI 操作/状态或诊断 | W | promote | 此处记录单次失败/拒绝且游戏仍可继续；按 W 并补 reason=，不是状态成功事件。 |
| `store_projection.cpp:303` | extension_bag | I | `store view installed bag=%d window=%d capacity=%d` | UI 操作/状态或诊断 | I | keep | 有限的状态/操作结果变化；正文含操作对象或结果字段。 |
| `store_tabs.cpp:137` | extension_bag | I | `store tab delete failed control=%p` | UI 操作/状态或诊断 | W | promote | 此处记录单次失败/拒绝且游戏仍可继续；按 W 并补 reason=，不是状态成功事件。 |
| `store_tabs.cpp:189` | extension_bag | I | `store tab create failed index=%d` | UI 操作/状态或诊断 | W | promote | 此处记录单次失败/拒绝且游戏仍可继续；按 W 并补 reason=，不是状态成功事件。 |
| `store_tabs.cpp:194` | extension_bag | I | `store tab install failed index=%d stage=%s` | UI 操作/状态或诊断 | W | promote | 此处记录单次失败/拒绝且游戏仍可继续；按 W 并补 reason=，不是状态成功事件。 |
| `store_tabs.cpp:212` | extension_bag | D | `store tab create index=%d control=%p parent=%p data=%p item=%p` | UI 操作/状态或诊断 | D | keep | debug 定位字段/分支信息，按需开启。 |
| `store_tabs.cpp:239` | extension_bag | I | `store tabs re-anchored root=%p` | UI 操作/状态或诊断 | D | demote | 过程/回调细节不是状态迁移；UI 操作过程按 D 记录，避免每次触发 INFO。 |
| `store_tabs.cpp:274` | extension_bag | I | `store tabs installed root=%p generation=%llu complete=%d created=%d children_before=%d children_after=%d recounted=%d` | UI 操作/状态或诊断 | I | keep | 有限的状态/操作结果变化；正文含操作对象或结果字段。 |
| `store_trade.cpp:95` | extension_bag | I | `store button sell view=%d module=%d count=%d item=%p` | UI 操作/状态或诊断 | D | demote | 调用位于查询/按钮执行/投影诊断路径，非有限状态迁移；应为 D 或删除高频明细。 |
| `store_trade.cpp:108` | extension_bag | I | `store quantity dialog unavailable; falling back count=%d` | UI 操作/状态或诊断 | W | promote | 此处记录单次失败/拒绝且游戏仍可继续；按 W 并补 reason=，不是状态成功事件。 |
| `store_trade.cpp:176` | extension_bag | I | `store sell reject reason=identity_or_plan bag=%d slot=%d` | UI 操作/状态或诊断 | W | promote | 此处记录单次失败/拒绝且游戏仍可继续；按 W 并补 reason=，不是状态成功事件。 |
| `store_trade.cpp:184` | extension_bag | I | `store sell reject reason=price_mismatch bag=%d slot=%d count=%d price=%lld displayed=%lld` | UI 操作/状态或诊断 | W | promote | 此处记录单次失败/拒绝且游戏仍可继续；按 W 并补 reason=，不是状态成功事件。 |
| `store_trade.cpp:197` | extension_bag | I | `store sell action rejected bag=%d slot=%d count=%d status=%d` | UI 操作/状态或诊断 | W | promote | 此处记录单次失败/拒绝且游戏仍可继续；按 W 并补 reason=，不是状态成功事件。 |
| `store_trade.cpp:212` | extension_bag | I | `store sell committed through inventory.action bag=%d slot=%d count=%d view=%d price=%lld` | UI 操作/状态或诊断 | I | keep | 有限的状态/操作结果变化；正文含操作对象或结果字段。 |
| `store_trade.cpp:224` | extension_bag | I | `store buy gate original lookup unavailable item=%p` | UI 操作/状态或诊断 | W | promote | 此处记录单次失败/拒绝且游戏仍可继续；按 W 并补 reason=，不是状态成功事件。 |
| `store_trade.cpp:236` | extension_bag | I | `store buy gate cannot read projected capacity window=%d` | UI 操作/状态或诊断 | W | promote | 此处记录单次失败/拒绝且游戏仍可继续；按 W 并补 reason=，不是状态成功事件。 |
| `store_trade.cpp:240` | extension_bag | I | `store buy gate cannot restore original capacity window=%d` | UI 操作/状态或诊断 | W | promote | 此处记录单次失败/拒绝且游戏仍可继续；按 W 并补 reason=，不是状态成功事件。 |
| `store_trade.cpp:253` | extension_bag | I | `store buy gate view changed during original lookup generation=%llu->%llu` | UI 操作/状态或诊断 | — | delete | 无变量静态文本，不能提供对象/结果定位信息。 |
| `store_trade.cpp:259` | extension_bag | I | `store buy gate failed to restore projected capacity window=%d` | UI 操作/状态或诊断 | W | promote | 此处记录单次失败/拒绝且游戏仍可继续；按 W 并补 reason=，不是状态成功事件。 |
| `store_trade.cpp:266` | extension_bag | I | `store buy gate received invalid original slot=%d` | UI 操作/状态或诊断 | D | demote | 过程/回调细节不是状态迁移；UI 操作过程按 D 记录，避免每次触发 INFO。 |
| `store_trade.cpp:273` | extension_bag | I | `store buy gate original lookup failed code=%d` | UI 操作/状态或诊断 | W | promote | 此处记录单次失败/拒绝且游戏仍可继续；按 W 并补 reason=，不是状态成功事件。 |
| `store_trade.cpp:285` | extension_bag | I | `store buy gate extension preflight rejected item=%p reason=source-read` | UI 操作/状态或诊断 | W | promote | 此处记录单次失败/拒绝且游戏仍可继续；按 W 并补 reason=，不是状态成功事件。 |

## 缺失日志建议

| file + 函数 | 事件 | 目标等级 | 必填字段 | 理由 |
|---|---|---|---|---|
| `store_projection.cpp` + `store_install_module_view_locked` / `store_restore_module_view_locked` | 商店扩展视图实际安装/还原结果 | I | `bag=`、`result=installed/restored` | 现有安装仅有未分级 INFO 混合诊断，恢复无结果日志；应让用户可见的视图切换可回放。
| `store_projection.cpp` + `store_refresh_item_after_unlock` | 投影刷新最终失败（重试仍未恢复） | W | `op=refresh_store_item`、`bag=`、`reason=`、`attempt=` 或 `attempts=` | 当前缺 adapter/API/root 时记 INFO 或反复重试；需统一记录失败原因并避免逐次噪声。
| `mix.cpp` + `mix_install_module_view_locked` / `mix_restore_module_view_locked` | 合成器扩展视图安装/还原结果 | I | `panel=mix`、`bag=`、`result=installed/restored` | 面板状态/用户切换最终结果应可回放；现有 view installed/restore 使用默认 INFO，但 restore 明细不宜每次重复。
| `equip.cpp` + `equip_module_item_on_character_locked` | 详情装备操作最终拒绝原因 | W | `bag=`、`slot=`、`item=`、`target_slot=`、`reason=` | 多个前置条件失败直接返回 false，UI 不记录最终拒绝原因，用户无法区分不兼容、槽位不可用和操作失败。
| `detail_actions.cpp` + `extension_desc_item_execute_at` | 详情页切换/执行委托失败的最终原因 | W | `item=`、`bag=`、`slot=`、`action=`、`reason=` | call_original 返回失败当前被忽略，详情按钮失败无可见原因；只建议记录失败结果，不增加每按钮点击日志。

## 统计

- 现有日志：123 条。
- disposition：keep 21，demote 26，promote 29，delete 9，merge 0，augment 38。
- 补充建议：5 条。

## Top 发现

1. `store_projection.cpp` / `mix.cpp` 中投影快照与 draw-end/UI 高频诊断通过默认 INFO 输出；应将定位细节降为 D，失败结果单独结构化为 W。
2. `detail_actions.cpp` 中详情按钮进入、匹配、报价和分支细节含 INFO；点击过程不是状态迁移，应降为 D，操作成功/失败结果再单独记录。
3. `equip.cpp` 拖拽装备拒绝和 `store_trade.cpp` 出售拒绝属于用户操作失败，目标 W，需补 `reason=` 及 bag/slot/item/目标槽等字段。
4. `detail_hooks.cpp` 的 resolve/bind 跳过属于可选按钮/面板分支，应保持 D；真正影响 hook 能力的失败应为 W，不能与 `detail_actions.cpp` 对同一 stale 失败重复记录。
5. `VIRTBAG_LOG` 默认是 I；现有默认宏用于过程日志和失败记录的情况较多，不能按宏名直接视为合规 INFO。

## 审核裁决

### 批准并执行

- **promote I→W（29 条）**：`detail_actions.cpp:91,297,352,457,639,855`；`mix.cpp:143,205,210,366,402,414`；`store_projection.cpp:139,239,247,253`；`store_tabs.cpp:137,189,194`；`store_trade.cpp:108,176,184,197,224,236,240,259,273,285`。
- **demote（26 条）**：按原清单全部执行，其中 22 条 I→D，`equip_hooks.cpp:46,148,165,259` 为 E→W。
- **delete（2 条）**：`detail_actions.cpp:195,516`。
- **augment 白名单（23 条）**：`detail_actions.cpp:226,235,249,391,399,409,421,484,541,719,734,784,806,827`；`equip.cpp:123,142,147,171,187,233,279,317`；`store_projection.cpp:218`。均保留原正文并在末尾追加指定 `reason=`。
- **新增 I 里程碑**：`store_projection.cpp` 记录 `op=store_view result=installed|restored bag=%d`；`mix.cpp` 记录 `op=mix_view result=installed|restored bag=%d`，仅在状态转换时输出。
- **新增 W 失败结果**：装备详情最终拒绝记录 `op=equip_detail result=rejected bag=%d slot=%d item=%p reason=%s`；详情 `call_original` 失败记录 `op=desc_execute result=failed bag=%d slot=%d action=%d reason=original_failed`。

### 驳回

- **驳回删除并保留**：`detail_actions.cpp:658`（保留含 price/custom 字段的 I）；`mix.cpp:352`、`store_projection.cpp:120`（保留含 `reason=missing-adapter` 的日志）；`store_trade.cpp:253`（保留 generation 变更诊断）。
- **保留并追加字段**：`detail_actions.cpp:662` 改为 W 并追加 `reason=custom_confirmation_unavailable`；`mix.cpp:387`、`store_projection.cpp:156` 保持 I 并追加 `reason=missing_item_root`。
- **不改**：`detail_actions.cpp:263`、`detail_hooks.cpp:53,57,62,63,64`、`detail_actions.cpp:682`、`equip.cpp:103`，已有 `reason=` 或完整诊断字段。
- “store item refresh 最终失败”建议驳回；现有 `store_projection.cpp:139` 刷新失败日志已按批准项改为 W。
