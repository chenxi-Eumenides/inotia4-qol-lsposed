# 日志审计清单：批次 2（extension_bag 外围）

依据 `docs/reference/logging-standards.md`、`docs/reference/logging.md` 与批次 1 清单格式。范围：`transaction/`、`public/`、`api/`、`persistence/`、`runtime/`、`assembly/`、`usecases/`；排除 tests、Kotlin、core、ui。等级按真实宏展开：`VIRTBAG_LOG`=I，后缀 `_WARN`=W、`_ERROR`=E、`_DEBUG`=D；`QOL_LOG_*` 由宏名确定。

## 现有日志

| file:line | domain | level_now | message 摘要 | event_class | level_target | disposition | rationale |
|---|---|---|---|---|---|---|---|
| `transaction/ext_ext.cpp:33` | extension_bag | W | txn reject extension swap token active src={arg}/{arg} active={arg} pending={arg} dst={arg}/{arg} active={arg} pending={arg} | 失败/拒绝 | W | keep | 正文已有原因/对象或事务字段，无需重复 augment。 |
| `transaction/ext_ext.cpp:64` | extension_bag | W | txn reject extension swap stale endpoint src={arg}/{arg} dst={arg}/{arg} | 失败/拒绝 | W | keep | 正文已有原因/对象或事务字段，无需重复 augment。 |
| `transaction/ext_ext.cpp:130` | extension_bag | W | txn reject extension swap endpoint materialization failed src={arg}/{arg} dst={arg}/{arg} | 失败/拒绝 | W | keep | 正文已有原因/对象或事务字段，无需重复 augment。 |
| `transaction/ext_ext.cpp:198` | extension_bag | I | txn committed id={arg} extension swap {arg}/{arg} <-> {arg}/{arg} generation={arg}/{arg} | 过程/诊断 | D | demote | 内部阶段/过程诊断不属于有限生命周期状态变化，应为 D。 |
| `transaction/ext_ext.cpp:265` | extension_bag | W | cross move reject extension->extension src={arg}/{arg} dst_bag={arg} requested_dst={arg} | 失败/拒绝 | W | keep | 正文已有原因/对象或事务字段，无需重复 augment。 |
| `transaction/ext_ext.cpp:271` | extension_bag | W | cross move reject extension->extension source empty={arg}/{arg} | 失败/拒绝 | cross move reject extension->extension source empty={arg}/{arg} | keep | 正文已有 src/dst/size/target/status/原因或对象定位字段，按裁决不追加同义 reason。 |
| `transaction/ext_ext.cpp:279` | extension_bag | W | cross move reject extension->extension reason={arg} src={arg}/{arg} size={arg} | 失败/拒绝 | W | keep | 正文已有原因/对象或事务字段，无需重复 augment。 |
| `transaction/ext_ext.cpp:307` | extension_bag | W | cross move reject extension->extension merge count mode unavailable src={arg}/{arg} dst={arg}/{arg} | 失败/拒绝 | W | keep | 正文已有原因/对象或事务字段，无需重复 augment。 |
| `transaction/ext_ext.cpp:319` | extension_bag | W | extension merge count mode not ready; using legacy view src={arg}/{arg} dst={arg}/{arg} | 状态变更/结果 | W | keep | 正文已有原因/对象或事务字段，无需重复 augment。 |
| `transaction/ext_ext.cpp:323` | extension_bag | W | cross move reject extension->extension merge count mode read failed src={arg}/{arg} dst={arg}/{arg} code={arg} value={arg} | 失败/拒绝 | W | keep | 正文已有原因/对象或事务字段，无需重复 augment。 |
| `transaction/ext_ext.cpp:342` | extension_bag | W | cross move reject extension->extension merge count facade unavailable src={arg}/{arg} dst={arg}/{arg} | 失败/拒绝 | W | keep | 正文已有原因/对象或事务字段，无需重复 augment。 |
| `transaction/ext_ext.cpp:361` | extension_bag | W | cross move reject extension->extension merge count snapshot failed src={arg}/{arg} dst={arg}/{arg} existing={arg} source={arg} limit={arg} mode={arg} | 失败/拒绝 | W | keep | 正文已有原因/对象或事务字段，无需重复 augment。 |
| `transaction/ext_ext.cpp:377` | extension_bag | W | cross move reject extension->extension merge count gate src={arg}/{arg} dst={arg}/{arg} | 失败/拒绝 | W | keep | 正文已有原因/对象或事务字段，无需重复 augment。 |
| `transaction/ext_ext.cpp:389` | extension_bag | W | cross move reject extension->extension invalid destination={arg} capacity={arg} | 失败/拒绝 | cross move reject extension->extension invalid destination={arg} capacity={arg} | keep | 正文已有 src/dst/size/target/status/原因或对象定位字段，按裁决不追加同义 reason。 |
| `transaction/ext_ext.cpp:403` | extension_bag | W | cross move reject extension->extension swap failed={arg}/{arg} -> {arg}/{arg} | 失败/拒绝 | cross move reject extension->extension swap failed={arg}/{arg} -> {arg}/{arg} | keep | 正文已有 src/dst/size/target/status/原因或对象定位字段，按裁决不追加同义 reason。 |
| `transaction/ext_ext.cpp:407` | extension_bag | W | cross move reject extension->extension destination unavailable={arg}/{arg} | 失败/拒绝 | cross move reject extension->extension destination unavailable={arg}/{arg} | keep | 正文已有 src/dst/size/target/status/原因或对象定位字段，按裁决不追加同义 reason。 |
| `transaction/ext_ext.cpp:428` | extension_bag | W | cross move reject extension->extension destination full={arg} capacity={arg} | 失败/拒绝 | cross move reject extension->extension destination full={arg} capacity={arg} | keep | 正文已有 src/dst/size/target/status/原因或对象定位字段，按裁决不追加同义 reason。 |
| `transaction/ext_ext.cpp:447` | extension_bag | I | txn prepared id={arg} extension merge {arg}/{arg} -> {arg}/{arg} count={arg}+{arg}={arg} | 状态变更/结果 | I | keep | 有限状态变更/结果日志；保留。 |
| `transaction/ext_ext.cpp:454` | extension_bag | W | txn reject id={arg} extension merge source object in use | 失败/拒绝 | W | keep | 正文已有原因/对象或事务字段，无需重复 augment。 |
| `transaction/ext_ext.cpp:473` | extension_bag | W | txn committed but destination materialize failed id={arg} dst={arg}/{arg} | 失败/拒绝 | W | keep | 正文已有原因/对象或事务字段，无需重复 augment。 |
| `transaction/ext_ext.cpp:478` | extension_bag | E | ERROR ext2ext destination view rollback failed id={arg} dst={arg}/{arg} | 失败/拒绝 | W | demote | 局部/单次操作失败，游戏可继续，E 应降为 W。 |
| `transaction/ext_ext.cpp:486` | extension_bag | W | txn destination view borrow failed id={arg} dst={arg}/{arg} | 失败/拒绝 | W | keep | 正文已有原因/对象或事务字段，无需重复 augment。 |
| `transaction/ext_ext.cpp:491` | extension_bag | E | ERROR ext2ext destination view rollback failed id={arg} dst={arg}/{arg} | 失败/拒绝 | W | demote | 局部/单次操作失败，游戏可继续，E 应降为 W。 |
| `transaction/ext_ext.cpp:502` | extension_bag | E | ERROR ext2ext destination view rollback failed id={arg} dst={arg}/{arg} | 失败/拒绝 | W | demote | 局部/单次操作失败，游戏可继续，E 应降为 W。 |
| `transaction/ext_ext.cpp:506` | extension_bag | W | txn abort id={arg} extension merge source release rejected | 失败/拒绝 | W | keep | 正文已有原因/对象或事务字段，无需重复 augment。 |
| `transaction/ext_ext.cpp:511` | extension_bag | I | txn committed id={arg} extension->extension {arg}/{arg} -> {arg}/{arg} cat={arg} count={arg} | 状态变更/结果 | I | keep | 有限状态变更/结果日志；保留。 |
| `transaction/ext_orig.cpp:54` | extension_bag | W | cross move reject original->extension no assignable slot bag={arg} capacity={arg} occupied={arg} | 失败/拒绝 | W | keep | 正文已有原因/对象或事务字段，无需重复 augment。 |
| `transaction/ext_orig.cpp:84` | extension_bag | W | cross move reject original->extension source={arg}/{arg} dst={arg} | 失败/拒绝 | W | keep | 正文已有原因/对象或事务字段，无需重复 augment。 |
| `transaction/ext_orig.cpp:101` | extension_bag | W | cross move reject original->extension stale source={arg}/{arg} | 失败/拒绝 | cross move reject original->extension stale source={arg}/{arg} | keep | 正文已有 src/dst/size/target/status/原因或对象定位字段，按裁决不追加同义 reason。 |
| `transaction/ext_orig.cpp:113` | extension_bag | W | cross move reject original->extension source serialize failed={arg}/{arg} | 失败/拒绝 | cross move reject original->extension source serialize failed={arg}/{arg} | keep | 正文已有 src/dst/size/target/status/原因或对象定位字段，按裁决不追加同义 reason。 |
| `transaction/ext_orig.cpp:128` | extension_bag | W | cross move reject original->extension destination full dst={arg} | 失败/拒绝 | W | keep | 正文已有原因/对象或事务字段，无需重复 augment。 |
| `transaction/ext_orig.cpp:135` | extension_bag | E | cross move reject original->extension inventory.action unavailable | 失败/拒绝 | W | demote | 局部/单次操作失败，游戏可继续，E 应降为 W。 |
| `transaction/ext_orig.cpp:149` | extension_bag | W | cross move rejected original->extension status={arg} source={arg}/{arg} dst={arg}/{arg} count={arg} | 失败/拒绝 | W | keep | 正文已有原因/对象或事务字段，无需重复 augment。 |
| `transaction/ext_orig.cpp:159` | extension_bag | I | cross move committed through inventory.action original->extension source={arg}/{arg} dst={arg}/{arg} count={arg} | 状态变更/结果 | I | keep | 有限状态变更/结果日志；保留。 |
| `transaction/ext_orig.cpp:172` | extension_bag | W | cross move reject extension->original source={arg}/{arg} target={arg} | 失败/拒绝 | cross move reject extension->original source={arg}/{arg} target={arg} | keep | 正文已有 src/dst/size/target/status/原因或对象定位字段，按裁决不追加同义 reason。 |
| `transaction/ext_orig.cpp:187` | extension_bag | W | cross move reject extension->native target={arg} candidate={arg} size={arg} | 失败/拒绝 | cross move reject extension->native target={arg} candidate={arg} size={arg} | keep | 正文已有 src/dst/size/target/status/原因或对象定位字段，按裁决不追加同义 reason。 |
| `transaction/ext_orig.cpp:193` | extension_bag | W | cross move reject extension->original target size unavailable={arg} | 失败/拒绝 | cross move reject extension->original target size unavailable={arg} | keep | 正文已有 src/dst/size/target/status/原因或对象定位字段，按裁决不追加同义 reason。 |
| `transaction/ext_orig.cpp:203` | extension_bag | W | cross move reject extension->original source empty={arg}/{arg} | 失败/拒绝 | cross move reject extension->original source empty={arg}/{arg} | keep | 正文已有 src/dst/size/target/status/原因或对象定位字段，按裁决不追加同义 reason。 |
| `transaction/ext_orig.cpp:213` | extension_bag | E | cross move reject extension->original inventory.action unavailable | 失败/拒绝 | W | demote | 局部/单次操作失败，游戏可继续，E 应降为 W。 |
| `transaction/ext_orig.cpp:231` | extension_bag | W | cross move rejected extension->original status={arg} source={arg}/{arg} target={arg} ref={arg}/{arg}/{arg} handle={arg}/{arg} | 失败/拒绝 | cross move rejected extension->original status={arg} source={arg}/{arg} target={arg} ref={arg}/{arg}/{arg} handle={arg}/{arg} | keep | 正文已有 src/dst/size/target/status/原因或对象定位字段，按裁决不追加同义 reason。 |
| `transaction/ext_orig.cpp:242` | extension_bag | I | cross move committed through inventory.action extension->original source={arg}/{arg} target={arg}/{arg} count={arg} | 状态变更/结果 | I | keep | 有限状态变更/结果日志；保留。 |
| `transaction/transaction_coordinator.cpp:26` | extension_bag | I | txn pending reject id={arg} reason=invalid_domain | 失败/拒绝 | W | demote | 宏实际输出 INFO，但正文对应失败/拒绝；单次失败应为 W。 |
| `transaction/transaction_coordinator.cpp:31` | extension_bag | I | txn stage=pending-recorded durable=false id={arg} direction={arg} src={arg}/{arg} dst={arg}/{arg} | 过程/诊断 | D | demote | 内部阶段/过程诊断不属于有限生命周期状态变化，应为 D。 |
| `transaction/transaction_coordinator.cpp:98` | extension_bag | I | op=tx result=rolled_back tx={arg} src={arg}/{arg} dst={arg}/{arg} reason={arg} | 失败/拒绝 | I | keep | 统一事务终态日志记录每笔 rollback 结果，作为终态事件保留 I。 |
| `public/bridge.cpp:109` | extension_bag | I | extension source protection update deferred enabled={arg} | 状态变更/结果 | I | keep | 有限状态变更/结果日志；保留。 |
| `public/bridge.cpp:127` | extension_bag | I | prepare save-slot load preflight failed | 失败/拒绝 | W | demote | 宏实际输出 INFO，但正文对应失败/拒绝；单次失败应为 W。 |
| `public/bridge.cpp:132` | extension_bag | I | prepare save-slot load blocked by active module object | 失败/拒绝 | W | demote | 宏实际输出 INFO，但正文对应失败/拒绝；单次失败应为 W。 |
| `public/projection.cpp:123` | extension_bag | I | equip control blocked caller=UIEquip_EquipControlEventProc source={arg}/{arg}/{arg} target={arg} descriptor={arg} session={arg} | 失败/拒绝 | W | demote | 宏实际输出 INFO，但正文对应失败/拒绝；单次失败应为 W。 |
| `public/projection.cpp:150` | extension_bag | E | ERROR equip control token finish failed source={arg}/{arg}/{arg} aborted={arg} isolated={arg} | 失败/拒绝 | W | demote | 局部/单次操作失败，游戏可继续，E 应降为 W。 |
| `public/projection.cpp:155` | extension_bag | I | equip control blocked caller=UIEquip_EquipControlEventProc source={arg}/{arg}/{arg} target={arg} finished={arg} consumed={arg} | 失败/拒绝 | W | demote | 宏实际输出 INFO，但正文对应失败/拒绝；单次失败应为 W。 |
| `public/projection.cpp:194` | extension_bag | I | equip control committed session token={arg} source={arg}/{arg} target_slot={arg} | 状态变更/结果 | I | keep | 有限状态变更/结果日志；保留。 |
| `public/projection.cpp:202` | extension_bag | I | equip control handled caller=UIEquip_EquipControlEventProc source={arg}/{arg}/{arg} target={arg} result={arg} | 状态变更/结果 | I | keep | 有限状态变更/结果日志；保留。 |
| `public/projection.cpp:229` | extension_bag | I | apply reject reason=context src={arg}/{arg} dst={arg}/{arg} | 失败/拒绝 | W | demote | 宏实际输出 INFO，但正文对应失败/拒绝；单次失败应为 W。 |
| `public/projection.cpp:238` | extension_bag | I | apply reject reason=root src={arg}/{arg} dst={arg}/{arg} | 失败/拒绝 | W | demote | 宏实际输出 INFO，但正文对应失败/拒绝；单次失败应为 W。 |
| `public/projection.cpp:253` | extension_bag | I | apply reject reason=identity src={arg}/{arg} dst={arg}/{arg} | 失败/拒绝 | W | demote | 宏实际输出 INFO，但正文对应失败/拒绝；单次失败应为 W。 |
| `public/projection.cpp:266` | extension_bag | I | apply reject reason=controls src={arg}/{arg} dst={arg}/{arg} | 失败/拒绝 | W | demote | 宏实际输出 INFO，但正文对应失败/拒绝；单次失败应为 W。 |
| `public/projection.cpp:276` | extension_bag | I | apply reject reason=material kind={arg} jewel={arg} scroll={arg} remover={arg} | 失败/拒绝 | W | demote | 宏实际输出 INFO，但正文对应失败/拒绝；单次失败应为 W。 |
| `public/projection.cpp:291` | extension_bag | I | apply reject reason=not_applicable kind={arg} equip={arg} material={arg} | 失败/拒绝 | W | demote | 宏实际输出 INFO，但正文对应失败/拒绝；单次失败应为 W。 |
| `public/projection.cpp:300` | extension_bag | I | apply reject reason=symbol_unresolved kind={arg} equip={arg} material={arg} | 失败/拒绝 | W | demote | 宏实际输出 INFO，但正文对应失败/拒绝；单次失败应为 W。 |
| `public/projection.cpp:307` | extension_bag | I | apply reject reason=stale_token kind={arg} equip={arg} material={arg} | 失败/拒绝 | W | demote | 宏实际输出 INFO，但正文对应失败/拒绝；单次失败应为 W。 |
| `public/projection.cpp:312` | extension_bag | I | apply branch kind={arg} equip={arg} material={arg} | 过程/诊断 | D | demote | 内部阶段/过程诊断不属于有限生命周期状态变化，应为 D。 |
| `public/projection.cpp:325` | extension_bag | E | ERROR apply token cleanup failed kind={arg} equip={arg} material={arg} | 失败/拒绝 | W | demote | 局部/单次操作失败，游戏可继续，E 应降为 W。 |
| `public/projection.cpp:328` | extension_bag | I | apply reject reason=is_ok_failed kind={arg} equip={arg} material={arg} | 失败/拒绝 | W | demote | 宏实际输出 INFO，但正文对应失败/拒绝；单次失败应为 W。 |
| `public/projection.cpp:345` | extension_bag | E | ERROR apply token cleanup failed kind={arg} equip={arg} material={arg} | 失败/拒绝 | W | demote | 局部/单次操作失败，游戏可继续，E 应降为 W。 |
| `public/projection.cpp:348` | extension_bag | I | apply reject reason={arg} kind={arg} equip={arg} material={arg} native_apply_failed stale_token | 失败/拒绝 | W | demote | 宏实际输出 INFO，但正文对应失败/拒绝；单次失败应为 W。 |
| `public/projection.cpp:359` | extension_bag | I | apply reject reason=material_release_failed kind={arg} equip={arg} material={arg} | 失败/拒绝 | W | demote | 宏实际输出 INFO，但正文对应失败/拒绝；单次失败应为 W。 |
| `public/projection.cpp:381` | extension_bag | I | apply reject reason=stale_target kind={arg} equip={arg} material={arg} | 失败/拒绝 | W | demote | 宏实际输出 INFO，但正文对应失败/拒绝；单次失败应为 W。 |
| `public/projection.cpp:387` | extension_bag | I | apply reject reason=target_payload_sync_failed kind={arg} equip={arg} material={arg} | 失败/拒绝 | W | demote | 宏实际输出 INFO，但正文对应失败/拒绝；单次失败应为 W。 |
| `public/projection.cpp:394` | extension_bag | I | apply committed kind={arg} equip={arg} material={arg} | 状态变更/结果 | I | keep | 有限状态变更/结果日志；保留。 |
| `public/projection.cpp:418` | extension_bag | I | drop rejected: extension source view mismatch src={arg}/{arg} view={arg} root={arg} | 失败/拒绝 | W | demote | 宏实际输出 INFO，但正文对应失败/拒绝；单次失败应为 W。 |
| `public/projection.cpp:426` | extension_bag | I | drop rejected: extension source control is not current projection src={arg}/{arg} source={arg} expected={arg} root={arg} live_root={arg} | 失败/拒绝 | W | demote | 宏实际输出 INFO，但正文对应失败/拒绝；单次失败应为 W。 |
| `public/projection.cpp:446` | extension_bag | I | drop rejected: extension source invalid projected target src={arg}/{arg} dst={arg} | 失败/拒绝 | W | demote | 宏实际输出 INFO，但正文对应失败/拒绝；单次失败应为 W。 |
| `public/projection.cpp:457` | extension_bag | I | drop rejected: extension source session mismatch src={arg}/{arg} phase={arg} | 失败/拒绝 | W | demote | 宏实际输出 INFO，但正文对应失败/拒绝；单次失败应为 W。 |
| `public/projection.cpp:476` | extension_bag | I | drop rejected: extension source session transition failed src={arg}/{arg} | 失败/拒绝 | W | demote | 宏实际输出 INFO，但正文对应失败/拒绝；单次失败应为 W。 |
| `public/projection.cpp:536` | extension_bag | I | drop routed ext->ext dst_slot={arg} | 状态变更/结果 | I | keep | 有限状态变更/结果日志；保留。 |
| `public/projection.cpp:539` | extension_bag | I | drop rejected: inventory.action move status={arg} src={arg}/{arg} dst={arg} | 失败/拒绝 | W | demote | 宏实际输出 INFO，但正文对应失败/拒绝；单次失败应为 W。 |
| `public/projection.cpp:556` | extension_bag | I | original item input blocked mode={arg} selected={arg} | 失败/拒绝 | W | demote | 宏实际输出 INFO，但正文对应失败/拒绝；单次失败应为 W。 |
| `public/projection.cpp:584` | extension_bag | I | drop rejected: extension source entering original proc item={arg} caller=libgame+0x{arg} view={arg} session={arg} source={arg}/{arg} target_slot={arg} | 失败/拒绝 | W | demote | 宏实际输出 INFO，但正文对应失败/拒绝；单次失败应为 W。 |
| `public/projection.cpp:601` | extension_bag | I | original drop blocked for extension target control={arg} item={arg} target_slot={arg} | 失败/拒绝 | W | demote | 宏实际输出 INFO，但正文对应失败/拒绝；单次失败应为 W。 |
| `public/projection.cpp:643` | extension_bag | E | ERROR projected control sync failed bag={arg} slot={arg} control={arg} | 失败/拒绝 | W | demote | 局部/单次操作失败，游戏可继续，E 应降为 W。 |
| `api/api_impl.cpp:161` | extension_bag | E | ERROR API use token mismatch bag={arg} slot={arg} item={arg} marker_cleared={arg} | 失败/拒绝 | W | demote | 局部/单次操作失败，游戏可继续，E 应降为 W。 |
| `api/bag_source.cpp:142` | extension_bag | W | bag source enumerate skipped unmaterializable item bag={arg} slot={arg} | 状态变更/结果 | W | keep | 正文已有原因/对象或事务字段，无需重复 augment。 |
| `api/bag_source.cpp:369` | extension_bag | E | bag source import rollback could not release object bag={arg} slot={arg} | 失败/拒绝 | E | keep | 导入回滚时对象未能释放，所有权状态不一致，保留 E。 |
| `api/bag_source.cpp:410` | extension_bag | W | bag source remove count rejected bag={arg} slot={arg} count={arg} visible={arg} | 失败/拒绝 | W | keep | 正文已有原因/对象或事务字段，无需重复 augment。 |
| `api/bag_source.cpp:442` | extension_bag | E | bag source remove persist failed and restore materialization failed bag={arg} slot={arg} | 失败/拒绝 | E | keep | 持久化失败后恢复物化也失败，可能造成数据/对象状态不一致，保留 E。 |
| `api/bag_source.cpp:468` | extension_bag | W | bag source remove patch rejected bag={arg} slot={arg} category={arg} count={arg} | 失败/拒绝 | W | keep | 正文已有原因/对象或事务字段，无需重复 augment。 |
| `api/bag_source.cpp:477` | extension_bag | W | bag source remove write rejected bag={arg} slot={arg} category={arg} count={arg} | 失败/拒绝 | W | keep | 正文已有原因/对象或事务字段，无需重复 augment。 |
| `api/bag_source.cpp:489` | extension_bag | E | bag source remove rollback count write failed bag={arg} slot={arg} | 失败/拒绝 | E | keep | 回滚时数量写入失败，状态一致性无法保证，保留 E。 |
| `api/bag_source.cpp:516` | extension_bag | W | bag source replace rejected reason=request category={arg} count={arg} payload={arg} | 失败/拒绝 | W | keep | 正文已有原因/对象或事务字段，无需重复 augment。 |
| `api/bag_source.cpp:528` | extension_bag | W | bag source replace rejected reason=payload category={arg} count={arg} size={arg} | 失败/拒绝 | W | keep | 正文已有原因/对象或事务字段，无需重复 augment。 |
| `api/bag_source.cpp:544` | extension_bag | W | bag source replace rejected reason=handle slot={arg} generation={arg} | 失败/拒绝 | W | keep | 正文已有原因/对象或事务字段，无需重复 augment。 |
| `api/bag_source.cpp:553` | extension_bag | W | bag source replace rejected reason=slot bag={arg} slot={arg} | 失败/拒绝 | W | keep | 正文已有原因/对象或事务字段，无需重复 augment。 |
| `api/bag_source.cpp:558` | extension_bag | W | bag source replace rejected reason=empty bag={arg} slot={arg} | 失败/拒绝 | W | keep | 正文已有原因/对象或事务字段，无需重复 augment。 |
| `api/bag_source.cpp:576` | extension_bag | W | bag source replace rejected reason=valid_payload bag={arg} slot={arg} size={arg} | 失败/拒绝 | W | keep | 正文已有原因/对象或事务字段，无需重复 augment。 |
| `api/bag_source.cpp:599` | extension_bag | I | bag source replace bag={arg} slot={arg} category={arg} count={arg} | 状态变更/结果 | I | keep | 有限状态变更/结果日志；保留。 |
| `api/bag_source.cpp:647` | extension_bag | I | bag source use rejected bag={arg} slot={arg} body={arg} | 失败/拒绝 | W | demote | 宏实际输出 INFO，但正文对应失败/拒绝；单次失败应为 W。 |
| `persistence/persistence.cpp:126` | extension_bag | I | slot data load slot={arg} presence=missing epoch={arg} default state | 失败/拒绝 | I | keep | 缺失数据首次加载为空背包是正常初始化状态，保留 I。 |
| `persistence/persistence.cpp:137` | extension_bag | W | slot data blocked reason=version stored={arg} expected={arg} | 失败/拒绝 | W | keep | 正文已有原因/对象或事务字段，无需重复 augment。 |
| `persistence/persistence.cpp:143` | extension_bag | W | slot data blocked reason=unreadable | 失败/拒绝 | W | keep | 正文已有原因/对象或事务字段，无需重复 augment。 |
| `persistence/persistence.cpp:148` | extension_bag | W | slot data blocked reason=missing_payload_bytes slot={arg} size={arg} | 失败/拒绝 | W | keep | 正文已有原因/对象或事务字段，无需重复 augment。 |
| `persistence/persistence.cpp:157` | extension_bag | I | slot data migrate section_version={arg}->{arg} stored_bag_count=legacy bagCount | 状态变更/结果 | I | keep | 有限状态变更/结果日志；保留。 |
| `persistence/persistence.cpp:161` | extension_bag | I | slot data migrate section_version={arg}->{arg} stored_bag_count=5 | 状态变更/结果 | I | keep | 有限状态变更/结果日志；保留。 |
| `persistence/persistence.cpp:174` | extension_bag | W | pending isolated reason=invalid_transaction_domain direction={arg} src={arg}/{arg} dst={arg}/{arg} | 失败/拒绝 | W | keep | 正文已有原因/对象或事务字段，无需重复 augment。 |
| `persistence/persistence.cpp:182` | extension_bag | W | pending isolated reason=malformed_pending_record | 状态变更/结果 | W | keep | 正文已有原因/对象或事务字段，无需重复 augment。 |
| `persistence/persistence.cpp:219` | extension_bag | W | slot data blocked reason=parse_failed slot={arg} | 失败/拒绝 | W | keep | 正文已有原因/对象或事务字段，无需重复 augment。 |
| `persistence/persistence.cpp:230` | extension_bag | I | slot data load slot={arg} parsed=1 epoch={arg} | 状态变更/结果 | I | keep | 有限状态变更/结果日志；保留。 |
| `persistence/persistence.cpp:252` | extension_bag | I | slot data capture slot={arg} epoch={arg} bytes={arg} nonempty={arg} | 状态变更/结果 | I | keep | 有限状态变更/结果日志；保留。 |
| `persistence/persistence.cpp:274` | extension_bag | W | slot data committed skipped clear reason=capture_identity_changed slot={arg} | 状态变更/结果 | W | keep | 正文已有原因/对象或事务字段，无需重复 augment。 |
| `persistence/persistence.cpp:298` | extension_bag | W | slot data blocked reason=outcome={arg} slot={arg} tx={arg} | 失败/拒绝 | W | keep | 正文已有原因/对象或事务字段，无需重复 augment。 |
| `persistence/persistence.cpp:351` | extension_bag | W | slot data register failed code={arg} reason=register_failed | 失败/拒绝 | W | augment | 失败缺少原因及可定位对象/事务字段，需补最小字段。 |
| `persistence/persistence.cpp:356` | extension_bag | I | slot data registered handle={arg} initial_req={arg} | 状态变更/结果 | I | keep | 有限状态变更/结果日志；保留。 |
| `persistence/persistence.cpp:371` | extension_bag | W | slot data unregister failed code={arg} reason=unregister_failed | 失败/拒绝 | W | augment | 失败缺少原因及可定位对象/事务字段，需补最小字段。 |
| `runtime/inventory_action.h:31` | extension_bag | E | inventory.action unavailable code={arg}; item mutation rejected | 失败/拒绝 | W | demote | 局部/单次操作失败，游戏可继续，E 应降为 W。 |
| `runtime/projection_controller.cpp:111` | extension_bag | D | write_set bag={arg} | 过程/诊断 | D | keep | 仅诊断用途，置于 D；高频路径由 debug 门控。 |
| `runtime/state_load.cpp:38` | extension_bag | E | save validation reject: over-capacity slot holds live object bag={arg} slot={arg} | 失败/拒绝 | W | demote | 局部/单次操作失败，游戏可继续，E 应降为 W。 |
| `runtime/state_load.cpp:49` | extension_bag | W | save validation discarded {arg} over-capacity item(s) | 状态变更/结果 | W | keep | 正文已有原因/对象或事务字段，无需重复 augment。 |
| `assembly/claims.cpp:212` | extension_bag | E | ERROR popup restore unavailable popup={arg} entry={arg} | 失败/拒绝 | W | demote | 局部/单次操作失败，游戏可继续，E 应降为 W。 |
| `assembly/claims.cpp:219` | extension_bag | E | ERROR popup restore failed popup={arg} entry={arg} code={arg} | 失败/拒绝 | W | demote | 局部/单次操作失败，游戏可继续，E 应降为 W。 |
| `usecases/move.cpp:104` | extension_bag | W | extension->extension action rejected status={arg} src={arg}/{arg} dst={arg}/{arg} | 失败/拒绝 | W | keep | 正文已有原因/对象或事务字段，无需重复 augment。 |
| `usecases/move.cpp:110` | extension_bag | I | extension->extension committed through inventory.action src={arg}/{arg} dst={arg}/{arg} count={arg} | 状态变更/结果 | I | keep | 有限状态变更/结果日志；保留。 |
| `usecases/set_bag_count.cpp:44` | extension_bag | W | bag count apply deferred reason=runtime cleanup failed old={arg} requested={arg} | 失败/拒绝 | W | keep | 正文已有原因/对象或事务字段，无需重复 augment。 |

## 缺失日志建议

| file + 函数 | 事件 | 目标等级 | 必填字段 | 理由 |
|---|---|---|---|---|
| `transaction/ext_ext.cpp` + `txn_abort_locked` | 每笔失败事务唯一 rolled_back 终态 | I | `op=tx`、`result=rolled_back`、`tx=`、`src=`、`dst=`、`reason=` | 已在公共终结函数记录；既有 committed 路径不重复。 |
| `transaction/ext_orig.cpp` + `move_extension_to_original_locked` | count<=0 拒绝 | W | `op=cross_move`、`result=rejected`、`reason=invalid_count`、`count=`、`src=`、`dst=` | 已在无日志的直接失败返回前添加记录。 |
| `runtime/entry_guard.cpp` + `EntryGuard` 拒绝分支 | 非主线程/重入拒绝 | W | `entry=`、`reason=` | 已有两个拒绝日志，原来缺入口标识与拒绝原因；在现有正文末尾补字段，没有新增调用点。 |

## 统计

- 现有日志：121 条。
- disposition：keep 74，demote 45，promote 0，delete 0，merge 0，augment 2。
- 缺失日志建议：3 条（已采纳并实施）。

## Top 发现

1. transaction 拒绝/abort 正文按实际格式串检查；已含 `reason=`、`id=`、`src/dst` 的条目不再因模板规则 augment。
2. `public/projection.cpp` apply reject 已带 `reason=` 与 src/dst，等级依据失败语义改 W，不补重复字段。
3. persistence blocked/load 已有 `reason=` 或 `slot=`，不作模板化 augment；损坏不可恢复才可为 E。
4. API bag source 拒绝按正文中的 bag/slot/reason/handle 字段保留；正常查询空结果不是失败。
5. `VIRTBAG_LOG` 默认实际是 INFO，需重点把失败/拒绝降为 W、内部过程降为 D；entry_guard 等动态等级看调用参数。

## 审核备注

本次重做是因为上一版存在多行日志占位摘要和模板化 augment 裁决。现已逐条读取日志调用源码，将多段格式串合并为真实摘要，并按正文已有字段及实际宏等级重裁；未修改源码。

## 审核裁决

- 驳回 `api/bag_source.cpp:369`、`:442`、`:489` 的 E→W：回滚/释放失败涉及所有权或状态一致性，保留 E。
- 驳回 `persistence/persistence.cpp:126` 的 I→W：presence=missing 是首次加载默认状态，保留 I。
- 驳回其余 12 条 augment：正文已有对象、源/目标、大小、状态或原因字段，不再追加同义字段；仅保留 register/unregister 两条 augment。
- 接受的等级裁决已执行；公共事务 rollback 终态记录保留 I，正文统一为 `op=tx result=rolled_back tx=... src=... dst=... reason=...`。
- `entry_guard` 检查发现非主线程拒绝和重入拒绝两处既有日志均缺 `entry=`/`reason=`；已在正文末尾追加，不新增日志调用点。
