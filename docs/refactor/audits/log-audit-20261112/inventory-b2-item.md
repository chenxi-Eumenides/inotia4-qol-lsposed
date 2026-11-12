# 日志审计清单：批次 2（core.item + core.inventory）

依据 `docs/reference/logging-standards.md` 与 `docs/reference/logging.md`。审计范围为两个插件生产 Native 日志；不含 tests、Kotlin 与 extension_bag。domain 均为 `inventory`（core.item 生命周期及 state 直接 SDK 日志分别为 `core` 域/插件域，见对应行）。

## 现有日志

| file:line | domain | level_now | message 摘要 | event_class | level_target | disposition | rationale |
|---|---|---:|---|---|---:|---|---|
| `core.item/hooks/inventory_hooks.cpp:173` | inventory | E | hook backup missing；hook | hook backup 缺失 | W | demote | 单个 hook backup 不可用但游戏可继续；应为 W，并补 `reason=backup_missing`。与 core.inventory 同文案但 hook 标识各异，合并没有必要；统一结构字段即可。 |
| `core.item/hooks/inventory_hooks.cpp:362` | inventory | D | UnequipItemToInven 每次调用观测；slot/result/recursive | 高频 hook 调用结果 | D | keep | DEBUG 有装备槽、结果、递归标记区分事件；wrapper 每次调用时 debug 开关/日志门控避免生产默认噪声。 |
| `core.item/hooks/inventory_hooks.cpp:366` | inventory | E | UnequipItemToInven failed；equip_slot | 单次卸装失败 | W | demote | 失败影响本次操作、游戏可继续；补 `reason=`。 |
| `core.item/hooks/inventory_hooks.cpp:590` | inventory | D | Unequip adopt；category/space/bag/slot | 扩展袋卸装写入成功 | D | keep | 有物品类别与目标位置，可区分成功写入。 |
| `core.item/hooks/inventory_hooks.cpp:630` | inventory | I | S2 Divide source writeback；item/count | 物品拆分源堆计数写回 | D | demote | Divide wrapper 每次调用均可能执行，不能用 INFO；定位 item 与数量字段齐备，降为 D。 |
| `core.item/hooks/inventory_hooks.cpp:634` | inventory | E | S2 Divide source mismatch skip；pre/post/expected | 原版计数写入不符而跳过修复 | W | demote | 单次写侧不匹配，采取 fail-closed 跳过且游戏继续；应 W 并加 `reason=original_write_mismatch`。 |
| `core.item/hooks/inventory_hooks.cpp:645` | inventory | I | S2 Divide new item writeback；item/count | 拆分新物品计数写回 | D | demote | 物品使用/拆分 wrapper 每次调用可发生；不应 INFO，D 有 item/count 区分。 |
| `core.item/hooks/inventory_hooks.cpp:844` | inventory | D | provider_equip 成功；category/target/old_count/bag/slot | 扩展源装备成功 | D | keep | 有来源与目标、旧槽数量，属于可按需诊断的单次操作。 |
| `core.item/hooks/inventory_hooks.cpp:864` | inventory | E | provider equip failed；bag/slot/equip_slot | 单次扩展源装备失败 | W | demote | 失败只影响本次装备；`reason=` 当前被内部多处失败路径汇总丢失，后续应传递失败原因。 |
| `core.item/hooks/inventory_hooks.cpp:877` | inventory | W | equip rejected registered-source item；bag/slot | 不支持原版路径处理注册源物品 | W | keep | 本次装备被拒并由 provider 路径承接；字段可定位槽位。 |
| `core.item/hooks/inventory_hooks.cpp:886` | inventory | I | EquipItemFromInvenToSlot；bag/slot/equip_slot/result | 原生物品装备结果 | I | augment | 每次操作都输出，不是有限生命周期变化；需移至 D；失败应另有 W，结果值需映射为明确 outcome/reason。 |
| `core.item/hooks/inventory_hooks.cpp:1025` | inventory | I | sell button blocked；button | 出售按钮被拦截 | I | augment | UI 按钮调用频次不保证有限；改 D 或仅记录状态变化，补拦截 `reason`/操作对象槽位。 |
| `core.item/hooks/inventory_hooks.cpp:1041` | inventory | I | vanilla sell c18 popup；price | 打开原版出售确认弹窗 | I | keep | 有限用户交互状态变化，带展示价格。 |
| `core.item/hooks/inventory_hooks.cpp:1071` | inventory | W | mercenary seal use unsupported；bag/slot | 单次注册源物品使用不支持 | W | keep | 单次操作失败但游戏可继续；位置明确，宜补 `reason=unsupported`。 |
| `core.item/hooks/inventory_hooks.cpp:1174` | inventory | W | vanilla destroy blocked registered-source item；bag/slot | 原版销毁注册源物品被拒 | W | keep | 防止错误路径修改 provider 数据；有槽位定位。 |
| `core.item/hooks/inventory_hooks.cpp:1222` | inventory | I | vanilla sell backup reason=no_sell；bag/slot/category | 不可出售项走原版备份路径 | I | demote | 每次出售尝试可发生，且这属于正常拒绝分支；D 更合适。 |
| `core.item/hooks/inventory_hooks.cpp:1239` | inventory | I | vanilla sell backup quote_unavailable；bag/slot/category | 报价不可用时回退原版 | I | demote | 每次操作的降级分支不应 INFO；降 D，并注明 canonical/原因字段。 |
| `core.item/hooks/inventory_hooks.cpp:1251` | inventory | I | vanilla sell preview；bag/slot/price/count | 出售预览 | I | keep | 用户操作阶段状态且字段充分；若此路径每次按键反复触发，应去重/改 D。 |
| `core.item/hooks/inventory_hooks.cpp:1266` | inventory | E | sell credit failed；bag/slot/price | 单次出售金币入账失败 | W | demote | 单次失败仍可继续游戏，但可能影响交易一致性；需 W 并补 `reason`/错误码。 |
| `core.item/hooks/inventory_hooks.cpp:1275` | inventory | E | sell stale before remove；refunded | 出售前对象已变化且退款成功 | W | demote | 已退款且仅本次出售失败；W 合适，补明确 `reason=stale_item`。 |
| `core.item/hooks/inventory_hooks.cpp:1278` | inventory | E | sell stale before remove；refund_failed/code | 对象变化且退款失败 | E | keep | 钱已入账、对象未扣除且退款失败，出现经济数据不一致需人工处理；补价格及结构化 `reason`。 |
| `core.item/hooks/inventory_hooks.cpp:1288` | inventory | E | sell remove failed；refunded | 扣物失败且退款成功 | W | demote | 退款成功恢复一致，仅本次操作失败。 |
| `core.item/hooks/inventory_hooks.cpp:1291` | inventory | E | sell remove failed；refund_failed/code | 扣物及退款均失败 | E | keep | 金币/物品状态可能不一致，E 合理；补 `reason` 与实际返回码。 |
| `core.item/hooks/inventory_hooks.cpp:1298` | inventory | I | vanilla sell committed；bag/slot/category/count/price | 出售最终成功 | I | keep | 明确最终提交状态，字段可复盘。 |
| `core.item/hooks/inventory_hooks.cpp:1380` | inventory | D | CHAR_ProcessShortcut registered-source；category/shortcut | 快捷键由注册源物品满足 | D | keep | 有类别与快捷键区分事件，且限于 debug trace。 |
| `core.item/hooks/inventory_hooks.cpp:1389` | inventory | D | CHAR_ProcessShortcut；character/shortcut/result/state/state_calls | 快捷键处理汇总结果 | D | keep | 按单次外层调用汇总多次状态查询，避免重复；具有调用区分字段。 |
| `core.item/hooks/inventory_hooks.cpp:1431` | inventory | D | CHAR_UseItemEx trace；trace/character/item/flag/result | 物品使用 wrapper 结果 | D | keep | debug 开关与活动 trace 门控，字段能区分调用；非无参数重复。 |
| `core.item/hooks/inventory_hooks.cpp:1550` | inventory | I | diagnostic hook bound；resource/backup | 诊断 hook backup 绑定 | I | keep | 绑定是有限生命周期状态变化，带 hook 和 backup。 |
| `core.item/hooks/inventory_hooks.cpp:1555` | inventory | E | unknown resource id | hook 资源 ID 未识别 | W | demote | 单个资源绑定请求失败，功能可局部降级；补 `reason=unknown_resource`。 |
| `core.item/usecases/sell_item.cpp:227` | inventory | E | inventory sell rollback failed；source/space/bag/slot/price/refund_code | 出售回滚失败 | E | keep | 回滚失败意味着物品/货币状态不一致；已有 refund_code，但建议补 `reason`/remove 结果。 |
| `core.item/usecases/sell_item.cpp:232` | inventory | W | inventory sell removal failed; refund succeeded；source/位置 | 出售扣除失败并成功退款 | W | keep | 单次失败且已恢复一致；正文可结构化并补 `reason`。 |
| `core.item/usecases/item_action.cpp:126` | inventory | D | inventory resolve provider ref；输入/输出/结果/身份 | provider item resolve 诊断 | D | keep | 多字段区分输入与输出，适合 debug。 |
| `core.item/usecases/item_action.cpp:135` | inventory | W | inventory resolve stale ref；输入/输出/身份 | 对象引用过期 | W | keep | 单次引用失效，游戏可继续；字段充分。 |
| `core.item/api/state.cpp:28` | core | W | snapshot serialize failed（静态） | 快照序列化失败 | W | augment | 单次状态快照失败可继续；静态无变量文案应改为结构化 `fail op=snapshot reason=serialize_failed` 并携带错误原因。 |
| `core.item/runtime/lifecycle.cpp:29` | core | I | 生命周期 message（动态） | 插件生命周期状态 | I | keep | 该写点输出调用方提供的生命周期事件；每条具体消息应有可辨识状态字段。 |
| `core.inventory/hooks/storage_hooks.cpp:109` | inventory | E | hook backup missing；hook | hook backup 缺失 | W | merge | 与 core.item 同文案、同单 hook 失败事实语义：保留一个共同结构化事件约定（hook/reason），各调用点仍需独立报告不同 hook 身份，不删除有用故障定位。 |
| `core.inventory/hooks/storage_hooks.cpp:196` | inventory | W | registered source consume failed；space/bag/slot/count/code | 单次扩展源扣除失败 | W | augment | 等级正确且操作字段齐备；将 `code` 映射为 `reason`，避免只有数值错误码。 |
| `core.inventory/hooks/storage_hooks.cpp:478` | inventory | D | S2 writeback degenerate skip；op/target/delta | 特定 delta 下跳过计数修正 | D | keep | op、对象与 delta 可区分；只在特殊分支输出。 |
| `core.inventory/hooks/storage_hooks.cpp:487` | inventory | E | S2 writeback mismatch skip；op/target/pre/post/expected | 原版计数写入不匹配 | W | demote | fail-closed 跳过单次修正，游戏可继续；降 W 并补 `reason=original_write_mismatch`。 |
| `core.inventory/hooks/storage_hooks.cpp:497` | inventory | E | S2 facade write failed；op/target/expected | 单次数量 facade 写入失败 | W | demote | 单物品写回失败未导致整体能力不可用；应 W 并携带 API 错误原因。 |
| `core.inventory/hooks/storage_hooks.cpp:503` | inventory | D | S2 merge writeback；op/target/count/field | 合并计数写回结果 | D | keep | operation、目标与前后计数区分清楚。 |
| `core.inventory/hooks/storage_hooks.cpp:588` | inventory | I | RemoveItemData count writeback；category/count/item | 删除操作计数写回成功 | I | demote | 此 hook 每次移除可触发，不能 INFO；D 带 item/category/count。 |
| `core.inventory/hooks/storage_hooks.cpp:833` | inventory | E | SaveItemDirect eb failed；origin/item/位置/code/category/count/payload | 单次背包写入失败 | W | demote | 本次存储失败可继续游戏，非整体不可用；保存操作单次失败应 W，补 `reason`。 |
| `core.inventory/hooks/storage_hooks.cpp:841` | inventory | D | SaveItemDirect eb imported；origin/item/位置/category/count | 扩展背包写入成功 | D | keep | 写入对象及位置明确，适合按需诊断。 |
| `core.inventory/hooks/storage_hooks.cpp:852` | inventory | W | INVEN_FindItem dispatch unavailable；category | 查找 dispatch 不可用 | W | augment | 单个方法/dispatch 查找失败应 W；缺 `reason`，补 hook/symbol 与 API 状态。 |
| `core.inventory/hooks/storage_hooks.cpp:940` | inventory | D | INVEN_ConsumeItem call；call/item/extension/位置/recursive/backup | 消耗 hook 入口观测 | D | keep | 有递增 call id 与对象/位置；明确 trace_enabled 门控，不是重复无参 DEBUG。 |
| `core.inventory/hooks/storage_hooks.cpp:991` | inventory | E | ConsumeItem item removed unexpectedly；对象/位置/原数量 | 原版消耗意外移除对象 | E | augment | 注释表明对象已释放且与预期行为矛盾，潜在库存数据损坏；保留 E，补 `reason=unexpected_item_removal` 和操作数量。 |
| `core.inventory/hooks/storage_hooks.cpp:1005` | inventory | I | ConsumeItem borrow writeback；item/count | 单次消耗计数修复成功 | D | demote | 每次消耗 wrapper 可发生；改 D，保留 item/count。 |
| `core.inventory/hooks/storage_hooks.cpp:1014` | inventory | E | ConsumeItem borrow mismatch；item/pre/post/restored | 修复不匹配后回滚原字段 | W | demote | 状态已恢复原值，单次偏差可继续；W 并补 `reason`。 |
| `core.inventory/hooks/storage_hooks.cpp:1022` | inventory | E | ConsumeItem extension consume failed；bag/slot/item | 单次扩展物品消耗失败 | W | demote | 单次消费失败但游戏继续；补 `reason`/source 返回码。 |
| `core.inventory/hooks/storage_hooks.cpp:1027` | inventory | D | ConsumeItem completion；call/dispatched/extension/位置/borrow | 消耗 hook 结果汇总 | D | keep | call id 与阶段字段明确，且 debug trace 门控。 |
| `core.inventory/hooks/storage_hooks.cpp:1051` | inventory | D | RemoveItem call；call/item/recursive/backup/dispatch/branch/result | 移除 hook 汇总 | D | keep | 汇总调用链状态并带 call id，避免子调用逐次刷屏。 |
| `core.inventory/hooks/storage_hooks.cpp:1065` | inventory | D | RemoveItem 每 60 次观测；call/item/recursive/backup | 高频移除 hook | D | keep | 明确周期抽样且带单调 call id、对象指针，不是无区分重复。 |
| `core.inventory/hooks/storage_hooks.cpp:1137` | inventory | D | FindSaveSlot resolved；category/space/bag/slot/merge | 背包目标槽解析 | D | keep | 目标槽与合并状态明确。 |
| `core.inventory/hooks/storage_hooks.cpp:1172` | inventory | D | SaveItem direct_eb；item/backup | 直接扩展背包保存分支 | D | keep | 分支选择含 item/backup 区分。 |
| `core.inventory/hooks/storage_hooks.cpp:1179` | inventory | D | SaveItem direct_eb_failed；item | 保存失败分支选择 | D | delete | 同一失败已在 833 记录带 origin/位置/错误码；此无原因的二次汇总只重复事实。 |
| `core.inventory/hooks/storage_hooks.cpp:1185` | inventory | E | INVEN_SaveItem failed；category/direct_failed/backup | 本次原版存储失败 | W | demote | 单次背包写入失败且游戏可继续；同一失败在 direct import 失败时已由 833 记录，应合并由 833 作唯一 W 记录。 |
| `core.inventory/hooks/storage_hooks.cpp:1240` | inventory | D | SaveItemDirect eb rejected；item/bag/slot | 非预期上下文的扩展保存拒绝 | D | keep | 受深度门控的诊断分支，字段足以定位对象与目标。 |
| `core.inventory/hooks/storage_hooks.cpp:1292` | inventory | I | RemoveItemData registered-source fallback；category/need/consumed | 物理库存不足转注册源补扣 | I | keep | 降级路径实际生效且含数量，符合 INFO。 |
| `core.inventory/hooks/storage_hooks.cpp:1311` | inventory | W | MoveItem rejected registered-source item；bag/slot | 注册源物品不能走原版移动 | W | keep | 单次操作拒绝，槽位明确；补 `reason=provider_owned`。 |
| `core.inventory/hooks/storage_hooks.cpp:1330` | inventory | D | MoveItem passthrough；item/count/target/result | 原版移动路径结果 | D | keep | 每次移动有 item/count/target 区分，等级符合热路径要求。 |
| `core.inventory/hooks/storage_hooks.cpp:1436` | inventory | I | storage hooks attached；hooks=12 | storage hook 安装成功 | I | keep | 生命周期安装里程碑；建议字段改 `hook_count=12` 并对应每个 hook 安装状态。 |
| `core.inventory/hooks/storage_hooks.cpp:1479` | inventory | E | unknown storage resource id | 未识别存储资源 ID | W | demote | 单个资源绑定失败而非整体能力失效；补 `reason=unknown_resource`。 |
| `core.inventory/transfer/move_item.cpp:224` | inventory | E | transfer target mismatch；目标/item/count/expected | 单次物品转移目标校验不符 | W | demote | 该次事务可拒绝并回滚，能力仍可继续；若下方 rollback 确认失败另由 248 保留 E；补 `reason=target_mismatch`。 |
| `core.inventory/transfer/move_item.cpp:248` | inventory | E | transfer rollback failed；源/目标/item/count | 物品转移回滚失败 | E | augment | 回滚失败可造成库存状态不一致，E 合理；必须补 `reason` 及源/目标操作结果码。 |
| `core.inventory/plugin.cpp:90` | core.inventory | 动态 | logger helper 将 message 写到 SDK | 插件日志出口（调用点不明） | — | keep | 此处是通用日志出口而非独立业务日志，逐条事件由调用方决定；不计入调用点统计。 |

## 缺失日志建议

| file + 函数 | 事件 | 目标等级 | 必填字段 | 理由 |
|---|---|---|---|---|
| `core.item/hooks/inventory_hooks.cpp` + `char_use_item_ex_wrapper` / `char_process_shortcut_wrapper` | 物品使用最终结果（成功/拒绝/失败） | I（状态变化时）/ W（失败） | `item=`、`bag=`、`slot=`、`count=`、`result=` 或 `reason=` | 当前 CHAR_UseItemEx 仅在活跃 debug trace 时记录；生产日志无法回放最终操作结果。 |
| `core.item/hooks/inventory_hooks.cpp` + `equip_item_from_inven_to_slot_wrapper` / `provider_equip_item` | 物品移动/装备最终结果 | I（成功）/ W（失败） | `item=`、`from_bag=`、`from_slot=`、`to_slot=`、`result=`/`reason=` | 普通装备当前仅有每次 INFO 的数字结果，provider 装备失败把多个失败原因压成同一条；需结构化最终结果。 |
| `core.item/hooks/inventory_hooks.cpp` + `vanilla_sell_commit` | 出售从预览到最终提交/取消的结果 | I（提交或状态变化）/ W（单次失败） | `bag=`、`slot=`、`item=`、`count=`、`price=`、`result=`/`reason=` | 当前 commit 有成功与部分财务/删除失败日志，但缺少统一最终结果及取消/拒绝结果。 |
| `core.item/hooks/inventory_hooks.cpp` + hook claim/install 与恢复回调 | backup 建立和恢复结果 | I（成功）/ W（失败） | `hook=`、`op=install|restore`、`result=`、失败时 `reason=` | 目前主要记录 backup 缺失和少数 diagnostic hook bind；其余 hook 的建立/恢复结果不可审计。 |
| `core.inventory/hooks/storage_hooks.cpp` + `storage_hooks_attach` | 各 storage hook 的安装/backup 建立结果 | I（成功）/ W（失败） | `hook=`、`op=install`、`result=`、失败时 `reason=` | 目前只输出汇总 `hooks=12`，且失败可能仅返回 false，无法指出哪个 hook 不可用。 |
| `core.inventory/hooks/storage_hooks.cpp` + `find_save_slot` / `save_item_direct_wrapper` | 库存容量不足及拒绝原因 | W | `op=save_item`、`category=`、`count=`、`capacity=`、`used=`、`reason=inventory_full` | 找不到空槽/可合并槽是用户可见的单次写入拒绝，应明确容量原因；正常空槽缺失探测不逐槽记日志。 |
| `core.inventory/hooks/storage_hooks.cpp` + `move_item_wrapper` | 移动物品最终结果及容量拒绝 | I（成功）/ W（失败） | `item=`、`from_bag=`、`from_slot=`、`to_bag=`、`to_slot=`、`count=`、`result=`/`reason=` | 当前只有 DEBUG passthrough；业务移动的提交成功、失败与容量拒绝无法从默认日志判断。 |
| `core.inventory/transfer/move_item.cpp` + `move_item` | 事务提交/正常回滚结果 | D（成功细节）/ W（本次失败） | `from=`、`to=`、`count=`、`result=`、失败 `reason=` | 目前只在目标不符或回滚失败时记录，普通事务最终结果不完整；单次失败可恢复用 W。 |

## 统计

- 清单行数：66 条（core.item 35 条、core.inventory 31 条；含动态生命周期/通用 plugin logger 出口行各 1 条）。
- disposition：keep 35，demote 22，promote 0，delete 1，merge 1，augment 7。
- 补充建议：8 条。

## Top 发现

1. hook 安装/backup 缺失、单次保存/消费/装备失败多被记为 E；按能力是否整体不可用判定，多数应降为 W，只有交易回滚失败或状态不一致保留 E。
2. item 的 Divide 写回和 inventory 的 Consume/Remove 写回在高频 wrapper 内使用 INFO；逐次成功应降到带对象/数量的 D。
3. `core.item` 与 `core.inventory` 共用 `hook=%s backup missing` 语义，应统一为 `fail hook=<id> reason=backup_missing`，但保留不同 hook id 的独立事件。
4. core.inventory 的 SaveItemDirect 失败同时有详细 W/E 以及 `direct_eb_failed`、`INVEN_SaveItem failed` 汇总；建议只留一个具备位置与原因的单次失败记录。
5. 移动、使用、出售尚缺生产环境可见的最终结果；库存无容量时也没有明确拒绝原因，建议补充结构化结果/容量字段。

## 审核裁决

按最终裁决实施；本节覆盖下列指定行，其他清单行结论不变。

### 等级调整

- E→W：`core.item/hooks/inventory_hooks.cpp:173,366,634,864,1266,1275,1288,1555`；`core.inventory/hooks/storage_hooks.cpp:109,487,497,833,1014,1022,1185,1479`；`core.inventory/transfer/move_item.cpp:224`。
- I→D：`core.item/hooks/inventory_hooks.cpp:630,645,886,1025`；`core.inventory/hooks/storage_hooks.cpp:588,1005,1222,1239`。
- `core.inventory/hooks/storage_hooks.cpp:1179` 保留 D：分支选择诊断和 833 的失败正文不同，不删除。

### 正文增强

- `core.item/hooks/inventory_hooks.cpp:173`、`core.inventory/hooks/storage_hooks.cpp:109`：分别保留调用点，末尾追加 `reason=backup_missing`，不跨文件合并。
- `core.item/api/state.cpp:28`：改为 `fail op=snapshot reason=serialize_failed`。
- `core.inventory/hooks/storage_hooks.cpp:196`：保留 `code=%d`，末尾追加 `reason=consume_failed`。
- `core.inventory/hooks/storage_hooks.cpp:852`：末尾追加 `reason=dispatch_unavailable`。
- `core.inventory/hooks/storage_hooks.cpp:991`：保留 E，末尾追加 `reason=unexpected_item_removal`。
- `core.inventory/transfer/move_item.cpp:248`：保留 E，末尾追加 `reason=rollback_failed`。
- `core.item/hooks/inventory_hooks.cpp:864`：保持 W，末尾追加 `reason=provider_equip_failed`。
- `core.inventory/hooks/storage_hooks.cpp:833`：失败行追加 `capacity=<capacity>` 与 `reason=inventory_full`；该分支已有 bag info capacity。

### 采纳的缺失日志

- `core.item/hooks/inventory_hooks.cpp` / `char_use_item_ex_wrapper`：仅当原版返回失败时输出一次 W：`op=use_item result=failed bag=<n> slot=<n> item=<ptr> reason=native_use_failed`；成功不新增 INFO。
- `core.inventory/hooks/storage_hooks.cpp` / `import_registered_item_direct`：利用现有 833 失败记录追加容量及 `reason=inventory_full`，不新增独立日志。
- 驳回其余缺失建议：出售最终提交/失败、hook 建立、事务结果已有对应日志；移动成功不新增 INFO，1330 保持 D。
