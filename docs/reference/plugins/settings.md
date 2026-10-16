# `settings` 插件

## 1. 作用与 owner 定位

`settings` 是模块设置整页的普通成品页插件，只负责页面呈现：页面只展示 feature 开关和 feature entry，不展示插件 desired/overall enable 行；feature 开关经 Host 常驻管理 API 读写 feature。它**不拥有**配置 schema、Store、持久化或配置 HTTP adapter——这些由 Host 常驻模块管理设施持有，与设置页生命周期分离。

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

- 页面通过 Host 管理面 `read_catalog` 读取 feature catalog，本地构建行模型；catalog 元数据（id/name/description/type/category/default/action_label/display_priority）来自各插件 descriptor，不持久化。Host catalog 包含全部已注册 feature 定义，唯一按身份排除注册 ID 为 `settings` 的设置页插件，不按 `core.*` namespace 过滤。分类名称与 priority 由 Host 的单一静态注册表提供，管理 SDK v1.26 的 `read_categories` 返回 Host 生命周期内不可变的 POD 数组；页面不硬编码分类元数据。消费者必须以 SDK minor ≥26 校验带 display priority 的 feature catalog stride。分类和 feature 均按 priority 升序稳定排序，同 priority 保持 catalog 注册顺序。Debug 显示全部分类；Release 使用相同排序，仅不显示 Hidden 分类，不按 feature 所属插件过滤。General 中的 API 服务、OP 权限与 `debug.logging` 在 Release 仍显示。除 General 的 `core.api.api.enabled` 外，core feature 均为 Hidden；Debug 中会显示 API 服务开关和 `core.custom_ui.ui_tools` 等已注册 core feature。`core.custom_ui.open_debug_components` 仅在 `QOL_DEBUG_COMPONENTS` Debug 构建中注册为 Hidden entry，因此 Release descriptor 不含该 entry。完整注册目录、配置与管理 API 仍保留。
- Host 分类注册表保持现有五个 category ID 与名称：Hidden「调试开关」-10、General「基础设置」-9、BasicOptimization「基础优化设置」-8、Gameplay「更多优化功能」-5、Tools「更多工具」-3。priority 较小者先显示；没有 feature 的分类不显示。
- 每个 feature toggle 行只通过 `read_feature` 读取值，并经 `request_set_feature` 提交 feature 值；不读取或写入插件 desired/overall enable。Host 在每次成功的 feature 提交后（包括值未变化）根据全部已注册 feature 值重算 `enable`。
- 带 `on_entry` 的 entry 行仅显示 `action_label` 指定的动作按钮，不显示 toggle；entry 按钮的可用性不依赖其 feature 当前为 true。feature 为 false 时，设置页先检查 `QolFeatureControlApi` 覆盖 `read_actual` 尾字段且函数非空，再经 Host `request_set_feature(true)`；单独的 `EntryContinuation` 保存复制的 plugin/feature ID、request ID 与页面 generation，不与 toggle receipt 混用。每个设置页 process tick 最多回取一次 receipt；持久化确认后还须重新读到 feature 为 true、且 Host actual `enabled` 为 true 才调用一次 `invoke_entry`。receipt 与 actual 等待从请求 admission 起共用最多 300 个 process tick；`read_actual` 返回 NotReady 或 enabled 尚未为 true 时继续等待，其他读取错误、持久化失败或超时均走现有错误状态且不调用。页面关闭/销毁或 generation 改变会清除 continuation；Host 已持久化的 true 不回滚。feature 已为 true 时，同样先校验 `read_actual` ABI 并要求 actual `enabled==1`，随后直接调用一次 `invoke_entry`、不提交 feature 写入；actual 未就绪或读取失败时立即显示错误并停止。Host `invoke_entry` 成功仅代表 admission，不保证页面已打开。无 entry callback 的 toggle 保持可编辑，不受所属插件当前 `enable=false` 影响。
- feature 开关不因所属插件当前 `enable=false` 而禁用，用户可通过开启 feature 恢复插件 desired 状态。直接调用 Host `invoke_entry` 的契约不变：只提交调用，不修改 feature/enable，Host 仍执行实际运行状态校验；设置页的先启用再调用仅是该 UI 的编排。
- 每行固定 96 逻辑单位高。标题与描述各最多绘制一行，超出宽度以省略号截断；无描述时标题垂直居中。动作按钮或 toggle 固定在行右侧，不增加第三行。

### 调试端点

插件自注册四条 `/api/debug/settings-ui/*` POST 命令（inject/restore/open-option/open-panel），属调试端点，随插件 deactivate 显式注销 owner（置墓碑），不经 Host 常驻控制面。（`/api/debug/settings-ui/status` 为 GET，走 Kotlin/JNI status handler，不由插件命令注册。）

## 3. 依赖与消费面

- 必需 capability：`ui_panels` v1.3（面板 open/close）与 `ui_components` v1.13（原生控件与文本）。Host 注入的 `QolUiAdapter` ABI 为 v1.20；组件 capability 版本与 adapter ABI minor 各自独立协商。
- Host 管理面经 `feature_control` 窄接口取得：`read_catalog`、`read_categories`、`read_feature`、`request_set_feature`、`invoke_entry`、`receipt`。此 catalog 消费要求 SDK minor ≥26，以适配 `QolFeatureCatalogItem` 新 stride；设置页 UI 不调用 `read_desired` 或 `request_desired`。
- Host 另为 Kotlin 内部 listener fallback 提供一次性 receipt observer：等待中的 receipt 完成时只通知一次；订阅已是终态的 receipt 时在 Host 锁外重放终态，不返回活动订阅 token。Host observer 容量为 256，JNI 回调携带 request id 与 `feature_receipt_json` 的 JSON。该内部推送不替代设置页 entry continuation 每个页面 process tick 至多一次的 receipt 查询，也不改变公开 `GET /api/config/receipt/{id}` 的轮询契约；内部 receipt 等待没有 200ms/50ms 定时轮询。
- 不再注册旧 settings catalog contribution：`settings_contribution.cpp` 的 `register_settings`/`register_categories` 为 no-op；feature 元数据只来自 descriptor。
- 不消费 `save_backup` panel action，也不登记任何面板项；备份页入口由 `save_backup` 自己的 entry feature 承担，按钮文案为其注册的 `打开`。

## 4. 生命周期与撤页重建

- 注册阶段安装 `panel_id="settings"` 的 `PanelProvider`，并启动 interval=1 的主线程帧任务维护「更多游戏」替换（主菜单可能重建原生控件）。插件注册 ID 为 `settings`，源码目录为 `plugins/settings/`；Host namespace 与 C++ namespace 仍为 `core.settings`。
- 替换只在适配器快照证明原生 More Games 控件仍是当前主菜单 root 的子节点后执行；快照要求已观测 `main_menu` 且至少跨过一帧。identity/lifetime Unknown 或离开主菜单时保留替换控件及回调上下文，不盲目解引用或删除；只有身份明确且实例变化后才释放旧组件。替换控件使用原生按钮的同一 parent 与快照 rect，文案为“模块设置”，使用现有原生文本按钮绘制。绑定前保存原布尔 Active，再事务化关闭原控件 Show/Enabled/Active；任一步失败都会恢复原状态，不报告 live。释放只在再次确认原控件仍是当前实例时恢复原 Show/Enabled 与原布尔 Active。
- 原生 More Games 图片由游戏在 `STATE_DRAW_MAIN_MENU` 中直接绘制，普通控件 Show/DrawProc 无法替代该调用。设置插件只声明一个 required、fail-closed call patch：`settings.main_menu.more_games.draw`，定位单个 `GRPX_DrawPart` BL（`+0xa70`，`+0xa6c` 是参数设置）。Android 仅 arm64 登记该目标，非 Android host test 构建也包含该站点；其他 Android ABI 不提供替代补丁。替换组件未被确认 live，或组件绘制失败时，调用原生绘制一次；仅组件绘制成功后抑制原图。
- 替换按钮沿用现有原生文本按钮绘制与标签机制，菜单按钮取原生快照 rect；其中 300×92 几何若标准 70 px 中段无法在 1 px 容差内匹配，会保留两侧圆角 cap、改用连续的中段裁切搜索。`test_native_text_button_render.cpp` 的 300×92 用例断言 slice 边界连续、绘制宽度为各 slice 取整宽度之和（无额外 `+1`）。这是该几何用例的宿主断言，不等于所有设备分辨率的逐像素验收。替换按钮的绘制 rect 另按 decorated 紧凑比例取值（`height = min(rect.h, rect.w × 48 / 220)` 且垂直居中），只影响绘制；原生控件 rect、命中范围与 identity 判定不变（`plugins/core.custom_ui/ui_components.cpp:785-812`；`tests/test_core_ui_components.cpp` 断言绘制 rect 高度比例与原控件 rect 未被改写）。按压态叠加同源橙色胶囊层（避开烘焙 dash，见 [`core.custom_ui.md`](core.custom_ui.md) §2）。
- 设置页左右两个 pane 现以 `draw_container_ornamented` 绘制四角装饰（`plugins/settings/panels/settings.cpp:576`），并据此把 `draw_container_ornamented` 纳入 `components_complete()` 的必需入口（`plugins/settings/panels/settings.cpp:515-519`）。General/catalog 行模型与 toggle/entry 交互不变；entry 动作按钮仍用 capsule `kUiTextButtonFontLarger`（`plugins/settings/panels/settings.cpp:248-254`）。
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
| 设置页实现 | `module/app/src/main/cpp/plugins/settings/panels/settings.cpp`、`settings.h` | catalog snapshot、页面事件、toggle/entry 与管理请求 |
| 页面模型/绑定 | `module/app/src/main/cpp/plugins/settings/panels/settings_model.h`、`settings_binding.cpp`、`settings_binding.h` | 行模型及更多游戏入口绑定逻辑 |
| feature 注册 | `module/app/src/main/cpp/plugins/settings/feature.cpp`、`feature.h` | `page` entry 回调与面板 provider |
| 主菜单入口编排 | `module/app/src/main/cpp/plugins/settings/panels/settings.cpp`、`settings_binding.cpp`、`settings_binding.h`、`module/app/src/main/cpp/plugins/settings/plugin.cpp` | 帧任务、入口状态事务与必需 call patch 声明 |
| 原生菜单安全边界 | `module/app/src/main/cpp/contracts/ui.h`、`module/app/src/main/cpp/host/ui_adapter.cpp`、`module/app/src/main/cpp/plugins/core.custom_ui/ui_components.cpp`、`module/app/src/main/cpp/game/access/ui_kit.cpp`、`module/app/src/main/cpp/game/access/ui_native_text_button.cpp`、`module/app/src/main/cpp/game/access/patches.cpp` | root-child 快照、组件 owner 生命周期及单点绘制替换 |
| 旧贡献占位 | `module/app/src/main/cpp/plugins/settings/settings_contribution.cpp` | 保留兼容入口，`register_*` 为 no-op |
| 管理面契约 | `module/app/src/main/cpp/contracts/feature.h`、`contracts/host_sdk.h` | `QolFeatureApi` 与 v1.26 `QolFeatureControlApi.read_categories`（Host 分类 catalog） |

## 7. 注册 feature 目录（含 Hidden/debug-only 项）

插件注册 feature（不含 Debug device probe 插件）如下，`id` / 默认 / 分类 / 类型 / priority 以各 `plugins/*/plugin.cpp` descriptor 为准；Host-owned feature 另列于表后。除 General 的 `core.api.api.enabled` 外，core feature 均为 Hidden 并进入 Host catalog；Debug 设置页显示 Hidden 项，Release 只不显示 Hidden 分类。`core.custom_ui.open_debug_components` 仅在 Debug 构建中注册。Host 只在配置缺少已注册 feature 值时按 descriptor default 补入，已有值（包括显式 `false`）保持不变；以下四个基础优化项默认开启。未列特殊值的 `display_priority` 均为 0。

| feature id | 默认 | 分类 | 类型 | display priority |
|---|---:|---|---|---:|
| `autosell.auto_sell` | 0 | Gameplay | toggle | -1 |
| `move_merge.drag_merge` | 1 | Basic optimization | toggle | 0 |
| `push_all.push` | 0 | Gameplay | toggle | 0 |
| `simple_mode.combat` | 0 | Gameplay | toggle | 0 |
| `custom_recipe.recipes` | 0 | Gameplay | toggle | -1 |
| `extension_bag.extended_bag` | 1 | Gameplay | toggle | -2 |
| `save_backup.open_manager` | 1 | Tools | entry | -2 |
| `stack_limit.limit_999` | 0 | Gameplay | toggle | 0 |
| `world_teleport.map_jump` | 1 | Basic optimization | toggle | 0 |
| `attribute_range_display.ranges` | 1 | Basic optimization | toggle | 0 |
| `special_equip.equipment_rules` | 1 | Hidden | toggle | 0 |
| `character_panel_i18n.translation` | 1 | Basic optimization | toggle | 0 |
| `core.quest.completion_check` | 1 | Hidden | toggle | 0 |
| `core.custom_ui.ui_tools` | 1 | Hidden | toggle | 0 |
| `core.custom_ui.open_debug_components` | 1 | Hidden | entry（Debug catalog exception） | -5 |
| `page`（`settings` 插件） | 1 | Hidden | entry | 0 |
| `core.api` / `api.enabled` | 1 | General | toggle | -5 |
| `core.character.provider` | 1 | Hidden | toggle | 0 |
| `core.craft.provider` | 1 | Hidden | toggle | 0 |
| `core.data.catalog` | 1 | Hidden | toggle | 0 |
| `core.item.provider` | 1 | Hidden | toggle | 0 |
| `core.save.provider` | 1 | Hidden | toggle | 0 |
| `core.system.snapshots` | 1 | Hidden | toggle | 0 |
| `core.ui_state.provider` | 1 | Hidden | toggle | 0 |
| `core.world.provider` | 1 | Hidden | toggle | 0 |
| `attribute_quality.provider` | 1 | Hidden | toggle | 0 |
| `inventory_hooks.inventory_support` | 1 | Hidden | toggle | 0 |
| `core.feature.compatibility_routes` | 1 | Hidden | toggle | 0 |

Host-owned feature（不属于玩家 core/plugin feature 表）：

| owner / feature id | 默认 | 分类 | 类型 | display priority | 说明 |
|---|---:|---|---|---:|---|
| `module.platform` / `debug.logging` | 0 | General | toggle | -3 | 名称为「debug日志」；Host 调试日志开关。 |
| `module.security` / `op.enabled` | 0 | General | toggle | -4 | 通用 OP 授权；仅作用于 endpoint 明确声明 OP 的操作，Debug/Release 均显示。 |

`core.api.features.api.enabled` 默认开启、属于 General（priority -5），并由 Kotlin listener adapter 按 Host runtime effect 的 actual、desired 与有效 listener config 控制 HTTP listener。`module.security.features.op.enabled` 默认关闭、属于 General（priority -4），dispatcher 独立检查带 OP 标记的 endpoint；API 开关不级联、不改写 OP 值，开启 API 不代表授予 OP。旧 API feature `module.platform.features.api.enabled` 与 `core.api.features.core.api.http_registry` 按 Host Store 的迁移规则保守合并；root `apiEnabled` 仅在旧 `module.platform` feature 缺失时作 fallback；已有新目标值优先。OP 迁移只接收 Host Store 明确支持的旧值，并保留显式 false，细节见 [`api-optional-op-design.md` §6](../../refactor/api-optional-op-design.md)。

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
| `move_merge.moveMergeEnabled` | `move_merge` / `move_merge.drag_merge` |
| `simple_mode.simpleModeEnabled` | `simple_mode` / `simple_mode.combat` |

历史基线为 10 个 toggle；`op.enabled` 属于 `module.security` 的 General feature，在 Debug/Release 均显示。当前非隐藏的常规 toggle 集合为 11 项：七个既有非 core 功能开关、`module.platform.debug.logging` 和三个新增基础优化开关；`move_merge.drag_merge` 属于既有七项之一，只调整为基础优化分类。`save_backup.open_manager` 继续作为公开 Tools entry。`core.api.api.enabled` 默认开启、分类为 General、priority -5，Debug/Release 均显示；这些呈现元数据不改变 API runtime feature 行为。`module.platform.debug.logging` priority -3、名称为「debug日志」；其他显式优先级见注册表。历史 `core.settings.*` 命名不表示现行 API/OP owner 仍属于该 namespace。`special_equip.equipment_rules` 同样为 Hidden，Debug 可见、Release 不显示 Hidden 分类。`scripts/verification/test_core_feature_exposure.py` 断言常规公开集合与元数据、Hidden 项、分离的 API/OP owners、core 描述和 Debug entry 注册条件。

## 8. 验证证据与范围

- Host 定向测试：`settings_panel_widget_tests`、`settings_panel_model_tests`、`core_settings_panel_migration_tests` 为 3/3；`production_feature_facade_tests` 为 1/1；既有 `.tmp/` 日志记录的 static exposure 检查为 7 项。当前脚本增加 API/OP owner、catalog 与迁移来源断言；本轮通过 8/8 个静态测试（2026-10-13）。这些结果不等同于完整 CTest 或全插件验收。历史日志：`.tmp/settings-entry-actual-gate-review/ctest-final.log`、`.tmp/core-custom-ui-catalog-entry/ctest-final.log`、`.tmp/core-custom-ui-catalog-entry/static-exposure.log`。
- Debug 真机使用模块 `output/inotia4_qol_lsposed_debug_2610112252_4431d979a436.apk`（SHA-256 `4431d979a436a5620f037dd80e843b0dd856b08841c5dee18eff6bb05bd20031`），运行时 module identity SHA 一致。854×384 主菜单点击“模块设置”进入页面，在“调试开关”分类看到 `打开调试面板` entry，点击 `打开` 后组件调试面板实际出现；设置恢复后 screen 为 `main_menu` 且 panel stack 为空。坐标只对应该次设备截图，不可移用于其他尺寸。截图及运行身份日志位于 `.tmp/debug-panel-entry-delivery/`：`main-menu-before-settings.png`、`settings-debug-category.png`、`debug-components-panel.png`、`main-menu-final.png`、`logcat-runtime.txt`。
- **2026-10-15 子集 A 追加（脚本交付与用户安装已核对，视觉仍待验收）**：同一个原生文本按钮后端（设置页 entry 动作按钮与主菜单“模块设置”按钮共用）增加**按压轮廓校准**——按压可见主体按 75/61 源像素比例对齐（`round(75*p/61)`、不做 10% 量化、按压画布上限 492%），Toggle 执行几何不变、只补 alpha 边界测试；本轮**未**做双 TEXT 工厂与 Debug 并列展示、mainmenu 精确字号 20→30 / 高度 ×1.5 / 容器 clip（详见 [`core.custom_ui.md`](core.custom_ui.md) §2）。设置页自身面板/行模型未改；**用户观察**：大小/位置**正确**；两横线间黄色块**已定位为真实 alpha 采样缺陷**（v29 的 Title/UI006 素材字节哈希与 v31 相同；中段 `[30,50)×[40,60)` RGB 单一但 alpha 为 2..82、共 80 种 RGBA，每片末 alpha 2 跳回起点 71 造成周期性黄块，分数缩放下的片间重叠为次要因素；候选 `[100,120)×[40,60)` 同 y 各行 RGBA 横向一致、alpha 0..10）。**Phase1B 已实施最小修复**：生产仅把中段采样 `sx 30→100`（`ui_native_text_button.cpp:343-348`），几何（75/61、492%、body center、halo、cap、rect/hitbox）与 Toggle 均不变；定向 Host **4/4 PASS**（`ui_adapter_tests`、`native_toggle_render_tests`、`native_text_button_render_tests`、`core_ui_components_tests`），并有独立 fixture（`tests/fixtures/native_text_button_pressed_rgba.inc`：v29 真实 PNG 506 像素逐点核对、旧 edge alpha 重置 69 负对照、透明白合法）。新候选包 `output/Inotia4_v1.3.2_monster_v29_lspatched.apk`（SHA-256 `6598fc5f919f71547db2b4c743793cdf925e4ee1c8cf9ac6ff3ffbddc473026d`，内嵌 module `cdeb2118fb00ace445038ca26c27025c97abdc5dfcd41a273ba6fe6121660ecc`）**尚未安装**，在机运行仍是先前已装包（module `ed9081c4…`），无新 VM。几何冻结。整体视觉仍 **NOT_ACCEPTED**。
- **2026-10-15 UI 细化批历史证据（非 Phase1B 新源码验证）**：原生 toggle 分层居中、旧文本按钮按压层（当时 `sx=30`、Host 断言 `578+30=608`）、主菜单绘制比例及两栏 ornament 由 `core_custom_ui_host_tests`、`ui_adapter_tests`、`native_toggle_render_tests`、`native_text_button_render_tests`、`core_ui_components_tests`、`settings_panel_widget_tests`、`settings_panel_model_tests` 覆盖，**7/7 PASS**（`.tmp/custom-ui-polish-20261015/final-ctest.log`）。旧 dash bbox 检查仅判断 RGB，不能证明 alpha 连续性。旧 Debug 产物 `output/inotia4_qol_lsposed_debug_2610150537_3cc7410c87df.apk`，SHA-256 `3cc7410c87df6871ac07c7b4626b5cec497e4689ef027108ba5de65c52255670`，构建日志 `final-build-debug.log`/`final-apk.json` 同目录；此路径仅作历史记录。素材证据为 monster v31 简中，`monster-atlas-evidence.json`/`monster-center-strip-evidence.json` 核对 `loc 7/9/f/10`、标题 atlas 与 `UI_006`，不扩到其他 variant。当时未部署、无视觉手测，不能代替本次 RGBA 回归或 VM-D1；Release 旧产物未重建、未改动。
- 此设备验证只覆盖设置入口和 Debug 面板实际打开路径；没有实际点击 `save_backup.open_manager`，也没有验证备份/导出或其他业务 entry。`test_native_text_button_render.cpp` 包含 300×92 fallback 几何断言（含无额外 `+1` 绘制宽度），但单次设备截图和该几何用例不证明所有分辨率的像素级结果。本节证据不表示插件 A–G 全量审计或完整 acceptance 已完成。
