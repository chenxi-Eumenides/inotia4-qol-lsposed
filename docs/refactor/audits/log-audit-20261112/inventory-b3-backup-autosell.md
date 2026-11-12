# 日志审计清单：批次 3（save_backup + autosell）

依据 `docs/reference/logging-standards.md` 与 `docs/reference/logging.md`。domain 分别为 `save_backup` 与 `autosell`。本清单纳入直接插件宏及 `SB_LOG`/`SB_LOG_WARN`、`SB_UI_LOG`、`AUTOSELLUI_LOG` 对插件宏的别名调用；通用出口/非日志匹配单独核对。

## 现有日志

| file:line | domain | level_now | message 摘要 | event_class | level_target | disposition | rationale |
|---|---|---:|---|---|---:|---|---|
| `save_backup/usecases/meta_backfill.cpp:80` | save_backup | W | backfill entry meta write failed；path | 单次备份元数据回填失败 | W | augment | 单次回填失败不使游戏不可用；应改为 `fail op=backfill_meta reason=write_failed`，避免路径正文并补对象标识。 |
| `save_backup/usecases/meta_backfill.cpp:86` | save_backup | I | backfill entry meta；path/level/class | 元数据回填成功 | D | demote | 备份列表解析时可逐条回填，属内部细节，不是有限生命周期状态；降 D 并用不含路径的 `slot/item` 上下文。 |
| `save_backup/backup_files.cpp:43` | save_backup | I | skip invalid warehouse name；slot/name | 导出跳过单个非法仓库名 | D | demote | 单项过滤细节；不影响整体导出，降 D。 |
| `save_backup/backup_files.cpp:51` | save_backup | I | warehouse too large；slot/size/name | 单个仓库文件超限导致导出失败 | W | promote | 已设置导出错误并中止本次备份，当前 INFO 错级；补 `reason=file_too_large`。 |
| `save_backup/backup_files.cpp:58` | save_backup | I | warehouse read failed；slot/name | 仓库文件读取失败 | W | promote | 失败中止此次备份，不是状态成功；补 `reason=read_failed`。 |
| `save_backup/backup_files.cpp:63` | save_backup | I | warehouse too large；slot/size/name | 读取后确认文件超限 | W | promote | 与 51 是同一规则在 stat 竞态后的另一检查点，但可由不同失败路径抵达；应是 W，并合并为同一个错误出口。 |
| `save_backup/backup_files.cpp:83` | save_backup | I | warehouse decrypted for cross-device；slot/name/bytes | 仓库明文解密成功 | D | demote | 每个文件的导出过程细节，不是有限状态变更；降 D。 |
| `save_backup/backup_files.cpp:92` | save_backup | I | kept as ciphertext；slot/name/bytes | 解密不可用时保留密文 | D | demote | 这是降级路径细节；若该备份仍可导出，D 足够，建议补 `reason=decrypt_unavailable`。 |
| `save_backup/backup_files.cpp:102` | save_backup | I | warehouse validation failed；slot/error | 仓库集合校验失败 | W | promote | 校验失败令本次导出失败，应 W，包含结构化 `reason=`。 |
| `save_backup/backup_files.cpp:112` | save_backup | I | inline WH96 segment stripped；slot/origLen | 导出时剥离内嵌仓库段 | D | demote | 逐文件格式转换细节降 D；原始长度不是必要状态字段。 |
| `save_backup/backup_files.cpp:115` | save_backup | I | inline warehouse kept；slot/state | 内嵌仓库未剥离 | D | demote | 单次导出分支选择，降 D。 |
| `save_backup/backup_files.cpp:119` | save_backup | I | no external warehouse；keep inline | 未找到外置仓库，保留内嵌段 | D | demote | 正常可选仓库缺失，保留诊断并降 D。 |
| `save_backup/backup_files.cpp:184` | save_backup | I | c18 bag absent | C18 原生背包数据目录不可用 | D | demote | 正常可选目录缺失，保留诊断并降 D。 |
| `save_backup/backup_files.cpp:200` | save_backup | I | c18 bag source；slot/source/records | C18 背包镜像来源确定 | D | demote | 单次文件探测结果细节，降 D。 |
| `save_backup/backup_files.cpp:211` | save_backup | I | c18 bag absent；slot | 未找到 C18 背包镜像 | D | demote | 正常可选镜像缺失，保留诊断并降 D。 |
| `save_backup/backup_files.cpp:218` | save_backup | I | c18 bag source；slot/source/records | 共享 C18 背包镜像选中 | D | demote | 与 200 同为导出文件来源细节，降 D。 |
| `save_backup/usecases/backup_usecases.cpp:142` | save_backup | I | import gate world state；state | 导入因非主菜单拒绝 | W | augment | 单次用户操作拒绝，应 W；用 `reject op=import reason=not_main_menu slot=`，避免仅输出状态值。 |
| `save_backup/usecases/backup_usecases.cpp:146` | save_backup | W | world state unavailable；code/fallback | 主菜单状态读取失败，走备用判断 | I | demote | 备用 `current_slot` 判定继续工作，属于实际降级路径；等级降为 I；正文已有 code，不再 augment。 |
| `save_backup/usecases/backup_usecases.cpp:149` | save_backup | W | world facts unavailable；fallback | 缺少 world facts，走备用判断 | I | augment | 备用判断仍可执行，记降级 I；补 `op=import fallback=current_slot reason=world_facts_unavailable`。 |
| `save_backup/usecases/backup_usecases.cpp:161` | save_backup | W | current_slot unavailable；code | 导入门禁无法读取当前槽 | W | augment | 单次导入拒绝且游戏可继续；补 `op=import reason=current_slot_unavailable`，保留 code。 |
| `save_backup/usecases/backup_usecases.cpp:166` | save_backup | I | current_slot not main menu；slot | 当前槽已加载而拒绝覆盖导入 | W | promote | 单次导入被拒，非状态成功；用 `reject op=import slot=... reason=not_main_menu`。 |
| `save_backup/usecases/backup_usecases.cpp:170` | save_backup | I | fallback current_slot；slot | 备用门禁确认主菜单 | D | demote | 单次判定过程细节降 D；非面向运维的状态变化。 |
| `save_backup/usecases/backup_usecases.cpp:430` | save_backup | W | autobackup skipped；external dir unavailable/slot | 自动备份因目录缺失跳过 | W | keep | 本次备份失败但游戏继续；已有 slot 与明确失败缘由。 |
| `save_backup/usecases/backup_usecases.cpp:435` | save_backup | W | autobackup skipped；backup dir unavailable/slot | 自动备份目录创建失败 | W | keep | 单次备份失败，槽与原因明确。 |
| `save_backup/usecases/backup_usecases.cpp:451` | save_backup | W | autobackup export failed；slot/error | 自动备份导出失败 | W | keep | 单次创建失败 W 合适，带 slot 与错误。 |
| `save_backup/usecases/backup_usecases.cpp:457` | save_backup | W | fingerprint write failed；slot | 备份已导出但指纹写入失败 | W | keep | 本次指纹写失败但备份可用；正文已有 slot，不再 augment。 |
| `save_backup/usecases/backup_usecases.cpp:548` | save_backup | I | list skip；name | 列表中跳过 stat 异常/超限文件 | D | demote | 列表扫描逐文件细节，降 D；应携带明确 `reason=`，不输出文件路径。 |
| `save_backup/usecases/backup_usecases.cpp:553` | save_backup | I | scan stopped；total/limit | 列表扫描达到总读取上限 | D | demote | 防护性扫描边界细节，降 D；`limit=` 比重复自然语言更明确。 |
| `save_backup/usecases/backup_usecases.cpp:562` | save_backup | I | list skip；name | bundle 读取或解析失败后跳过 | W | promote | 文件损坏/不可读是可恢复的单项失败，不能无声跳过；W 并附 `reason=unreadable_or_corrupt`。 |
| `save_backup/usecases/backup_usecases.cpp:606` | save_backup | I | list files skip；name | 文件列表中跳过异常文件 | D | demote | 逐文件列表细节；降 D 并给枚举原因。 |
| `save_backup/usecases/backup_usecases.cpp:611` | save_backup | I | list files stopped；stat total/limit | 文件列表达到大小上限停止 | D | demote | 扫描策略细节，降 D。 |
| `save_backup/usecases/backup_usecases.cpp:816` | save_backup | I | deduplicated；file/checksum | 导出内容命中已有备份 | I | keep | 有限单次操作结果，且文件/checksum 能定位去重命中。 |
| `save_backup/usecases/backup_usecases.cpp:821` | save_backup | I | dedup unreadable/mismatched；file | 去重候选不可读/校验不符，改写新包 | W | augment | 不可信候选被回退处理且新写仍继续；按降级 I 记录 `fallback=write_new_bundle`，带 `reason=`。 |
| `save_backup/usecases/backup_usecases.cpp:856` | save_backup | I | exported；file/bytes | 备份创建成功 | I | keep | 明确创建结果与文件/大小；但需补 `slot=`，并以最终结果口径覆盖 UI/自动备份的创建确认。 |
| `save_backup/transaction/transactions.cpp:96` | save_backup | W | C18 import target rejected；reason/series/version/layout | C18 目标身份不符拒绝导入 | W | keep | 单次导入拒绝且 reason/目标字段齐全。 |
| `save_backup/transaction/transactions.cpp:105` | save_backup | W | C18 target rejected；reason/series/version/layout | C18 目标布局未知拒绝导入 | W | keep | 单次导入拒绝，结构字段充分。 |
| `save_backup/transaction/transactions.cpp:352` | save_backup | W | invalid index entry；index | 删除恢复索引格式错误 | W | augment | 当前解析失败、保留索引；需 `op=restore reason=invalid_index`，不要输出路径。 |
| `save_backup/transaction/transactions.cpp:366` | save_backup | W | rejected index entry；index | 删除恢复索引安全校验拒绝 | W | augment | 防止任意路径恢复的单次拒绝；补 `reason=unsafe_index_entry`。 |
| `save_backup/transaction/transactions.cpp:381` | save_backup | W | target exists, keep trash；target | 恢复时目标冲突、保留 trash | W | augment | 恢复未完成且证据保留；增加 `op=restore reason=target_exists`。 |
| `save_backup/transaction/transactions.cpp:387` | save_backup | W | rename back failed；trash | 删除恢复 rename 失败 | W | augment | 单次恢复失败；补 `reason=rename_failed`。 |
| `save_backup/transaction/transactions.cpp:436` | save_backup | W | target exists, keep trash；target | 孤立 sidecar 恢复目标冲突 | W | augment | 恢复未完成，带 `reason=target_exists`。 |
| `save_backup/transaction/transactions.cpp:441` | save_backup | W | rename back failed；target | 孤立 sidecar 恢复失败 | W | augment | 单次 rename 失败，补 `reason=rename_failed`。 |
| `save_backup/transaction/transactions.cpp:469` | save_backup | W | unreadable index；index | 删除恢复索引不可读 | W | augment | 单次恢复失败，保留索引；增加 `reason=read_failed`。 |
| `save_backup/transaction/transactions.cpp:473` | save_backup | W | recovery incomplete；index | 删除恢复未完成 | W | augment | 当前可重试且游戏可继续，W 正确；补失败原因/未恢复计数。 |
| `save_backup/transaction/transactions.cpp:483` | save_backup | I | restored files；count/index | 删除恢复成功 | I | augment | 有状态变化，但应补 `op=restore` 与槽/交易 id；避免路径。 |
| `save_backup/transaction/transactions.cpp:495` | save_backup | W | external dir unavailable；slot | 删除槽后 sidecar 未处理 | W | keep | 单次清理失败但游戏继续；正文已有 slot，不再 augment。 |
| `save_backup/transaction/transactions.cpp:503` | save_backup | W | slot operation unavailable；slot | 删除槽 sidecar 操作锁失败 | W | keep | 单次操作未完成；正文已有 slot，不再 augment。 |
| `save_backup/transaction/transactions.cpp:509` | save_backup | W | saves dir unavailable；slot | sidecar 保存目录不可用 | W | keep | 单次失败；正文已有 slot，不再 augment。 |
| `save_backup/transaction/transactions.cpp:516` | save_backup | W | backup dir unavailable；slot | 删除索引目录不可用 | W | keep | 单次失败；正文已有 slot，不再 augment。 |
| `save_backup/transaction/transactions.cpp:556` | save_backup | W | trash mkdir failed；slot | 删除事务临时目录创建失败 | W | keep | 单次删除辅助步骤失败；正文已有 slot，不再 augment。 |
| `save_backup/transaction/transactions.cpp:563` | save_backup | W | data trash mkdir failed；slot | 数据临时目录创建失败 | W | keep | 单次失败；正文已有 slot，不再 augment。 |
| `save_backup/transaction/transactions.cpp:571` | save_backup | W | index write failed；slot | 删除恢复索引写入失败 | W | keep | 单次操作失败；正文已有 slot，不再 augment。 |
| `save_backup/transaction/transactions.cpp:577` | save_backup | W | trash api unavailable；slot | 删除 API 不支持 trash | W | keep | 单次删除降级/失败；正文已有 slot，不再 augment。 |
| `save_backup/transaction/transactions.cpp:591` | save_backup | W | sidecar move failed；slot | sidecar 移入 trash 失败 | W | keep | 单次删除失败；正文已有 slot，不再 augment。 |
| `save_backup/transaction/transactions.cpp:600` | save_backup | W | wh4 move failed；slot/file | 仓库文件移入 trash 失败 | W | keep | 单项移动失败；正文已有 slot/file，不再 augment。 |
| `save_backup/transaction/transactions.cpp:615` | save_backup | W | c18 move failed；slot/file | C18 文件移入 trash 失败 | W | keep | 单项移动失败；正文已有 slot/file，不再 augment。 |
| `save_backup/transaction/transactions.cpp:638` | save_backup | W | rollback incomplete；index | 删除回滚未完整完成 | E | promote | 槽数据/sidecar 删除后回滚不完整，可能留下不可用存档及人工恢复证据，属不可恢复状态；该插件缺 E 通道。 |
| `save_backup/transaction/transactions.cpp:647` | save_backup | I | removed sidecar/wh4/c18；slot/counts | 删除槽关联文件成功 | I | keep | 状态变化明确，槽与清理计数齐全。 |
| `save_backup/transaction/transactions.cpp:701` | save_backup | I | C18 bag skipped；slot/layout/reason | 导入跳过不兼容 C18 镜像 | D | demote | 单次导入的可选数据分支细节记 D；正文已有 reason，不再 augment。 |
| `save_backup/transaction/transactions.cpp:713` | save_backup | I | missing C18 bag；materialized empty | 旧备份导入时生成空镜像 | I | augment | 实际导入内容发生降级变化，补 `slot=` 与 `fallback=empty_c18_bag`。 |
| `save_backup/transaction/transactions.cpp:801` | save_backup | I | warehouse companion skipped；slot/state/files | 仓库伴生数据跳过 | I | augment | 导入主存档继续但伴生数据降级；补 `fallback=skip_warehouse`，当前已有 slot/state/files。 |
| `save_backup/transaction/transactions.cpp:1012` | save_backup | I | warehouse files wrote/skipped/reencrypted | 仓库伴生文件导入统计 | I | keep | 单次事务汇总结果，计数及状态足够。 |
| `save_backup/transaction/transactions.cpp:1077` | save_backup | I | imported；file/slot | 备份恢复成功 | I | augment | 这是恢复最终结果，但缺导入结果字段/数据项数；补 `op=restore result=success slot=`。 |
| `save_backup/transaction/transactions.cpp:1109` | save_backup | I | delete failed；path | 删除备份文件失败 | W | promote | 单次文件删除失败应 W；当前静态等级过低且只有路径，无 `reason=`。 |
| `save_backup/transaction/transactions.cpp:1112` | save_backup | I | deleted；path | 删除备份成功 | I | augment | 状态变化应保留；用 `op=delete_backup result=success` 加对象名，不输出路径。 |
| `save_backup/transaction/transactions.cpp:1214` | save_backup | W | rollback incomplete；index | 删除事务回滚不完整 | E | promote | 同一删除状态不一致/证据保留，按数据不可用升级 E；缺 E 通道。 |
| `save_backup/transaction/transactions.cpp:1253` | save_backup | W | trash api unavailable；slot | 槽删除时 trash API 不可用 | W | keep | 可恢复单次失败，已有槽和原因。 |
| `save_backup/transaction/transactions.cpp:1260` | save_backup | I | sidecar move failed；slot | sidecar 移动失败 | W | promote | 当前忽略/继续分支仍发生单次失败，改 W 并补 reason/code。 |
| `save_backup/transaction/transactions.cpp:1270` | save_backup | I | wh4 move failed；path | 仓库文件移动失败 | W | promote | 单次文件操作失败改 W，补 slot/reason。 |
| `save_backup/transaction/transactions.cpp:1281` | save_backup | I | c18 move failed；path | C18 文件移动失败 | W | promote | 单次文件操作失败改 W，补 slot/reason。 |
| `save_backup/transaction/transactions.cpp:1290` | save_backup | I | unlink failed；trash path | 删除 trash 文件失败 | W | promote | 单次删除失败改 W，补 `reason=`；若后续回滚不完整，再由最终 E 唯一记录。 |
| `save_backup/transaction/transactions.cpp:1295` | save_backup | W | rollback incomplete；index | 槽删除回滚不完整 | E | promote | 可能使存档关联数据不一致，需人工处理；缺 E 通道。 |
| `save_backup/transaction/transactions.cpp:1304` | save_backup | I | slot deleted；slot/dat/sidecar/wh4/c18 | 槽删除完成 | I | keep | 有限状态变化且含槽及分项计数。 |
| `save_backup/plugin.cpp:532` | save_backup | I | hook backup handover；orig pointer | 删除槽 hook backup 接管 | D | demote | hook 内部指针诊断，不是面向用户状态变化；降 D。 |
| `save_backup/ui/panel.cpp:320` | save_backup | I | backup files enumerated；files/pages/queue | 面板备份列表解析任务建立 | D | demote | 面板逐次刷新/解析过程细节，不是每次都发生的状态变化；降 D。 |
| `save_backup/ui/panel.cpp:357` | save_backup | I | auto backup parse failed；slot | 自动备份条目解析失败 | W | promote | 单个备份条目不可读，本次面板操作可继续；应 W，现有 slot 已足够定位，不再 augment。 |
| `save_backup/ui/panel.cpp:368` | save_backup | I | warehouse not decryptable；name | 备份仓库不可解密 | W | promote | 单个仓库数据不可用但面板可继续；应 W；文件名已提供对象，不再 augment。 |
| `save_backup/ui/panel.cpp:371` | save_backup | I | backup entry parse failed；name | 备份条目解析失败 | W | promote | 单个候选备份解析失败；应 W；已有 name 定位，不再 augment。 |
| `save_backup/ui/panel.cpp:552` | save_backup | I | provider close；panel inactive | 面板 provider 关闭 | I | keep | 面板关闭是有限生命周期状态变化，静态文案有明确状态语义。 |
| `save_backup/ui/panel.inc:124` | save_backup | I | controls failed；component API validation | 面板组件 API 校验失败 | W | promote | 控件树无法建立，本次功能面板不可用；游戏仍可继续。 |
| `save_backup/ui/panel.inc:132` | save_backup | I | controls failed；root panel_create | 根面板创建失败 | W | promote | 面板能力不可用但游戏可继续，按单次失败 W。 |
| `save_backup/ui/panel.inc:143` | save_backup | I | controls failed；slot button index | 槽按钮创建失败 | W | promote | 单次控件创建失败；索引可定位。 |
| `save_backup/ui/panel.inc:150` | save_backup | I | controls failed；export arrow | 导出箭头创建失败 | W | promote | 面板部分能力失败；单次 W。 |
| `save_backup/ui/panel.inc:154` | save_backup | I | controls failed；delete hit button | 删除按钮创建失败 | W | promote | 面板部分能力失败；单次 W。 |
| `save_backup/ui/panel.inc:160` | save_backup | I | controls failed；import arrow | 导入箭头创建失败 | W | promote | 面板部分能力失败；单次 W。 |
| `save_backup/ui/panel.inc:171` | save_backup | I | controls failed；side export button | 侧栏导出按钮创建失败 | W | promote | 面板部分能力失败；单次 W。 |
| `save_backup/ui/panel.inc:175` | save_backup | I | controls failed；side import button | 侧栏导入按钮创建失败 | W | promote | 面板部分能力失败；单次 W。 |
| `save_backup/ui/panel.inc:182` | save_backup | I | controls failed；backup row index | 备份行控件创建失败 | W | promote | 面板部分能力失败；行索引可定位。 |
| `save_backup/ui/panel.inc:187` | save_backup | I | controls failed；auto entry | 自动备份条目控件创建失败 | W | promote | 面板部分能力失败；单次 W。 |
| `save_backup/ui/panel.inc:197` | save_backup | I | controls failed；pager arrows | 分页箭头创建失败 | W | promote | 面板部分能力失败；单次 W。 |
| `save_backup/ui/panel.inc:202` | save_backup | I | controls failed；navigation geometry | 导航根几何校验失败 | W | promote | 面板结构不匹配导致当前面板能力受限；单次 W。 |
| `save_backup/ui/panel.inc:210` | save_backup | I | controls failed；navigation button | 导航按钮创建失败 | W | promote | 面板部分能力失败；单次 W。 |
| `save_backup/ui/panel.inc:214` | save_backup | I | component tree created；root pointer | 面板控件树创建成功 | D | demote | 指针身份仅用于内部诊断；不是产品生命周期事件。 |
| `save_backup/ui/panel.inc:219` | save_backup | I | panel enter | 面板打开 | I | keep | 用户可观察的有限状态变化。 |
| `save_backup/ui/panel.inc:237` | save_backup | I | panel symbols not resolved | 面板 hook 符号解析失败 | W | promote | 当前面板能力不可用，游戏可继续；单次失败 W。 |
| `save_backup/ui/panel.inc:253` | save_backup | I | panel enter；mode/duration | 面板初始化完成耗时 | I | keep | 有限面板打开事件，包含模式与耗时。 |
| `save_backup/ui/panel.inc:305` | save_backup | I | panel f3 terminate | F3 终止面板 provider | D | demote | 内部调试/终止过程，不是面向用户的功能状态。 |
| `save_backup/ui/injection.inc:27` | save_backup | I | export succeeded；slot | 用户触发导出成功 | I | keep | 有限用户操作状态变化，包含 slot。 |
| `save_backup/ui/injection.inc:31` | save_backup | I | export failed；slot/error | 用户触发导出失败 | W | promote | 单次备份创建失败且游戏可继续；已有 slot 与错误信息。 |
| `save_backup/ui/injection.inc:43` | save_backup | I | export unavailable；no occupied slot | 未选择有效存档槽而拒绝导出 | W | promote | 单次用户操作拒绝；当前 INFO 应改 W。 |
| `save_backup/ui/injection.inc:103` | save_backup | I | confirmation open failed | 确认弹窗创建失败 | W | promote | 单次 UI 操作失败，游戏可继续。 |
| `save_backup/ui/injection.inc:112` | save_backup | I | import unavailable；invalid selection | 导入选择无效而拒绝 | W | promote | 单次导入被拒，当前 INFO 过低。 |
| `save_backup/ui/injection.inc:137` | save_backup | I | delete unavailable；invalid selection | 删除选择无效而拒绝 | W | promote | 单次删除被拒，当前 INFO 过低。 |
| `save_backup/ui/injection.inc:163` | save_backup | I | public export API not ready | 下载导出 API 不可用 | W | promote | 当前操作能力不可用但游戏继续；单次 W。 |
| `save_backup/ui/injection.inc:175` | save_backup | I | backup dir unresolved | 下载导出备份目录无法解析 | W | promote | 单次导出准备失败，当前 INFO 过低。 |
| `save_backup/ui/injection.inc:184` | save_backup | I | auto backup file missing | 自动备份文件不存在 | D | demote | 可选自动备份缺失是正常情况；按规则记 D，不删除。 |
| `save_backup/ui/injection.inc:190` | save_backup | I | no backup selected | 未选择备份而拒绝下载 | W | promote | 用户请求无效，单次拒绝 W。 |
| `save_backup/ui/injection.inc:207` | save_backup | I | public export API not ready | 从文件导入 API 不可用 | W | promote | 单次用户路径不可用，游戏可继续。 |
| `save_backup/ui/injection.inc:211` | save_backup | I | picker unavailable | 文件选择器不可用 | W | promote | 单次导入失败，游戏可继续。 |
| `save_backup/ui/injection.inc:242` | save_backup | I | export to download failed；name | 下载导出失败 | W | promote | 单次备份文件创建失败，已有 name 定位。 |
| `save_backup/ui/injection.inc:244` | save_backup | I | export to download queued；name | 下载导出请求排队 | I | keep | 用户操作进入异步队列，状态发生变化且有对象名。 |
| `save_backup/ui/injection.inc:264` | save_backup | I | import/delete succeeded | 导入或删除成功 | I | keep | 有限用户操作状态变化。 |
| `save_backup/ui/injection.inc:267` | save_backup | I | import/delete failed；error | 导入或删除失败 | W | promote | 单次备份操作失败但游戏可继续；应 W。 |
| `autosell/scan.cpp:72` | autosell | W | inventory.action sell unavailable | 自动出售能力不可用 | W | augment | 状态转变时只输出静态文本；补 `reason=capability_unavailable|sell_unavailable`，以及 action 标识。 |
| `autosell/scan.cpp:91` | autosell | I | scan gate blocked；frame/reason | 定时扫描门禁阻断 | D | demote | 扫描每次 tick 驱动且逐帧变化，不能 INFO；作为诊断 D，frame 可留。 |
| `autosell/scan.cpp:107` | autosell | I | scan cancelled；frame/epoch | 扫描取消 | D | demote | 每轮取消是调度细节，降 D。 |
| `autosell/scan.cpp:181` | autosell | W | item view skipped；reason/category/space/bag/slot | 扫描单项构造失败 | W | keep | 当前按 reason 一次性去重，W 可保留；正文已有 bag/slot，不再 augment。 |
| `autosell/scan.cpp:191` | autosell | W | no-rule；category/slot/config | 单项无匹配规则 | D | demote | 正常筛选结果不是失败；热扫描路径的逐项诊断只应 D。 |
| `autosell/scan.cpp:208` | autosell | W | no-sell；category/space/bag/slot | 不可出售物品被跳过 | W | keep | 出售拒绝 W 合适；正文已有 bag/slot，不再 augment。 |
| `autosell/scan.cpp:235` | autosell | W | sell stale；space/bag/slot/category | 出售时物品引用过期 | W | keep | 单次出售失败 W 合适；正文已有 slot，不再 augment。 |
| `autosell/scan.cpp:243` | autosell | W | sell empty slot；space/bag/slot/category | 出售时槽已空 | W | keep | 正文已有 slot，不再 augment。 |
| `autosell/scan.cpp:251` | autosell | W | sell no handle；space/bag/slot/category | 出售对象句柄无效 | W | keep | 正文已有 slot，不再 augment。 |
| `autosell/scan.cpp:257` | autosell | W | sell failed；space/bag/slot/category/status | 单次出售失败 | W | keep | 单次失败等级正确；正文已有 slot，不再 augment。 |
| `autosell/scan.cpp:326` | autosell | W | bag source failed；space/code | 扫描某来源失败 | W | keep | 仅错误码变化时记录且可继续扫描；正文已有 code，不再 augment。 |
| `autosell/scan.cpp:374` | autosell | W | native space failed；space/code | 原生背包扫描失败 | W | keep | 单个扫描来源失败可继续；正文已有 code，不再 augment。 |
| `autosell/scan.cpp:414` | autosell | W | scan refs=0；frame | 扫描没有物品引用 | D | delete | 空背包是正常状态且该文案无状态语义；删除。 |
| `autosell/scan.cpp:426` | autosell | W | scan stats；多项扫描计数 | 一轮扫描变化汇总 | I | augment | 一轮统计摘要应为 I（每轮一次），不能 W；当前还只在计数变化时输出，应按轮汇总并包含 skipped。 |
| `autosell/scan.cpp:433` | autosell | I | scan sold/failed；frame | 扫描出售结果汇总 | I | augment | 汇总等级正确，但应每轮一次且包含 `sold/skipped/failed`（当前逐帧只在 sold/failed>0输出，缺 skipped）。 |
| `autosell/scan.cpp:478` | autosell | I | task removed | 定时任务移除 | I | augment | 状态变化但静态消息不能区分原因；补 `reason=` 或 `trigger=`。 |
| `autosell/scan.cpp:511` | autosell | I | task registered；id | 定时任务注册 | I | keep | 生命周期状态变化且有任务 id。 |
| `autosell/scan.cpp:520` | autosell | W | save enter rejected；slot mismatch | 存档进入事件槽号不匹配 | W | augment | 单次拒绝；补 `reason=slot_mismatch`。 |
| `autosell/scan.cpp:525` | autosell | I | save enter；slot/sync task | 存档进入 | I | keep | 有限生命周期事件，有 slot。 |
| `autosell/scan.cpp:531` | autosell | I | save exit; remove task | 存档退出 | I | augment | 状态变化明确但静态文案无法识别事件对象；补 `op=stop reason=save_exit`。 |
| `autosell/scan.cpp:542` | autosell | I | master switch old/new | 自动出售配置启停 | I | keep | 配置确实变化，前后状态明确。 |
| `autosell/scan.cpp:555` | autosell | I | scan timer reset after panel close | 面板关闭后重置扫描计时 | I | delete | 纯实现细节且无状态字段，不是用户可观察功能状态。 |
| `autosell/scan.cpp:560` | autosell | I | global enabled/save active | 全局开关状态 | I | augment | 全局 setter 每次均记录，调用可重复；仅值变化时记录，并加 `feature=autosell`。 |
| `autosell/plugin.cpp:329` | autosell | W | runtime capability acquisition failed | 启用时获取能力失败 | W | augment | 单次启用失败但游戏继续；补 `reason=` 与 capability 标识。 |
| `autosell/store.cpp:221` | autosell | W | retry task removal failed；id/code | 重试任务移除失败 | W | augment | 单次调度操作失败；补 `op=remove_retry_task reason=`。 |
| `autosell/store.cpp:274` | autosell | W | slot data load rejected；presence/version | 配置数据格式拒绝 | W | augment | 单槽加载失败可继续；补 slot 与 `reason=unsupported_format`。 |
| `autosell/store.cpp:325` | autosell | W | catch-up write failed；slot | 追赶写入失败保留 dirty revision | W | keep | 单次写失败可重试；正文已有 slot，不再 augment。 |
| `autosell/store.cpp:373` | autosell | I | slot data write；slot/committed | 配置写入结果 | D | demote | 异步/重试写入可能多次触发，不应 INFO 热路径；失败单次 W、成功只在配置提交状态变化时 I。 |
| `autosell/store.cpp:382` | autosell | I | newer revision remains dirty；slot | 新 revision 仍待重试 | D | demote | 写入流程状态细节，降 D。 |
| `autosell/store.cpp:405` | autosell | W | request status unavailable；request | 请求状态不可用 | W | augment | 保留请求可继续处理；补 `reason=status_unavailable`。 |
| `autosell/store.cpp:414` | autosell | W | unknown request state；state/request | 请求状态未知 | W | augment | 单次状态不匹配；补 `reason=unknown_state`。 |
| `autosell/store.cpp:445` | autosell | W | write request failed；slot/revision/code | 配置写请求失败 | W | keep | 正确等级；正文已有 slot/code，不再 augment。 |
| `autosell/store.cpp:456` | autosell | W | write returned empty request；slot/revision | 写请求返回空句柄 | W | keep | 单次失败但可处理；正文已有 slot，不再 augment。 |
| `autosell/store.cpp:500` | autosell | W | retry deferred；slot/epoch | 重试暂缓 | D | demote | 暂缓重试是正常状态过程，不是失败终态；降 D。 |
| `autosell/store.cpp:538` | autosell | W | re-register blocked；unregister state unavailable | 注销状态不可用阻止重新注册 | W | augment | 单次重试路径受阻；补 `reason=unregister_state_unavailable`。 |
| `autosell/store.cpp:552` | autosell | W | retry scheduler unavailable | 重试调度器不可用 | W | augment | 当前静态文案缺少操作及原因字段；补 `op=schedule_retry reason=scheduler_unavailable`。 |
| `autosell/store.cpp:580` | autosell | W | config write rejected without active session | 无活动槽会话拒绝配置写 | W | augment | 单次请求拒绝；增加 `reason=no_active_session` 与 slot（若有）。 |
| `autosell/store.cpp:584` | autosell | W | config write rejected while session unloaded | 槽数据尚未加载拒绝配置写 | W | augment | 单次请求拒绝；增加 `reason=session_not_loaded slot=`。 |
| `autosell/store.cpp:592` | autosell | W | revision exhausted；slot | 配置 revision 耗尽拒写 | W | keep | 单次失败；正文已有 slot，不再 augment。 |
| `autosell/store.cpp:607` | autosell | W | revision retained for retry；slot | revision 保留等待重试 | D | demote | 当前写入降级/重试状态细节，降 D；若最终重试耗尽再 W/E。 |
| `autosell/store.cpp:618` | autosell | W | unregister failed；code | 槽数据注销失败 | W | keep | 单次失败；正文已有 code，不再 augment。 |
| `autosell/store.cpp:621` | autosell | W | retry task could not resume after unregister failure | 注销失败后重试任务未恢复 | W | augment | 单次调度恢复失败；补 `reason=` 与 slot/request。 |
| `autosell/store.cpp:625` | autosell | W | unregister returned empty request | 注销返回空请求 | W | augment | 单次失败；补 `op=unregister reason=empty_request slot=`。 |
| `autosell/store.cpp:628` | autosell | W | retry task could not resume after invalid unregister result | 无效注销结果后重试未恢复 | W | augment | 单次失败；补原因与 slot。 |
| `autosell/store.cpp:644` | autosell | I | unregister pending；request | 注销异步等待 | D | demote | 中间异步状态非完成状态，降 D。 |

## 缺失日志建议

| file + 函数 | 事件 | 目标等级 | 必填字段 | 理由 |
|---|---|---|---|---|
| `save_backup/usecases/backup_usecases.cpp` + `export_json_with_lease` / `save_backup_autobackup_pre_save` | 手动/自动备份创建最终结果（成功或失败） | I（成功）/ W（单次失败） | `op=create_backup`、`slot=`、`result=`；失败 `reason=`；成功 `file=`/`bytes=` | 当前成功仅局部写成功日志，构包、目录、落盘等早退均无统一最终结果；需一次覆盖整个操作。 |
| `save_backup/transaction/transactions.cpp` + `import_bundle_file` | 备份恢复最终结果（成功或失败） | I（成功）/ W（单次失败）/ E（恢复后存档不可用） | `op=restore`、`slot=`、`result=`；失败 `reason=`；不完整回滚 `attempts=`/`reason=` | 多个错误通过返回 JSON 早退，缺统一恢复结果；回滚不完整可能令存档不可用，应走 E。 |
| `autosell/scan.cpp` + `scan_body` | 自动出售每轮扫描汇总 | I | `sold=`、`skipped=`、`failed=`、`round=`/`frame=` | 当前成功摘要仅 sold/failed 非零时输出，另一条统计 W 且只在计数变化时输出；应由一条每轮 INFO 汇总取代。 |
| `autosell/scan.cpp` + `process_ref` | 受保护物品跳过原因 | W | `reason=protected`、`bag=`、`slot=`、`item=` | 当前 no-sell 统计未明确保护来源，且按类别仅首次警告；需能判断具体受保护物品为何未售。 |

## 通道缺口

- `save_backup/log.h` 仅定义 INFO/WARN/DEBUG；`autosell/log.h` 仅定义 INFO/WARN/DEBUG。二者均没有 ERROR 宏。`infra/log/qol_log.h` 有 `QOL_LOG_ERROR`，但插件 helper 的 domain 与写入入口不提供该宏级别，不能在现有插件通道中发 E。
- 不完整回滚、损坏备份造成恢复后存档不可用等不可恢复失败应为 E；当前只能经 WARN 输出，属于通道缺口，不应把 WARN 视作符合等级契约。
- 范围内 grep 同时核对 `QOL_LOG_`、`log.write(`、`log_info(`、`log_warn(`、`log_error(`、`log_debug(`：插件 `log.h` 通过 `sdk->log.write` 发日志；没有发现 `log_info/log_warn/log_error/log_debug` 额外调用点。`QOL_LOG_ERROR` 只存在公共 logger 定义，不是这两个插件可用宏。

## 统计

- 清单行数：158 条（save_backup 113，autosell 45）。
- 原审计 disposition：keep 45，demote 28，promote 43，delete 5，merge 0，augment 37；按审核裁决驳回三项 delete 后，执行口径为 keep 45，demote 31，promote 43，delete 2，merge 0，augment 37。
- 缺失日志建议：4 条。

## Top 发现

1. 存档恢复/删除回滚不完整可能造成数据不可用，应为 E；两个插件均无 ERROR 宏，当前只能 WARN，属于明确通道缺口。
2. autosell 扫描每帧/定时执行，`scan gate`、取消与逐项分支不应使用 INFO；逐轮结果应由单条 I 汇总 `sold/skipped/failed`。
3. autosell 拒绝/跳过日志需解释不可出售或受保护原因，并保留 `bag/slot/item`；已有 slot 的正文不重复补位置字段。
4. save_backup 多条导出/恢复失败被记为 INFO，逐文件转换/列表诊断又被提升为业务状态；单次失败 W、细节 D，创建及恢复需统一最终结果。
5. save_backup 在恢复 C18/warehouse 时采用跳过或空镜像降级，日志应明确 `fallback=`；而静态无变量的正常缺失/重置文案应删除或补齐状态语义。

## 补全说明

- 本次复扫覆盖 save_backup 真实调用点 113 条：`SB_LOG`（INFO）40 条、`SB_LOG_WARN`（WARN）32 条、`SB_UI_LOG`（INFO）39 条、直接 `SAVE_BACKUP_LOG_INFO`/`SAVE_BACKUP_LOG_WARN` 各 1 条；补入此前遗漏的 `SB_UI_LOG` 调用 39 条。
- autosell 复核 `AUTOSELL_LOG_WARN` 32 条、`AUTOSELL_LOG_INFO` 13 条，共 45 条真实调用，当前清单全部覆盖；其余宏定义行不作为独立日志事件统计。

## 审核裁决

- 已采纳：新增 save_backup ERROR 通道；transactions.cpp 的 rollback incomplete 三处用 ERROR，其余 promote/demote 按上表目标等级执行；scan 426/433 合并为每轮一条 INFO；受保护 no-sell 记录追加 `reason=protected`；全局开关仅状态变化时输出。
- 驳回 `backup_files.cpp:119,184,211` 的 delete，改为 D 保留正常可选缺失诊断；最终仅删除 `autosell/scan.cpp:414,555`。
- 驳回新增备份创建/恢复最终结果建议：现有 exported/imported INFO 已记录最终成功，失败路径由既有 W/E 覆盖。
- 驳回 autosell ERROR 宏：审定 autosell 无不可恢复失败场景，不新增 ERROR 通道。
