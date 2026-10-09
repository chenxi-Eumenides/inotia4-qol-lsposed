# `core.settings` 插件

## 1. 作用与 owner 定位

`core.settings` 是模块设置整页的 owner，承载类别/设置项呈现、用户输入与配置桥接，并经共享 UI 组件和面板能力运行。它是成品页面插件，不是通用 UI 组件库，也不拥有 Loader 配置存储。

## 2. 功能

### 路由与配置项

`core.settings` 自身不在 descriptor 中声明 API HTTP handler。全局 API 的 `/api/config/list` owner 为 `core.settings`，由 settings API/controller 路径提供；`POST /api/config/set` 的 owner 是 `module.config`。相关规格见 [API 参考](../api-reference.md)。

设置 catalog contribution 注册基础设置类别和 API 服务、OP 操作、调试日志等 toggle 项。另有 `PanelItemSpec` action：owner=`core.settings`、`panel_id="save_backup"`，通过标准 `kAction` 注册接口加入 panel registry。源码同时注明，设置页当前仍由静态 11 项表渲染；该 action 登记用于前向兼容，当前并非静态页面列表的数据驱动项。

### Capability、ports 与注册面

- 面板 provider：`panel_id="settings"`，登记到 Host SDK panels registry。
- 消费 `ui_panels`（面板 open/close），并使用 `ui_components`、UI adapter、settings catalog 与 settings config 接口。
- settings category/item catalog 通过 Host SDK 的 settings catalog 面登记；插件 descriptor 不声明领域 capability provides/ports。
- 配置值读写经 `settings_config` bridge 发起；底层配置文件持久化由 Kotlin/Loader 负责，不归设置页插件所有。

## 3. 依赖前置与消费者

- Host SDK 提供 `ui`、`panels`、scheduler、settings catalog/config 与日志接口；通过 capability lookup 获取 `ui_panels` 和 `ui_components`。
- `save_backup` 插件提供目标面板 provider；设置 action 通过 catalog action invocation / panel registry 打开 `save_backup`。共享组件由 `core.custom_ui` 提供。
- 设置项由配置 catalog contributors 提供；用户通过页面操作，配置请求交给 config bridge 处理。

## 4. 职责边界

- 拥有模块设置页自身页面内容、类别/项目呈现、设置控件状态以及页面内的配置/action 请求编排。
- 不拥有设置值的配置文件持久化（Kotlin/Loader 配置设施）、共享组件和通用 panel registry（`core.custom_ui`/Host SDK）、备份功能本身（`save_backup`），或被打开的任意业务 panel 内容。
- `save_backup` action 是设置页注册的导航动作，目标 panel 内容仍由 `save_backup` 提供。

## 5. 核心实现链

- **目录读取/缓存**：页面打开时获取 settings catalog snapshot，将类别和项目拷贝到本地有界数组后释放 token；按周期刷新配置值与 capability availability。状态摘要在互斥锁保护下供状态查询读取。
- **配置写入**：toggle 回调先记录期望值，在主线程帧安全点向 settings config bridge 提交异步 set 请求；后续轮询 request status 并同步确认值/状态。action button 也先排队，在帧安全点通过 catalog `invoke_action` 调用注册 action。
- **线程边界**：面板打开、处理、输入与控件创建要求 UI 主线程；事件回调只记录待执行状态，页面处理阶段再推进配置请求、action 和列表更新。配置落盘不由页面插件实现。
- **生命周期**：注册阶段安装 `settings` PanelProvider、`save_backup` PanelItem 并启动每帧「更多游戏」按钮绑定维护任务；页面 open/close 创建/销毁原生控件树；插件卸载撤销 owner 注册、移除任务并释放绑定。

## 6. 文件地图

| 子职责 | 文件 | 职责 |
|---|---|---|
| 插件生命周期 | `module/app/src/main/cpp/plugins/core.settings/plugin.cpp`、`feature.cpp` | descriptor、SDK 绑定、provider/action 注册和运行时任务 |
| 设置页实现 | `module/app/src/main/cpp/plugins/core.settings/panels/settings.cpp`、`settings.h` | catalog snapshot、页面事件、toggle/action 与配置桥接 |
| 页面模型/绑定 | `module/app/src/main/cpp/plugins/core.settings/panels/settings_model.h`、`settings_binding.cpp`、`settings_binding.h` | 行模型及更多游戏入口绑定逻辑 |
| 设置贡献 | `module/app/src/main/cpp/plugins/core.settings/settings_contribution.cpp`、`settings_contribution.h` | 类别与内置 toggle 项 catalog registration |
| 契约与 UI 工具 | `module/app/src/main/cpp/contracts/panels.h`、`settings.h`、`settings_config.h` | PanelItem/Provider、设置 catalog 与 config bridge 类型 |
| Kotlin 配置边界 | `module/app/src/main/java/com/inotia4/qol/api/controller/ConfigController.kt` | 配置 API 请求入口；持久化由 Kotlin/Loader 路径处理 |
