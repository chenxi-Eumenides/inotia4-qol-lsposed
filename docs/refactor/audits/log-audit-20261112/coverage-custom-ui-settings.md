# 功能级日志覆盖审计：core.custom_ui + settings

依据 `docs/reference/logging-standards.md`。范围仅限 `module/app/src/main/cpp/plugins/core.custom_ui/` 与 `plugins/settings/`；按 UI 装配、panel provider、feature catalog 与设置回执路径梳理生命周期事件。`present` 表示关键节点已有可定位日志，`weak` 表示只覆盖部分状态/失败或字段不足，`missing` 表示功能关键状态没有日志。逐项变化、逐帧处理只建议 D，不要求逐项 I。

## 功能覆盖

| 功能 / 生命周期节点 | 代码行为与现有证据 | 覆盖 | 缺口 / 判定 |
|---|---|---|---|
| PopupHost 托管接管、provider 面板打开成功 | `core.custom_ui/panels/host.cpp:135,141` 记录 popup resolve/acquire 结果；`:250` 记录 `panel/owner/result=opened`。 | present | 打开成功有对象和结果；接管的失败分支 acquire failed 仍走 level I，虽有结果但应视为弱点（单次失败需要 W）。 |
| 面板常规关闭、弹窗托管释放 | `host.cpp:264,335` 记录关闭排队；`:451` close force-complete；`:521` restore complete。 | weak | 正常 close/provider 关闭完成没有明确 `panel/owner/result=closed`；restore complete 只记栈数值，未关联 provider。异常 close/恢复有覆盖。 |
| PopupHost 打开/关闭拒绝、失败与异常收敛 | `host.cpp:135` resolve 失败；`:245,476,499` restore pending；`:650` enter 后 inactive；`:451` stall force-complete。 | weak | stack full、generation 分配失败、provider.open 返回 false、set_process_info 拒绝、capture snapshot 失败等有拒绝/失败分支，但并非均有独立结果日志；其中 panel 打开失败的重要原因不可回放。 |
| PopupHost dispatch 生命周期/逐帧处理 | `host.cpp:627,674` 记录 enter/process；`host.cpp:579` log_snapshot 记录栈边界。 | present | 仅诊断证据；process 为逐帧路径，保持 D，不补 INFO。 |
| 页签/分类切换 | `settings/panels/settings.cpp:366` 选择分类，`:949-950` 应用 pending 分类并重建右侧列表；Debug Components 页切换在 `core.custom_ui/panels/debug_components.cpp:280,1451-1462`。 | missing | 成功切换无功能级状态日志；只需在分类实际改变/页面成功重建后记一次 I（带 `panel=`、`section=`），构建失败由现有/缺失的失败日志覆盖。Debug 页纯调试分类切换可只用 D。 |
| 设置项读取/初始载入 | `settings/panels/settings.cpp:234-274` 读取 feature/choice 并更新状态；`:278-310` 读取 catalog/category、构建 items；`:920` 打开时 refresh。 | weak | 读取值与 catalog 成功载入均无日志；不应逐项记录正常读取。关键缺口是 catalog 读取/构建失败只在面板打开分支汇总为 `:916` W，没有明确 `op=load_catalog reason=`；无法分辨读取失败、描述符损坏等原因。 |
| 设置项 toggle/choice 写入和持久化 | `settings/panels/settings.cpp:577-583,669-668` 发请求；`:593-615,626-642` 处理最终 receipt，成功记 `feature/section/result=applied`，最终失败记 W。 | weak | 最终持久化有关键覆盖；请求提交失败（`request_set_feature/choice` 不成功、receipt 未 admission）和 `receipt()` 查询失败只显示 UI error，不记 W。choice 成功结果还受 refresh 后状态匹配条件，状态不匹配支路目前没有专属日志。 |
| 特性开关联动 / owner availability | `settings/panels/settings.cpp:156-175` 检测 owner availability 改变并同步每个相关控件；`:571-584` 执行 toggle 请求；UI 会禁用不可用项。 | missing | owner 状态变化与开关请求被接受/拒绝均不记录；只建议对 owner 状态实际转变做一次 I/D（按 `feature=` 或 `owner=` 聚合），不要逐项/逐帧输出。写入失败沿 receipt W 覆盖。 |
| 设置项 action/入口 feature | `settings/panels/settings.cpp:378-386,670-749` 接收 entry 点击、请求启用并调用 invoke_entry；`:765-868` 轮询 continuation。 | weak | 请求/入口成功没有 feature 级结果日志；失败主要显示在状态提示中。关键入口最终成功/失败可在 continuation 结束处各记一次 I/W（`feature=`、`result=` 或 `reason=`）；轮询过程与每个中间状态不记 INFO。 |
| 配置数据生效 / 持久化 | `settings/panels/settings.cpp:593-642` receipt 成功和最终失败已有 INFO/W，记录 `feature` 与 category 数值 `section`。 | weak | 能识别设置写入结果，但 `section` 是数字，不便于按 section 语义检索；写请求前置失败、receipt 查询失败也未记。其余逐项读值不是配置生效，不需要额外日志。 |
| 样式参数、布局与外观应用 | `debug_components.cpp:447-453` 记录文本页宽高/style/subpage 变化；`:1000-1027` 仅绘制失败时 `debug_log`；`:1446-1472` 记录调试页开关请求；设置主菜单布局矩形 `settings/panels/settings.cpp:1083` 为 D。 | weak | 参数变化有调试观测，但不是用户侧正式功能；样式绘制成功通过 `g_style_applied` 状态更新（`:1013-1026`）却不记录应用结果。若该状态需回放，仅在首次变为 applied 时 D 或 I；逐项尺寸/矩形保持 D。 |
| UI 重装、重新绑定及恢复 | `settings/panels/settings.cpp:1094-1106` 重新 bind 时先销毁旧 root、重置依赖；`settings/feature.cpp:83-88` disable 时释放锚定按钮；`:1156-1162` 每帧处理主菜单入口可见性；`core.custom_ui/plugin.cpp:264-293` enable/disable 会安装/撤销 choice、confirmation 与 debug panels。 | missing | 正常重装/恢复成功、入口重绑成功、disable 后释放完成均无状态日志；仅 `settings/feature.cpp:58-68` panels capability 缺失有 W，custom UI confirmation overlay 安装失败在 `plugin.cpp:267-274` 有 W。建议只记录一次安装/恢复完成及最终失败，不记录每帧重绑检查。 |
| 用户可见错误提示与错误归因 | `settings/panels/settings.cpp:131-134,597-605,620-636,661-663,681-737,785-866` 错误显示在 `g_status`/item message；设置面板构造失败有多处 W（`:483-538,904-950`）。 | weak | 用户能看到提示，但非构造类失败缺少日志关联 `feature/section/reason`；仅把关键最终失败落 W，避免 UI status 与下层同一失败重复记录。 |
| Custom UI confirmation dialog/overlay | `core.custom_ui/ui_confirmation.cpp:917-927` frame overlay 注册失败 W；`:230-234` native popup draw 失败 I；注销失败在 `:260-287` 有 W。 | weak | 注册/注销明确失败有覆盖；native popup draw failed 文案没有 `op/owner/reason`，且使用 I；确认框成功打开、用户 confirm/cancel/关闭没有生命周期结果日志。建议只补确认框打开/关闭完成及实际 draw 降级失败的关键事件，不记录每帧 draw。 |
| Debug help / scroll / components panels | `debug_help.cpp:54-60,163-197`、`debug_scroll.cpp:45-50,140-196`、`debug_components.cpp:188-197,1430-1472` 分别以共用 log_result/debug_log 记录调试操作和有限诊断。 | weak | 有调试页事件，但共用出口把选择、关闭、失败、绘制错误混记 I；本审计不建议逐调用点拆分。除明确失败/关键状态缺失外，保持现有调试日志，不新增逐项明细。 |

## 优先补全项

1. **设置请求的提交/receipt 查询失败 W**：在 toggle、choice、entry 请求被拒或 receipt 查询最终失败处记录一次；字段 `feature=`、`section=`、`reason=`。目前只更新屏幕提示，属于操作失败而非逐帧状态。
2. **设置页实际关闭与分类切换 I**：只在关闭状态变化及选中分类实际切换后记录；字段 `panel=settings`、关闭时 `result=closed`，切换时 `section=`。不得记录每个 list callback/process。
3. **特性联动状态改变**：在 `refresh_owner_availability` 发现 owner 状态变化时按 owner 记录一次 I/W；字段 `owner=`、`result=` 或 `reason=`。无需对该 owner 下每条 feature 分别输出。
4. **UI provider 打开失败原因 W**：补齐 PopupHost provider.open、process-info 设置、stack capacity/generation 与捕获 owned snapshot 等关键打开失败出口；字段 `panel=`、`owner=`、`reason=`。已存在 popup resolve 失败，不重复记录同一失败。
5. **设置页 action/entry 完成结果**：在 continuation 的最终成功/失败位置记录一次，字段 `feature=`、`result=`，失败时 `reason=`；不在 300-tick 轮询中逐帧输出。
6. **重装/恢复生命周期**：settings provider/入口重新绑定和 UI 资源恢复成功或最终失败时各记录一次，字段 `op=`、`panel=`/`owner=`、`result=`；排除每帧 ensure/draw 检查。
7. **确认框生命周期及降级**：成功 open 与 confirm/cancel 完成状态目前无日志；native popup draw 失败已有日志但需作为 W 且带 `owner=`、`reason=`。只覆盖事件边界，不记录逐帧 draw。
