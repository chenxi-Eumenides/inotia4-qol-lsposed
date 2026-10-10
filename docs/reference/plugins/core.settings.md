# `settings` 插件

## 1. 作用与 owner 定位

`settings` 是模块设置整页的普通成品页插件，只负责页面呈现：它渲染 Host 的 feature catalog，并经 Host 常驻管理 API 读/写 feature、desired enable 与 config。它**不拥有**配置 schema、Store、持久化或配置 HTTP adapter——这些由 Host 常驻模块管理设施持有，与设置页生命周期分离。

配置文件与 Store 由 native Host 常驻设施管理，生命周期长于设置页插件。`GET /api/config/list`、`POST /api/config/set`、`POST /api/op/features/lifecycle`、`GET /api/config/receipt/{id}` 由 Host 常驻 resident route 直接处理，不经设置页插件；完整请求/响应契约见 [API 参考 §7.6](../api-reference.md)。

插件 id 为 `settings`（不是 `core.settings`）；目录名仍为 `plugins/core.settings/`。

## 2. 功能

### 页面入口 feature

descriptor 只注册一个 `page` entry feature：

| feature id | 默认 | 分类 | 类型 | 说明 |
|---|---:|---|---|---|
| `page` | 1 | Tools | entry | 在主菜单打开模块设置页；关闭后页面入口不可用。 |

`page` 与其他 feature 一样有布尔状态并参与 `want_run` 判定；它是 `kEntry`，不是无状态动作。

### Host catalog 渲染

- 页面通过 Host 管理面 `read_catalog` 读取 feature catalog，本地构建行模型；catalog 元数据（id/name/description/type/category/default）来自各插件 descriptor，不持久化。
- 分类标签为 General/Gameplay/Tools/Hidden。**Debug** 构建显示 Hidden 分类且可修改；**Release** 构建仅隐藏 Hidden 显示，不增加来源判别，也不额外用 API 拒绝用户来源（无来源 ACL）。
- 每个 toggle 行先 `read_desired` 取 desired enable（entry 行沿用）并 `read_feature` 取 feature 当前值；改动经 `request_set_feature`（普通 feature）或 `request_desired`（enable）提交，按 receipt 轮询确认后更新已确认 UI 状态。
- entry 行动作经 `invoke_entry` 触发对应插件回调；inactive/停用插件的业务回调不执行。

### 调试端点

插件自注册四条 `/api/debug/settings-ui/*` POST 命令（inject/restore/open-option/open-panel），属调试端点，随插件 deactivate 显式注销 owner（置墓碑），不经 Host 常驻控制面。（`/api/debug/settings-ui/status` 为 GET，走 Kotlin/JNI status handler，不由插件命令注册。）

## 3. 依赖与消费面

- 必需 capability：`ui_panels`（面板 open/close）与 `ui_components`（原生控件与文本）。
- Host 管理面经 `feature_control` 窄接口取得：`read_catalog`、`read_desired`、`read_feature`、`request_set_feature`、`request_desired`、`invoke_entry`、`receipt`。
- 不再注册旧 settings catalog contribution：`settings_contribution.cpp` 的 `register_settings`/`register_categories` 为 no-op；feature 元数据只来自 descriptor。
- 不消费 `save_backup` panel action，也不登记任何面板项；备份页入口由 `save_backup` 自己的 entry feature 承担。

## 4. 生命周期与撤页重建

- 注册阶段安装 `panel_id="settings"` 的 `PanelProvider`，并启动每帧「更多游戏」按钮绑定维护帧任务（主菜单多次重建需重新绑定）。
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

## 7. 注册 feature 目录（Host catalog 全量）

设置页渲染的 Host feature catalog 覆盖全部注册插件。产品 feature（不含 Debug device probe 插件）如下，`id` / 默认 / 分类 / 类型以各 `plugins/*/plugin.cpp` descriptor 为准：

| feature id | 默认 | 分类 | 类型 |
|---|---:|---|---|
| `autosell.auto_sell` | 0 | Gameplay | toggle |
| `move_merge.drag_merge` | 0 | Gameplay | toggle |
| `push_all.push` | 0 | Gameplay | toggle |
| `simple_mode.combat` | 0 | Gameplay | toggle |
| `custom_recipe.recipes` | 0 | Gameplay | toggle |
| `craft_ui.custom_crafting` | 1 | Hidden | toggle |
| `extension_bag.extended_bag` | 1 | Gameplay | toggle |
| `save_backup.open_manager` | 1 | Tools | entry |
| `stack_limit.limit_999` | 0 | Gameplay | toggle |
| `world_teleport.map_jump` | 1 | Gameplay | toggle |
| `attribute_range_display.ranges` | 1 | Gameplay | toggle |
| `special_equip.equipment_rules` | 1 | Gameplay | toggle |
| `character_panel_i18n.translation` | 1 | Gameplay | toggle |
| `core.quest.completion_check` | 1 | Gameplay | toggle |
| `core.custom_ui.ui_tools` | 1 | Hidden | toggle |
| `core.custom_ui.open_debug_components` | 1 | Hidden | entry |
| `page`（`settings` 插件） | 1 | Tools | entry |
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
