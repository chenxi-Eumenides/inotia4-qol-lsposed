# `autosell` 插件参考

## 摘要

功能插件（声明式资源 owner、叶子、无 `provides`）：拥有自动出售判定策略、按存档筛选配置、背包页入口按钮和游戏内配置面板。出售事务由 `inventory_hooks` 的 `inventory.action` 执行；物品读取走 `item.facts`/Game Host；宝石质量走 `attribute.quality`；按存档 sidecar 读写走 `core.save` 的 `slot.data`。唯一开关为 descriptor feature `autosell.auto_sell`。插件不提供能力，不被其他插件依赖。

除特别说明，文中路径均相对 `module/app/src/main/cpp/`。

## 定位与版本

- 插件 id `autosell`，版本 `0.1.0`（`plugins/autosell/plugin.cpp:35-36`）。
- descriptor：`requires=6`、`optional_requires=3`、`provides=0`、`resources=2`、`jni=0`、`ports=1`、`features=1`、`http_route_metadata=2`（`plugin.cpp:283-306`）；`routes.inc` 两条（`plugins/autosell/routes.inc:1-2`）。
- 注册于 `host/assembly.cpp:216`。
- 文件规模：`plugin.cpp` 305 行、`config.cpp` 81、`read_only.cpp` 142、`view.cpp` 102、`rules.cpp` 109、`scan.cpp` 585、`scan_session.cpp` 58、`store.cpp` 704、`ui/panel.cpp` 862。
- 稳定端口头：`contracts/autosell.h`（`Config`、`SpecialType`、运行时配置/持久化声明）。

## 公开能力与路由

无 capability provides。唯一用户可见 feature（`plugin.cpp:260-265`，`docs/reference/plugins/settings.md:101`）：

| 字段 | 值 |
|---|---|
| id | `autosell.auto_sell` |
| default | `0`（关闭） |
| category / type | `kGameplay` / `kToggle` |
| name / description | 自动出售 / 自动出售符合当前存档筛选规则的物品。 |
| `display_priority` | `-1` |
| `on_toggle` | no-op（返回 `ok()`） |

feature 的 `want_run` 驱动插件安装/停用；插件进入 `install` 即视为启用（`plugin.cpp:208-212`）。旧扁平键 `autoSellEnabled` 映射到该 feature（`docs/reference/plugins/settings.md:149`）。

HTTP 路由由 Kotlin controller 直接处理（native 只声明元数据，`owner="autosell"`、`op_gated=false`、`lease_required=true`）：

| 方法 | 路径 | 行为 |
|---|---|---|
| GET | `/api/feature/autosell/config` | 返回 `autosell_status_json()` 原文（`config.cpp:57-81`）：`enabled`、`rarity`、`enhance`、`socket`、`gemTier`、`gemRange`、`special`（类型名数组）、`sold`、`failed`、`lastScanFrame`、`slot`、`loadedSlot`、`persisted`、`hostInstalled` |
| POST | `/api/feature/autosell/config` | 解析 JSON 后经 JNI 写入运行时配置并按当前 session 持久化；返回写后的同一状态 JSON |

POST 字段：`enabled`（bool）、`rarity` 0..5、`enhance` 0..32、`socket` 0..16、`gemTier` 0..5、`gemRange` 0..5、`special` 字符串数组（`backpack`/`normalSeal`/`dice`）。字段类型、整数范围或 `special` 元素非法时返回 400；JSON 非法返回 400 `bad request`；当前无活动存档 session 时返回 409 `config rejected (no active save session)`，且不修改运行时配置（`AutoSellFeatureController.kt:37-106`、`store.cpp:609-650`）。native 侧保留数字钳制作为最后防线；缺失字段按 0/false/空数组处理。成功响应为写后的状态 JSON，不含 `ok` 包裹。

JNI 面（`bridge/native/gamebridge_autosell.cpp`）：
- `nativeSetAutoSellConfig`：钳制入参 → `autosell_store_commit_config`，返回实际提交结果（成功 `JNI_TRUE`，无 session/持久化失败 `JNI_FALSE`）（`:26-60`）。
- `nativeAutoSellStatusJson`：返回状态 JSON（`:81-88`）。
- `nativeSetAutoSellEnabled`：冻结兼容 stub，忽略参数返回 `JNI_FALSE`（`:62-68`）。
- `nativeAutoSellRunNow`：保留签名，无立即扫描，返回 `JNI_TRUE`（`:70-79`）。

## 游戏内行为

### 入口按钮

- 绘制宿主为 `patch:SCENE_DRAW_EQUIP+0x1cc` 的 `bl UIDesc_Draw` 调用点；wrapper 先复刻原调用再绘制入口（`plugins/autosell/ui/panel.cpp:766-770`）。
- 入口挂在原版袋容器（与扩展背包页签同宿主），位置按袋 0 绝对坐标 +129、任务袋行对齐，尺寸 68×68（`panel.cpp:120-133,393-429`）。
- 绘制使用 world HUD 齿轮 `unit 0x16 / loc 0x00`、flip=1；不可用时退化为深色底 + 金边 +「售」字（`panel.cpp:135-140`、`core.custom_ui/ui_components.cpp` 的 `icon_button_draw`）。
- 句柄存活与身份校验由 `ui_components` 的锚定按钮（minor 20）统一负责（`panel.cpp:179,431-457`）：每帧 `anchored_button_ensure` 按 `root_kind` 实时解析活袋容器，先做子链表纯指针比对（不解引用句柄），再用 Host 的 `click_binding_is_current`（“点击绑定元组 + ExecuteProc 仍是 adapter trampoline”）证明身份。游戏在切页/关闭背包页时销毁容器子树，句柄随之失效且地址可能被扩展背包页签复用；失效/不符时只注销组件记录（不删 native）并重建，**不得 delete**（命中失效内存即 SIGSEGV，命中复用控件会删掉扩展背包页签）。关闭态用 `anchored_button_set_visible(0)` 只隐藏可证明归属的控件。
- 锚定层在 `unbind` 时跳过 `native_externally_owned` 组件，不删除游戏拥有的 native；`forget_native` 与 `click_binding_is_current` 见 `contracts/ui.h:392-410,636-644` 与 `plugins/core.custom_ui/ui_components.cpp`。

### 配置面板

- 通过 `own:popup.autosell` 占用 `PopupState` 死条目 `F_PANEL_UNK3_ENTER` 的五回调；`UI_SetPopupProcessInfo` 打开/关闭（`panel.cpp:896-937,868-892`）。
- 面板使用 `core.custom_ui` 组件：主面板装饰框、装备/宝石无装饰角分组框、屏幕左上返回、屏幕右上 √/X 总开关、文本按钮规则/类别控件与缩放原生箭头（`panel.cpp`、`contracts/ui.h`）。
- 规则项与档位：装备品质（关/≤白/≤绿/≤蓝/≤黄/≤紫）、装备强化耐久度（关/≤0..≤31）、装备总孔数（关/≤0..≤15）、宝石档位（关/≤低级..≤混沌）、宝石属性范围（关/≤30%/≤60%/≤75%/≤90%/≤99%）。
- 面板编辑本地草稿 `g_draft`；左上返回释放时调用 `autosell_store_commit_config` 提交，过程中不即时生效；右上 √/X 切换 `enabled`。
- 面板也登记为 `PanelProviderSpec`（`panel_id="autosell"`），供 `core.ui` 面板容器打开/查询；关闭态 `provider_open` 拒绝（`plugin.cpp:84-97`、`panel.cpp:868-892`）。

### 自动扫描与出售

- 主线程帧任务每 60 帧执行一次（`scan.h:17`、`scan.cpp:505-536`）。
- 门禁顺序（`scan.cpp:505-536`）：feature 全局开启 → 存档 session（slot+epoch）活跃 → 存档配置 `enabled` → 世界/游戏状态与 UI 空闲（`scan.cpp:322-360`）。
- 世界门禁：`world.facts.state == 5`；`game_state_gate != 0`；EB `module_view_installed()` 为假；`ui.state.read_layers` 中存在非 `none` 的面板/对话框层时视为 UI 占用（`scan.cpp:319-378`）。UI 门禁独立于 `extension_bag`；EB 能力不可用时物理袋继续扫描、逻辑袋 6..10 跳过。
- save-exit 会使 slot-data session 失效，下一次 `sd_load` 成功前配置写入被拒绝（`scan.cpp:612-617`、`store.cpp:609-650`）。
- 每轮扫描在 tick 入口捕获取消纪元；关闭 feature、关闭存档配置或 save-exit 会递增纪元，扫描在下一个槽/下一笔出售前停止（`scan.cpp:505-536,539-547`）。已进入 `inventory.action.sell` 的单笔事务不中断。
- 扫描范围：物理袋 0..4（inventory_extras port 遍历，任务袋 5 排除）；逻辑袋 6..10 仅在 EB `is_logical_bag` 为真时，经 `item.facts.item_ref_at` 逐槽读取，要求 provider 句柄、有效 handle 且 category>0（`scan.cpp:21-24,425-502`）。
- 视图构建：`item.facts.view` 提供 rarity/equip/enchant_remaining/socket_filled/socket_total 等字段；缺 `HasRarity` 时 rarity 记 `-1`，不因缺少品质字段跳过整件物品；`is_jewel_category` 判宝石；宝石质量经 `attribute.quality.query_item` 取百分位，探测不可用/失败保持 `-1`（`view.cpp:40-102`）。
- 特殊类型：背包/骰子经 Game Host `is_backpack_category`/`is_dice`；普通骰子仅 category 82..85，混沌骰子 category 86 不设置 `kSpecialDice`（永不出售）；普通徽章固定 category 42..47（`view.cpp:14-21,92-107`）。
- 出售前排除 `item.facts.is_no_sell` 命中的类别；出售调用 `inventory.action.sell(bag, slot, handle, "autosell")`（`scan.cpp:362-421`）。
- `stale`/`empty_slot`/`no_item_handle` 静默跳过；其他失败计入 `failed` 并打 WARN；成功计入 `sold`（`scan.cpp:408-421`）。

### 规则语义

纯规则引擎见 `plugins/autosell/rules.cpp:10-112` 与 `contracts/autosell.h:10-46`：

- 「值即开关」：0=关；正整数 v 表示阈值 `v-1`，比较方向统一 `<=`。
- 装备三规则（rarity/enhance/socket）同类内 **AND**：所有已开启规则必须同时命中；全关不参与。rarity 未知（`-1`）且 rarity 规则开启时不命中（fail-closed）。
- 宝石两规则（gemTier/gemRange）同类内 **OR**。
- 装备、宝石、特殊类型之间 **OR**，任一命中即出售。
- 硬保护优先于一切规则：已强化（`I_ENCHANT bits6-10>0`）或已镶嵌（`I_SOCKET bits0-3>0`）的物品永不出售（`rules.cpp:90-95`）。
- 宝石属性范围阈值：1→30、2→60、3→75、4→90、5→99；百分位 `-1` 不参与（fail-closed）。
- `Config.enabled=false` 时恒不出售。

### 资源 claim

2 条常驻资源，`gate=nullptr`、`resumable=true`、`owner="autosell"`、`step_budget=120`（`plugin.cpp:100-129`）：

| id | mechanism | requirement / failure | handler |
|---|---|---|---|
| `patch:SCENE_DRAW_EQUIP+0x1cc` | `kCallPatch` | `kRequired` / `kFailClosed` | `autosell_equip_desc_draw_wrapper` |
| `own:popup.autosell` | `kOwnership` | `kRequired` / `kFailSoft` | `kPopupOwnershipOps`（acquire/release 写回 PopupState） |

## 使用的 Host SDK 与 Game 面

required capabilities（`plugin.cpp:38-52`）：

| 能力 | 版本 | 使用点 |
|---|---|---|
| `attribute.quality` | 2.0 | 宝石百分位（`view.cpp:66-90`） |
| `inventory.action` | 2.0 | 出售事务（`scan.cpp:70-85,362-421`） |
| `world.facts` | 2.0 | 世界状态与游戏状态门禁（`read_only.cpp:106-115`） |
| `item.facts` | 3.6 | 物品 view/category/is_jewel/is_no_sell/item_ref_at（`read_only.cpp`） |
| `save.slot_data` | 1.0 | sidecar 配置读写（`store.cpp`） |
| `ui.state` | 1.1 | 扫描 UI 占用门禁（`scan.cpp:339-378`） |

optional capabilities（`plugin.cpp:53-60`）：
- `extension_bag` 3.0：逻辑袋判定与 module view 门禁；运行期按需 `require`，缺失时物理袋继续扫描、逻辑袋 6..10 跳过。
- `http.registry` 1.1：路由元数据。
- `ui_panels` 1.3：`deactivate` 时撤销本插件在 core.ui 面板容器的占用（动态 require）。

port：`QOL_PORT_INVENTORY_EXTRAS` 1.0（`plugin.cpp:69-73`），仅用于物理袋 0..4 遍历（`scan.cpp:425-450`）。

Host SDK：`log`、`scheduler`（扫描任务 + slot-data 重试任务）、`events`（save_enter/save_exit）、`capabilities`、`panels.register_provider/unregister_owner`、`hooks.popup_resolve/popup_acquire/popup_restore`、`ui` 适配面。

Game Host：`read_only.item.is_backpack_category`、`read_only.item.is_dice`（`read_only.cpp:68-78`）。

## 消费者与职责边界

- 无插件级消费者：不 `provides`，其他插件不 `require` 本插件。
- `contracts/autosell.h` 的调用方是 bridge JNI（`gamebridge_autosell.cpp`）与自身面板（`ui/panel.cpp:19-23`）。
- 下游依赖：`inventory_hooks`（`inventory.action`）、`attribute_quality`、`item.facts`/`core.item`、`world.facts`/`core.world`、`core.save`（slot-data）、`core.ui_state`（`ui.state`）。上述任一 required 能力不可用时，插件由 Host 判为 `blocked`；`extension_bag` 关闭只影响逻辑袋扫描与 module view 门禁。
- 资源交界：扩展背包占用同一绘制函数的 `+0x210` `bl GRPX_End`，本插件占用 `+0x1cc` `bl UIDesc_Draw`，两者站点不同（`panel.cpp:3-8`）。
- 边界外：售价与扣减/退款（`inventory_hooks`）、物品内存读取（`item.facts`/Game Host）、属性范围计算（`attribute_quality`）、扩展背包存储（`extension_bag`）、sidecar 文件 IO（`core.save`）、设置页渲染（`settings`）。

## 读写、线程与生命周期

- 配置快照：`g_config_mtx` 保护 `g_config`；JVM/主线程可读写（`config.cpp:16-39`）。
- 扫描：仅在游戏主线程由 `infra::scheduler` 派发；tick 不在插件私有锁内调用其他插件，跨插件调用不持锁（`scan.h:9-14`、`scan.cpp:505-536`）。不取扩展背包虚拟袋锁、不调 `op_ok()`、不主动 save。
- 任务生命周期：`g_task_mtx` 保护任务句柄；注册条件 = feature 全局开启 + session 活跃 + 存档配置 `enabled`，取得任务锁后再次复核（`scan.cpp:549-582`）。
- 轮次取消：`g_scan_cancel_epoch` 在 tick 入口捕获；关闭 feature/存档配置或 save-exit 递增纪元，当前轮次在下一个槽或下一笔出售前停止（`scan.cpp:32,308-320,505-547`）。
- 存档事件：`save_enter` 校验 slot 与 store session，成功则同步任务；`save_exit` 关闭 session 并删除任务（`scan.cpp:582-610`）。回调在 install 注册、deactivate 注销，幂等（`scan.cpp:628-654`）。
- 持久化写路径：面板/JNI → `autosell_store_commit_config` → 更新运行时配置、递增 revision、置 dirty、`request_write`；无活动 session 或 session 未 load 时拒绝，不修改运行时配置（`store.cpp:609-650`）。
- sidecar：section `autosell` v2、`SessionBound` 读、`Immediate` 写；load 前严格 JSON 校验与字段钳制；`(slot, epoch)` 双校验，过期回调不生效（`store.cpp:20-21,554-558`、`scan_session.cpp`）。
- 重试：每 60 帧 `store_flush` 补发 dirty revision；完成回调只按 `captured_revision == revision` 清 dirty，不在回调内重入 provider API，更新 revision 留给 retry task（`store.cpp:192-223,349-383,479-501`）。
- 生命周期：`install` 取 world.facts/item.facts/slot_data/ui.state，注册 save 回调、`apply_gate(true)`、注册面板 provider；失败回滚全局指针并返回 1（`plugin.cpp:203-229`）。`deactivate` 注销 slot-data participant、撤销面板 owner、注销 save 回调、清指针、`apply_gate(false)`、注销面板 provider（`plugin.cpp:231-252`）。`on_enabled` 重新获取四项能力并重注册 slot-data，失败保持门禁关闭；`on_disabled` 注销 slot-data 并清空缓存的 capability 指针（`plugin.cpp:254-269`）。
- UI 门控：`g_enabled` 同时门控入口按钮与面板；关闭时移除入口引用并请求关闭面板（`panel.cpp:843-864`）。

## 失败语义

- 扫描门禁失败按原因节流记录：`global_disabled`、`session_inactive`、`config_disabled`、`world_unavailable`、`not_in_world`、`game_state_unavailable`、`game_state_blocked`、`module_view_installed`、`ui_busy`（`scan.cpp:86-130,505-536`）。物理袋遍历失败按错误码变化限速 WARN；`inventory.action` 能力或 `sell` 不可用时按状态变化限速 WARN（`scan.cpp:83-90,443-450,394-410`）。
- 入口绘制资源为 `kFailClosed`：安装失败会阻止插件进入可用态；PopupState 占用为 `kFailSoft`，失败按整段快照写回策略处理（`plugin.cpp:100-129`）。
- `install` 任一步失败返回 1，已获取的全局指针回滚；`register_provider` 失败同样返回 1（`plugin.cpp:187-213`）。
- sidecar load 遇到 `kUnreadable`/`kVersionUnsupported` 返回错误；JSON 非法、超过 4096 字节或未来版本返回错误（`store.cpp:265-326`）。
- 写请求失败时保留 dirty，由重试任务补发；unregister 失败会恢复 handle 并重新确保重试任务（`store.cpp:479-501,660-703`）。
- JNI 配置写入返回 `autosell_store_commit_config` 的实际结果；无活动 session 时 HTTP 层返回 409，成功响应的 `persisted` 反映持久化状态。

## 文件地图

| 文件 | 事实位置 |
|---|---|
| `plugins/autosell/plugin.cpp` | `:35-36` id/version；`:38-52` requires；`:55-63` optional；`:65-76` 路由元数据/ports；`:84-97` 面板 provider/ownership；`:100-129` 资源；`:143-269` 能力获取/生命周期；`:275-306` feature/descriptor |
| `plugins/autosell/routes.inc` | `:1-2` 两条 HTTP 路由元数据 |
| `plugins/autosell/rules.cpp` | `:10-112` 规则判定与硬保护 |
| `plugins/autosell/config.cpp` | `:16-45` 配置快照/计数；`:57-81` 状态 JSON |
| `plugins/autosell/read_only.cpp` | `:20-140` item.facts/Game Host 读取封装 |
| `plugins/autosell/view.cpp` | `:21-102` 物品引用 → 规则视图 |
| `plugins/autosell/scan.cpp` | `:322-378` 门禁（含 ui.state）；`:362-421` 单件处置；`:425-502` 扫描体；`:505-547` tick/取消；`:549-582` 任务同步；`:582-654` 存档事件/注册 |
| `plugins/autosell/scan_session.cpp` | `:15-58` session slot/epoch 状态机 |
| `plugins/autosell/scan.h` | `:17` 60 帧节拍；`:22-48` 生命周期接口 |
| `plugins/autosell/store.h` | `:54-191` special 映射与 sidecar JSON；`:193-205` participant 接口 |
| `plugins/autosell/store.cpp` | `:20-21` section v2；`:265-383` load/capture/complete；`:385-501` 写请求/重试；`:522-703` 注册/提交/注销 |
| `plugins/autosell/ui/panel.cpp` | `:120-133` 入口常量；`:385-457` 入口点击/布局/锚定 ensure；`:577-764` 面板绘制/事件；`:766-770` 绘制宿主；`:791-862` PanelProvider 与 PopupState |
| `plugins/autosell/ui/panel.h` | 入口/面板接口声明 |
| `contracts/autosell.h` | `:10-46` Config/SpecialType；运行时配置声明 |
| `bridge/native/gamebridge_autosell.cpp` | `:26-88` 4 个 JNI 导出 |
| `module/app/src/main/java/com/inotia4/qol/api/controller/AutoSellFeatureController.kt` | `:37-106` GET/POST 路由与严格校验 |
| `plugins/autosell/CMakeLists.txt` | 插件静态库源文件清单 |
| `tests/test_autosell_rules.cpp`、`test_autosell_view_diagnostic.cpp`、`test_autosell_store.cpp`、`test_autosell_slot_data.cpp`、`test_autosell_plugin.cpp` | host 5 个 target（`tests/CMakeLists.txt:306-358,1087-1098`） |

验收状态、缺陷与无法验证项分别登记在 `docs/refactor/verification.md`、`docs/refactor/issues.md` 和对应审计 `verification-log.md`，不在本文件内作结论。
