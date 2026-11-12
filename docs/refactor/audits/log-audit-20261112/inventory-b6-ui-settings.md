# 日志审计清单：批次 6（UI / settings）

依据 `docs/reference/logging-standards.md` 与 `docs/reference/logging.md`。逐项检索 `log_message(`、`log_info(`、`log_warn(`、`log_error(`、`log_debug(`、`CORE_UI_LOG_`、`QOL_LOG_`、`log.write(`；宏/包装器定义及底层公共写出口不计事件。排除已审计的 `extension_bag/ui`、`world_teleport`、`game/access/ui_components`。实际等级按底层 `sdk->log.write` level 解析（0=D、1=I、2=W、3=E），不是函数/宏名称。

## 现有日志

| file:line | domain | level_now | message 摘要 | event_class | level_target | disposition | rationale |
|---|---|---:|---|---|---:|---|---|
| `plugins/core.custom_ui/panels/host.cpp:135` | ui | I | ensure_popup resolve=failed | 弹窗托管获取失败 | W | promote | 面板打开前解析宿主 popup 失败，open 返回失败；单次操作失败且游戏可继续。 |
| `plugins/core.custom_ui/panels/host.cpp:141` | ui | I | resolve/acquire 成败 | 弹窗托管接管 | I | keep | 接管成功或失败的边界状态；失败支路需要按上行 W 处理，成功为有限状态变化。 |
| `plugins/core.custom_ui/panels/host.cpp:161` | ui | I | open conditions 状态快照 | 面板打开状态变化诊断 | D | demote | 每次条件值改变都可能发生，不是面板已打开状态；仅用于排查门控原因。 |
| `plugins/core.custom_ui/panels/host.cpp:245` | ui | I | owned popup snapshot unavailable; restore held pending | popup 快照异常收敛 | W | keep | 捕获 popup 快照失败使关闭恢复挂起；按约定保留异常收敛信息。 |
| `plugins/core.custom_ui/panels/host.cpp:264` | ui | I | close queued panel/current/owned | 关闭排队等待一致性 | I | keep | 防止弹出非本模块栈项而延迟关闭的状态变化，正文含 panel/current。 |
| `plugins/core.custom_ui/panels/host.cpp:333` | ui | I | deactivate queued owner/panel | owner 释放延迟 | I | keep | owner 释放因宿主栈快照不一致进入 pending，包含 owner/panel。 |
| `plugins/core.custom_ui/panels/host.cpp:447` | ui | I | close force-complete panel/stall_frames/limit/pop | 关闭异常强制收敛 | W | keep | 超过停滞阈值后强制关闭收敛；已有 panel，需补 owner/reason 才完整。 |
| `plugins/core.custom_ui/panels/host.cpp:470` | ui | I | restore held pending baseline_snapshot_unavailable | 恢复异常挂起 | W | augment | 恢复无法继续；目前仅 reason，无 panel/owner。 |
| `plugins/core.custom_ui/panels/host.cpp:489` | ui | I | restore held pending game_stack_unexpected current/owned | 恢复异常挂起 | W | keep | 已含 reason=current 状态，具体描述意外栈且状态可诊断。 |
| `plugins/core.custom_ui/panels/host.cpp:499` | ui | I | pop issued; restore deferred | 恢复阶段过程 | D | demote | 正常中间步骤，不是状态最终变化；逐帧 pump 内的过程细节。 |
| `plugins/core.custom_ui/panels/host.cpp:528` | ui | I | popup snapshot unavailable main_thread/hook | 快照不可用诊断 | D | demote | 快照可选/暂不可用状态且受 snapshot 日志门控，不构成最终操作失败。 |
| `plugins/core.custom_ui/panels/host.cpp:540` | ui | I | popup snapshot result/count/top | 栈快照状态诊断 | D | demote | 状态快照值变化诊断，不是操作完成事件。 |
| `plugins/core.custom_ui/panels/host.cpp:510` | ui | I | restore complete stack/count/top | 弹窗托管释放完成 | I | keep | 宿主 popup 恢复完成，明确生命周期变化。 |
| `plugins/core.custom_ui/panels/host.cpp:577` | ui | I | log_snapshot event/stack/count/target | panel popup 栈状态变化 | I | keep | 用于记录 open baseline、owned-entry、pop-issued 等托管边界；调用由状态边界触发且去重。 |
| `plugins/core.custom_ui/panels/host.cpp:587` | ui | I | log_message 泛化事件 | 生命周期/诊断共用出口 | I | augment | 实际输出覆盖状态变化、失败和逐帧 dispatch，level 固定 I；调用点需拆分级别，不能由单出口全记 I。 |
| `plugins/core.custom_ui/panels/host.cpp:604` | ui | I | dispatch enter panel/thread/count | dispatch enter 细节 | D | demote | enter 回调分发诊断，不代表面板状态变化。 |
| `plugins/core.custom_ui/panels/host.cpp:627` | ui | I | dispatch enter inactive; closing panel | 面板 enter 后未激活关闭 | W | promote | 已主动清理不可见栈顶；属于打开失败，单次可恢复失败。 |
| `plugins/core.custom_ui/panels/host.cpp:651` | ui | I | dispatch process panel/thread/count | 逐帧 process 计数 | D | demote | process 每帧执行，仅首帧及每 120 帧输出仍是逐帧路径诊断。 |
| `plugins/core.custom_ui/ui_confirmation.cpp:232` | ui | I | native popup draw failed; V1 remains invisible | 确认弹窗绘制失败 | W | keep | 绘制失败但功能降级为不可见，游戏仍可运行；消息清晰说明降级结果。 |
| `plugins/core.custom_ui/panels/debug_help.cpp:59` | ui | I | log_result action/result/selected | debug help 状态与操作结果 | I | augment | 单出口同时记录 open/close/select 与 process/draw/event 错误；操作状态可 I，诊断错误应 W、帧过程应 D 并补 panel/reason。 |
| `plugins/core.custom_ui/panels/debug_scroll.cpp:49` | ui | I | log_result action/result/selected | debug scroll 状态与操作结果 | I | augment | 单出口混合 close/select 生命周期、每帧 draw/process 失败；按具体事件拆级并限热路径。 |
| `plugins/core.custom_ui/panels/debug_components.cpp:190` | ui | I | debug_log 静态/动态文案 | debug component 诊断 | I | augment | debug_log 直接写 INFO，调用包含页面构建失败和接口诊断；失败应 W，状态变化 I，纯过程改 D。 |
| `plugins/settings/feature.cpp:61` | ui | I | 设置 feature 静态消息 | 设置页初始化/失败 | I | augment | 静态正文及调用分支需核对语义；若失败须 W 并携 reason，否则无变量状态文案无效。 |
| `plugins/settings/plugin.cpp:98` | ui | I | ui action/result | 设置 UI 命令结果 | I | augment | 用户请求完成可 I，但 result JSON 需结构化为 action/result，失败需 reason，且需避免敏感/自由文本。 |
| `plugins/settings/plugin.cpp:159` | ui | I | scheduler task id | 设置帧任务注册成功 | I | keep | 插件初始化阶段成功注册任务，有限生命周期事件。 |
| `plugins/settings/plugin.cpp:177` | ui | I | ui_panels require ok/failed/version | 设置面板能力依赖获取 | W | demote | require 失败时停用流程不能释放面板能力，属于单次失败；成功支路不需重复记录依赖查询。 |
| `plugins/settings/plugin.cpp:185` | ui | I | deactivate_owner result | 设置面板 owner 释放 | I | keep | 记录 owner 释放完成/排队状态变化。 |
| `plugins/settings/plugin.cpp:188` | ui | I | deactivate_owner skipped capability_unavailable | owner 释放失败/跳过 | W | promote | capability 不可用导致面板释放操作未执行，游戏可继续。 |
| `plugins/settings/plugin.cpp:191` | ui | I | ui_panels require failed reason=api_unavailable | owner 释放能力不可用 | W | promote | 明确失败及 reason，当前因失效 api 无法继续注销面板。 |
| `plugins/settings/plugin.cpp:193` | ui | I | deactivate_owner skipped capability_unavailable | 重复记录释放跳过 | W | merge | 同一 capability 缺失事件已在 191 记录；与失败原因合并到唯一调用点。 |
| `plugins/settings/panels/settings.cpp:483` | ui | I | settings right list create failed code | 设置控件创建失败 | W | promote | 右栏列表创建失败使页面构建失败，单次 UI 操作失败。 |
| `plugins/settings/panels/settings.cpp:488` | ui | I | settings right list selection failed code | 设置控件配置失败 | W | promote | selection 配置失败后销毁列表并返回失败。 |
| `plugins/settings/panels/settings.cpp:497` | ui | I | settings row get failed row/code | 设置控件读取失败 | W | promote | 行获取失败后销毁列表、终止构建，属于明确失败。 |
| `plugins/settings/panels/settings.cpp:527` | ui | I | settings choice arrow create failed item/code | 设置控件创建失败 | W | promote | choice 箭头创建失败后页面构建中止。 |
| `plugins/settings/panels/settings.cpp:538` | ui | I | settings toggle create failed item/code | 设置控件创建失败 | W | promote | toggle 创建失败后页面构建中止。 |
| `plugins/settings/panels/settings.cpp:891` | ui | I | settings enter blocked preconditions | 面板打开失败 | W | promote | 前置条件不满足导致面板未打开；应携带具体 reason 和 panel。 |
| `plugins/settings/panels/settings.cpp:899` | ui | I | settings enter blocked ui api | 面板打开失败 | W | promote | 所需 UI API 缺失，打开失败且游戏继续。 |
| `plugins/settings/panels/settings.cpp:903` | catalog | I | settings enter blocked catalog | 配置目录读取失败 | W | promote | 目录读取失败导致设置页面无法打开；不是状态变更。 |
| `plugins/settings/panels/settings.cpp:912` | ui | I | settings enter blocked root create | 根控件创建失败 | W | promote | 页面根控件创建失败，单次打开操作失败。 |
| `plugins/settings/panels/settings.cpp:932` | ui | I | settings enter blocked left list code | 左栏列表创建失败 | W | promote | 页面列表创建失败后销毁页面并退出。 |
| `plugins/settings/panels/settings.cpp:937` | ui | I | settings enter blocked right list | 右栏列表创建失败 | W | promote | 页面列表构建失败后销毁页面并退出；需带具体 code/reason。 |
| `plugins/settings/panels/settings.cpp:1066` | ui | I | main-menu more-games rect/gear rect | 主菜单锚点几何诊断 | D | demote | 矩形坐标是绘制/定位细节，不是功能状态变化。 |
| `host/ui_adapter.cpp:1472` | ui | W | UI adapter 操作失败 | UI 控件适配失败 | W | keep | adapter 在收到失败结果时记录告警；属于单次可恢复 UI 操作失败。 |
| `plugins/core.custom_ui/plugin.cpp:272` | ui | W | 插件路由/能力注册失败 | custom UI 安装失败 | W | keep | 注册失败时插件能力不可用但游戏可继续，level=2 对应 W。 |

## 缺失日志建议

| file + 函数 | 事件 | 目标等级 | 必填字段 | 理由 |
|---|---|---|---|---|
| `plugins/core.custom_ui/panels/host.cpp` + `open` | 面板成功打开/关闭状态变化 | I | `panel=`、`owner=`、`result=` | 现有日志多为调度和栈状态诊断；没有独立记录 provider.open 成功和常规 close 完成。 |
| `plugins/core.custom_ui/panels/host.cpp` + `force_complete_close` | close force-complete 的 owner 与失败原因 | W | `panel=`、`owner=`、`reason=` | 已记录 panel 和停滞帧，owner/reason 缺失；用于异常状态收敛定位。 |
| `plugins/core.custom_ui/panels/host.cpp` + `restore_if_idle` | baseline snapshot 持续不可用的恢复挂起上下文 | W | `panel=`、`owner=`、`reason=` | restore held pending 的 baseline 分支只写 reason，没有关联具体面板及 owner。 |
| `plugins/settings/panels/settings.cpp` + `process_pending_toggles` / `poll_requests` | 用户设置持久化并生效 | I | `feature=`、`section=`、`result=applied` | 功能设置值成功持久化只更新 UI 状态，无结构化配置生效日志；不要逐帧记录 poll。 |
| `plugins/settings/panels/settings.cpp` + `poll_requests` | 配置持久化失败 | W | `feature=`、`section=`、`reason=` | 失败只呈现在界面状态中；错误日志须在 receipt 最终失败时单次记录。 |
| `plugins/settings/panels/settings.cpp` + `settings_panel_enter` | 设置面板打开成功/失败结果 | I/W | `panel=settings`、`result=`、失败时 `reason=` | 多个失败分支仅固定 INFO 文案；成功设置 `g_panel_active` 后没有打开事件。 |
| `plugins/core.custom_ui/ui_confirmation.cpp` + overlay 注册/释放失败路径 | 确认弹窗恢复/注销失败 | W | `owner=`、`reason=`、`result=` | 有 native popup draw failure 记录，但 unregister_frame_overlay / 恢复失败仅返回 false，不留下失败上下文。 |
| `game/read/game_tiles.cpp` + tile catalog load | catalog 数据损坏/不可恢复 | E | `op=load_catalog`、`reason=`、`entry=`（若有） | catalog 读取若遇不可恢复损坏，UI 只汇总为 catalog blocked；应在唯一加载层留具体数据完整性错误。 |

## 统计

- 日志事件：43 条；其中 `core.custom_ui` 23 条（含插件注册失败）、`settings` 20 条、剩余 `ui` 域 1 条（host UI adapter）。
- 处置：keep 12，demote 8，promote 15，delete 0，merge 1，augment 7。
- 缺失日志建议：8 条。
- 说明：`log_result`/`log_message`/`debug_log` 共享出口按实际可观察日志行为列一次；单出口跨调用场景需按其语义拆分等级，而非把定义算作单条业务事件。

## Top 发现

1. settings 构建及打开失败共 12 个调用点用 INFO；全部是本次操作失败，应为 W，并带 panel/reason。
2. `panels/host.cpp` 的 `log_message` 出口固定 INFO，导致异常恢复、popup 接管失败和逐帧 dispatch 混用同级；按具体事件拆级。
3. `restore held pending`（host.cpp:470/489）与 `close force-complete`（447）是异常收敛，保留 W；force-complete 还需 owner/reason，已有 current/reason 不重复补字段。
4. `dispatch process`、滚动/绘制诊断应为 D；debug panels 的共用 `log_result` 也会在逐帧绘制失败时输出，需门控并限制频率。
5. settings UI 配置持久化成功/失败目前仅更新屏幕状态，没有 I/W 日志；最终 receipt 事件位置最适合单次记录，不能在 poll 每帧打印。

## 审核裁决

- **promote（15）**：`core.custom_ui/panels/host.cpp:135,627`；`settings/plugin.cpp:188,191`；`settings/panels/settings.cpp:483,488,497,527,538,891,899,903,912,932,937`。全部改为真实 level 2，原正文保留。
- **demote（8）**：`host.cpp:161,499,528,540,604,651`、`settings/panels/settings.cpp:1066` 改为 level 0；`settings/plugin.cpp:177` 在 require 失败时改为 level 2。
- **merge（1）**：删除 `settings/plugin.cpp` 原 193 行重复记录，保留 191 的 `reason=api_unavailable`。
- **augment（3 项 D 类裁决）**：`host.cpp:470`、`settings/plugin.cpp:98`、`settings/feature.cpp:61`；另按 E.2 在 host close force-complete 既有日志追加字段。host restore 两处也按 E.3 补上下文。
- **采纳缺失日志建议**：host provider open 成功记录 `panel open panel=%s owner=%s result=opened`（I）；settings receipt 最终成功/失败分别记录 `feature=%s section=%d result=applied`（I）和 `feature=%s section=%d reason=persistence_failed`（W）；settings 面板成功打开记录 `panel=settings result=opened`（I）；确认 overlay 注册/释放失败记录 `op=overlay owner=core.custom_ui result=failed reason=...`（W）。
- **检查结论**：`settings/feature.cpp:61` 位于 panels API 缺失失败分支，已改为 W 并补 `panel=`、`reason=capability_unavailable`；`game/read/game_tiles.cpp` 的 base64 解码失败会跳过该地图并继续，最终 `ready` 仅按成功条目数设置，没有不可恢复解析失败分支，故不加 E 日志。
- **驳回**：`host.cpp:587` 与 `debug_*` 共用 `log_result`/`debug_log` 不按调用点拆级，保持原状。
