# 功能级日志覆盖审计：host / bridge / patch / infra

范围：`module/app/src/main/cpp/{host,bridge,patch,infra,game/access,contracts}`。按 `logging-standards.md` §1、§3、§5、§6 的等级、正文结构、事件类别及克制原则审查。功能清单以 `host/assembly.cpp:229-265` 内建插件注册表、`contracts/plugin.h` 描述符契约、host SDK / Game Host / bridge 导出入口为基准；逐帧过程不要求 INFO。

状态定义：`present` 表示关键成功/状态变化及失败路径有合适记录；`weak` 表示有相邻记录但生命周期、关键字段或恢复状态不完整；`missing` 表示关键生命周期/结果没有日志。`file:line` 均相对 `module/app/src/main/cpp/`。

## 功能覆盖表

| 功能事件 | 触发条件 | 目标等级 | 现有日志 file:line | 状态 | 建议（含建议文案） |
|---|---|---:|---|---|---|
| native 初始化与 host 装配 | JNI native init 绑定完成、assembly 收尾；装配硬失败 | I / E | `bridge/native/gamebridge.cpp:287-295`; `host/assembly.cpp:877-880` | present | 保留既有里程碑；失败日志维持区分模块不可用与局部降级。 |
| 内建插件注册表（module_health、core.*、功能插件） | 插件描述符被注册、依赖排序、启停/回滚 | I / W | `host/assembly.cpp:229-265,733-770`; `host/plugin_registry.cpp:1704,1900-1910,2870,2912,2963` | weak | 通用事务已有失败/激活记录，但缺少稳定统一的成功停用、恢复事件；在状态真正变化处记录 `op=deactivate plugin=<id> result=inactive`，不要各插件重复记账。 |
| module health / 初始化健康状态 | 健康位初始化、错误置位及恢复 | I / W | `host/assembly.cpp:652,675,779,785,804,809,875-880`; `host/plugin_registry.cpp:2113` | weak | 状态被健康 API 暴露但健康位转变未统一落日志；仅在状态变化时记录 `op=health_update bit=<name> state=degraded reason=<token>`，恢复时 `op=health_update bit=<name> state=ready`。 |
| 平台与安全配置 owner | host 注册 platform/security 配置 owner、配置加载或失效 | I / W | `host/assembly.cpp:774-786,791-804`; `host/features/production.cpp:20-27` | weak | 注册失败有日志，成功 bootstrap 与后续配置持久化结果未形成可回放事件；建议仅 bootstrap 状态变化记 `op=config_bootstrap result=ready` 或 `op=config_bootstrap result=degraded reason=<token>`。 |
| settings / feature catalog 与功能开关 | 目录初始化、用户请求启停/改配置、receipt 完成或拒绝 | I / W | `host/features/production.cpp:367-434,517-566`; `bridge/native/gamebridge.cpp:345-409` | weak | API 返回错误和 receipt JNI 异常部分有覆盖，但用户请求的拒绝原因、配置保存最终结果缺统一日志；请求被拒记 `op=feature_set plugin=<id> feature=<key> result=rejected reason=<token>`，避免记录每次读取。 |
| feature reconciler 生命周期 | desired 与 actual 对齐、资源安装/撤销、可重试失败后恢复 | I / W | `host/plugin_registry.cpp:1900-2113,2279-2345,2702-2760,2870-2972` | weak | 有失败/延迟和激活信息，成功停用、事务回滚完成及 degraded 清除没有稳定终态日志；在终态变化处一条 `op=feature_transition plugin=<id> state=<state> result=<ok|degraded> reason=<token>`。 |
| capability 依赖及 optional 降级 | required 缺失/版本不符、optional 缺失、依赖环降级、provider 恢复 | W / I | `host/plugin_registry.cpp:908-915,872-957,1041-1066`; `infra/capabilities.cpp` | weak | optional 降级有 WARN，required 依赖失败细节主要进入状态文本，provider 恢复清除 degraded 未记；恢复才值得 INFO：`op=capability_restore capability=<id> consumer=<plugin> result=ready`。 |
| 游戏符号与变体站点解析 | 所需符号/站点缺失或版本不支持 | W；影响全模块时 E | `host/plugin_registry.cpp:988-1037`; `host/assembly.cpp:532-545`（仅设备 probe） | weak | 普通资源安装失败在 registry 侧有记录，但初次 preflight 拒绝与 variant 选择缺少一致事件；按一次 claim 汇总 `op=resolve claim=<id> result=failed reason=symbol_unresolved variant=<token>`，不逐符号成功刷屏。 |
| 常驻兼容 patch（Hive 登录跳过、存档位段守卫、monster 数量兼容） | boot/bridge 后安装、非目标版本跳过、恢复原字 | I / W | `host/core_patches.cpp:80-111,118-158,163-190` | weak | 失败与 monster 命中有记录，但 hub/bitend 安装成功及重复安装/恢复路径无统一成功里程碑；成功后记 `op=install patch=<claim> result=ready`，恢复时记 `op=restore patch=<claim> result=restored`。 |
| hook / call / instruction / pointer-slot patch 原语 | 安装、拒绝（原字/映射冲突）、释放/恢复 | I / W | `patch/instr_patch.cpp:18-28,265-287`; `patch/call_patch.cpp:84,107,436-449,563-563,611-801`; `patch/ptr_slot.cpp` | weak | 原语错误与部分成功/恢复已有日志，但机制与 claim 关联不一致，pointer-slot 状态尤其缺可见事件；建议在 claim 生命周期层统一记录 `op=patch_install claim=<id> mechanism=<kind> result=ok`，不在每个写指令 helper 重复记。 |
| entry hook 安装/撤销 | 插件声明 hook，入口不可解析、冲突、安装失败或成功卸载 | I / W | `patch/native_hook.cpp:19-37`; `host/plugin_registry.cpp:1900-2113,2870-2972` | weak | native hook API 初始化有记录，业务 hook 依赖通用资源日志；通用成功日志若未包含 claim id，需补 `claim=<id>`；失败需含 `reason=`。 |
| save enter / exit 注入与保存路径恢复 | 开始存档前置注入、保存后退出回调、未就绪重试/耗尽 | I / W | `patch/save_enter.cpp:30,41,54,66`; `patch/save_exit.cpp:24`; `host/assembly.cpp:451-519,849-863` | present | 已覆盖注入失败、补装耗尽、成功补装和退出触发；进一步增强仅需确保成功退出正文含 slot/result，不加逐帧 retry 日志。 |
| frame source / render 与 logic anchors | 逻辑帧和渲染帧锚点安装、失败后重试 | I / W | `host/frame_source.cpp:108-189`; `host/assembly.cpp:666-670` | present | 安装成功与失败有记录；避免重复重试失败噪声（已由状态签名约束），不需要每帧运行日志。 |
| scheduler / command queue / lifecycle 命令 | 任务注册失败、命令执行拒绝、队列溢出 | W；重要命令终态 I | `host/assembly.cpp:549-570,672-676,828-847,869-872`; `infra/scheduler.cpp`; `infra/command_queue.cpp` | weak | 关键任务注册失败有记录；队列拒绝/命令最终失败需要在操作边界记录对象与 reason，调度执行过程应保持 DEBUG 或无日志。 |
| event state 发布、订阅与恢复触发 | save-ready 状态源切换、订阅失败、owner 恢复 | I / W | `host/assembly.cpp:573-607,637-645,826-847`; `infra/event_state.cpp` | weak | 订阅失败有 WARN，但 host fallback 接管、provider 恢复及订阅回调失败无统一状态日志；只在 source/availability 变化时 `op=event_state source=<host|provider> state=<available|unavailable>`。 |
| SaveRoute owner 绑定、停用与 drain | owner 获得/撤销 save route、binding 因停用退出 | I / W | `host/assembly.cpp:624-627`; `host/save_route.cpp` | missing | 仅装配注入回调，没有 route 真正绑定/解除或拒绝日志；关键状态变化建议 `op=save_route owner=<id> result=<bound|unbound> reason=<token>`。 |
| Save transaction / save preflight / evidence | 保存事务 gate 拒绝、原版 save call、证据写入与恢复 | W / I | `host/assembly.cpp:608-614,662-664`; `game/access/save_transaction.cpp`; `game/access/save_preflight.cpp`; `game/access/save_file_evidence.cpp` | weak | gate 与 adapter 路径是关键持久化安全边界，但跨组件拒绝结果没有统一调用点日志；最终不可恢复失败记 E，普通 gate 拒绝记 W：`op=save_transaction result=rejected slot=<n> reason=<token>`。成功的每次低层调用不记录，仅会话级保存完成可 I。 |
| core.save / save_backup 持久化与恢复插件 | 插件安装、备份/回滚、恢复后状态确认 | I / W / E | `host/assembly.cpp:234,254`; `host/plugin_registry.cpp:1900-2113`; `patch/save_enter.cpp:30-66` | weak | 生命周期通用日志不含操作域内的恢复结果，回滚失败风险最高；对最终恢复失败记 `op=restore slot=<n> result=failed reason=<token>`（E），成功恢复记一次 I，不重复底层保存日志。 |
| Game Host / SDK 跨插件调用与 capability ports | ABI 版本/结构不兼容、required port 缺失、调用拒绝 | W / E | `host/assembly.cpp:618-731`; `host/plugin_registry.cpp:384-555,960-985`; `contracts/host_sdk.h`, `contracts/game_host.h` | weak | 装配绑定失败有部分 WARN，具体 port id/version 多保存在 error text；拒绝插件装配时保证唯一 WARN 携带 `plugin=<id> port=<id> reason=<token>`。正常查询不需要日志。 |
| host public export API（crash logs / backup / import picker） | Kotlin bridge 不可用、请求拒绝、调用成功 | W / I | `host/public_export.cpp:12-30`; `bridge/native/gamebridge_public_export.cpp:48-106,112-134` | weak | JNI 注册成功、env/Java 异常有日志；导出/导入成功完成在 Kotlin executor 异步发生，native 只确认入队且不应宣称文件已导出；建议记录 enqueue 拒绝 `op=public_export kind=<kind> result=failed reason=<token>`，成功完成由实际完成方一条 I。 |
| PublicExport bridge 注册与 JNI 运行时回调 | JNI 方法 lookup、全局引用、callback subscribe/unsubscribe、receipt 回调 | I / W | `bridge/native/gamebridge_public_export.cpp:77-106`; `bridge/native/gamebridge.cpp:111-118,120-150,299-342,345-409` | weak | bridge 注册异常有记录，runtime effect subscribe/unsubscribe 的静默失败/无效参数和成功注册/取消没有生命周期日志；仅真实失败记 `op=runtime_effect_subscribe result=failed reason=<token>`，不要记常规 callback 每次派发。 |
| Kotlin 到 native config receipt 回送 | receipt 回调创建、JNI 转换、回调抛错/线程 attach 失败 | W | `bridge/native/gamebridge.cpp:59-73,111-118,120-150,345-409` | present | JNI 失败路径有明确 reason 与操作字段；receipt 正常回调属于过程，无需 INFO。 |
| feature runtime effects observer | 插件订阅、状态变化通知、observer 抛异常、owner 停用取消订阅 | I / W | `host/features/production.cpp:56-143,195-212,250-283,602-621` | weak | observer 异常有 WARN，订阅成功/按 owner 清理无记录；重要订阅生命周期仅建议 `op=runtime_effect_subscribe plugin=<id> result=ready` 与 stop 时汇总取消数，不逐次记录效果通知。 |
| HTTP endpoint provider / dispatcher 与 host resident routes | provider 注册/撤销、endpoint 绑定降级、请求拒绝 | I / W | `host/http_registry.cpp`; `host/http_endpoint_binder.cpp`; `host/http_dispatch.cpp`; `host/assembly.cpp:812` | weak | host 调度入口有绑定但关键注册和 provider 失效的状态日志不完整；提供者整体降级时一条 `op=http_provider state=degraded reason=<token>`，请求级结果由 API 调用点记录，不重复 dispatcher。 |
| 对外 HTTP/API/export 接口（feature control、config、status、item count） | 参数拒绝、未就绪、权限 gate 拒绝、持久化失败 | W / E | `host/features/production.cpp:367-434,436-499`; `host/public_export.cpp:12-30`; `host/item_count_facade.cpp`; `bridge/native/gamebridge.cpp:299-409` | weak | 多数 API 返回结构化错误但不记日志；返回给可见 UI/调用方的失败应由唯一业务边界记录，建议 `op=<api> result=rejected reason=<token>`；不要给成功查询加 INFO。 |
| owner identity 与 owner-scoped store | owner token 创建/失效、存储绑定、持久化读写恢复 | I / W / E | `host/assembly.cpp:743-748`; `host/plugin_registry.cpp:147-178`; `host/owner_identity.cpp`; `host/core_settings_config_store.cpp`; `host/core_settings_config_service.cpp` | weak | store 绑定失败有 WARN 但没有成功 bootstrap/读写失败后的持久化恢复摘要；配置文件最终不可读/损坏记 E，否则 W 并明确 fallback：`op=config_load result=fallback reason=<token>`。避免记录每个 key 的读写。 |
| game/access 对游戏对象、世界态与符号访问 | 必要对象缺失/版本不符、可选对象缺失、写入失败 | W（可选缺失 D） | `game/access/symbols.cpp`; `game/access/game_access.cpp`; `game/access/save.cpp`; `game/access/world.cpp`; `game/access/inventory.cpp` | weak | 该层大量适配器直接返回状态而无业务边界日志；不建议给 getter 批量加日志。只对导致整项用户操作失败且上层不会再记的最终拒绝记 `op=<operation> result=failed reason=<token>`，可选对象缺失只 D。 |
| contracts ABI 与结构化 Result 契约 | ABI 不兼容、空接口/缺字段、跨层调用失败 | W / E | `contracts/plugin.h`; `contracts/result.h`; `contracts/host_sdk.h`; `contracts/game_host.h`; `host/plugin_registry.cpp:960-985` | weak | contracts 本身为声明/纯值契约，不应主动打日志；需由唯一验证/装配边界将 descriptor ABI 拒绝记录为 `op=plugin_register plugin=<id> result=rejected reason=abi_mismatch`。 |
| infra config / durable store | 配置初始化、损坏/不兼容、写入失败、fallback 与恢复 | I / W / E | `host/assembly.cpp:789-804`; `infra/config.cpp`; `infra/store.cpp`; `host/core_settings_config_store.cpp` | weak | bootstrap 总体失败已有 WARN，但文件读写/原子替换、恢复到旧值缺少稳定事件；最终持久化失败记 W/E 视可恢复性，建议 `op=config_write result=failed reason=<token> keep=previous`。 |
| infra status / degraded 清除 | 插件 status 从 enabled 转 degraded/blocked/failed 再恢复 | I / W | `host/plugin_registry.cpp:608-638,2113,3158`; `infra/status.cpp` | weak | 状态接口可读，只有部分业务变化日志；避免 status 层泛化重复日志，统一在 registry 状态跃迁处记录一次恢复 `op=feature_recovery plugin=<id> result=ready`。 |
| infra slot operation gate / operation lease | 操作锁冲突、owner/lease/slot 校验拒绝、释放 | W | `host/assembly.cpp:608-614`; `infra/slot_operation_gate.cpp`; `host/plugin_registry.cpp` | missing | 跨 owner 互斥和 TOCTOU 准入是调用拒绝关键点，拒绝目前仅返回 Result；由业务事务边界一次记录 `op=slot_operation result=rejected slot=<n> reason=<token>`，成功开锁不记录。 |
| infra scheduler/cache/frame transition dispatch | 消费者任务注册/注销、锚点不可用、迁移恢复 | W / I | `host/assembly.cpp:672-676,865-872`; `infra/frame/transition_dispatch.cpp:72,113`; `host/frame_source.cpp:108-189` | present | 任务注册失败与 transition 初始化成功/失败已有关键记录；不补 tick/缓存命中日志。 |
| log/crash infrastructure | 主日志初始化/轮转、崩溃现场文件与初始化失败 | I / W / E | `infra/log/qol_log.cpp`; `infra/log/qol_crash.cpp`; bridge crash export `bridge/native/gamebridge_public_export.cpp:112-116` | weak | 正常日志初始化有 banner，crash handler 异步安全路径需克制；崩溃数据导出由导出 API 追踪。只在日志文件 sink 不可用且影响取证时记 W，禁止信号处理器内扩展非 async-signal-safe 日志。 |

## 优先补全项

1. **存档事务最终结果与恢复失败**：`game/access/save_transaction.cpp`、`save_preflight.cpp`、`save_file_evidence.cpp`；用户数据风险最高。对最终不可恢复失败用 E，包含 slot、op、reason；成功只在操作完成边界记录 I。
2. **save route / save backup 回滚状态**：`host/save_route.cpp`、`plugins/save_backup` 由 assembly 装配；增加唯一 owner 绑定/解除与最终恢复结果，避免无从判断恢复链是否接管。
3. **feature lifecycle 终态与恢复**：`host/plugin_registry.cpp`；统一记录启用/停用/降级/恢复的状态变化，涵盖 feature id 与结果，不在插件和 registry 双记。
4. **配置持久化失败与 fallback**：`infra/store.cpp`、`host/core_settings_config_store.cpp`；区分保留旧配置的可恢复 W 与配置不可用 E。
5. **slot operation gate 拒绝**：`infra/slot_operation_gate.cpp`；记录发生冲突的 slot、owner/operation token 与原因，定位跨组件调用被拒。
6. **bridge public export 完成回执**：Native 仅能说明 JNI enqueue；Kotlin executor 真正完成/失败处需记录一次终态，避免用户看到无反馈但日志误报成功。
7. **HTTP provider / feature endpoint 降级与恢复**：`host/http_endpoint_binder.cpp`、`host/http_registry.cpp`；仅记录 provider 状态变化，HTTP 单请求继续由 endpoint 负责。

## 统计

- 审计功能事件：33
- `present`：5；`weak`：26；`missing`：2
- 本轮只写入此审计文档；未修改源码，未执行构建或测试。
