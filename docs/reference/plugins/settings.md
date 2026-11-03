# `settings` 插件

## 1. 作用与 owner 定位

`settings` 是模块设置整页的普通成品页插件，只负责页面呈现：页面展示 feature 开关、feature entry、feature choice 和只读「游戏信息」，不展示插件 desired/overall enable 行；这些 feature 经 Host 常驻管理 API 读写。它**不拥有**配置 schema、Store、持久化或配置 HTTP adapter——这些由 Host 常驻模块管理设施持有，与设置页生命周期分离。

配置文件与 Store 由 native Host 常驻设施管理，生命周期长于设置页插件。`GET /api/config/list`、`POST /api/config/set`、`POST /api/op/features/lifecycle`、`GET /api/config/receipt/{id}` 由 Host 常驻 resident route 直接处理，不经设置页插件；完整请求/响应契约见 [API 参考 §7.6](../api-reference.md)。

插件注册 ID 为 `settings`；源码目录为 `module/app/src/main/cpp/plugins/settings/`。三条 `/api/config/*` 路由的 route metadata owner 也为 `settings`，但 Host 仍按路径常驻处理、不经设置页插件 handler；配置 namespace 仍为 `core.settings`。源码目录名、运行时插件 ID、route owner 字符串与 Host 配置 namespace 是不同概念；目录改名不改变插件 ID 或 namespace。

## 2. 功能

### 页面入口 feature

descriptor 只注册一个 `page` entry feature：

| feature id | 默认 | 分类 | 类型 | 说明 |
|---|---:|---|---|---|
| `page` | 1 | Hidden | entry | 设置页常驻入口状态，不向玩家 catalog 暴露。 |

`page` 与其他 feature 一样有布尔状态并参与 `want_run` 判定；它是 `kEntry`，不是无状态动作。

### Host catalog 渲染

- 页面通过 Host 管理面 `read_catalog` 读取 feature catalog，本地构建行模型；catalog 元数据（owner/id/name/description/type/category/default/action_label/display_priority/choices）来自已注册 descriptor 或 Host-owned feature 定义，不持久化。catalog 包含全部已注册 feature 定义，并按 owner 身份排除设置页自身的 `settings`；不按 `core.*` namespace 过滤。分类名称与 priority 由 Host 的静态注册表提供，管理 SDK v1.26 的 `read_categories` 返回 Host 生命周期内不可变的 POD 数组。feature-control 管理面的 v1.28 尾字段 `read_owner_availability` 提供插件级可用性投影；v1.30 追加 choice 选项元数据及读写函数，不改变此前 ABI 前缀。分类按 category priority、条目按 descriptor `display_priority` 升序稳定排序，同 priority 保持注册顺序。Debug 显示 Hidden 分类；Release 隐藏整个 Hidden 分类，其他分类中的 feature 不因所属插件或 `core.*` 前缀而过滤。当前 Debug 构建定义 `QOL_DEBUG_COMPONENTS` 时 catalog 有 35 项（含 Host-owned 项）；未定义时少一个 Debug-only entry，为 34 项。Release 隐藏 Hidden 项后显示 20 项（14 toggle、2 choice、4 entry）。General 中的 `core.api / api.enabled`、Host-owned `module.platform / debug.logging`、`module.security / op.enabled` 与 `save_backup / save_backup.export_crash_logs` 在 Debug/Release 均显示。`extension_bag / move_merge.drag_merge` 是 BasicOptimization 分类中的非 Hidden feature（自 `core.item` 迁入）。
- Host 分类注册表包含六个 category ID 与名称：Hidden「调试开关」-10、GameInfo「游戏信息」-9、General「基础设置」-8、BasicOptimization「基础优化设置」-7、Gameplay「更多优化功能」-5、Tools「更多工具」-3。priority 较小者先显示；一般分类没有 feature 时不显示，GameInfo 是设置页保留的只读信息分类，例外保留。
- 每个 feature toggle 行只通过 `read_feature` 读取值，并经 `request_set_feature` 提交 feature 值；不读取或写入插件 desired/overall enable。Host 在每次成功的 feature 提交后（包括值未变化）根据全部已注册 feature 值重算 `enable`。
- `choice` 行通过 Host catalog 的静态 `choices` 元数据绘制。选项顺序与插件注册顺序一致，首项固定为隐式「关闭」，其后逐项对应稳定、非空的 `value` 与显示 `label`。右侧控件在原 toggle/entry 控件列内按 220 逻辑单位总宽布局：60 单位左箭头、100 单位选项文字、60 单位右箭头，整体与原按钮列同宽并共用中轴；箭头复用 `core.custom_ui` 的 `arrow_button_*` 组件。边界处对应箭头禁用，不循环。点击只经 `request_set_choice` 单次提交，并在 receipt 确认持久化后重新 `read_choice` 核实；pending 时两箭头禁用。失败、未知值、读取/API 不可用均不得显示为已保存：未知值显示「未知选项」且控件不可用，缺失能力也不伪装成「关闭」。该类型不等同 toggle，不调用 `request_set_config` 或 `request_set_feature`。
- 带 `on_entry` 的 entry 行仅显示 `action_label` 指定的动作按钮，不显示 toggle；entry 按钮的可用性不依赖其 feature 当前为 true。feature 为 false 时，设置页先检查 `QolFeatureControlApi` 覆盖 `read_actual` 尾字段且函数非空，再经 Host `request_set_feature(true)`；单独的 `EntryContinuation` 保存复制的 plugin/feature ID、request ID 与页面 generation，不与 toggle receipt 混用。每个设置页 process tick 最多回取一次 receipt；持久化确认后还须重新读到 feature 为 true、且 Host actual `enabled` 为 true 才调用一次 `invoke_entry`。receipt 与 actual 等待从请求 admission 起共用最多 300 个 process tick；`read_actual` 返回 NotReady 或 enabled 尚未为 true 时继续等待，其他读取错误、持久化失败或超时均走现有错误状态且不调用。页面关闭/销毁或 generation 改变会清除 continuation；Host 已持久化的 true 不回滚。feature 已为 true 时，同样先校验 `read_actual` ABI 并要求 actual `enabled==1`，随后直接调用一次 `invoke_entry`、不提交 feature 写入；actual 未就绪或读取失败时立即显示错误并停止。Host `invoke_entry` 成功仅代表 admission，不保证页面已打开。无 entry callback 的 toggle 保持可编辑，不受所属插件当前 `enable=false` 影响。
- 页面不再绘制顶部标题行；此前标题为 `QoL 模块设置 v<模块版本>`。模块版本仍用于「游戏信息」第三行。
- feature 开关不因所属插件当前 `enable=false` 而禁用，用户可通过开启 feature 恢复插件 desired 状态。直接调用 Host `invoke_entry` 的契约不变：只提交调用，不修改 feature/enable，Host 仍执行实际运行状态校验；设置页的先启用再调用仅是该 UI 的编排。
- Host `read_owner_availability` 返回稳定的插件级状态：Host-owned owner 为 `available`；普通 owner 的 `registered`、`resolving`、`installed_disabled`、`enabled`、`deactivating`、`inactive` 均为 `available`。`blocked` 显示“依赖不可用”，`unsupported` 显示“当前版本不支持”，`invalid` 显示“插件无效”；`failed` 按显式记录的失败阶段显示“安装失败”“激活失败”或通用“插件不可用”，不解析底层 `error` 文本。owner 不可用时保留其 catalog 行，但该 owner 的**所有 feature toggle、entry 与 choice**统一禁用，feature 名称改用错误红色，描述替换为对应短原因。恢复为 available 后恢复原描述和交互。正常 `inactive`（包括未启用）及 `installed_disabled` 仍可操作，以允许恢复开关；`degraded` 是正交健康标记，不改变 owner availability：仅存在 fail-soft/resource degradation 而 lifecycle 仍可用的 owner 仍可操作；required fail-closed 失败若令 owner 进入 `failed` 等不可用状态，则禁用其全部行。状态读写未加锁，`read_actual` 和 `read_owner_availability` 仅允许在 Host 主线程调用。
- 每行固定 96 逻辑单位高。标题与描述各最多绘制一行，超出宽度以省略号截断；无描述时标题垂直居中。动作按钮、toggle 或 choice 控件固定在行右侧，不增加第三行。

### 游戏信息分类

- 左栏「游戏信息」固定位于「调试开关」之后、「基础设置」之前；Release 隐藏「调试开关」时它是首个分类。
- 右栏不使用滚动列表或选项行，整屏居中绘制四行信息：
  - `艾诺迪亚4 v<游戏 APK versionName>`（例如 `艾诺迪亚4 v1.3.2`）
  - `<版本系列> <变体标签>`（例如 `Monster版 v29`；系列映射为 `原版` / `盗版大修` / `Monster版` / `C18版` / `未知版本`）
  - `QoL模块 v<模块版本>`
  - `Android <系统版本> (API <level>) <32位/64位>`（例如 `Android 13 (API 33) 64位`）
- 视觉层级：首行金色 font 2（38），第二、三行常规/暗色 font 3（28），第四行暗色 font 1（24）；四行在右栏内水平居中，行距按 58/44/44 逻辑单位排布。不绘制行分割线、行底色、toggle/entry 或信息区命中区。
- 数据来源与 `/api/health` 的 `variant` 相同：Game Host v2.4 尾接口 `game_identity.read` 读取 `game_variant()` / `game_series_name()` / `GameVariant.version_label`；宿主游戏 APK `versionName`、Android 系统版本与 API level 由启动期 Kotlin `PackageManager`/`Build.VERSION` 读取后经 `nativeSetSystemIdentity` 下发，32/64 位由 native `sizeof(void*)` 判定当前进程。模块版本复用 Host SDK `module_version`。Game Host 缺失、minor < 4、版本名或身份未下发时首行仅显示游戏名、第二行显示「未知版本」、第四行显示 `Android 未知 <位数>`，页面仍可打开。
- 选择该分类时不创建右栏控件；分类切换或撤页按既有右栏清理流程释放，不创建子控件，也不对信息区做命中测试。

### 调试端点

插件自注册四条 `/api/debug/settings-ui/*` POST 命令（inject/restore/open-option/open-panel），属调试端点，随插件 deactivate 显式注销 owner（置墓碑），不经 Host 常驻控制面。（`/api/debug/settings-ui/status` 为 GET，走 Kotlin/JNI status handler，不由插件命令注册。）

## 3. 依赖与消费面

- 必需 capability：`ui_panels` v1.3（面板 open/close）与 `ui_components` v2.23（原生控件与文本；Major 2 移除旧容器入口）。Host 注入的 `QolUiAdapter` ABI 为 v2.35；组件 capability 版本与 adapter ABI minor 各自独立协商。
- Host 管理面经 `feature_control` 窄接口取得：`read_catalog`、`read_categories`、`read_owner_availability`、`read_feature`、`request_set_feature`、`read_choice`、`request_set_choice`、`invoke_entry`、`receipt`。v1.26 引入的 category/catalog metadata 保持原 ABI 前缀；完整基础管理面要求 Host SDK ABI minor ≥28，以校验 `read_owner_availability` 尾字段和非空函数。choice 行另检查 v1.30 追加的 `read_choice`/`request_set_choice` 尾字段，缺失时该行不可用，不会回退到 config/feature 布尔写入。该 ABI 版本变化不改变 Android `minSdk`。`read_actual` 与 `read_owner_availability` 读取未加锁的 infra 状态表，只允许在 Host 主线程调用；不得从 HTTP 或后台线程调用。设置页 UI 不调用 `read_desired` 或 `request_desired`。
- Host 另为 Kotlin 内部 listener fallback 提供一次性 receipt observer：等待中的 receipt 完成时只通知一次；订阅已是终态的 receipt 时在 Host 锁外重放终态，不返回活动订阅 token。Host observer 容量为 256，JNI 回调携带 request id 与 `feature_receipt_json` 的 JSON。该内部推送不替代设置页 entry continuation 每个页面 process tick 至多一次的 receipt 查询，也不改变公开 `GET /api/config/receipt/{id}` 的轮询契约；内部 receipt 等待没有 200ms/50ms 定时轮询。
- 游戏信息分类消费 Game Host v2.4 尾接口 `game_identity.read`；`QolGameHost` 尾槽在 v2.4 内追加 `game_version_name`，不改变 `QolTargetApi.query_variant` 与既有前缀。插件不 include `game/`，变体事实仍由 Game Host 统一收口。
- 不再注册旧 settings catalog contribution：`settings_contribution.cpp` 的 `register_settings`/`register_categories` 为 no-op；feature 元数据只来自 descriptor。
- 不消费 `save_backup` panel action，也不登记任何面板项；备份页入口由 `save_backup` 自己的 entry feature 承担，按钮文案为其注册的 `打开`。

## 4. 生命周期与撤页重建

- 注册阶段安装 `panel_id="settings"` 的 `PanelProvider`，并启动 interval=1 的主线程帧任务维护「更多游戏」替换（主菜单可能重建原生控件）。插件注册 ID 为 `settings`，源码目录为 `plugins/settings/`；Host namespace 与 C++ namespace 仍为 `core.settings`。
- **2026-10-21 起主菜单入口改为叠加齿轮（不再替换「更多游戏」）**：`patch:settings.main_menu.more_games.draw` 钩子先照常绘制原版按钮，再由锚定层 `anchored_button_ensure/draw` 在同一父控件上绘制入口齿轮（`contracts::QolUiRootKind::kMainMenuParent`，由 Adapter 主菜单快照证明父控件存活）。齿轮资源与自动出售入口一致（unit `0x16` / loc `0x00`），尺寸 68×68（与自动出售入口相同，初版 136 已按需求减半），位置为原按钮相对 rect 右侧 +10、垂直居中；点击打开设置页。原按钮保持原 Show/Enabled/Active，不再置灰或隐藏。真机 monster v29 实测主菜单逻辑 rect：更多游戏 `(869,470,300,92)` → 齿轮 `(1179,482,68,68)`（右侧 +10、垂直居中、2 倍尺寸）；模块 `4daca3bd50d3`、集成包 `376acc3d…`，齿轮点击开设置页、原更多游戏按钮保持显示/可交互，进出存档页后入口仍为单个；设置页「打开调试面板」可正常打开调试页并返回（SET-03），无 FATAL/SIGSEGV。旧「模块设置」文本替换、绑定状态机与 original 恢复链已不再被运行时调用（`more_games_bind_tick`/`more_games_draw` 模型保留给既有 Host 测试）。
- 原生 More Games 图片由游戏在 `STATE_DRAW_MAIN_MENU` 中直接绘制，普通控件 Show/DrawProc 无法替代该调用。设置插件只声明一个 required、fail-closed call patch：`settings.main_menu.more_games.draw`，定位单个 `GRPX_DrawPart` BL（`+0xa70`，`+0xa6c` 是参数设置）。Android 仅 arm64 登记该目标，非 Android host test 构建也包含该站点；其他 Android ABI 不提供替代补丁。替换组件未被确认 live，或组件绘制失败时，调用原生绘制一次；仅组件绘制成功后抑制原图。
- 替换按钮沿用现有原生文本按钮绘制与标签机制，菜单按钮取原生快照 rect；其中 300×92 几何若标准 70 px 中段无法在 1 px 容差内匹配，会保留两侧圆角 cap、改用连续的中段裁切搜索。`test_native_text_button_render.cpp` 的 300×92 用例断言 slice 边界连续、绘制宽度为各 slice 取整宽度之和（无额外 `+1`）。这是该几何用例的宿主断言，不等于所有设备分辨率的逐像素验收。替换按钮的绘制 rect 另按 decorated 紧凑比例取值（`height = min(rect.h, rect.w × 48 / 220)` 且垂直居中），只影响绘制；原生控件 rect、命中范围与 identity 判定不变（`plugins/core.custom_ui/ui_components.cpp:785-812`；`tests/test_core_ui_components.cpp` 断言绘制 rect 高度比例与原控件 rect 未被改写）。按压态叠加同源橙色胶囊层（避开烘焙 dash，见 [`core.custom_ui.md`](core.custom_ui.md) §2）。
- **入口回归修复（2026-10-16；候选已构建，真机待验）**：`0ba1dad1` 同时引入 normal/pressed 画布并集命中区与父界/兄弟碰撞拒绝；旧工厂沿用原生 snapshot rect，没有该兄弟碰撞拒绝策略。现在恢复原生命中区，绘制范围另行计算，并保留对原生既有重叠的兼容。主菜单入口改用 `UiMainMenuDecoratedButtonDesc` 的**显式目标尺寸 `290×72`**（`settings.cpp:801-802`；该 POD 不再有 `height_percent`，工厂以原生控件中心为锚点直接使用该可见尺寸），28 号字、75/61/492、RGBA `x100`、Toggle、alpha 与 Renderer 算法不变；旧『272×96 绘制框 / `height_percent=150`』表述已废弃。**Active 协议（含一次错误候选的撤回）**：原生 `ControlObject_SetActive`（v29 `0x9dbd8`）收的是 **bool 入参**（`uxtb` 后按入参是否为零 `csel` 写 `ctrl+0xc` 为 `0x20/0x21`），`0x20/0x21` 是**内部编码后的存储值**；事件门（`0x9e244`、`0xaa818`）只读存储值 `==0x20` 才派发。故 HEAD 的 `true→1 / false→0` **本来就是正确协议**，settings 读取侧 `0x20/0x21` guard 也一贯正确、未放宽。**撤回**：本任务先前把「写入 `1/0` 改为 `0x20/0x21`」当作入口缺失的修复、并让测试替身「原样记入参」——两者都是**本任务的错误候选，不是原有缺陷**（传 `0x21`(33) 非零被原生化成 `0x20`，原按钮保持激活）。现 Host 已恢复 bool `1/0`，替身区分「callee 入参」与「编码后存储值」以模拟原生转换；Show/Enabled/原生 ABI/回调未动。
  - 有界可观察性：Host 与 settings 各加**每 stage 独立 32 槽**静态去重日志（stage 为内部字面量；同 stage 数值未变不输出、满表不再登记），覆盖 settings tail/snapshot/active/factory/bind/live/release/waiting 与 Host tree/identity/parent/sibling 拒绝；**不记录地址与用户文本**，不新增公共 endpoint/ABI；`reject-components-tail`/`reject-adapter-tail` 读取遵守 major/minor/size guard（不破坏尾槽短路顺序）。
  - **兄弟重叠守卫**（Host `main_menu_replacement_rect_available`）：读取原控件 rect 与绝对原点，若 `cand ∩ child ⊆ original_abs_rect` 则记 `sibling-overlap-existing` 并放行，否则记 `sibling-overlap-new` 拒绝；读不到原矩形、父界或身份不成立仍拒绝。原 rect 与该兄弟本身已有 **27×70** 交集，恢复原命中区时仍需兼容这部分原生布局；不采用像素阈值豁免。
  - 实测碰撞（`geometry-fix/current-runtime.log`，PID 15448 / module `bb7e…`）：原控件 `{869,470,300,92}`；候选 `{876,454,287,128}` ∩ 兄弟 `{527,492,369,124}` = `20×90`，交集底边由原控件的 562 扩至 582，仍被 `sibling-overlap-new` 拒绝（factory code 3）。此次 factory 在写 Active 前已失败；选择框修复没有改变此菜单工厂。
  - **回归点（Git 定位）**：`96a76` 的 `main_menu_text_button_create` 以**原 snapshot 作 hit**；`0ba1dad1` 同提交引入 decorated 工厂/父界兄弟拒绝，并把 hit 改成 normal+pressed 并集。前两轮把新守卫当既定前提、只调判据而未先比对旧版，**已纠正**；恢复原 hit 后越界消失，`sibling-overlap-existing` 方能合法命中。
  - 宽度已闭合：候选宽 **287** = body 405 换算 **235** + 两侧 inset **26×2**（不再保留「279 差 8 未知」的说法）。
  - **保留限制**：可见 96 高框比原生 92 高 **3/1px**，透明 margin 未做逐像素证明 ⇒ 不假报全视觉验收；**不能**宣布入口已恢复（仍待真机日志），也不再称唯一 root 为「原生既有重叠」。
  - 状态：**两阶段真机事实**——用户手测 `2390…`：入口**已可见**，但点击走**原生页面**（原按钮未被停用）；父 fresh 只读 `click-fix/current-runtime.log`（PID 16351 / module `2390…`）accepted/bound、`GET /api/debug/settings-ui/status` live 但 panel inactive；explorer 已证新按钮走独立 callback（`more_games_clicked`→settings）、未继承原 `ExecuteProc`，live 不证明原按钮已禁用。**本轮修复后点击是否进入设置页仍待用户以新候选手测**，不写「全恢复」。本轮 `ui_adapter_tests` 只编译并运行**一次**：**451 assertions PASS**（数量同前，但替身语义已更正，不算旧的同源复用）；`core_ui_components_tests` 源码未变、**211** 复用未重跑。直接 `scripts/build-debug.sh --offline` 成功（7s），`scripts/patch-apk.sh` 成功 4 / 失败 0。**新候选**模块 `output/inotia4_qol_lsposed_debug_2610161719_59f764b85682.apk`（SHA-256 `59f764b8…3e5402cb`）、canonical `output/Inotia4_v1.3.2_monster_v29_lspatched.apk`（SHA-256 `34bd043a…4223735f`）；**`2390b620…` 与 canonical `065a7871…` 是用户已装的前候选，`74e607c30a94`/`86…`/`bb7e4d5a8dba`/`bd700cac…` 更早，全部属历史**。新候选未安装/未重启/未备份/未 touch，包身份不是运行身份，**NOT_ACCEPTED**，先手测后提交。
- 设置页左右两个 pane 以 `draw_container_styled`（`ornamented=1`；底色 `fill_rgb565=0x0000`、`alpha=60`，即黑色 60%）绘制边框与四角装饰，扩框仍按既有 `{x-8, y-8, w+32, h+16}`（见 `plugins/settings/panels/settings.cpp`），并据此把 `draw_container_styled` 纳入 `components_complete()` 的必需入口。左栏分类行改用 adapter v2.35 的 `draw_help_option_row`：每行组件高 50（约为原行高的 70%），在 73 高的行内垂直居中；宽 284、相对列表视口 x=16，使组件相对左面板框左右各留 24，滚动条位于右侧留白内且不参与选项宽度计算。选中/未选中沿用帮助页选项组件的原生圆角底色，按下态也走该组件的原生贴片；行内不再画满行底色或 2px 金线。左栏文字字级 1，在组件内水平、垂直居中。右栏行样式未变。迁移前的 `draw_container_ornamented` 路径已随 ui_components Major 2 移除，不再可用。General/catalog 行模型与 toggle/entry 交互不变；entry 动作按钮仍用 capsule `kUiTextButtonFontLarger`（`plugins/settings/panels/settings.cpp`）。
- 停用时整个设置页/控件/回调撤销：移除帧任务、释放控件槽绑定、关闭页面、注销面板 owner；重新 activate 后从注册元数据与当前持久状态重建，不复用旧控件或旧 token。
- 设置页自停后不通过自身页面恢复（用户已确认）；其他普通插件停用时，active 设置页可经 Host 管理 API 修改 feature 恢复。
- 停用设置页不影响 Host 常驻 Store、配置读取或 `GET /api/config/list` 等控制端点。

## 5. 职责边界

- 拥有设置页自身页面内容、分类/项目呈现、控件状态与页面内的管理请求编排。
- 不拥有 Store/`config.json` I/O、配置 schema、配置/lifecycle HTTP adapter（Host 常驻设施）、共享组件与 panel registry（`core.custom_ui`）或被打开的业务面板内容。

## 6. 文件地图

| 子职责 | 文件 | 职责 |
|---|---|---|
| 插件生命周期 | `module/app/src/main/cpp/plugins/settings/plugin.cpp`、`feature.cpp` | descriptor、SDK 绑定、panel provider 注册、调试端点与帧任务 |
| 设置页实现 | `module/app/src/main/cpp/plugins/settings/panels/settings.cpp`、`settings.h` | catalog snapshot、页面事件、toggle/entry、只读游戏信息行与管理请求 |
| 页面模型/绑定 | `module/app/src/main/cpp/plugins/settings/panels/settings_model.h`、`feature_model.h`、`settings_binding.cpp`、`settings_binding.h` | 布局/文本规则、catalog 行与 owner 可用性映射、更多游戏入口绑定逻辑 |
| feature 注册 | `module/app/src/main/cpp/plugins/settings/feature.cpp`、`feature.h` | `page` entry 回调与面板 provider |
| 主菜单入口编排 | `module/app/src/main/cpp/plugins/settings/panels/settings.cpp`、`settings_binding.cpp`、`settings_binding.h`、`module/app/src/main/cpp/plugins/settings/plugin.cpp` | 帧任务、入口状态事务与必需 call patch 声明 |
| 原生菜单安全边界 | `module/app/src/main/cpp/contracts/ui.h`、`module/app/src/main/cpp/host/ui_adapter.cpp`、`module/app/src/main/cpp/plugins/core.custom_ui/ui_components.cpp`、`module/app/src/main/cpp/game/access/ui_kit.cpp`、`module/app/src/main/cpp/game/access/ui_native_text_button.cpp`、`module/app/src/main/cpp/game/access/patches.cpp` | root-child 快照、组件 owner 生命周期及单点绘制替换 |
| 旧贡献占位 | `module/app/src/main/cpp/plugins/settings/settings_contribution.cpp` | 保留兼容入口，`register_*` 为 no-op |
| 管理面契约 | `module/app/src/main/cpp/contracts/feature.h`、`contracts/host_sdk.h` | `QolFeatureApi`、v1.26 `read_categories`、v1.28 `read_owner_availability`、v1.30 choice catalog/control tail |

## 7. 注册 feature 目录（含 Hidden/debug-only 项）

下表列出当前插件 descriptor 注册的 feature 定义；`settings / page` 单独说明，因为 Host catalog 按 owner 身份排除设置页自身。分类使用代码中的 `FeatureCategory` 名称，页面显示文案见 §2；`display_priority` 未标特殊值的默认均为 0。配置缺少已注册 feature 值时，Host 按 default 补入；已有值（包括显式 `false`）保持不变。当前目录未要求把任何既有 toggle、entry 或业务功能迁移为 choice；只有 descriptor 明确注册 `kChoice` 的 feature 才使用此控件。

| Owner | Feature ID | 显示名称 | 默认 | Category | 类型 | Priority | 注册条件 |
|---|---|---|---:|---|---|---:|---|
| `attribute_quality` | `attribute_quality.provider` | 属性品质服务 | 1 | Hidden | toggle | 0 | — |
| `attribute_range_display` | `attribute_range_display.ranges` | 属性范围渲染 | 1 | BasicOptimization | toggle | 0 | — |
| `autosell` | `autosell.auto_sell` | 自动出售 | 0 | Gameplay | toggle | -1 | — |
| `character_panel_i18n` | `character_panel_i18n.translation` | 属性面板汉化 | 1 | BasicOptimization | toggle | 0 | — |
| `core.api` | `api.enabled` | API服务 | 1 | General | toggle | -5 | — |
| `core.character` | `core.character.provider` | 角色领域服务 | 1 | Hidden | toggle | 0 | — |
| `core.craft` | `core.craft.provider` | 合成领域服务 | 1 | Hidden | toggle | 0 | — |
| `core.custom_ui` | `core.custom_ui.ui_tools` | 共享界面工具 | 1 | Hidden | toggle | 0 | — |
| `core.custom_ui` | `core.custom_ui.open_debug_components` | 打开调试面板 | 1 | Hidden | entry | -5 | 仅 `QOL_DEBUG_COMPONENTS` 已定义且为真时注册 |
| `core.data` | `core.data.catalog` | 静态数据服务 | 1 | Hidden | toggle | 0 | — |
| `core.feature` | `core.feature.compatibility_routes` | 迁移占位路由 | 1 | Hidden | toggle | 0 | — |
| `core.item` | `core.item.provider` | 物品领域服务 | 1 | Hidden | toggle | 0 | — |
| `core.quest` | `core.quest.completion_check` | 任务完成检查 | 1 | Hidden | toggle | 0 | — |
| `core.save` | `core.save.provider` | 存档领域服务 | 1 | Hidden | toggle | 0 | — |
| `core.system` | `core.system.snapshots` | 系统状态服务 | 1 | Hidden | toggle | 0 | — |
| `core.ui_state` | `core.ui_state.provider` | 界面状态服务 | 1 | Hidden | toggle | 0 | — |
| `core.world` | `core.world.provider` | 世界领域服务 | 1 | Hidden | toggle | 0 | — |
| `craft_lottery` | `craft_lottery.guarantee` | 合成器传说保底 | 0 | BasicOptimization | toggle | -1 | — |
| `custom_map` | `custom_map.load` | 自定义地图 | 1 | BasicOptimization | toggle | 0 | — |
| `custom_map` | `custom_map.open_panel` | 查看自定义地图 | 1 | Tools | entry | 0 | — |
| `custom_recipe` | `custom_recipe.recipes` | 自定义合成 | 0 | Gameplay | toggle | -1 | — |
| `better_move` | `better_move.direction` | 移动优化 | 0 | Gameplay | choice | 0 | — |
| `better_move` | `better_move.step` | 移动步长 | 0 | Gameplay | choice | 0 | — |
| `extension_bag` | `extension_bag.extended_bag` | 额外背包 | 1 | Gameplay | toggle | -2 | — |
| `extension_bag` | `move_merge.drag_merge` | 拖动合并 | 1 | BasicOptimization | toggle | 0 | 自 `core.item` 迁入 |
| `module_health` | `module_health.enabled` | Module health | 1 | Hidden | toggle | 0 | — |
| `popup_blocker` | `popup_blocker.enabled` | 禁用弹窗 | 1 | BasicOptimization | toggle | 0 | — |
| `push_all` | `push_all.push` | 推怪 | 0 | Gameplay | toggle | 0 | — |
| `save_backup` | `save_backup.open_manager` | 存档备份 | 1 | Tools | entry | -2 | — |
| `world_tuning` | `world_tuning.panel` | 游戏世界调整 | 0 | Gameplay | entry | 0 | — |
| `special_equip` | `special_equip.equipment_rules` | NPC装备 | 1 | Hidden | toggle | 0 | — |
| `stack_limit` | `stack_limit.limit_999` | 堆叠上限 | 0 | Gameplay | toggle | 0 | — |
| `world_teleport` | `world_teleport.map_jump` | 更好的传送 | 1 | BasicOptimization | toggle | 0 | — |

设置页自身 `settings / page`（Hidden、entry、default `1`）用于设置页生命周期，不进入 `read_catalog` 返回值。Host-owned features 由 Host 注册，但同样进入设置页 catalog：

| Owner | Feature ID | 显示名称 | 默认 | Category | 类型 | Priority |
|---|---|---|---:|---|---|---:|
| `module.platform` | `debug.logging` | DEBUG日志 | 0 | General | toggle | -3 |
| `save_backup` | `save_backup.export_crash_logs` | 导出日志 | 1 | General | entry | -2 |
| `module.security` | `op.enabled` | 高级权限 | 0 | General | toggle | -4 |

当前 Debug 构建开启 `QOL_DEBUG_COMPONENTS` 时 catalog 共 35 项，其中 15 项属于 Hidden；Release 隐藏 Hidden 分类后显示 20 项（14 toggle、2 choice、4 entry）。`core.api / api.enabled`、两个 Host-owned features 与 `save_backup / save_backup.export_crash_logs` 都是 General，在 Debug/Release 均显示，core features 并非全部 Hidden；`extension_bag / move_merge.drag_merge` 是 BasicOptimization 项。设置页的 `page` 不计入上述 catalog 数量。

`core.api / api.enabled` 默认开启、属于 General（priority -5），由 Kotlin listener adapter 按 Host runtime effect 的 actual、desired 与有效 listener config 控制 HTTP listener。`module.security / op.enabled` 默认关闭、属于 General（priority -4），dispatcher 独立检查带 OP 标记的 endpoint；API 开关不级联、不改写 OP 值，开启 API 不代表授予 OP。旧 API feature `module.platform.features.api.enabled` 与 `core.api.features.core.api.http_registry` 按 Host Store 的迁移规则保守合并；root `apiEnabled` 仅在旧 `module.platform` feature 缺失时作 fallback；已有新目标值优先。OP 迁移只接收 Host Store 明确支持的旧值，并保留显式 false，细节见 [`api-optional-op-design.md` §6](../../refactor/api-optional-op-design.md)。

### 设置架构迁移前的公开开关基线

基线取自迁移提交 `516cf305` 的父版本 `516cf305^`，证据为该版本 `tests/test_settings_contributions.cpp` 的 `test_static_declarations_and_public_keys`，以及各 `settings_contribution.cpp` 的静态条目；共 10 个 toggle，另有备份与调试面板两个 action（不是开关）。下表用于识别历史条目与现行 feature 的语义对应关系，不单独定义运行时迁移；迁移输入与优先级以 [`api-optional-op-design.md` §6](../../refactor/api-optional-op-design.md) 和当前 Host Store 为准。

| 历史公开条目 id（配置 key 为末段） | 现行对应项 / 兼容状态 |
|---|---|
| `core.settings.apiEnabled` | `core.api` / `api.enabled`（历史语义对应；不据此推导其他旧 namespace 的迁移） |
| `core.settings.opEnabled` | `module.security` / `op.enabled`（明确保存的旧 false 也保留） |
| `core.settings.debugLogEnabled` | `module.platform` / `debug.logging` |
| `extension_bag.extensionBagEnabled` | `extension_bag` / `extension_bag.extended_bag` |
| `autosell.autoSellEnabled` | `autosell` / `autosell.auto_sell` |
| `custom_recipe.customRecipeEnabled` | `custom_recipe` / `custom_recipe.recipes` |
| `stack_limit.stackLimitIncrease` | `stack_limit` / `stack_limit.limit_999` |
| `push_all.pushAllEnabled` | `push_all` / `push_all.push` |
| `move_merge.moveMergeEnabled` | `extension_bag` / `move_merge.drag_merge`（旧 `core.item` / `move_merge` 布局的 `move_merge.drag_merge` 键与扁平 `moveMergeEnabled` 均迁移到 `extension_bag`） |
| `simple_mode.simpleModeEnabled` | 已由 `world_tuning` / `world_tuning.panel` 取代（旧键不迁移） |

历史基线为 10 个 toggle，仅用于解释旧配置迁移，不代表当前目录。当前 catalog 项和呈现元数据以 §7 表格及注册 descriptor 为准；当前 Debug catalog 共 35 项（取决于 `QOL_DEBUG_COMPONENTS`），Release 可见 20 项（14 toggle、2 choice、4 entry）。`core.api / api.enabled` 默认开启、分类为 General、priority -5，Debug/Release 均显示；这些呈现元数据不改变 API runtime feature 行为。`module.platform / debug.logging` priority -3、名称为「DEBUG日志」；`module.security / op.enabled` priority -4。历史 `core.settings.*` 命名不表示现行 API/OP owner 仍属于该 namespace。`scripts/verification/test_core_feature_exposure.py` 仍用于核对公开集合、Hidden 项、分离的 API/OP owners、core 描述和 Debug entry 注册条件。

## 8. 验证证据与范围

> **当前整体验收：NOT_ACCEPTED（2026-10-23）。** Host CTest、Debug 构建及当前设备的健康态观察不能替代“owner 失败时设置行置灰/红名/原因文案”的真机失败态验收。当前 Debug 构建 `output/inotia4_qol_lsposed_debug_2610230406_9e29f09336fc.apk` SHA-256 为 `9e29f09336fc722c2e5fa51594c73b5c559b0faf138fdf35796b671c22010347`；设备 `192.168.3.56:5555` 当前为 C18 v512，`/api/health` ready，`settings` 与 `popup_blocker` 均 `enabled`，设置页 `panel_active=true`、`categories=6`、`items=33`，没有 `failed`/`unsupported`/`blocked`/`invalid` owner，因此当前无失败行可目视验收。任务过程中曾观察到一次 `popup_blocker=unsupported` 而设置页仍显示原描述；重新安装/启动 C18 v512 后状态变为 `enabled`，该差异当前无法复现，原因未确认。

- 当前 Host 证据：完整 CTest 154/154 通过；最新定向命令 `cmake --build module/app/src/main/cpp/tests/build --target settings_panel_model_tests settings_panel_widget_tests -j2 && ctest --test-dir module/app/src/main/cpp/tests/build --output-on-failure -R 'settings_panel_(model|widget)_tests'` 通过 2/2；widget/model 覆盖 owner 失败映射、红名/原因、同 owner 联动禁用、恢复、单项 resource degradation 不误禁及入口回调拦截。`scripts/build-debug.sh` 成功；`uv run python scripts/maintenance/check_arch.py` 全通过。以上为 Host/构建/架构证据，不构成失败态真机验收。
- Choice 控件局部验证（2026-10-29）：`cmake --build module/app/src/main/cpp/tests/build --target settings_panel_model_tests settings_panel_widget_tests -j2` 成功；随后 `ctest --test-dir module/app/src/main/cpp/tests/build --output-on-failure -R 'settings_panel_(model|widget)_tests'` 为 `100% tests passed, 0 tests failed out of 2`。覆盖 catalog choice 元数据校验、关闭首项、注册顺序、两端禁用、不循环、点击防重入、receipt 持久化后回读、失败/未知/API 缺失状态。该证据仅为 Host 测试，不是 Android 构建或真机验收，不改变本节整体 `NOT_ACCEPTED` 结论。

> 下方记录是不同日期、候选包和范围的历史/局部证据，保留追溯用途；不得视为当前 HEAD 的完整验收或覆盖上述 NOT_ACCEPTED 结论。

- Host 定向测试：`settings_panel_widget_tests`、`settings_panel_model_tests`、`core_settings_panel_migration_tests` 为 3/3；`production_feature_facade_tests` 为 1/1；既有 `.tmp/` 日志记录的 static exposure 检查为 7 项。当前脚本增加 API/OP owner、catalog 与迁移来源断言；本轮通过 8/8 个静态测试（2026-10-13）。这些结果不等同于完整 CTest 或全插件验收。历史日志：`.tmp/settings-entry-actual-gate-review/ctest-final.log`、`.tmp/core-custom-ui-catalog-entry/ctest-final.log`、`.tmp/core-custom-ui-catalog-entry/static-exposure.log`。
- Debug 真机使用模块 `output/inotia4_qol_lsposed_debug_2610112252_4431d979a436.apk`（SHA-256 `4431d979a436a5620f037dd80e843b0dd856b08841c5dee18eff6bb05bd20031`），运行时 module identity SHA 一致。854×384 主菜单点击“模块设置”进入页面，在“调试开关”分类看到 `打开调试面板` entry，点击 `打开` 后组件调试面板实际出现；设置恢复后 screen 为 `main_menu` 且 panel stack 为空。坐标只对应该次设备截图，不可移用于其他尺寸。截图及运行身份日志位于 `.tmp/debug-panel-entry-delivery/`：`main-menu-before-settings.png`、`settings-debug-category.png`、`debug-components-panel.png`、`main-menu-final.png`、`logcat-runtime.txt`。
- **2026-10-15 子集 A 追加（脚本交付与用户安装已核对，视觉仍待验收）**：同一个原生文本按钮后端（设置页 entry 动作按钮与主菜单“模块设置”按钮共用）增加**按压轮廓校准**——按压可见主体按 75/61 源像素比例对齐（`round(75*p/61)`、不做 10% 量化、按压画布上限 492%），Toggle 执行几何不变、只补 alpha 边界测试；（历史：该条写作时尚未做双 TEXT 工厂/Debug 并列/menu 字号高度/容器 clip；Phase 1C 源码已实现，见下。）设置页自身面板/行模型未改；**用户观察**：大小/位置**正确**；两横线间黄色块**已定位为真实 alpha 采样缺陷**（v29 的 Title/UI006 素材字节哈希与 v31 相同；中段 `[30,50)×[40,60)` RGB 单一但 alpha 为 2..82、共 80 种 RGBA，每片末 alpha 2 跳回起点 71 造成周期性黄块，分数缩放下的片间重叠为次要因素；候选 `[100,120)×[40,60)` 同 y 各行 RGBA 横向一致、alpha 0..10）。**Phase1B 已实施最小修复**：生产仅把中段采样 `sx 30→100`（`ui_native_text_button.cpp:343-348`），几何（75/61、492%、body center、halo、cap、rect/hitbox）与 Toggle 均不变；定向 Host **4/4 PASS**（`ui_adapter_tests`、`native_toggle_render_tests`、`native_text_button_render_tests`、`core_ui_components_tests`），并有独立 fixture（`tests/fixtures/native_text_button_pressed_rgba.inc`：v29 真实 PNG 506 像素逐点核对、旧 edge alpha 重置 69 负对照、透明白合法）。新候选包 `output/Inotia4_v1.3.2_monster_v29_lspatched.apk`（SHA-256 `6598fc5f919f71547db2b4c743793cdf925e4ee1c8cf9ac6ff3ffbddc473026d`，内嵌 module `cdeb2118fb00ace445038ca26c27025c97abdc5dfcd41a273ba6fe6121660ecc`）**尚未安装**，在机运行仍是先前已装包（module `ed9081c4…`），无新 VM。几何冻结。整体视觉仍 **NOT_ACCEPTED**。
- **2026-10-16 Phase 1C（要求 4 已撤销；候选已生成，真机待验）**：设置页全部 feature entry（含调试面板打开、存档备份导出）统一使用 `capsule_text_button_create`，保留原 220×80 与 `kUiTextButtonFontLarger`。**要求 4（容器/裁切/几何/编排）已撤销**：`container_draw_content` 的 24 单位内接 content 保护与 `get_clip_rect` 快照路径删除；两 pane 当时回到「直接 `fill_rect` + `draw_container_ornamented`（扩框 `{x-8, y-8, w+32, h+16}`；该旧 API 已在 ui_components Major 2 迁移中移除，现由 `draw_container_styled` 承担同一几何）+ 无条件依次绘制标题 / 左列表 / 右列表 / 状态 / 返回按钮」（当时行号 `settings.cpp:577-592`）；左右 pane 几何恢复为 `24,100,300,444` / `364,100,572,444`（`kLeftX/kLeftY/kLeftW/kLeftH`、`kRightX/kRightY/kRightW/kRightH`，`settings.cpp:28-29`），Debug/Help 三页视口与绘制恢复。主菜单“模块设置”入口改用 `main_menu_decorated_button_create` 的**显式目标尺寸 290×72**（不再有 `height_percent`），命中区＝原生 snapshot rect。旧「hit 为两态包围并集」「容器化/视口重排」表述已废弃。构建与打包记录见 §8 与 `verification.md` §5.16；视觉与真机仍 **NOT_ACCEPTED**。
- **2026-10-15 UI 细化批历史证据（非 Phase1B 新源码验证）**：原生 toggle 分层居中、旧文本按钮按压层（当时 `sx=30`、Host 断言 `578+30=608`）、主菜单绘制比例及两栏 ornament 由 `core_custom_ui_host_tests`、`ui_adapter_tests`、`native_toggle_render_tests`、`native_text_button_render_tests`、`core_ui_components_tests`、`settings_panel_widget_tests`、`settings_panel_model_tests` 覆盖，**7/7 PASS**（`.tmp/custom-ui-polish-20261015/final-ctest.log`）。旧 dash bbox 检查仅判断 RGB，不能证明 alpha 连续性。旧 Debug 产物 `output/inotia4_qol_lsposed_debug_2610150537_3cc7410c87df.apk`，SHA-256 `3cc7410c87df6871ac07c7b4626b5cec497e4689ef027108ba5de65c52255670`，构建日志 `final-build-debug.log`/`final-apk.json` 同目录；此路径仅作历史记录。素材证据为 monster v31 简中，`monster-atlas-evidence.json`/`monster-center-strip-evidence.json` 核对 `loc 7/9/f/10`、标题 atlas 与 `UI_006`，不扩到其他 variant。当时未部署、无视觉手测，不能代替本次 RGBA 回归或 VM-D1；Release 旧产物未重建、未改动。
- 此设备验证只覆盖设置入口和 Debug 面板实际打开路径；没有实际点击 `save_backup.open_manager`，也没有验证备份/导出或其他业务 entry。`test_native_text_button_render.cpp` 包含 300×92 fallback 几何断言（含无额外 `+1` 绘制宽度），但单次设备截图和该几何用例不证明所有分辨率的像素级结果。本节证据不表示插件 A–G 全量审计或完整 acceptance 已完成。
