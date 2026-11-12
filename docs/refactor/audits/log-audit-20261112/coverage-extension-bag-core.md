# 功能级日志覆盖审计：extension_bag 内核

范围：`module/app/src/main/cpp/plugins/extension_bag/{core,api,runtime,transaction,persistence,usecases}`。按插件描述符/宿主装配、`QolBagSource` 与 inventory action、SlotData、公开 item-use/投影入口枚举功能；用 `logging-standards.md` 的四级语义和有限事件原则评估。`present` 表示有合适的事件证据；`weak` 表示仅覆盖部分生命周期/结果，或记录等级/字段/位置不适合回放；`missing` 表示关键状态结果没有日志。拒绝原因由返回码告知调用方但不进入统一日志，仍视为日志覆盖缺口。

| 功能事件 | 触发条件 | 目标等级 | 现有日志 file:line | 状态 | 建议（含建议文案） |
|---|---|---|---|---|---|
| 插件安装与跨组件能力绑定 | install 时绑定 SDK、item/save/craft/world facts、UI/库存扩展、move-merge、SlotData 等；回滚安装 | I；单能力失败 W，整体不可用 E | `plugins/extension_bag/plugin.cpp:221-240`（回滚无日志）；安装失败静态写日志见 `plugin.cpp:261-319` | weak | 补一次安装成功/回滚终态；单项能力失败日志增加接口/阶段与 reason。示例：`install extension_bag result=ready capabilities=...`、`install extension_bag result=rolled_back reason=<stage>`。 |
| 插件启用/停用 | owner token 更新、注册回调、关准入与注销数据源/投影回调 | I；可降级失败 W | `plugin.cpp:717-791`（成功启停没有状态日志；失败仅 static text `725-766`） | weak | 在启用真正生效和停用完成处各记一次：`start feature=extension_bag result=enabled` / `stop feature=extension_bag result=disabled`；失败日志带 `reason=`、`stage=`，避免静态文本无法区分失败点。 |
| bagCount 配置读取、持久化与应用 | 启动读取设置；异步写入/receipt；运行时调整背包袋数 | I（配置生效）；W（写入失败） | `plugin.cpp:258-324`（配置写入失败只有 static text）；`usecases/set_bag_count.cpp:43-55`（失败有一条 W，成功无 I） | weak | 只在最终生效时记录一次：`apply feature=extension_bag bag_count=<n> previous=<n> result=applied`；失败日志补 `reason=`、`requested=` 和 receipt/result。 |
| QolBagSource 注册/注销 | 插件启停、owner suspend/resume 时注册 bag source；关闭准入后注销 | I（注册状态改变）；W（失败） | `plugins/extension_bag/plugin.cpp:723-777`（enable 注册失败静态日志）；`api/bag_source.cpp`（注册状态切换未见事件日志） | weak | 当前错误路径有静态日志但无成功/注销终态。增加单次：`register source=extension_bag result=registered space=2`、`unregister source=extension_bag result=unregistered`；失败用 `reason=`。 |
| SlotData section 注册/注销 | save capability 注册 `extension_bag` section；停用或 owner 变更时注销 | I（成功）；W（失败） | `persistence/persistence.cpp:351-371`（register 成功/失败、unregister 失败；unregister 成功缺失） | weak | 补注销成功事件并在注册/注销消息带 `section=`、`version=`、`result=`；无需记录每次 query。 |
| SlotData load、迁移与损坏隔离 | 载入缺失 section、兼容旧版本、解析状态/pending 或遇到不可读/不支持版本 | I（load/migrate）；W（会话阻断）；E（不可恢复损坏） | `persistence/persistence.cpp:126-163,173-182,219-231` | present | 现有日志能区分默认状态、迁移、pending 隔离与 parse/version/unreadable 失败；不可读日志建议补 `slot=`，避免缺少存档对象定位。 |
| SlotData capture/complete/保存事务结果 | 保存前序列化，provider 返回 staged/committed/cancelled/degraded/recovery outcome | I（提交状态改变）；W（可重试失败）；E（重试耗尽/数据不可恢复） | `persistence/persistence.cpp:252-255,260-304` | weak | capture INFO 不是最终保存结果；staged/cancelled/original-save-failed 分支静默。只补最终结果一次：`save section=extension_bag slot=<n> tx=<id> result=committed|rolled_back|degraded reason=<reason>`；中间重试需 `attempt=`。 |
| dirty staging / SlotBound 异步写请求 | 状态变更后在安全点提交 `request_write`，等候 provider 请求终态 | W（请求失败）、I（提交终态） | `persistence/persistence.cpp:386-409`（request_write 失败重置 staged 后静默） | missing | 失败会保留 dirty 并重试，但没有可观察证据。仅在安全点请求失败时记 `save section=extension_bag slot=<n> op=stage attempt=<i> reason=<code>`，以及完成时记一次 `save section=extension_bag slot=<n> result=staged|committed`；不要每帧记录 pending。 |
| 通用事务 pending 与回滚 | 三条移动路径建立 pending，失败时恢复逻辑状态/dirty | D（内部阶段）；I（回滚终态）；W/E（回滚失败） | `transaction/transaction_coordinator.cpp:26,31-36,94-103` | weak | pending-recorded 为 D 合适，`txn_abort` 记录了事务 ID、source/destination、reason；但 rollback 被记为 INFO 且不体现阶段。将失败后可恢复的回滚终态定为 W，正文用 `op=tx result=rolled_back tx=... stage=... src=... dst=... reason=...`。 |
| 原版袋 → 扩展袋移动 | 从原版库存导入扩展槽；包括源 identity 校验、容量拒绝、移动提交 | W（单次拒绝）；I（提交） | `transaction/ext_orig.cpp:51-58,79-163` | present | 成功与容量/身份/动作失败均有对应日志及 source/destination；对错误拒绝点已提供 `reason` 或 status，避免再加调用层重复日志。 |
| 扩展袋 → 原版袋移动 | 导出到普通或 native 目标袋，验证 native layout/容量及 inventory action 结果 | W（失败/拒绝）；I（提交） | `transaction/ext_orig.cpp:166-250` | present | 覆盖目标无效/不可用、源为空、动作失败、提交结果；不建议额外在 UI 路由层重复记录同一结果。 |
| 扩展袋 → 扩展袋移动、合并与交换 | 同袋/跨袋移动、堆叠合并、交换，以及物化/投影/释放失败后的回滚 | W（拒绝/失败）；I（事务提交）；D（过程态） | `transaction/ext_ext.cpp:33-38,64-66,130-132,265-430,447-455,473-507,511-514`；swap 完成仅 `ext_ext.cpp:198-201` D | weak | 普通移动/合并有 INFO，交换成功只写 DEBUG，默认日志无法回放；改为一次 `I op=move result=committed mode=swap src=... dst=... tx=...`。合并扫描/计数分支保持 D，不逐内部槽补 INFO。 |
| 公开 move API 参数拒绝 | 外部调用传入 disabled/not-ready、task bag、非法袋/槽、同为原版等无效请求 | W（调用失败但游戏继续） | `usecases/move.cpp:117-166`（只有底层部分路径有记录；API 早期返回静默） | weak | 统一在 API 边界补一条最终拒绝记录，不在每个校验 return 重复写：`reject op=move src=<bag>/<slot> dst=<bag>/<slot> reason=<error>`；成功结果复用具体移动路径的提交日志。 |
| QolBagSource enumerate/export 读取 | 下游枚举物品或导出 transfer payload；不改变库存状态 | D（仅异常诊断）；正常查询不要求 I | `api/bag_source.cpp:129-160,162-202`（仅 enumerate 物化失败有 W） | present | 正常读/导出不是生命周期状态变化，静默合适；枚举无法物化已有 `bag/slot` WARN。不得增加每个枚举项 INFO。 |
| QolBagSource import/remove/replace | 外部 craft/inventory 等跨组件导入、扣减或替换扩展袋物品 | W（拒绝/失败）；I（成功状态改变） | `api/bag_source.cpp:205-382,385-508,510-569`（拒绝及部分回滚失败有日志；成功提交缺失） | weak | 补每次成功 mutation 的单一 INFO，包含操作类型、`bag/slot`、`count`、`result=committed`；持久化失败/回滚已记录处不重复，上层调用者不再记录同一事实。 |
| 所有权 ledger、对象分配/释放/移交 | 物化对象、借用视图、延迟 free、custody 交接与队列 drain | D（逐对象审计）；W（本次失败）；E（所有权不一致/释放不可恢复） | `core/ownership.cpp:104-155,220-299,342-437,452-482,587-783`；跨流程一致性错误见 `core/tabs.cpp:31-53` | present | ledger 与释放路径有对象/handle 级状态和失败证据；逐对象操作不升 INFO。只需确保容量/队列失败信息能关联 `item/handle`。 |
| 扩展物品 use/token 生命周期 | API 取得 use token，调用原版物品/封印/箱子逻辑，再 finish/abort 并可能释放对象 | W（操作失败）；E（token/ownership 状态无法恢复）；I（消耗/状态改变） | `api/api_impl.cpp:127-176`（token mismatch 有 WARN；正常消费/普通失败只有返回信封）；`core/ownership.cpp:587-754` | weak | 成功消耗/最终使用结果缺少回放；在操作结果确定后记一次 `apply op=use bag=<n> slot=<n> item=<id> result=consumed|rejected reason=<token>`。一般 cooldown 等调用结果可由 caller 处理时不重复记录。 |
| 投影视图 install/restore 与 root 重装 | 切入扩展袋视图、恢复原视图、native root 失效后重装 | I（视图启停）；W（局部失败）；E（能力整体不可用） | `core/projection.cpp:344-395,469-490`（安装成功仅 D；失败 W）；`core/orchestrator.cpp:482-498`（恢复失败 E） | weak | install/uninstall 是面板状态变化，默认应可回放；将成功 D 提升为 I，文案如 `open feature=extension_bag bag=<n> result=projected window=<n>` / `close feature=extension_bag result=restored`。恢复失败需区分局部 UI 降级与全能力不可用，避免无依据 E。 |
| 投影槽刷新与对象绑定 | 游戏刷新后发现控件内容被覆盖，重新绑定当前对象/handle | D（逐槽诊断）；W（单槽刷新失败） | `core/projection.cpp:508-525` | weak | 失败日志已有对象定位；`projection post-refresh` INFO 逐槽输出且可能随刷新反复出现，应降 D，并仅在实际绑定变化时输出。 |
| 原版 UI 拖放、触摸路由及 session | item proc/touch release 开始或结束 session；转发给原版或进入扩展路径 | I（session/用户状态变化）；D（坐标、状态机）；W（业务拒绝） | `core/drag_session.cpp:19,46,55`；`core/lifecycle.cpp:162-268,368-410`；`core/drop_routing.cpp:107-237,313-398,478-482` | present | 会话开始/清理/取消有记录，输入细节受 D 门控，拒绝与提交按结果记录；避免在每个内部路由重复相同事务事实。 |
| tab 控件安装/失效/切换与描述窗口 | 插件创建/删除 tab 控件、切换袋、打开描述栏 | I（有限状态变化）；W（单次 UI 操作失败）；D（重复布局观察） | `core/tabs_ui.cpp:203,278,283,378-389,516-544,575-668` | present | 安装、切换、失败及描述操作都有事件证据；重复 cap/re-anchor 仅 D。切换成功日志目前 `bag=` 足以标识对象，不需额外逐次点击日志。 |
| native tab layout/reflow | C18 root/child 校验，布局重排、还原与 resize | I（布局状态改变）；W（失败/拒绝）；D（结构快照） | `core/native_tab_layout.cpp:92,121,246-304,329-396`；`core/render.cpp:162,380` | present | fail-closed、回滚、aborted 与应用状态均可定位 root/generation/child；成功应用只在状态变化处记录。 |
| unequip/adopt 与角色/背包装备跨组件操作 | 原版袋卸下生成袋物品、接收槽持久化、装备扩展角色/背包 | I（提交/成功状态）；W（单次失败）；E（所有权/状态不可恢复） | `core/host_adapter.cpp:87-140,220-305,342-353,368-576` | present | 主要成功、拒绝及回滚路径均有日志和袋/槽/阶段字段；已有 `stage=` 的分支不应再新增重复日志。 |
| lifecycle main-menu 清理与退出恢复 | 回到主菜单时清空缓存、释放安全对象；失败时保留并安排重试 | I（清理完成）；W（可重试/对象保留）；E（状态损坏） | `core/lifecycle.cpp:596,701-711`；`core/orchestrator.cpp:147-165,472-501` | weak | 失败有 WARN，退出细节为 D；清理成功没有一次性终态 INFO。补 `stop op=main_menu_cleanup result=cleared|retained reason=<reason>`，重试后仅记录最终结果。 |
| 跨组件 equip/apply/craft 等 item callback | 通过 public projection/bridge 调用原版 equip/apply，或由 bag source 与其他插件交互 | W（校验拒绝）；I（操作提交）；E（token/所有权损坏） | `public/projection.cpp:123-202,229-394,418-539`；`public/bridge.cpp:109-132`；`api/bag_source.cpp:205-508` | present | apply/equip 有明确拒绝与提交日志，跨组件路径可用 source/destination/target 定位；`extension source protection` 为状态更新 INFO。避免给每种内部 callback 各加入口/出口日志。 |

覆盖结论：共 25 个功能事件，present 10、weak 14、missing 1。

## 优先补全项

1. **SlotBound 写请求失败/终态**：`persistence/persistence.cpp:401-408` 的失败保留 dirty 并重试但完全静默；缺少持久化排障证据，优先级最高。
2. **QolBagSource mutation 提交结果**：`api/bag_source.cpp` import/remove/replace 成功会改变袋状态但无统一提交日志；加单条 mutation INFO 支持外部状态回放。
3. **投影视图开关结果**：`core/projection.cpp:469` install 成功只有 DEBUG，关闭 debug 时无法知道用户进入/离开扩展袋视图。
4. **Bag count 配置最终生效**：配置应用是用户可见状态变化；现有写失败多为静态文本，成功选择和持久化 receipt 不可回放。
5. **扩展袋 swap 成功**：`transaction/ext_ext.cpp:198-201` 把已完成交换记为 DEBUG；需要有限次数 I 才能还原事务结果。
6. **插件启停/数据源注册终态**：关键能力注册失败会关闭插件，但没有稳定的成功启用/停用事件，且失败行缺具体 stage。
7. **SlotData 保存事务最终结果**：capture INFO 不是 provider commit；需区分 staged、committed、cancelled、degraded，避免把保存准备误认为落盘成功。
