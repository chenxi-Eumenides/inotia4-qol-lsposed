# `settings` 插件

## 1. 作用与 owner 定位

`settings` 是模块设置整页的普通成品页插件，只负责页面呈现：页面只展示 feature 开关和 feature entry，不展示插件 desired/overall enable 行；feature 开关经 Host 常驻管理 API 读写 feature。它**不拥有**配置 schema、Store、持久化或配置 HTTP adapter——这些由 Host 常驻模块管理设施持有，与设置页生命周期分离。

配置文件与 Store 由 native Host 常驻设施管理，生命周期长于设置页插件。`GET /api/config/list`、`POST /api/config/set`、`POST /api/op/features/lifecycle`、`GET /api/config/receipt/{id}` 由 Host 常驻 resident route 直接处理，不经设置页插件；完整请求/响应契约见 [API 参考 §7.6](../api-reference.md)。

插件 id 为 `settings`（不是 `core.settings`）；目录名仍为 `plugins/core.settings/`。

## 2. 功能

### 页面入口 feature

descriptor 只注册一个 `page` entry feature：

| feature id | 默认 | 分类 | 类型 | 说明 |
|---|---:|---|---|---|
| `page` | 1 | Hidden | entry | 设置页常驻入口状态，不向玩家 catalog 暴露。 |

`page` 与其他 feature 一样有布尔状态并参与 `want_run` 判定；它是 `kEntry`，不是无状态动作。

### Host catalog 渲染

- 页面通过 Host 管理面 `read_catalog` 读取玩家 feature catalog，本地构建行模型；catalog 元数据（id/name/description/type/category/default/action_label）来自各插件 descriptor，不持久化。Host 排除全部 `core.*` 插件及注册 id 为 `settings` 的核心设置页，唯一例外是 Debug 构建中 `core.custom_ui.open_debug_components` 的 Hidden entry。该例外要求精确 feature id、entry 类型、Hidden 分类及非空 `on_entry`；不暴露 `core.custom_ui.ui_tools` toggle 或其他 core feature。Release descriptor 不注册该 entry，因此 Release catalog 中没有 core feature。完整注册目录、配置与管理 API 仍保留。
- 分类按“基础设置、基础优化设置、更多优化功能、更多工具、调试开关”顺序呈现；没有条目的分类不显示。**Debug** 构建显示调试开关分类且可修改；**Release** 构建仅隐藏调试开关分类，不增加来源判别，也不额外用 API 拒绝用户来源（无来源 ACL）。
- 每个 feature toggle 行只通过 `read_feature` 读取值，并经 `request_set_feature` 提交 feature 值；不读取或写入插件 desired/overall enable。Host 在每次成功的 feature 提交后（包括值未变化）根据全部已注册 feature 值重算 `enable`。
- 带 `on_entry` 的 entry 行仅显示 `action_label` 指定的动作按钮，不显示 toggle；entry 按钮的可用性不依赖其 feature 当前为 true。feature 为 false 时，设置页先经 Host `request_set_feature(true)`，再等待持久化 receipt、feature 值仍为 true 且 Host actual `enabled` 为 true 后调用一次 `invoke_entry`；actual 尚未就绪时最多等待 300 个设置页 process tick。读取/持久化失败、超时或设置页关闭时不调用 entry；已持久化的 true 不回滚。feature 已为 true 时，设置页先确认 Host actual `enabled` 为 true，再直接调用一次 `invoke_entry`，不重复提交 feature 写入；actual 未就绪或读取失败时不调用。无 entry callback 的 toggle 保持可编辑，不受所属插件当前 `enable=false` 影响。
- feature 开关不因所属插件当前 `enable=false` 而禁用，用户可通过开启 feature 恢复插件 desired 状态。直接调用 Host `invoke_entry` 的契约不变：只提交调用，不修改 feature/enable，Host 仍执行实际运行状态校验；设置页的先启用再调用仅是该 UI 的编排。
- 每行固定 96 逻辑单位高。标题与描述各最多绘制一行，超出宽度以省略号截断；无描述时标题垂直居中。动作按钮或 toggle 固定在行右侧，不增加第三行。

### 调试端点

插件自注册四条 `/api/debug/settings-ui/*` POST 命令（inject/restore/open-option/open-panel），属调试端点，随插件 deactivate 显式注销 owner（置墓碑），不经 Host 常驻控制面。（`/api/debug/settings-ui/status` 为 GET，走 Kotlin/JNI status handler，不由插件命令注册。）

## 3. 依赖与消费面

- 必需 capability：`ui_panels` v1.3（面板 open/close）与 `ui_components` v1.13（原生控件与文本）。Host 注入的 `QolUiAdapter` ABI 为 v1.20；组件 capability 版本与 adapter ABI minor 各自独立协商。
- Host 管理面经 `feature_control` 窄接口取得：`read_catalog`、`read_feature`、`request_set_feature`、`invoke_entry`、`receipt`。设置页 UI 不调用 `read_desired` 或 `request_desired`。
- 不再注册旧 settings catalog contribution：`settings_contribution.cpp` 的 `register_settings`/`register_categories` 为 no-op；feature 元数据只来自 descriptor。
- 不消费 `save_backup` panel action，也不登记任何面板项；备份页入口由 `save_backup` 自己的 entry feature 承担，按钮文案为其注册的 `打开`。

## 4. 生命周期与撤页重建

- 注册阶段安装 `panel_id="settings"` 的 `PanelProvider`，并启动每帧「更多游戏」按钮绑定维护帧任务（主菜单多次重建需重新绑定）。
- 「更多游戏」替换需先经 adapter 快照与 `control_active` 读取原控件布尔 Active，再在创建替换控件后事务化地把原控件 Show/Enabled/Active 全部关闭（`main_menu_more_games_set_active` 写入原生非派发 Active）；任一步失败即回滚到原布尔 Active 并放弃替换，不对外报告 live。绑定入口要求 adapter minor ≥ 20 且 ABI 覆盖 `main_menu_more_games_set_active` 尾字段。释放仅在快照证明原控件仍是当前实例时恢复原 Show/Enabled 与原布尔 Active；身份不匹配不做任何 native 读或写。
- 停用时整个设置页/控件/回调撤销：移除帧任务、释放控件槽绑定、关闭页面、注销面板 owner；重新 activate 后从注册元数据与当前持久状态重建，不复用旧控件或旧 token。
- 设置页自停后不通过自身页面恢复（用户已确认）；其他普通插件停用时，active 设置页可经 Host 管理 API 修改 feature 恢复。
- 停用设置页不影响 Host 常驻 Store、配置读取或 `GET /api/config/list` 等控制端点。

## 5. 职责边界

- 拥有设置页自身页面内容、分类/项目呈现、控件状态与页面内的管理请求编排。
- 不拥有 Store/`config.json` I/O、配置 schema、配置/lifecycle HTTP adapter（Host 常驻设施）、共享组件与 panel registry（`core.custom_ui`）或被打开的业务面板内容。

## 6. 文件地图

| 子职责 | 文件 | 职责 |
|---|---|---|
| 插件生命周期 | `module/app/src/main/cpp/plugins/core.settings/plugin.cpp`、`feature.cpp` | descriptor、SDK 绑定、panel provider 注册、调试端点与帧任务 |
| 设置页实现 | `module/app/src/main/cpp/plugins/core.settings/panels/settings.cpp`、`settings.h` | catalog snapshot、页面事件、toggle/entry 与管理请求 |
| 页面模型/绑定 | `module/app/src/main/cpp/plugins/core.settings/panels/settings_model.h`、`settings_binding.cpp`、`settings_binding.h` | 行模型及更多游戏入口绑定逻辑 |
| feature 注册 | `module/app/src/main/cpp/plugins/core.settings/feature.cpp`、`feature.h` | `page` entry 回调与面板 provider |
| 旧贡献占位 | `module/app/src/main/cpp/plugins/core.settings/settings_contribution.cpp` | 保留兼容入口，`register_*` 为 no-op |
| 管理面契约 | `module/app/src/main/cpp/contracts/feature.h` | `QolFeatureApi`（catalog/desired/feature/config/receipt/entry） |

## 7. 注册 feature 目录（含不进入玩家 catalog 的内部项）

插件注册 feature（不含 Debug device probe 插件）如下，`id` / 默认 / 分类 / 类型以各 `plugins/*/plugin.cpp` descriptor 为准；`module.platform` 的 Host-owned feature 单列于表后。`core.*` 和 `settings` 的 feature 全部为 Hidden、描述为空；唯一进入 Debug 玩家 catalog 的 core feature 是 `core.custom_ui.open_debug_components` entry。其他 core feature（包括 `core.custom_ui.ui_tools` toggle）仅用于内部生命周期/管理，不进入玩家 catalog。Release descriptor 不注册该 debug entry。`attribute_range_display.ranges`、`character_panel_i18n.translation`、`world_teleport.map_jump` 与 `move_merge.drag_merge` 属于基础优化设置，注册默认值为开启；Host 只在配置缺少该 feature 时补入默认值，已有设备配置（包括显式 `false`）保持不变。

| feature id | 默认 | 分类 | 类型 |
|---|---:|---|---|
| `autosell.auto_sell` | 0 | Gameplay | toggle |
| `move_merge.drag_merge` | 1 | Basic optimization | toggle |
| `push_all.push` | 0 | Gameplay | toggle |
| `simple_mode.combat` | 0 | Gameplay | toggle |
| `custom_recipe.recipes` | 0 | Gameplay | toggle |
| `craft_ui.custom_crafting` | 1 | Hidden | toggle |
| `extension_bag.extended_bag` | 1 | Gameplay | toggle |
| `save_backup.open_manager` | 1 | Tools | entry |
| `stack_limit.limit_999` | 0 | Gameplay | toggle |
| `world_teleport.map_jump` | 1 | Basic optimization | toggle |
| `attribute_range_display.ranges` | 1 | Basic optimization | toggle |
| `special_equip.equipment_rules` | 1 | Hidden | toggle |
| `character_panel_i18n.translation` | 1 | Basic optimization | toggle |
| `core.quest.completion_check` | 1 | Hidden | toggle |
| `core.custom_ui.ui_tools` | 1 | Hidden | toggle |
| `core.custom_ui.open_debug_components` | 1 | Hidden | entry（Debug catalog exception） |
| `page`（`settings` 插件） | 1 | Hidden | entry |
| `core.api.http_registry` | 1 | Hidden | toggle |
| `core.character.provider` | 1 | Hidden | toggle |
| `core.craft.provider` | 1 | Hidden | toggle |
| `core.data.catalog` | 1 | Hidden | toggle |
| `core.item.provider` | 1 | Hidden | toggle |
| `core.save.provider` | 1 | Hidden | toggle |
| `core.system.snapshots` | 1 | Hidden | toggle |
| `core.ui_state.provider` | 1 | Hidden | toggle |
| `core.world.provider` | 1 | Hidden | toggle |
| `attribute_quality.provider` | 1 | Hidden | toggle |
| `inventory_hooks.inventory_support` | 1 | Hidden | toggle |
| `core.feature.compatibility_routes` | 1 | Hidden | toggle |

另有三个 Debug device probe 插件 feature（`device_probe_provider.capability_probe`、`device_probe_consumer.dependency_probe`、`device_probe_export_failure.preflight_probe`，均 Hidden/toggle），不进 release。

Host-owned platform feature（不属于 `core.*` 插件）：

| owner / feature id | 默认 | 分类 | 类型 | 说明 |
|---|---:|---|---|---|
| `module.platform` / `api.enabled` | 1 | General | toggle | 唯一 API 服务门禁；缺少配置时默认开启，显式持久值优先。 |
| `module.platform` / `debug.logging` | 0 | General | toggle | Host 调试日志开关。 |

遗留的 `module.platform.features.op.enabled` 不再注册为 feature、也不显示或接受 feature 写入；旧值仍可出现在配置投影中，但不影响操作路由。只有显式 `api.enabled=false` 才关闭 HTTP 服务和 `op_gated` 操作路由；旧 `op.enabled=false` 且缺少 `api.enabled` 时仍按新默认开启。

### 设置架构迁移前的公开开关基线

基线取自迁移提交 `516cf305` 的父版本 `516cf305^`，证据为该版本 `tests/test_settings_contributions.cpp` 的 `test_static_declarations_and_public_keys`，以及各 `settings_contribution.cpp` 的静态条目；共 10 个 toggle，另有备份与调试面板两个 action（不是开关）。下表仅用于识别历史条目与现行注册项的对应关系，**不表示运行时会迁移旧标识或旧配置字段**。

| 历史公开条目 id（配置 key 为末段） | 现行对应项 / 兼容状态 |
|---|---|
| `core.settings.apiEnabled` | `module.platform` / `api.enabled` |
| `core.settings.opEnabled` | 旧 `module.platform.features.op.enabled` 值可保留但不再注册或生效；旧平铺字段不导入 |
| `core.settings.debugLogEnabled` | `module.platform` / `debug.logging` |
| `extension_bag.extensionBagEnabled` | `extension_bag` / `extension_bag.extended_bag` |
| `autosell.autoSellEnabled` | `autosell` / `autosell.auto_sell` |
| `custom_recipe.customRecipeEnabled` | `custom_recipe` / `custom_recipe.recipes` |
| `stack_limit.stackLimitIncrease` | `stack_limit` / `stack_limit.limit_999` |
| `push_all.pushAllEnabled` | `push_all` / `push_all.push` |
| `move_merge.moveMergeEnabled` | `move_merge` / `move_merge.drag_merge` |
| `simple_mode.simpleModeEnabled` | `simple_mode` / `simple_mode.combat` |

历史基线为 10 个 toggle；其中 `op.enabled` 已退役，因此现仍注册的历史开关为七个非 core 功能开关与两个 `module.platform` 控制（`api.enabled`、`debug.logging`）。当前另外公开三个基础优化开关（属性范围显示、角色属性汉化、地图传送对话框），所以当前非 Hidden toggle 集合共 12 项；`move_merge.drag_merge` 属于既有七项之一，只调整为基础优化分类。历史 `core.settings.*` 命名不代表现行 `module.platform` 控制属于 core 插件。`special_equip.equipment_rules` 仍为 Hidden，仅 Debug 下可见。`scripts/verification/test_core_feature_exposure.py` 断言当前公开集合、Hidden 项、三个新增基础优化项、平台控制、core features 的 Hidden 元数据及唯一 Debug entry 例外。
