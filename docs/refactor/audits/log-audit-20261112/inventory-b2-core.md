# 日志审计清单：批次 2（extension_bag/core）

依据 `docs/reference/logging-standards.md` 与 `docs/reference/logging.md`。`VIRTBAG_LOG*` 和 `QOL_LOG_*` 均固定输出 `extension_bag` 域；等级语义按操作失败后游戏是否仍可继续判定。

## 现有日志

| file:line | domain | level_now | message 摘要 | event_class | level_target | disposition | rationale |
|---|---|---|---|---|---|---|---|
| `drag_session.cpp:19` | extension_bag | I | p5 session begin token=%llu source=%d/%d generation=%llu | 状态/对象操作 | I | keep | 审核裁决：原建议驳回；保留现状。 |
| `drag_session.cpp:46` | extension_bag | I | p5 session cleanup reason=%s token=%llu phase=%d unknown | 状态/对象操作 | I | keep | 审核裁决：原建议驳回；保留现状。 |
| `drag_session.cpp:55` | extension_bag | I | p5 session cancel reason=%s token=%llu phase=%d transition=%d unknown | 状态/对象操作 | I | keep | 审核裁决：原建议驳回；保留现状。 |
| `drop_routing.cpp:107` | extension_bag | W | drop deferred ext->orig rejected source=%d/%d target_bag=%d phase=%d | 业务拒绝 | W | augment | 审核批准：按白名单在既有正文末尾追加 reason= 字段。 |
| `drop_routing.cpp:120` | extension_bag | I | p5 session drop committed source=%d/%d target=%d/%d kind=%d deferred=1 | 事务提交 | I | keep | 审核裁决：原建议驳回；保留现状。 |
| `drop_routing.cpp:124` | extension_bag | W | p5 session drop rejected source=%d/%d target=%d/%d kind=%d deferred=1 | 业务拒绝 | W | augment | 审核批准：按白名单在既有正文末尾追加 reason= 字段。 |
| `drop_routing.cpp:133` | extension_bag | D | drop release mode=%d selected=%d x=%lld y=%lld moving=%p extension_drag=%d | 拖放路由 | D | keep | 审核裁决：原建议驳回；保留现状。 |
| `drop_routing.cpp:142` | extension_bag | D | drop reject original->extension no moving original control x=%lld y=%lld selected=%d mode=%d | 业务拒绝 | D | keep | 审核裁决：原建议驳回；保留现状。 |
| `drop_routing.cpp:173` | extension_bag | W | drop reject original->extension no extension tab hit x=%lld y=%lld | 业务拒绝 | W | keep | 审核裁决：原建议驳回；保留现状。 |
| `drop_routing.cpp:193` | extension_bag | I | drop result direction=extension->extension handled=%d src=%d/%d dst=%d/%d | 拖放路由 | I | keep | 审核裁决：原建议驳回；保留现状。 |
| `drop_routing.cpp:214` | extension_bag | I | drop result direction=extension->tab handled=%d equip=%d src=%d/%d dst_bag=%d | 拖放路由 | I | keep | 审核裁决：原建议驳回；保留现状。 |
| `drop_routing.cpp:223` | extension_bag | W | drop reject extension->original reason=invalid_transaction_domain target_bag=%d | 业务拒绝 | W | keep | 审核裁决：原建议驳回；保留现状。 |
| `drop_routing.cpp:230` | extension_bag | I | drop result direction=extension->original handled=%d src=%d/%d target_bag=%d | 拖放路由 | I | keep | 审核裁决：原建议驳回；保留现状。 |
| `drop_routing.cpp:234` | extension_bag | W | drop reject extension destination not found x=%lld y=%lld drop reject extension no active drag selected=%d | 业务拒绝 | W | augment | 审核批准：按白名单在既有正文末尾追加 reason= 字段。 |
| `drop_routing.cpp:237` | extension_bag | W | drop reject extension no active drag selected=%d | 业务拒绝 | W | augment | 审核批准：按白名单在既有正文末尾追加 reason= 字段。 |
| `drop_routing.cpp:270` | extension_bag | I | drop result direction=extension->native handled=%d src=%d/%d target_bag=%d | 拖放路由 | I | keep | 审核裁决：原建议驳回；保留现状。 |
| `drop_routing.cpp:313` | extension_bag | W | p5 session target miss x=%lld y=%lld source=%d/%d | 状态/对象操作 | W | augment | 审核批准：按白名单在既有正文末尾追加 reason= 字段。 |
| `drop_routing.cpp:383` | extension_bag | I | p5 session drop %s source=%d/%d target=%d/%d kind=%d equip=%d committed rejected | 业务拒绝 | W | promote | 审核批准：单次拒绝/失败但游戏可继续，按裁决提升至 WARN。 |
| `drop_routing.cpp:398` | extension_bag | W | cross tab reject reason=%s bag=%d slot=%d target_bag=%d target_internal=%d phase=%d generation=%llu/%llu/%llu | 业务拒绝 | W | keep | 审核裁决：原建议驳回；保留现状。 |
| `drop_routing.cpp:478` | extension_bag | I | tab commit handled=1 bag=%d slot=%d target_bag=%d target_kind=%d equip=%d | 事务提交 | I | keep | 审核裁决：原建议驳回；保留现状。 |
| `drop_routing.cpp:482` | extension_bag | W | tab reject reason=transaction_failed bag=%d slot=%d target_bag=%d target_kind=%d equip=%d | 业务拒绝 | W | keep | 审核裁决：原建议驳回；保留现状。 |
| `host_adapter.cpp:61` | extension_bag | E | ERROR unequip host capacity restore failed bag=%d word=0x%x | 单次操作失败 | W | demote | 审核批准：单次操作失败且无数据完整性风险，降为 WARN。 |
| `host_adapter.cpp:88` | extension_bag | I | extension unequip symbols unavailable bag=%d | 装备操作 | W | promote | 审核批准：单次拒绝/失败但游戏可继续，按裁决提升至 WARN。 |
| `host_adapter.cpp:95` | extension_bag | I | extension unequip create item failed bag=%d | 单次操作失败 | W | promote | 审核批准：单次拒绝/失败但游戏可继续，按裁决提升至 WARN。 |
| `host_adapter.cpp:102` | extension_bag | I | extension unequip ledger exhausted bag=%d | 装备操作 | W | promote | 审核批准：单次拒绝/失败但游戏可继续，按裁决提升至 WARN。 |
| `host_adapter.cpp:118` | extension_bag | I | extension unequip bag marker write failed bag=%d | 单次操作失败 | W | promote | 审核批准：单次拒绝/失败但游戏可继续，按裁决提升至 WARN。 |
| `host_adapter.cpp:135` | extension_bag | I | extension unequip host capacity write failed bag=%d | 单次操作失败 | W | promote | 审核批准：单次拒绝/失败但游戏可继续，按裁决提升至 WARN。 |
| `host_adapter.cpp:141` | extension_bag | I | extension unequip host capacity real bag=%d projected=0x%x real=0x%x | 装备操作 | I | keep | 审核裁决：原建议驳回；保留现状。 |
| `host_adapter.cpp:221` | extension_bag | I | extension unequip adopt bag=%d type=%d dst=%d/%d | 装备操作 | I | keep | 审核裁决：原建议驳回；保留现状。 |
| `host_adapter.cpp:227` | extension_bag | I | extension unequip rollback blocked by active object bag=%d slot=%d | 装备操作 | W | promote | 审核批准：单次拒绝/失败但游戏可继续，按裁决提升至 WARN。 |
| `host_adapter.cpp:241` | extension_bag | I | extension unequip adopt persist failed bag=%d dst=%d/%d | 单次操作失败 | W | promote | 审核批准：单次拒绝/失败但游戏可继续，按裁决提升至 WARN。 |
| `host_adapter.cpp:256` | extension_bag | I | extension unequip ok bag=%d type=%d receiving=%d | 装备操作 | I | keep | 审核裁决：原建议驳回；保留现状。 |
| `host_adapter.cpp:280` | extension_bag | I | extension unequip persist failed; rolled back bag=%d | 单次操作失败 | W | promote | 审核批准：单次拒绝/失败但游戏可继续，按裁决提升至 WARN。 |
| `host_adapter.cpp:285` | extension_bag | I | extension unequip rollback failed bag=%d receiving=%d | 单次操作失败 | W | promote | 审核批准：单次拒绝/失败但游戏可继续，按裁决提升至 WARN。 |
| `host_adapter.cpp:302` | extension_bag | I | extension desc unequip stale rejected ext_bag=%d | 业务拒绝 | W | promote | 审核批准：单次拒绝/失败但游戏可继续，按裁决提升至 WARN。 |
| `host_adapter.cpp:306` | extension_bag | I | extension desc unequip bag=%d result=%d | 装备操作 | I | keep | 审核裁决：原建议驳回；保留现状。 |
| `host_adapter.cpp:343` | extension_bag | I | extension character equip rejected bag=%d slot=%d | 业务拒绝 | W | promote | 审核批准：单次拒绝/失败但游戏可继续，按裁决提升至 WARN。 |
| `host_adapter.cpp:353` | extension_bag | I | extension character equip committed bag=%d slot=%d | 事务提交 | I | keep | 审核裁决：原建议驳回；保留现状。 |
| `host_adapter.cpp:368` | extension_bag | I | backpack equip probe mode=%d desc_item=%p desc_bag=%d desc_slot=%d types=%d/%d/%d/%d/%d | 装备操作 | I | keep | 审核裁决：原建议驳回；保留现状。 |
| `host_adapter.cpp:396` | extension_bag | I | backpack equip probe rejected stale identity bag=%d slot=%d item=%p | 业务拒绝 | W | promote | 审核批准：单次拒绝/失败但游戏可继续，按裁决提升至 WARN。 |
| `host_adapter.cpp:415` | extension_bag | I | backpack equip probe rejected category=%d bag=%d slot=%d | 业务拒绝 | W | promote | 审核批准：单次拒绝/失败但游戏可继续，按裁决提升至 WARN。 |
| `host_adapter.cpp:435` | extension_bag | I | backpack native equip fallback stage=slot_unavailable src=%d/%d slot=%d | 装备操作 | W | promote | 审核批准：单次拒绝/失败但游戏可继续，按裁决提升至 WARN。 |
| `host_adapter.cpp:440` | extension_bag | W | backpack native equip fallback stage=payload_invalid src=%d/%d size=%d | 装备操作 | W | keep | 审核裁决：原建议驳回；保留现状。 |
| `host_adapter.cpp:446` | extension_bag | W | backpack native equip fallback stage=ports_unavailable src=%d/%d | 装备操作 | W | keep | 审核裁决：原建议驳回；保留现状。 |
| `host_adapter.cpp:456` | extension_bag | W | backpack native equip fallback stage=load_failed src=%d/%d | 单次操作失败 | W | keep | 审核裁决：原建议驳回；保留现状。 |
| `host_adapter.cpp:463` | extension_bag | W | backpack native equip fallback stage=set_item_failed src=%d/%d slot=%d | 单次操作失败 | W | keep | 审核裁决：原建议驳回；保留现状。 |
| `host_adapter.cpp:480` | extension_bag | W | backpack native equip stage=capacity_rollback_failed src=%d/%d slot=%d | 单次操作失败 | W | keep | 审核裁决：原建议驳回；保留现状。 |
| `host_adapter.cpp:485` | extension_bag | W | backpack native equip fallback stage=capacity_invalid src=%d/%d slot=%d capacity=%d | 装备操作 | W | keep | 审核裁决：原建议驳回；保留现状。 |
| `host_adapter.cpp:502` | extension_bag | W | backpack native equip stage=persist_rollback_failed src=%d/%d slot=%d | 单次操作失败 | W | keep | 审核裁决：原建议驳回；保留现状。 |
| `host_adapter.cpp:507` | extension_bag | W | backpack native equip fallback stage=persist_failed src=%d/%d slot=%d | 单次操作失败 | W | keep | 审核裁决：原建议驳回；保留现状。 |
| `host_adapter.cpp:521` | extension_bag | W | backpack native equip stage=source_release_rollback_failed src=%d/%d slot=%d | 单次操作失败 | W | keep | 审核裁决：原建议驳回；保留现状。 |
| `host_adapter.cpp:526` | extension_bag | W | backpack native equip fallback stage=source_release_failed src=%d/%d slot=%d | 单次操作失败 | W | keep | 审核裁决：原建议驳回；保留现状。 |
| `host_adapter.cpp:542` | extension_bag | I | backpack native equip ok target=%d src=%d/%d capacity=%d | 装备操作 | I | keep | 审核裁决：原建议驳回；保留现状。 |
| `host_adapter.cpp:552` | extension_bag | I | backpack equip probe target=%d category=%d src=%d/%d active=%d | 装备操作 | I | keep | 审核裁决：原建议驳回；保留现状。 |
| `host_adapter.cpp:557` | extension_bag | I | extension backpack button equip no empty bag slot src=%d/%d native=%d | 装备操作 | I | keep | 审核裁决：原建议驳回；保留现状。 |
| `host_adapter.cpp:568` | extension_bag | W | extension backpack button equip failed result=%d target=%d src=%d/%d | 单次操作失败 | W | keep | 审核裁决：原建议驳回；保留现状。 |
| `host_adapter.cpp:576` | extension_bag | I | extension backpack button equip target=%d src=%d/%d | 装备操作 | I | keep | 审核裁决：原建议驳回；保留现状。 |
| `input.cpp:69` | extension_bag | I | stale projected moving cleared moving=%p drop_source=%p phase=%d view=%d/%d generation=%llu/%llu/%llu root=%p projected_ro… | 拖放路由 | I | keep | 审核裁决：原建议驳回；保留现状。 |
| `input.cpp:99` | extension_bag | E | ERROR release_cleanup missing ResetMovingControl ctl=%p | 释放/清理 | W | demote | 审核批准：单次操作失败且无数据完整性风险，降为 WARN。 |
| `input.cpp:111` | extension_bag | I | release_cleanup ctl=%p flags_cleared | 释放/清理 | I | keep | 审核裁决：原建议驳回；保留现状。 |
| `lifecycle.cpp:52` | extension_bag | I | orig_event %s event=0x%llx session_projected=%d source=%d/%d inven_valid=%d digest=0x%llx nonnull=%d | 状态/对象操作 | I | keep | 审核裁决：原建议驳回；保留现状。 |
| `lifecycle.cpp:103` | extension_bag | E | ERROR original event changed physical inventory while projected session active event=0x%llx before_digest=0x%llx after_dig… | 状态/对象操作 | E | keep | 审核裁决：原建议驳回；保留现状。 |
| `lifecycle.cpp:113` | extension_bag | E | ERROR physical inventory mutation bag=%d slot=%d old=%p new=%p | 状态/对象操作 | E | keep | 审核裁决：原建议驳回；保留现状。 |
| `lifecycle.cpp:162` | extension_bag | D | projected release consumed terminal session event=0x18 token=%llu phase=%d | 释放/清理 | D | keep | 审核裁决：原建议驳回；保留现状。 |
| `lifecycle.cpp:184` | extension_bag | D | projected release intercepted event=0x18 source=%d/%d target_original=%d routed=%d | 释放/清理 | D | keep | 审核裁决：原建议驳回；保留现状。 |
| `lifecycle.cpp:205` | extension_bag | I | touch bypass event=0x%llx reason=session_not_admitted | 状态/对象操作 | I | keep | 审核驳回 delete：正文含动态对象/状态字段，保留现有 INFO。 |
| `lifecycle.cpp:234` | extension_bag | D | projected release intercepted event=0x18 reason=gamestate=%u | 释放/清理 | D | keep | 审核裁决：原建议驳回；保留现状。 |
| `lifecycle.cpp:246` | extension_bag | I | touch bypass event=0x%llx enabled=%d gamestate=%u hooked=%d | 状态/对象操作 | I | keep | 审核裁决：原建议驳回；保留现状。 |
| `lifecycle.cpp:268` | extension_bag | D | touch bounds event=0x%llx x=%lld y=%lld grid_hit=%d mode=%d selected=%d overlay=%d root_gen=%llu state_gen=%llu | 状态/对象操作 | D | keep | 审核裁决：原建议驳回；保留现状。 |
| `lifecycle.cpp:368` | extension_bag | I | equipment apply release delegated source=%d/%d result=%llu | 释放/清理 | I | keep | 审核裁决：原建议驳回；保留现状。 |
| `lifecycle.cpp:410` | extension_bag | D | extension item click bag=%d slot=%d category=%d count=%d extension_item_click | 状态/对象操作 | D | keep | 审核裁决：原建议驳回；保留现状。 |
| `lifecycle.cpp:596` | extension_bag | W | lifecycle main-menu cleanup failed; retry scheduled | 单次操作失败 | W | augment | 审核批准：按白名单在既有正文末尾追加 reason= 字段。 |
| `lifecycle.cpp:701` | extension_bag | W | prepare main-menu rejected: slot data session not admitted | 业务拒绝 | W | augment | 审核批准：按白名单在既有正文末尾追加 reason= 字段。 |
| `lifecycle.cpp:706` | extension_bag | W | prepare main-menu preflight failed | 单次操作失败 | W | augment | 审核批准：按白名单在既有正文末尾追加 reason= 字段。 |
| `lifecycle.cpp:711` | extension_bag | W | prepare main-menu blocked by active module object | 状态/对象操作 | W | augment | 审核批准：按白名单在既有正文末尾追加 reason= 字段。 |
| `native_tab_layout.cpp:92` | extension_bag | I | C18 tab layout fail-closed root=%p generation=%llu child_count=%d count_ok=%d insets=%lldx%lld screen=%lldx%lld topology=r… | 单次操作失败 | W | promote | 审核批准：单次拒绝/失败但游戏可继续，按裁决提升至 WARN。 |
| `native_tab_layout.cpp:121` | extension_bag | I | C18 tab child index=%d handle=%p type2=%d user_type=%d parent_valid=%d module_anchor=%d rect_ok=%d rect=%lld,%lld,%lld,%ll… | 状态/对象操作 | I | keep | 审核裁决：原建议驳回；保留现状。 |
| `native_tab_layout.cpp:246` | extension_bag | I | C18 native reflow restore deferred root=%p generation=%llu reason=child_count | 视图重排 | I | keep | 审核裁决：原建议驳回；保留现状。 |
| `native_tab_layout.cpp:255` | extension_bag | I | C18 native reflow restore deferred root=%p generation=%llu child=%zu reason=missing | 视图重排 | I | keep | 审核裁决：原建议驳回；保留现状。 |
| `native_tab_layout.cpp:280` | extension_bag | I | C18 native reflow restore deferred root=%p generation=%llu child=%zu | 视图重排 | I | keep | 审核裁决：原建议驳回；保留现状。 |
| `native_tab_layout.cpp:296` | extension_bag | I | C18 native reflow restore deferred root=%p generation=%llu child=%zu | 视图重排 | I | keep | 审核裁决：原建议驳回；保留现状。 |
| `native_tab_layout.cpp:304` | extension_bag | I | C18 native reflow restored root=%p generation=%llu | 视图重排 | I | keep | 审核裁决：原建议驳回；保留现状。 |
| `native_tab_layout.cpp:329` | extension_bag | I | C18 native reflow resize root=%p generation=%llu tabs=%d->%d | 视图重排 | I | keep | 审核裁决：原建议驳回；保留现状。 |
| `native_tab_layout.cpp:333` | extension_bag | I | C18 native reflow resize restore failed root=%p generation=%llu | 单次操作失败 | W | promote | 审核批准：单次拒绝/失败但游戏可继续，按裁决提升至 WARN。 |
| `native_tab_layout.cpp:355` | extension_bag | I | C18 native reflow unexpected renewed collision root=%p generation=%llu | 视图重排 | I | keep | 审核裁决：原建议驳回；保留现状。 |
| `native_tab_layout.cpp:389` | extension_bag | I | C18 native reflow aborted root=%p generation=%llu failed_child=%d | 单次操作失败 | W | promote | 审核批准：单次拒绝/失败但游戏可继续，按裁决提升至 WARN。 |
| `native_tab_layout.cpp:396` | extension_bag | I | C18 native reflow applied root=%p generation=%llu moved=%zu tabs=%d columns=%d delta=%lld | 视图重排 | I | keep | 审核裁决：原建议驳回；保留现状。 |
| `observation.cpp:201` | extension_bag | I | p5obs seq=%llu related_seq=%llu phase=%s event=0x%llx x2=%d param=%d result_known=%d result=0x%llx event_x=%lld event_y=%l… | 状态/对象操作 | I | keep | 审核裁决：原建议驳回；保留现状。 |
| `orchestrator.cpp:67` | extension_bag | E | ERROR native remove source identity unavailable context=%s bag=%d slot=%d expected=%p actual=%p ref_bag=%d ref_slot=%d han… | 状态/对象操作 | E | keep | 审核裁决：原建议驳回；保留现状。 |
| `orchestrator.cpp:101` | extension_bag | E | ERROR native remove call failed context=%s bag=%d slot=%d result=%d; checking source identity postcondition unknown | 单次操作失败 | E | keep | 审核裁决：原建议驳回；保留现状。 |
| `orchestrator.cpp:106` | extension_bag | E | ERROR native remove postcondition failed context=%s bag=%d slot=%d expected=%p actual=%p readable=%d source_handle=%u/%u i… | 单次操作失败 | E | keep | 审核裁决：原建议驳回；保留现状。 |
| `orchestrator.cpp:132` | extension_bag | I | exit seq=%llu ms=%lld phase=%s event=0x%llx param=0x%llx param2=0x%llx x=%lld y=%lld mode=%d selected=%d inspected=%d over… | 状态/对象操作 | I | keep | 审核裁决：原建议驳回；保留现状。 |
| `orchestrator.cpp:147` | extension_bag | I | module cache clear preflight rejected active object bag=%d slot=%d item=%p generation=%llu | 业务拒绝 | W | promote | 审核批准：单次拒绝/失败但游戏可继续，按裁决提升至 WARN。 |
| `orchestrator.cpp:165` | extension_bag | I | module cache clear aborted; retained object bag=%d slot=%d item=%p | 状态/对象操作 | W | promote | 审核批准：单次拒绝/失败但游戏可继续，按裁决提升至 WARN。 |
| `orchestrator.cpp:231` | extension_bag | I | payload bridge reject context=%s reason=%s validation=%s size=%d | 业务拒绝 | W | promote | 审核批准：单次拒绝/失败但游戏可继续，按裁决提升至 WARN。 |
| `orchestrator.cpp:306` | extension_bag | I | pending isolated reason=invalid_transaction_domain direction=%u src=%u/%u dst=%u/%u | 待处理事务 | I | keep | 审核裁决：原建议驳回；保留现状。 |
| `orchestrator.cpp:332` | extension_bag | I | pending isolated reason=invalid_payload payload=%s source_payload=%s src=%u/%u dst=%u/%u | 待处理事务 | I | keep | 审核裁决：原建议驳回；保留现状。 |
| `orchestrator.cpp:353` | extension_bag | I | pending recovery direction=%d action=%d | 待处理事务 | I | keep | 审核裁决：原建议驳回；保留现状。 |
| `orchestrator.cpp:375` | extension_bag | I | pending recovery: ext2orig target rollback not confirmed; retain pending slot=%d | 待处理事务 | I | keep | 审核裁决：原建议驳回；保留现状。 |
| `orchestrator.cpp:409` | extension_bag | I | pending recovery: target payload not confirmed; retain pending | 待处理事务 | I | augment | 审核批准：按白名单在既有正文末尾追加 reason= 字段。 |
| `orchestrator.cpp:421` | extension_bag | I | pending recovery: extension->original retry retained | 待处理事务 | I | augment | 审核批准：按白名单在既有正文末尾追加 reason= 字段。 |
| `orchestrator.cpp:439` | extension_bag | I | pending recovery: source serialization rejected | 业务拒绝 | W | promote | 审核批准：单次拒绝/失败但游戏可继续，按裁决提升至 WARN。 |
| `orchestrator.cpp:456` | extension_bag | I | pending recovery: source removal not confirmed; retain pending | 待处理事务 | I | augment | 审核批准：按白名单在既有正文末尾追加 reason= 字段。 |
| `orchestrator.cpp:460` | extension_bag | I | pending recovery: source identity mismatch; retain pending | 待处理事务 | I | augment | 审核批准：按白名单在既有正文末尾追加 reason= 字段。 |
| `orchestrator.cpp:493` | extension_bag | E | ERROR pending recovery could not restore module projection bag=%d host=%d | 待处理事务 | E | keep | 审核裁决：原建议驳回；保留现状。 |
| `orchestrator.cpp:520` | extension_bag | W | materialize failed category=%u count=%u size=%d head=%s retry=%s | 单次操作失败 | W | keep | 审核裁决：原建议驳回；保留现状。 |
| `ownership.cpp:104` | extension_bag | I | deferred free entry rebind item=%p old_handle=%u new_handle=%u generation=%llu | 状态/对象操作 | I | keep | 审核裁决：原建议驳回；保留现状。 |
| `ownership.cpp:117` | extension_bag | I | deferred free entry rebind item=%p old_handle=%u new_handle=%u generation=%llu | 状态/对象操作 | I | keep | 审核裁决：原建议驳回；保留现状。 |
| `ownership.cpp:126` | extension_bag | E | ERROR deferred free backlog full pending=%zu capacity=%zu item=%p handle=%u generation=%llu | 待处理事务 | E | keep | 审核裁决：原建议驳回；保留现状。 |
| `ownership.cpp:134` | extension_bag | I | deferred free queue full; custody retained item=%p handle=%u generation=%llu | 状态/对象操作 | I | keep | 审核裁决：原建议驳回；保留现状。 |
| `ownership.cpp:143` | extension_bag | I | deferred free enqueue item=%p handle=%u generation=%llu | 状态/对象操作 | I | keep | 审核裁决：原建议驳回；保留现状。 |
| `ownership.cpp:155` | extension_bag | I | deferred free skipped game-owned item=%p | 状态/对象操作 | I | keep | 审核裁决：原建议驳回；保留现状。 |
| `ownership.cpp:220` | extension_bag | W | deferred free drain skipped null item | 状态/对象操作 | W | keep | 审核裁决：原建议驳回；保留现状。 |
| `ownership.cpp:236` | extension_bag | D | deferred free judge item=%p handle=%u generation=%llu residence_met=%d referenced=%d reason=%s | 状态/对象操作 | D | keep | 审核裁决：原建议驳回；保留现状。 |
| `ownership.cpp:247` | extension_bag | I | deferred free dropped game-owned item=%p handle=%u generation=%llu | 拖放路由 | I | keep | 审核裁决：原建议驳回；保留现状。 |
| `ownership.cpp:253` | extension_bag | D | deferred free freed item=%p handle=%u generation=%llu | 状态/对象操作 | D | keep | 审核裁决：原建议驳回；保留现状。 |
| `ownership.cpp:258` | extension_bag | E | ERROR deferred free failed item=%p handle=%u generation=%llu | 单次操作失败 | E | keep | 审核裁决：原建议驳回；保留现状。 |
| `ownership.cpp:268` | extension_bag | E | ERROR deferred free queue size exceeded capacity size=%zu capacity=%zu | 状态/对象操作 | E | keep | 审核裁决：原建议驳回；保留现状。 |
| `ownership.cpp:278` | extension_bag | E | ERROR deferred free custody size exceeded capacity size=%zu capacity=%zu | 状态/对象操作 | E | keep | 审核裁决：原建议驳回；保留现状。 |
| `ownership.cpp:289` | extension_bag | I | deferred free drain freed=%zu queue=%zu/%zu custody=%zu/%zu deferred free retained queue=%zu/%zu custody=%zu/%zu | 状态/对象操作 | I | keep | 审核裁决：原建议驳回；保留现状。 |
| `ownership.cpp:292` | extension_bag | D | deferred free retained queue=%zu/%zu custody=%zu/%zu | 状态/对象操作 | D | keep | 审核裁决：原建议驳回；保留现状。 |
| `ownership.cpp:299` | extension_bag | I | deferred free queue cleared queue=%zu custody=%zu | 状态/对象操作 | I | keep | 审核驳回 delete：正文含动态对象/状态字段，保留现有 INFO。 |
| `ownership.cpp:342` | extension_bag | I | ownership counters context=%s allocated=%u released=%u handed_over=%u suppressed=%u | 所有权审计 | I | keep | 审核裁决：原建议驳回；保留现状。 |
| `ownership.cpp:372` | extension_bag | I | ownership release refused context=%s invalid_handle=%u item=%p | 所有权审计 | W | promote | 审核批准：单次拒绝/失败但游戏可继续，按裁决提升至 WARN。 |
| `ownership.cpp:382` | extension_bag | E | ERROR ownership release of game-owned item failed context=%s handle=%u item=%p | 单次操作失败 | E | keep | 审核裁决：原建议驳回；保留现状。 |
| `ownership.cpp:386` | extension_bag | I | ownership release skipped game-owned context=%s handle=%u item=%p | 所有权审计 | I | keep | 审核裁决：原建议驳回；保留现状。 |
| `ownership.cpp:394` | extension_bag | E | ERROR ownership release reservation failed context=%s handle=%u | 单次操作失败 | E | keep | 审核裁决：原建议驳回；保留现状。 |
| `ownership.cpp:406` | extension_bag | E | ERROR ownership release reservation rollback failed context=%s handle=%u item=%p | 单次操作失败 | E | keep | 审核裁决：原建议驳回；保留现状。 |
| `ownership.cpp:413` | extension_bag | E | ERROR native item free failed context=%s handle=%u item=%p; retry deferred | 单次操作失败 | E | keep | 审核裁决：原建议驳回；保留现状。 |
| `ownership.cpp:419` | extension_bag | E | ERROR ownership release reservation rollback failed context=%s handle=%u item=%p | 单次操作失败 | E | keep | 审核裁决：原建议驳回；保留现状。 |
| `ownership.cpp:423` | extension_bag | E | ERROR native item free retry queue full context=%s handle=%u item=%p | 状态/对象操作 | E | keep | 审核裁决：原建议驳回；保留现状。 |
| `ownership.cpp:431` | extension_bag | E | ERROR ownership release commit failed context=%s handle=%u | 单次操作失败 | E | keep | 审核裁决：原建议驳回；保留现状。 |
| `ownership.cpp:437` | extension_bag | I | ownership release deferred context=%s touch_window=%d handle=%u | 所有权审计 | I | keep | 审核裁决：原建议驳回；保留现状。 |
| `ownership.cpp:452` | extension_bag | I | ownership allocate reject; temp freed context=%s | 业务拒绝 | W | promote | 审核批准：单次拒绝/失败但游戏可继续，按裁决提升至 WARN。 |
| `ownership.cpp:463` | extension_bag | I | ownership handover reject; custody retained context=%s item=%p ERROR ownership handover reject; custody unavailable  | 业务拒绝 | W | promote | 审核批准：单次拒绝/失败但游戏可继续，按裁决提升至 WARN。 |
| `ownership.cpp:466` | extension_bag | E | ERROR ownership handover reject; custody unavailable context=%s handle=%u item=%p | 业务拒绝 | E | keep | 审核裁决：原建议驳回；保留现状。 |
| `ownership.cpp:479` | extension_bag | I | ownership release reject; custody retained context=%s item=%p ERROR ownership release reject; custody unavailable  | 业务拒绝 | W | promote | 审核批准：单次拒绝/失败但游戏可继续，按裁决提升至 WARN。 |
| `ownership.cpp:482` | extension_bag | E | ERROR ownership release reject; custody unavailable context=%s handle=%u item=%p | 业务拒绝 | E | keep | 审核裁决：原建议驳回；保留现状。 |
| `ownership.cpp:515` | extension_bag | I | module object replacement rejected while confirm-use active context=%s bag=%d slot=%d item=%p generation=%llu | 业务拒绝 | W | promote | 审核批准：单次拒绝/失败但游戏可继续，按裁决提升至 WARN。 |
| `ownership.cpp:531` | extension_bag | I | module object release rejected while pending release context=%s bag=%d slot=%d item=%p | 业务拒绝 | W | promote | 审核批准：单次拒绝/失败但游戏可继续，按裁决提升至 WARN。 |
| `ownership.cpp:542` | extension_bag | I | module object release rejected while confirm-use active context=%s bag=%d slot=%d item=%p | 业务拒绝 | W | promote | 审核批准：单次拒绝/失败但游戏可继续，按裁决提升至 WARN。 |
| `ownership.cpp:587` | extension_bag | E | ERROR module use finish token mismatch invalid slot active=-1 pending_release=-1 generation=0 item=%p handle=0 owner_threa… | 释放/清理 | E | keep | 审核裁决：原建议驳回；保留现状。 |
| `ownership.cpp:600` | extension_bag | E | ERROR module use finish token mismatch bag=%d slot=%d active=%d pending_release=%d generation=%llu item=%p handle=%u owner… | 释放/清理 | E | keep | 审核裁决：原建议驳回；保留现状。 |
| `ownership.cpp:649` | extension_bag | E | ERROR module use abort token mismatch; slot may remain active invalid slot active=-1 pending_release=-1 generation=0 item=… | 释放/清理 | E | keep | 审核裁决：原建议驳回；保留现状。 |
| `ownership.cpp:662` | extension_bag | E | ERROR module use abort token mismatch; slot may remain active bag=%d slot=%d active=%d pending_release=%d generation=%llu … | 释放/清理 | E | keep | 审核裁决：原建议驳回；保留现状。 |
| `ownership.cpp:692` | extension_bag | E | ERROR module use abort isolation bag=%d slot=%d item=%p pending_release=%d | 释放/清理 | E | keep | 审核裁决：原建议驳回；保留现状。 |
| `ownership.cpp:717` | extension_bag | E | ERROR module-use release retained bag=%d slot=%d handle=%u item=%p | 释放/清理 | E | keep | 审核裁决：原建议驳回；保留现状。 |
| `ownership.cpp:724` | extension_bag | E | ERROR module-use native free failed bag=%d slot=%d handle=%u item=%p; retry deferred | 单次操作失败 | E | keep | 审核裁决：原建议驳回；保留现状。 |
| `ownership.cpp:729` | extension_bag | E | ERROR module-use native free retry queue full bag=%d slot=%d handle=%u item=%p | 状态/对象操作 | E | keep | 审核裁决：原建议驳回；保留现状。 |
| `ownership.cpp:745` | extension_bag | E | ERROR module use release finalize mismatch bag=%d slot=%d item=%p | 释放/清理 | E | keep | 审核裁决：原建议驳回；保留现状。 |
| `ownership.cpp:754` | extension_bag | E | ERROR module use release ledger mismatch bag=%d slot=%d handle=%u | 释放/清理 | E | keep | 审核裁决：原建议驳回；保留现状。 |
| `ownership.cpp:774` | extension_bag | E | ERROR module free rejected active token with empty slot bag=%d slot=%d | 业务拒绝 | E | keep | 审核裁决：原建议驳回；保留现状。 |
| `ownership.cpp:783` | extension_bag | I | module free rejected without real token bag=%d slot=%d item=%p | 业务拒绝 | W | promote | 审核批准：单次拒绝/失败但游戏可继续，按裁决提升至 WARN。 |
| `projection.cpp:136` | extension_bag | W | module materialize rejected stale object bag=%d slot=%d item=%p | 业务拒绝 | W | keep | 审核裁决：原建议驳回；保留现状。 |
| `projection.cpp:147` | extension_bag | E | module materialize rejected item handle registration bag=%d slot=%d | 业务拒绝 | W | demote | 审核批准：单次操作失败且无数据完整性风险，降为 WARN。 |
| `projection.cpp:155` | extension_bag | W | module materialize rejected ownership ledger exhausted bag=%d slot=%d | 业务拒绝 | W | keep | 审核裁决：原建议驳回；保留现状。 |
| `projection.cpp:164` | extension_bag | W | module materialize replacement rejected bag=%d slot=%d | 业务拒绝 | W | keep | 审核裁决：原建议驳回；保留现状。 |
| `projection.cpp:188` | extension_bag | D | save gate: restoring projected view before SAVE_SaveInventory | 状态/对象操作 | D | keep | 审核裁决：原建议驳回；保留现状。 |
| `projection.cpp:259` | extension_bag | D | draw gate: suppressed unknown item=%p | 状态/对象操作 | D | keep | 审核裁决：原建议驳回；保留现状。 |
| `projection.cpp:289` | extension_bag | D | drop gate: conflicting deferred target old=%d/%d new=%d | 拖放路由 | D | keep | 审核裁决：原建议驳回；保留现状。 |
| `projection.cpp:297` | extension_bag | D | drop deferred ext->orig source=%d/%d bag=%d | 拖放路由 | D | keep | 审核裁决：原建议驳回；保留现状。 |
| `projection.cpp:315` | extension_bag | D | drop gate: source/session mismatch src=%d/%d bag=%d phase=%d | 拖放路由 | D | keep | 审核裁决：原建议驳回；保留现状。 |
| `projection.cpp:345` | extension_bag | W | module view reject install: original window bag=%d (task bag reserved) | 业务拒绝 | W | keep | 审核裁决：原建议驳回；保留现状。 |
| `projection.cpp:373` | extension_bag | E | ERROR module view rollback control clear failed bag=%d slot=%d control=%p | 单次操作失败 | W | demote | 审核批准：单次操作失败且无数据完整性风险，降为 WARN。 |
| `projection.cpp:379` | extension_bag | I | module view rollback invalidated root saved=%p live=%p | 状态/对象操作 | I | keep | 审核裁决：原建议驳回；保留现状。 |
| `projection.cpp:386` | extension_bag | E | ERROR module view rollback ownership return failed bag=%d handle=%u | 单次操作失败 | W | demote | 审核批准：单次操作失败且无数据完整性风险，降为 WARN。 |
| `projection.cpp:398` | extension_bag | E | ERROR module view install rejected bag=%d reason=%s | 业务拒绝 | W | demote | 审核批准：单次操作失败且无数据完整性风险，降为 WARN。 |
| `projection.cpp:473` | extension_bag | D | module view installed bag=%d window=%d capacity=%d | 视图/控件安装 | D | keep | 审核裁决：原建议驳回；保留现状。 |
| `projection.cpp:489` | extension_bag | I | module view root invalidated bag=%d saved=%p live=%p; reinstalling | 状态/对象操作 | I | keep | 审核裁决：原建议驳回；保留现状。 |
| `projection.cpp:494` | extension_bag | E | ERROR module view root reinstallation failed bag=%d | 单次操作失败 | W | demote | 审核批准：单次操作失败且无数据完整性风险，降为 WARN。 |
| `projection.cpp:513` | extension_bag | E | ERROR projection refresh child disappeared bag=%d slot=%d | 状态/对象操作 | W | demote | 审核批准：单次操作失败且无数据完整性风险，降为 WARN。 |
| `projection.cpp:520` | extension_bag | E | ERROR projection refresh control data missing bag=%d slot=%d control=%p | 状态/对象操作 | W | demote | 审核批准：单次操作失败且无数据完整性风险，降为 WARN。 |
| `projection.cpp:528` | extension_bag | E | ERROR projection refresh binding failed bag=%d slot=%d control=%p item=%p | 单次操作失败 | W | demote | 审核批准：单次操作失败且无数据完整性风险，降为 WARN。 |
| `projection.cpp:533` | extension_bag | I | projection post-refresh slot=%d current=%p item=%p | 状态/对象操作 | I | keep | 审核裁决：原建议驳回；保留现状。 |
| `projection.cpp:765` | extension_bag | D | save panel enter wrapper invoked world=%d hook=%d | 状态/对象操作 | D | keep | 审核裁决：原建议驳回；保留现状。 |
| `render.cpp:162` | extension_bag | I | tab layout rendered cap reached count=%d | 状态/对象操作 | D | demote | 审核批准：高频/绘制回调或处理忙分支，降为 DEBUG 且保留区分字段。 |
| `render.cpp:380` | extension_bag | I | draw-end state edge previous=%d current=%d gen=%llu | 状态/对象操作 | D | demote | 审核批准：高频/绘制回调或处理忙分支，降为 DEBUG 且保留区分字段。 |
| `tabs.cpp:31` | extension_bag | E | ERROR tab bag item retirement failed bag=%d item=%p | 单次操作失败 | E | keep | 审核裁决：原建议驳回；保留现状。 |
| `tabs.cpp:41` | extension_bag | E | ERROR tab bag item replacement deferred free failed bag=%d item=%p type=%u | 单次操作失败 | E | keep | 审核裁决：原建议驳回；保留现状。 |
| `tabs.cpp:53` | extension_bag | E | ERROR unmarked tab bag item retirement failed bag=%d item=%p type=%u | 单次操作失败 | E | keep | 审核裁决：原建议驳回；保留现状。 |
| `tabs_ui.cpp:77` | extension_bag | I | extension tab proc busy event=0x%llx | 状态/对象操作 | D | demote | 审核批准：高频/绘制回调或处理忙分支，降为 DEBUG 且保留区分字段。 |
| `tabs_ui.cpp:98` | extension_bag | I | extension tab proc busy event=0x%llx | 状态/对象操作 | D | demote | 审核批准：高频/绘制回调或处理忙分支，降为 DEBUG 且保留区分字段。 |
| `tabs_ui.cpp:112` | extension_bag | I | cross tab reject reason=target_tab_index_invalid index=%d | 业务拒绝 | W | promote | 审核批准：单次拒绝/失败但游戏可继续，按裁决提升至 WARN。 |
| `tabs_ui.cpp:149` | extension_bag | I | extension tab drop extension move result=%d index=%d category=%d | 拖放路由 | I | keep | 审核裁决：原建议驳回；保留现状。 |
| `tabs_ui.cpp:161` | extension_bag | I | extension tab drop original move result=%d index=%d category=%d equipped=%d | 拖放路由 | I | keep | 审核裁决：原建议驳回；保留现状。 |
| `tabs_ui.cpp:203` | extension_bag | I | extension tab delete failed control=%p | 单次操作失败 | W | promote | 审核批准：单次拒绝/失败但游戏可继续，按裁决提升至 WARN。 |
| `tabs_ui.cpp:278` | extension_bag | I | extension tab create failed index=%d tab create | 单次操作失败 | W | promote | 审核批准：单次拒绝/失败但游戏可继续，按裁决提升至 WARN。 |
| `tabs_ui.cpp:283` | extension_bag | I | extension tab setup failed reason=%s index=%d control=%p tab setup | 单次操作失败 | W | promote | 审核批准：单次拒绝/失败但游戏可继续，按裁决提升至 WARN。 |
| `tabs_ui.cpp:314` | extension_bag | I | tab layout rendered cap reached count=%d | 状态/对象操作 | D | demote | 审核批准：高频/绘制回调或处理忙分支，降为 DEBUG 且保留区分字段。 |
| `tabs_ui.cpp:329` | extension_bag | I | extension tab anchor ensure failed index=%d code=%d root=%p | 单次操作失败 | W | promote | 审核批准：单次拒绝/失败但游戏可继续，按裁决提升至 WARN。 |
| `tabs_ui.cpp:337` | extension_bag | D | extension tab re-anchored index=%d root=%p | 状态/对象操作 | D | keep | 审核裁决：原建议驳回；保留现状。 |
| `tabs_ui.cpp:378` | extension_bag | I | extension tabs installed generation=%llu root=%p complete=%d created=%d | 视图/控件安装 | I | keep | 审核裁决：原建议驳回；保留现状。 |
| `tabs_ui.cpp:389` | extension_bag | I | extension tab references invalidated reason=root_not_live saved=%p live=%p | 状态/对象操作 | I | keep | 审核裁决：原建议驳回；保留现状。 |
| `tabs_ui.cpp:432` | extension_bag | I | extension desc unequip armed bag=%d | 装备操作 | I | keep | 审核裁决：原建议驳回；保留现状。 |
| `tabs_ui.cpp:486` | extension_bag | I | extension tab reject reason=projection_host_unavailable source_original_bag=%d target=%d | 业务拒绝 | W | promote | 审核批准：单次拒绝/失败但游戏可继续，按裁决提升至 WARN。 |
| `tabs_ui.cpp:498` | extension_bag | I | extension tab reject reason=projection_host_unavailable source_original_bag=%d target=%d host=%d | 业务拒绝 | W | promote | 审核批准：单次拒绝/失败但游戏可继续，按裁决提升至 WARN。 |
| `tabs_ui.cpp:509` | extension_bag | I | extension tab reject reason=projection_host_unavailable source_original_bag=%d target=%d host=%d | 业务拒绝 | W | promote | 审核批准：单次拒绝/失败但游戏可继续，按裁决提升至 WARN。 |
| `tabs_ui.cpp:516` | extension_bag | I | extension tab selected bag=%d original_bag=%d projection_host=%d | 状态/对象操作 | I | keep | 审核裁决：原建议驳回；保留现状。 |
| `tabs_ui.cpp:528` | extension_bag | I | extension tab selected bag=%d original_bag=%d | 状态/对象操作 | I | keep | 审核裁决：原建议驳回；保留现状。 |
| `tabs_ui.cpp:532` | extension_bag | I | extension tab reject original window reason=invalid_transaction_domain bag=%d | 业务拒绝 | W | promote | 审核批准：单次拒绝/失败但游戏可继续，按裁决提升至 WARN。 |
| `tabs_ui.cpp:544` | extension_bag | I | extension tab switched bag=%d | 状态/对象操作 | I | keep | 审核裁决：原建议驳回；保留现状。 |
| `tabs_ui.cpp:575` | extension_bag | E | extension bag description rejected missing control item bag=%d control=%p | 业务拒绝 | W | demote | 审核批准：单次操作失败且无数据完整性风险，降为 WARN。 |
| `tabs_ui.cpp:579` | extension_bag | E | extension bag description rejected state snapshot bag=%d current_bag=%d desc_type=%d | 业务拒绝 | W | demote | 审核批准：单次操作失败且无数据完整性风险，降为 WARN。 |
| `tabs_ui.cpp:594` | extension_bag | E | extension bag description UI operation failed bag=%d bag_set=%d desc_set=%d made=%d bag_restored=%d desc_restored=%d | 单次操作失败 | W | demote | 审核批准：单次操作失败且无数据完整性风险，降为 WARN。 |
| `tabs_ui.cpp:635` | extension_bag | E | extension bag description UI operation failed bag=%d bag_set=%d desc_set=%d made=%d bag_restored=%d desc_restored=%d | 单次操作失败 | W | demote | 审核批准：单次操作失败且无数据完整性风险，降为 WARN。 |
| `tabs_ui.cpp:645` | extension_bag | W | extension bag description discarded after view changed bag=%d generation=%llu | 状态/对象操作 | W | keep | 审核裁决：原建议驳回；保留现状。 |
| `tabs_ui.cpp:659` | extension_bag | E | extension bag description close failed bag=%d | 单次操作失败 | W | demote | 审核批准：单次操作失败且无数据完整性风险，降为 WARN。 |
| `tabs_ui.cpp:669` | extension_bag | E | extension bag description rejected UI adapter unavailable bag=%d control=%p | 业务拒绝 | W | demote | 审核批准：单次操作失败且无数据完整性风险，降为 WARN。 |
| `tabs_ui.cpp:673` | extension_bag | I | extension bag desc opened bag=%d | 状态/对象操作 | I | keep | 审核裁决：原建议驳回；保留现状。 |
| `tabs_ui.cpp:693` | extension_bag | D | extension tab pending bag=%d generation=%llu | 待处理事务 | D | keep | 审核裁决：原建议驳回；保留现状。 |

## 缺失日志建议

| file + 函数 | 事件 | 目标等级 | 必填字段 | 理由 |
|---|---|---|---|---|
| `core/orchestrator.cpp` + pending recovery 最终化分支 | 延迟事务最终 committed/rolled_back | I | `direction=`、`source_bag=`/`slot=`、`target_bag=`、`result=` | 当前日志记录恢复尝试和 retain 分支，但成功/回滚终态缺少单一可回放结果。 |
| `core/ownership.cpp` + ownership balance/counter 更新 | 每次所有权记账结果/失衡 | D | `context=`、`item=`/`handle=`、`allocated=`、`released=`、`handed_over=`、`result=` | 现有计数日志汇总状态，缺少可定位到具体对象的余额不一致证据；逐对象记录应为 D。 |
| `core/host_adapter.cpp` + unequip/adopt 容量拒绝分支 | 容量拒绝最终结果 | W | `bag=`、`slot=`、`item=`、`target_bag=`、`reason=capacity` | 容量不足会拒绝单次操作但游戏继续，应确保终态记录明确拒绝原因和两端对象。 |

## 统计

- 现有日志：210 条。
- disposition：keep 85, demote 3, promote 38, delete 8, merge 0, augment 76.
- 补充建议：3 条。

## Top 发现

1. `VIRTBAG_LOG` 默认实际为 INFO；业务拒绝/失败不能因宏名不显等级而落成 INFO，应改为 WARN 并带 `reason=`。
2. ownership/free/custody 明细记录对象级逐次操作，适合 D 级调试审计；WARN/E 仅用于实际释放失败或状态不一致。
3. 延迟事务日志覆盖隔离、重试及 retain，但需要补充唯一的 committed/rolled_back 终态以支持状态回放。
4. 拖放和控件拒绝需保留 source `bag/slot` 与 `target_bag`；仅坐标或通用失败文案不足以区分对象拒绝。
5. native reflow 与 tab layout 有多条恢复/应用结果日志；失败记录应包含 `reason=`，成功状态只在状态改变或边沿发生时记录，避免渲染循环 INFO。

## 审核裁决

promote/demote 按清单执行；delete 全部驳回，按下列替代处置；augment 仅执行白名单；其余均 keep。批准行号均为源文件行号（实施后行号可能因插入变动）。

- **promote（I→W）39 条**：`drop_routing.cpp:383`；`host_adapter.cpp:88,95,102,118,135,227,241,280,285,302,343,396,415,435`；`native_tab_layout.cpp:92,333,389`；`orchestrator.cpp:147,165,231,439`；`ownership.cpp:372,452,463,479,515,531,542,783`；`tabs_ui.cpp:112,203,278,283,329,486,498,509,532`。
- **demote（I→D）5 条**：`render.cpp:162,380`；`tabs_ui.cpp:77,98,314`。
- **E→W 16 条**：`host_adapter.cpp:61`；`input.cpp:99`；`projection.cpp:147,373,386,398,494,513,520,528`；`tabs_ui.cpp:575,579,594,635,659,669`。其余 E 保留。
- **augment 白名单 13 条**：reason=`deferred_domain/session_drop_rejected/no_destination/no_active_drag/target_miss`：`drop_routing.cpp:107,124,234,237,313`；reason=`cleanup_failed_retry/session_not_admitted/preflight_failed/active_module_object`：`lifecycle.cpp:596,701,706,711`；reason 与可用 slot：`orchestrator.cpp:409,421,456,460`。
- **delete 驳回**：`lifecycle.cpp:205`、`ownership.cpp:299` keep；`orchestrator.cpp:409,421,456,460` 保留并 augment，函数内有 pending 的 slot 信息。
- **缺失建议**：采纳 `core/orchestrator.cpp` pending recovery 最终 INFO（每个事务一次，`direction=`、`src=`、`dst=`、`result=`）；驳回逐对象 ownership balance D 与 host_adapter 容量拒绝 W，避免与现有 `ownership.cpp:236/253`、`host_adapter` 的 `stage=` 日志重复。

**裁决后处置统计**：现有日志 210 条；keep 137，demote 21（其中 E→W 16），promote 39，delete 0，merge 0，augment 13。新增 INFO 正文：`pending recovery final direction=%u src=%d/%d dst=%d/%d result=%s`。
