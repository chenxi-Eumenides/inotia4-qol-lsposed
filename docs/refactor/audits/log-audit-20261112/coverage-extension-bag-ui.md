# 功能级日志覆盖审计：extension_bag UI

依据 `docs/reference/logging-standards.md`。按 UI 装配 claim 与 PopupState（`assembly/claims.cpp:254-561`）梳理装备页、商店页、合成视图、详情动作、标签与拖拽入口。为体现完整 UI 行为链，表中同时引用 UI 支撑实现 `core/tabs_ui.cpp`、`core/host_adapter.cpp`、`core/drop_routing.cpp` 与 `core/projection.cpp`；不审计其他组件。`present` 表示关键生命周期及结果有合适日志；`weak` 表示已有日志但生命周期、最终结果、字段或等级仍有缺口；`missing` 表示未见该关键事件日志。

| 功能事件 | 触发条件 | 目标等级 | 现有日志 file:line | 状态 | 建议（含建议文案） |
|---|---|---|---|---|---|
| UI hook / PopupState 装配与卸载 | 装备、商店、混装 PopupState 或 UI entry hook 获取、释放 | I（成功）；W（单项失败） | `ui/equip_hooks.cpp:46,148,165,249,259`；注册表见 `assembly/claims.cpp:138-205,254-561` | weak | equip hook backup 已绑定且缺失时有日志；store/mix PopupState 获取和恢复没有本域的明确结果记录。若资源框架日志不足，按实际状态变化记 `install hook=<hook> result=bound` / `restore hook=<hook> result=restored`，失败用 `reason=`。 |
| 装备页扩展标签创建、重挂与删除 | 装备页 draw-end 检查/重建锚定控件，面板或 root 销毁 | I（有限安装变化）；W（失败）；D（诊断） | `core/tabs_ui.cpp:278,283,329,337-389` | present | 已有安装状态、创建/ensure 失败及 root 失效日志；重挂细节为 D。逐帧 ensure 的正常跳过无须补日志。 |
| 装备页面板打开/关闭 | `UIEquip` enter/F3 或 scene teardown | I | `core/tabs_ui.cpp:378,389`（标签安装/失效，不是面板生命周期） | missing | 没有明确面板进入/关闭结果。只在 `frame_active` 实际变化时记 `open panel=equip result=entered`、`close panel=equip result=closed`。 |
| 商店面板打开/关闭 | `store_enter_wrapper` / `store_f3_wrapper` | I | `ui/store_projection.cpp:363-410`（enter/F3 包装无生命周期结果） | missing | 投影切换日志不能替代面板开关结果；只在状态变化时记 `open panel=store result=entered`、`close panel=store result=closed`。 |
| 商店扩展视图安装/还原 | 选择商店扩展袋；切回原版袋或退出视图 | I | `ui/store_projection.cpp:303,345` | present | 已按视图状态变化记录 `op=store_view result=installed/restored bag=`，不需要额外逐帧记录。 |
| 商店详情页打开 | 商店物品详情交由 `store_make_desc_gate` 构建 | I（有限打开状态）；W（构建失败） | `ui/store_projection.cpp:416-433`（调用原版详情，无结果日志）；`ui/detail_actions.cpp:656` 只记录出售确认框创建 | missing | 商店详情打开及构建失败没有覆盖。建议仅在扩展物品详情实际打开/失败时记 `open panel=store_detail item=%p bag=%d slot=%d result=opened` 或 `fail op=store_detail reason=<原因>`；不记每次详情按钮调用。 |
| 商店出售 | 出售按钮打开数量框、报价校验、出售提交/拒绝 | I（提交）；W（拒绝/降级） | `ui/store_trade.cpp:108,176,184,197,212`；装备详情出售 `ui/detail_actions.cpp:215,262,296,304,319` | weak | 成交和主要拒绝已有记录；商店 action 拒绝 `197` 缺 `reason=`，数量框回退 `108` 未说明 `reason=`，且 `191-193` action API 缺失会静默拒绝。建议统一最终失败为 `reject op=store_sell bag=%d slot=%d item=%p count=%d reason=<原因>`，成功保留单条提交结果。 |
| 商店买入到扩展袋 | 原版袋无空位时调用买入查槽 gate；扩展袋预检并由原版完成买入 | I（实际买入成功）；W（实际买入失败） | `ui/store_trade.cpp:224-285`（只记录查槽/预检失败，无买入最终结果） | weak | 当前是 gate 过程诊断，不能确认用户买入是否最终完成。建议在最终交易结果唯一所有者记录 `apply op=store_buy item=%p bag=%d slot=%d result=committed` 或 `reject op=store_buy item=%p reason=<原因>`；不要在每个 preflight 分支重复记。 |
| 扩展背包物品装备为扩展袋 | 装备按钮或拖到空扩展标签，完成状态持久化/源物品释放 | I（成功）；W（失败/拒绝） | `ui/equip.cpp:103,123,127,142,147,163,171,187,279,284,317`；按钮路径 `core/host_adapter.cpp:557-576` | present | 装备成功与主要持久化、源释放失败均有记录；未要求为每个拒绝前置条件增加日志。按钮路径 `core/host_adapter.cpp:568` 的失败已有结果但原因字段薄弱，必要时统一 `reason=`。 |
| 普通装备栏装备扩展物品 | 详情页点击装备，角色槽位校验并执行装备事务 | I（成功）；W（最终拒绝） | `ui/equip.cpp:233-237`；`core/host_adapter.cpp:342-353`；封锁透传 `ui/equip_hooks.cpp:101` | weak | 事务成功与总体拒绝已有记录，但前置失败统一成 `equip_rejected`，不能区分可装备性、目标槽不可用或 action 失败。最终拒绝建议带 `target_slot=` 与稳定原因：`reject op=equip_detail bag=%d slot=%d item=%p target_slot=%d reason=<原因>`，只在最终拒绝点记一次。 |
| 扩展袋卸下 | 详情页卸下按钮完成扩展袋转移/持久化，或被拒 | I（成功）；W（拒绝/失败） | `core/tabs_ui.cpp:432`；`core/host_adapter.cpp:87-140,220-284,301-305` | weak | 事务有成功、回滚失败和 stale 拒绝日志；最终入口 `305` 仅枚举 `result=%d`，且正常成功 `255` 未统一到入口结果。建议由唯一入口记录 `apply op=bag_unequip bag=%d result=committed` 或 `reject op=bag_unequip bag=%d reason=<原因>`，避免与事务层同一失败重复记。 |
| 装备页扩展袋标签切换/检查详情 | 点击不同标签切换投影；二次点击打开袋详情 | I（状态切换/打开）；W（失败）；D（分支细节） | `core/tabs_ui.cpp:486-516,528-544,575-668` | weak | 正常切换与袋详情打开有 I；投影失败 `498,509` 仍用默认 INFO，且缺少 `reason=`/结果字段。改为 `W reject op=tab_select bag=%d reason=projection_install_failed`；保持当前 D 调试日志不升级。 |
| 商店扩展标签切换 | 商店页点击扩展袋标签并应用投影 | I；W（视图安装失败） | `ui/store_projection.cpp:239-269,303`；标签注册 `ui/store_tabs.cpp:189,194,274` | present | 安装视图时有 bag 与结果；锚定控件创建/设置失败为 W。点击过程不必单独记，现有状态变更点足够。 |
| 混装视图切换及恢复 | 混装面板切换扩展袋、切回原版袋、面板 teardown | I（状态变化）；W（失败）；D（逐帧状态诊断） | `ui/mix.cpp:352,366,387,402,414,443,449,467,608` | weak | 安装/恢复结果已有 I；但缺 adapter 与 item root 缺失仍为 I（`352,387`），应为可恢复失败 W 并带 `reason=`；`449` 另有一条旧式 restore INFO 与新状态结果重复，建议确认是否合并，避免同一恢复事实双记。 |
| 扩展物品使用 | 详情页直接使用、确认使用及专用物品动作 | I（成功消耗/状态变化）；W（拒绝）；D（过程） | `ui/detail_actions.cpp:456,483-500,539-557,728-758,793-818` | weak | lock/action 拒绝有 W，过程和结果主要在 D，默认日志无法回放成功消耗。只在一次使用事务最终提交时增加 `apply op=item_use bag=%d slot=%d item=%p result=consumed`；不要为各按钮分别记点击。 |
| 出售/使用确认对话框确认与取消 | 创建自定义出售确认框，确认提交或取消关闭 | I（打开/完成状态变化）；W（创建失败） | `ui/detail_actions.cpp:91,163-191,194-321,656-669` | weak | 创建与出售完成/失败有日志，取消回调无结果；且打开成功只有通用创建记录。建议仅补取消状态 `close panel=confirmation op=store_sell result=cancelled`，确认结果继续由唯一交易结果日志承载。 |
| 拖拽释放与最终落点 | 用户松开拖动，路由到扩展袋、原版袋或装备标签 | I（提交）；W（拒绝）；D（输入/状态机过程） | `core/lifecycle.cpp:406-425`；`core/drop_routing.cpp:120-126,133-175,193-237,313,383,398,478-482`；`ui/equip.cpp:279-284,317` | weak | 拖拽入口有 D、各目标多有最终落点；但 `drop result` 的 `handled=0` 仍为 INFO，且个别失败只有坐标或无 `reason=`。将失败结果降为 W 并补 `source bag/slot`、`target bag/slot`、`reason=`；保留成功提交 I 与入口 D。 |
| 装备页投影安装、刷新与恢复 | 切换标签安装投影；原版刷新覆盖后重绑；root 失效重装 | I（状态变化）；W（单次失败）；D（诊断） | `core/projection.cpp:344-395,469,485-490,508-525` | weak | 安装成功仅有 D，恢复成功没有状态结果；失败路径已有 W。建议安装/恢复状态变化用 `I op=module_view result=installed/restored bag=%d`；slot 重绑日志当前 INFO 且可能由刷新路径重复触发，应降为 D。 |
| 商店/混装投影刷新失败及恢复 | 面板有效时刷新控件树失败、root 缺失后重试；inactive 时正常跳过/restore | W（刷新失败）；D（正常跳过）；I（视图恢复） | `ui/store_projection.cpp:120,139,150-157`；`ui/mix.cpp:352,366,377-388` | weak | code 失败已有 W；missing adapter 和 root 缺失却是 INFO，且未统一 `op=`。改为 `W fail op=refresh_store_item|refresh_mix_item bag=%d reason=<原因>`；inactive/未安装视图的正常跳过不补日志。 |

## 优先补全项

1. 商店买入最终结果：gate 只观察原版槽查找和扩展袋预检，没有买入是否提交的最终事实；在唯一交易结果所有者记录一次成功/拒绝。
2. UI 面板生命周期：装备页、商店页 enter/F3 无明确开关结果；在 `frame_active` 真实变化时记录，不在每帧 draw-end 记 INFO。
3. 商店详情打开结果：扩展物品详情走原版构建但没有打开/失败结果，出售确认日志不能替代详情面板状态。
4. 用户操作失败等级与字段：装备页标签投影失败和拖拽 `handled=0` 不应使用 INFO；失败需 W 且含 `reason=`、来源/目标槽。
5. 物品使用最终提交：目前成功结果主要为 D，默认日志无法判断物品是否实际消耗；只在事务提交点增加一条 I。
6. 混装刷新缺失资源：adapter/root 缺失属于可恢复失败，应为 W；保留非激活面板的正常跳过为 D/静默。
7. 卸下与确认取消：统一记录卸下最终事务结果及确认取消状态；避免入口与底层对同一失败重复记录。
