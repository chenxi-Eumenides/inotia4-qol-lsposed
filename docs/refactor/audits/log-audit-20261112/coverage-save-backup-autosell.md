# 功能级日志覆盖审计：save_backup + autosell

依据 `docs/reference/logging-standards.md`，按两个插件描述符及其功能入口枚举。`save_backup` 描述符提供备份管理入口和崩溃日志导出入口（`plugins/save_backup/plugin.cpp:574-581`）；自动备份由 pre-save observer 触发。`autosell` 描述符提供 `autosell.auto_sell` 开关（`plugins/autosell/plugin.cpp:351-355`），按存档会话与配置调度扫描。

状态：`present`=关键生命周期节点已有合适日志；`weak`=有部分日志，但存在关键节点缺失、等级/字段/位置不合适；`missing`=关键事件无日志。行号指 `module/app/src/main/cpp/` 下相对路径。

| 功能事件 | 触发条件 | 目标等级 | 现有日志 file:line | 状态 | 建议（含建议文案） |
|---|---|---:|---|---|---|
| 备份插件启用/停用 | 插件 feature 生命周期切换 | I | `plugins/save_backup/plugin.cpp:516-524` 无日志 | weak | 仅状态变化时记录启停：`feature=save_backup enabled=%d`；停用同样记录 `enabled=0`。避免每次 install/deactivate 重复。 |
| 手动创建备份、内容去重与写入成功 | 创建备份请求完成、同 checksum 已存在或新 bundle 落盘 | I | `plugins/save_backup/usecases/backup_usecases.cpp:816-817,856` | present | 已有 deduplicated/exported 结果；失败由具体失败分支覆盖，无需再加最终结果日志。 |
| 备份轮转/保留策略 | 自动备份替换既有 slot 备份或清理旧项 | I | `plugins/save_backup/usecases/backup_usecases.cpp:415-459` 未见轮转结果日志 | missing | 若实际执行轮转，在完成后汇总一次：`op=autobackup_rotate result=ok slot=%d removed=%u`；若没有独立轮转行为，则从功能清单移除此项。 |
| 自动备份触发及完成/失败 | pre-save 到达、指纹变化后导出备份 | I / W | `plugins/save_backup/plugin.cpp:108` 触发无日志；`plugins/save_backup/usecases/backup_usecases.cpp:430-457` 仅失败 W；成功复用 `:856` | weak | 触发入口无需单独日志，成功处应明确自动备份结果：`op=autobackup result=ok slot=%d`；失败日志已有，避免与通用 exported 成功重复，可给成功事件加 `source=autosave`。 |
| 备份完整性校验 | 列表/恢复解析 bundle 并核验 checksum | W / E | `plugins/save_backup/usecases/backup_usecases.cpp:673-679` 校验失败返回错误；列表坏项 `:562` W；UI 解析错误 `plugins/save_backup/ui/panel.cpp:357-371` W | weak | UI `backup entry parse failed` 未区分 checksum 与格式错误；在校验失败边界按不可恢复数据损坏记 `E`：`op=backup_verify result=failed reason=checksum_mismatch`。普通列表可选坏项仍维持 W。 |
| 恢复备份：门禁、成功与失败 | 用户确认导入并覆盖当前槽 | I / W | 门禁 `plugins/save_backup/usecases/backup_usecases.cpp:142-170`；成功 `plugins/save_backup/transaction/transactions.cpp:1077`；失败分支覆盖于 `:701-801, transaction` | present | 已有拒绝/降级日志及 `op=restore result=success`；不另加同义最终结果。 |
| 删除备份文件/存档槽 | 用户确认删除并处理相关 sidecar、仓库文件 | I / W / E | `plugins/save_backup/transaction/transactions.cpp:1109-1112,638,1214,1295,1304` | present | 普通失败 W、回滚不完整 E、成功 I 均有；无需新增。 |
| 导出备份到文件或 Download | 选择备份并完成公共导出 | I / W | `plugins/save_backup/ui/injection.inc:163-244`；本地槽导出 `:19-33` | weak | queued 日志只表示入队，未记录异步完成结果；在 worker 完成处汇总 `op=backup_export result=%s name=%s`，失败 W。 |
| 自动出售启用/停用 | descriptor feature、全局开关状态变化 | I / W | `plugins/autosell/scan.cpp:529-539`；能力获取失败 `plugins/autosell/plugin.cpp:327-334` | weak | 全局变化已有 I；插件 `on_enabled/on_disabled` 成功路径无明确生命周期日志。增加状态变化事件 `feature=autosell enabled=%d`，与已有全局开关区分或统一避免重复。 |
| 自动出售扫描调度与停止 | save enter/config/global gate 打开或关闭调度任务 | I / W | `plugins/autosell/scan.cpp:442-483,485-503` | weak | 任务注册/移除与存档事件有日志；scheduler 不可用、add 返回失败目前静默。失败时 W：`op=autosell_scan_schedule result=failed code=%d`；不可用 API 可标 `reason=scheduler_unavailable`。 |
| 自动出售扫描门禁与取消 | 全局关闭、无活跃会话、配置关闭、世界/能力 gate 不满足 | D | `plugins/autosell/scan.cpp:87-112,409-439` | present | gate 与取消原因已限频 DEBUG；无需逐帧或逐次增加日志。 |
| 自动出售筛选与保护名单 | 物品视图构建、规则不匹配或 protected/no-sell | D / W | `plugins/autosell/scan.cpp:181-214` | present | 规则细节限频 DEBUG；保护名单拒绝 W 且标 `reason=protected`，符合关键拒绝事件。 |
| 逐轮出售结果汇总 | 一轮扫描执行结束 | I | `plugins/autosell/scan.cpp:392-406` | present | 每轮单条 INFO 汇总 `sold/skipped/failed`；避免逐物品成功日志。 |
| 出售失败及扫描重试 | 单物品出售失败、扫描源失败；slot config 写入暂缓/重试 | W / D | 出售失败 `plugins/autosell/scan.cpp:218-263,304-379`；持久化重试 `plugins/autosell/store.cpp:326-645` | weak | 扫描源、单次出售和配置写入失败均有日志；slot-data 重试暂缓未统一记录 attempt=i/n，且当前若干 WARN 没有重试序号。重试失败建议 `op=slot_data_write attempt=%u/%u result=retry slot=%d code=%d`；耗尽才记录最终失败。 |

## 优先补全项

1. **完整性校验最终失败**：校验损坏可能导致不可恢复数据问题，明确在 checksum mismatch 边界记 E，并带 `op/reason`。
2. **自动备份成功归因**：pre-save 触发无入口记录，通用 exported 未区分自动备份；用单条成功结果标明 `source=autosave`。
3. **异步导出完成结果**：queued 不是导出成功；在实际复制结束处记录一次成功/失败结果。
4. **slot-data 重试语义**：补 attempt=i/n 与耗尽结果，区分可恢复的暂缓和最终失败。
5. **扫描调度注册失败**：scheduler API 缺失或 add 失败会导致功能未运行，补充 W 及失败原因。
6. **描述符启停可观测性**：save_backup 与 autosell 成功启停路径静默，补有限频次状态变更日志，避免重复记录已有全局开关事件。
7. **轮转行为核实**：代码路径有自动备份指纹/单槽文件，但未发现明确轮转完成日志；若确有旧项清理，补一条删除数量汇总，否则不要把轮转列作独立功能。

## 覆盖计数

- 功能事件共 14 项：present 6、weak 7、missing 1。
- `missing` 的轮转项需先核实是否存在独立轮转行为；自动备份触发、恢复、创建/去重、删除、校验失败、扫描、保护规则、每轮汇总均已按可观察节点纳入。
