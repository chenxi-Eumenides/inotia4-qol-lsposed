# 功能级日志覆盖审计：extension_bag 外层

审计范围：`module/app/src/main/cpp/plugins/extension_bag/` 根目录及 `assembly/`、`model/`、`public/`。按 `docs/reference/logging-standards.md` 对功能生命周期、失败后果与日志频率审查；`api/`、`persistence/` 仅作为外层功能的接口落点查阅，不作其内部逐条审计。D 用于诊断过程，I 仅用于有限状态变化，W 用于可恢复/可降级失败，E 用于能力整体不可用或数据不可恢复。

插件描述符由 `plugin.cpp:868-895` 声明 8 个必需能力、4 个 ports、两个 Feature；`plugin_install`、`plugin_on_enabled`、`plugin_on_disabled`、`plugin_deactivate` 管理插件生命周期；`api/bag_source.cpp` 提供袋源原语；`persistence/persistence.cpp` 通过 `save.slot_data` 加载与捕获状态；`assembly/projection_registration.cpp` 注册投影回调；`model/virtual_bag_state.h:91-108` 承载 bag/slot、pending 事务与隔离记录。

## 功能事件覆盖

| 功能事件 | 触发条件 | 目标等级 | 现有日志 file:line | 状态 | 建议（含建议文案） |
|---|---|---|---|---|---|
| 插件安装前置能力/ports 校验失败 | `plugin_install` 收到无效 context、必需 port/capability 不可用或 ABI 不匹配 | E（安装被拒、插件能力不可用） | `plugin.cpp:426-596` 多个失败返回；部分末端失败记录在 `plugin.cpp:607,621` | weak | 多个安装拒绝路径静默返回，已有两条静态日志缺少阶段与原因。每个最终拒绝点统一结构化记录：`fail feature=extension_bag op=install stage=<stage> reason=<reason>`；避免 capability 本身错误重复记两次。 |
| 插件安装成功并具备数据面 | `plugin_install` 完成袋源、生命周期参与者、slot_data 注册 | I | 无成功日志；完成路径见 `plugin.cpp:645-662` | missing | 增加一次安装完成里程碑：`install feature=extension_bag result=ready bag_source=1 slot_data=1`。 |
| 启用状态实际切换 | `plugin_on_enabled` / `extension_bag_toggle` 成功打开袋源、投影回调与运行门禁 | I | 无成功状态日志，最终状态写入见 `plugin.cpp:773-778,829-836` | missing | 仅在状态确实切换后记录：`apply feature=extension_bag enabled=1 bag_count=%d projection=registered`；避免把每次重复调用都记成 INFO。 |
| 启用过程失败并回退关闭 | slot_data、袋源或投影回调重注册失败 | W | `plugin.cpp:725-728,750-756,763-771` | weak | 已有日志但正文是静态句子，缺阶段、原因和降级结果。追加结构化字段，例如 `fail feature=extension_bag op=enable stage=bag_source reason=register_failed fallback=disabled`。 |
| 停用/卸载生命周期 | `plugin_on_disabled` 或 `plugin_deactivate` 注销袋源、投影、slot_data 并关闭 gates | I | `plugin.cpp:665-714,780-791` 无状态结果日志 | missing | 在全部关闭后记录一次 `stop feature=extension_bag result=disabled bag_source=unregistered projection=unregistered`；仅生命周期真正结束时输出。 |
| 声明式资源装配与资源恢复 | host acquire/restore popup、hook、patch 资源 | I 成功；W 单项失败 | `assembly/claims.cpp:212,219` 仅 popup restore 失败；资源定义在 `assembly/claims.cpp:254-583` | weak | 当前仅覆盖 popup restore 失败，且没有资源安装成功/装配终态。资源成功只在装配阶段一次记录 `install feature=extension_bag hook=%s result=installed`；单项失败记录 `fail feature=extension_bag hook=%s reason=...`，不要在每帧 ready 重试中输出 INFO。 |
| 袋源注册/注销可用性 | core.item 注册表接入或注销扩展袋 source | I 注册成功；W 注册失败 | `plugin.cpp:389-399,603-611,748-758`；注册实现 `api/bag_source.cpp:700-718` | weak | 根日志只记录失败且为无字段静态文案，无注册成功/注销状态。建议一次 `register feature=extension_bag source=inventory.bag_source result=registered`，失败附 `reason=`；正常注销仅在状态变化时记 `unregister ... result=removed`。 |
| slot_data 来源状态加载/迁移 | 当前存档 slot 首次加载、解析完成或 legacy 版本迁移 | I 加载/迁移成功；W 单次不可读；E 不可恢复损坏 | `persistence/persistence.cpp:126-129,157-163,219-231` | present | 已覆盖 missing 默认状态、迁移和解析成功，也有 unreadable/version/parse_failed 阻断日志；成功日志带 slot/epoch，满足关键状态回放。解析失败当前 W 表达“损坏后阻断会话”，若确认无法恢复应按不可恢复结果采用 E。 |
| slot_data 保存快照捕获与完成 | 保存事务调用 capture，协调器随后提交/取消/降级 | I capture/提交；W 可重试失败；E 不可恢复失败 | `persistence/persistence.cpp:238-256,260-305` | weak | 有 capture INFO 和 pending/degraded W，但 capture 参数非法、容量不足、validate 失败会直接返回；complete 的 committed/cancelled/original-save-failed 无终态记录。建议只对最终结果记 `save op=slot_data slot=%d tx=%llu result=committed|failed reason=...`；不要另记逐帧 staging。 |
| bagCount 配置读取与 fallback | install/enable 读取 JSON 或旧 choice，非法配置时禁用扩展袋 | W 配置不可读且保留/回退；I fallback 生效 | `plugin.cpp:344-375` | weak | 目前只有不可用时静态日志，且 `fallback` 生效没有事件记录。失败补 `section=extension_bag key=bagCount reason=read_failed fallback=choice|disabled`；仅发生 fallback 时记 I，不为默认值未配置报警。 |
| bagCount 配置写入与 receipt | 用户更改数量后异步提交配置并轮询 receipt | I 持久化成功；W 请求/持久化失败 | `plugin.cpp:258-288,291-325` | weak | 已有失败日志但都是固定句子，无法区分请求、receipt 与最终持久化失败；没有成功落盘确认。增加关键最终结果：`save section=extension_bag key=bagCount result=stored value=%d`；失败用 `reason=`，保留 `runtime_value` fallback 字段。 |
| bag_count 运行态应用 | logic-pre 将已选 bag count 应用到当前 state，或清理对象失败而拒绝 | I 实际变化；W 拒绝 | `plugin.cpp:378-386`；usecase 失败例 `usecases/set_bag_count.cpp:44-47` | weak | 当前仅有缩容清理失败日志，成功应用及其他拒绝/持久化失败无明确生命周期结果。只在 count 真正改变后记录 `apply feature=extension_bag section=bagCount old=%d new=%d result=applied`；失败字段包括 `reason=`。 |
| inventory.projection 回调注册/注销 | 启用时接入/退出 projection registry | I 成功；W 单次注册失败 | `assembly/projection_registration.cpp:149-161` 无日志；调用方失败日志 `plugin.cpp:763-771,822-828` | weak | assembly 仅返回 QolResult，状态边界没有成功确认；上层失败文案不含 `reason`。在调用方集中记录一次 `register feature=extension_bag hook=inventory.projection result=registered` 或 `fail ... reason=%d`，不要在 assembly 与调用方重复记录。 |
| 袋源读取/枚举/变更操作结果 | core.item 查询/枚举、导入/移除/替换/使用 item | W 单次失败；I 仅实际写状态 | `api/bag_source.cpp:142-145,410-413,516-579,599-605,647-678` | weak | 关键失败及 replace 成功有日志，但 item-at/export/import/remove 成功多数静默，且 API 返回错误本身没有统一日志是合理的。功能级审计缺最终操作结果回放：仅在核心成功变更点补单次 I `apply op=bag_item action=<import|remove|replace|use> bag=%d slot=%d result=committed`；失败只补当前没有下层记录且影响调用方诊断的分支，勿为普通查询空结果加日志。 |
| 投影状态/快照同步与重建 | 原版显示窗口写入投影、同步槽或 root 失效后重建 | I 安装/恢复；W 单次同步失败；D 详细快照 | `public/projection.cpp:643-647` 同步失败；`public/bridge.cpp:124-143` 保存槽切换清理失败 | weak | `sync_projected_item_control` 的失败带 bag/slot，适合 W；公开投影成功同步和视图重建完成没有状态事件。增加一次 view install/restore 状态日志 `apply op=projection result=installed bag=%d window=%d` / `restore op=projection result=restored bag=%d`；控件快照、对象指针逐项变化只用 D。 |
| popup/resource 自检与 restore 失败 | release 时宿主无法恢复原 popup 入口 | W（单项资源恢复失败） | `assembly/claims.cpp:212-220` | present | 有资源名、entry 指针和失败 code；是可定位的单项失败，等级 W 合适。成功 restore 无需逐项 INFO，避免重复装配噪声。 |
| pending 事务恢复与隔离终态 | 加载时识别 pending，恢复源/目标状态或保留隔离记录 | I 最终 recovered/isolated；W 可重试保留；E 不可恢复数据问题 | `model/virtual_bag_state.h:102-108,111-120` 仅保存隔离记录，无日志；外层装配/公共接口无恢复终态日志 | missing | pending/隔离模型有状态但本范围内没有最终恢复结果。建议在恢复协调的唯一终结点记录 `op=tx result=recovered|isolated tx=%s src=%d/%d dst=%d/%d reason=%s`；内部阶段只用 D，禁止在模型序列化或各层重复记录。 |

## 优先补全项

1. 插件安装成功与关键前置校验最终失败：没有成功里程碑，且部分导致插件无法安装的分支静默返回。
2. pending 事务恢复/隔离终态：模型保存了 pending 与 isolation 记录，但没有可回放的恢复结论。
3. `save.slot_data` capture/complete 最终结果：已有快照大小日志，但失败返回和提交/取消结果覆盖不完整。
4. 启用、停用与 bagCount 实际状态变化：当前缺少 lifecycle I，用户无法从日志确认功能最终状态。
5. 配置写入最终 receipt：已有静态失败消息，缺 `section/key/result/reason` 与成功持久化确认。
6. projection 回调注册与投影 view install/restore：目前主要能看到局部失败，成功接管、恢复后的状态不可见。
7. 袋源变更成功终态：只对部分操作有成功 INFO；仅对实际写状态的 import/remove/use 增加一次结果，不记录普通查询/空结果。
