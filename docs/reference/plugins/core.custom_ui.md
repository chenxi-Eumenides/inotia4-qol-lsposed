# `core.custom_ui` 插件

## 1. 作用与 owner 定位

`core.custom_ui` 是普通核心共享 UI 工具/框架插件，注册 ID 为 `core.custom_ui`、版本 `0.1.1`。它拥有可复用 UI 组件、面板注册/导航、原生选择控件和文本机制；不拥有功能页面或其文案内容（`module/app/src/main/cpp/plugins/core.custom_ui/plugin.cpp:21-23,139-140`；架构边界见 [`ARCHITECTURE.md` §5.4、§7.2](../../../ARCHITECTURE.md)）。

## 2. 功能面

### Capabilities 与原生资源

| 面 | 版本 | 当前提供内容 |
|---|---:|---|
| `ui_components` | 1.13 | 通用组件与绘制/列表/导航接口；契约中的 `QolUiComponents` 由本插件实现 |
| `ui_panels` | 1.3 | 面板 provider 注册后的打开、关闭、状态/返回协作接口 |
| `ui_choice` | 1.1 | 原生选择框 open/active/close API |
| `text_layer` | 1.0 | `MEMORYTEXT_GetText` hook 上的文本订阅/分派接口 |

提供版本见 `module/app/src/main/cpp/plugins/core.custom_ui/plugin.cpp:27-67`；公共版本定义见 `contracts/ui.h:15-19`、`contracts/panels.h:16-18`、`contracts/ui_choice.h:21-23`、`contracts/text_layer.h:10-12`。本插件另声明三项必需资源：私有 `UIChoice_Init` entry hook、私有选择按钮 ptr slot、共享 `MEMORYTEXT_GetText` entry hook；均为 required/fail-closed 且无 feature gate（`plugin.cpp:81-132`）。

### Debug 专用入口

Debug 构建包含三个 POST 调试端点：`/api/debug/custom-ui/components`、`/api/debug/custom-ui/scroll`、`/api/debug/custom-ui/help`，分别驱动组件、原生滚动列表和帮助页探针。插件 descriptor 在 Debug 条件下声明 typed endpoint handlers；Host binder 按 descriptor lifecycle owner 接线。`routes.inc` 与 `core.api/routes.inc` 保存供同步/校验使用的 route facts，不由面板代码动态注册 HTTP handler。Debug 条件由 `core.custom_ui/CMakeLists.txt:11-19` 控制。

该构建还注册唯一的 debug `entry` feature `core.custom_ui.open_debug_components`，action label 为“打开”，callback 打开 `debug_components` 面板；另外的 `ui_tools` 是 Hidden toggle，不是入口项（`plugin.cpp:220-241`）。endpoint/feature 契约以路由元数据和 descriptor 为实现事实，API 公共契约以 [API 参考](../api-reference.md) 为准。

`PluginDescriptor` 通过 `http_endpoints` 声明这三个 Debug command routes（`plugin.cpp` 中的 `kHttpEndpoints` 与 descriptor 尾部）；各 Debug panel 注册函数只登记 UI panel provider 和 scheduler task，不另行向 `http_dispatch` 动态注册 HTTP route。

### 原生控件与面板绘制事实（2026-10-15）

以下为当前实现事实，供复核参考；本批已有 Host 定向测试与 Debug 构建证据，但**尚未部署、无手动/真机视觉验证，不能据此报告视觉 PASS**。

**Toggle（`game/access/ui_native_toggle.cpp:52-111`）**：按层绘制，每层源画布在基础 rect 内**居中**——`layer_x = x + (base.w − layer.w) * percent / 200`，纵向同理；`percent` 为统一缩放。按压层 `loc 7` 画布比基础画布每边多 8 px halo（262×103 对 246×87），若不按源画布居中而沿用 atlas 锚点，按压态会整体偏移。所有层统一使用基础层 `offset_x/offset_y`，不再取各层自身 offset。

**原生文本按钮（`game/access/ui_native_text_button.cpp:157-200,291-395`）**：按压态在同一源上叠加按压层——标题图 `loc 7` 的橙底白边胶囊，左右 cap 直接取两端，中段只平铺均匀橙色带；中段采样起点为 `sx = 30`（源空间 `26..46,40..60` 仍含白边像素，`30..50,40..60` 为单一纯橙色），避开的白色 dash 真实 bbox 为 `x[53,209) / y[48,55)`；Host 用例按 `sx 578 + 30 = 608` 实际断言。不使用新填充色，也不调用全局 clip；按压层读取失败时按压绘制整体失败，正常态仍可绘制。同一 textbutton 类型的**两侧饰裁切由 rect 比例决定**（`height_percent <= width_percent` 或 rect 不小于源 405×77 时等比整体缩放含两侧饰，否则水平切片只压缩中间填充、保留圆角端），不是按 fontstyle 枚举分支。设置页 entry 动作按钮沿用 capsule 样式 `kUiTextButtonFontLarger` 原样（`plugins/settings/panels/settings.cpp:248-254`）。

**按压轮廓校准（2026-10-15 子集 A；源码/Host + Debug 构建证据；未集成打包/未安装）**：按压主体按**可见主体**对齐，而非整张带 halo 的画布。三层实际主体：**Title `loc 7` pressed** = 220×61；**Toggle normal / pressed** 均为 220×61；**UI_006 文本按钮 normal 裁剪主体** = 405×75。`75/61` 校准的是 **UI_006 normal 裁剪主体（75）与 Title `loc 7` pressed 主体（61）** 这两层，**Toggle 不参与 75 校准**（其两态已同为 61，见下条）（`ui_native_text_button.cpp:302-322`）。旧实现 `floor(floor(77*p/103)/10)*10` 把 103 画布压进 normal 守卫并向下量化到 10%，得到小一圈的轮廓；现改为 `round(75*p/61)`、不做 10% 量化，整按压画布宽度为 `normal_visible_width + 2*round(21*scale/100)`，中心取正常胶囊真实 body center，主菜单绘制 rect 分支不进入该算法；按压源画布最大 scale 由 75/61 比例导出为 492%，避免原本合法的大按钮按住时不可绘制。
**Toggle 本轮不改执行几何（用户已反馈开关表现正确；本轮无执行几何变化）**：`loc 7` 按压与 normal 的 alpha 主体均为 220×61，现有代码只用基底 scale 并把按压画布移 (-8,-8) 居中即已 body-to-body 重合；本轮只补准确注释与真实 alpha 边界回归测试，若按 262/246 或 103/87 放大反而会制造新的真实变大（`game/access/ui_native_toggle.cpp`；`tests/test_native_toggle_render.cpp`）。
未纳入本批：双 TEXT 工厂与 Debug 并列展示、mainmenu 精确字号 20→30、高度 ×1.5、容器 clip；`ui_native_close_button.cpp` 的 X 底板/整金属框按压与 `draw_back_button` 外框效果亦为独立后续范围，本轮未改。

**主菜单替换按钮（`plugins/core.custom_ui/ui_components.cpp:785-812`）**：`text_button_draw` 只在绘制上使用 decorated 紧凑比例——`height = min(rect.h, rect.w × 48 / 220)` 并垂直居中，背景与标签共用该绘制 rect；原生菜单控件 rect、命中范围与 identity 判定不变（绑定/存活校验仍走 `main_menu_text_button_*` 原路径）。

**面板四角装饰**：`help_page` 左右两栏均改为 `draw_container_ornamented`（四角 ornament，`plugins/core.custom_ui/ui_components.cpp:1484-1499`）；`draw_container` 仍保留给无装饰容器。Debug 双栏对照示例（`plugins/core.custom_ui/panels/debug_components.cpp:302-303`）本就用 ornamented，本次不改；外壳/Frames 对照示例同样不改。设置页两侧 pane 的同类改动见 [`settings.md`](settings.md) §4。

前轮历史参考证据（**非本批按压修复的构建/部署**）：Host 定向 7/7（`core_custom_ui_host_tests`、`ui_adapter_tests`、`native_toggle_render_tests`、`native_text_button_render_tests`、`core_ui_components_tests`、`settings_panel_widget_tests`、`settings_panel_model_tests`，日志 `.tmp/custom-ui-polish-20261015/final-ctest.log`）；`scripts/build-debug.sh --offline` 构建成功，产物 `output/inotia4_qol_lsposed_debug_2610150537_3cc7410c87df.apk`（SHA-256 `3cc7410c87df6871ac07c7b4626b5cec497e4689ef027108ba5de65c52255670`，`.tmp/custom-ui-polish-20261015/final-build-debug.log`、`final-apk.json`）。本题材为 **monster v31 简中**：`monster-atlas-evidence.json` 与 `monster-center-strip-evidence.json` 确认 `loc 7/9/f/10` metadata 与标题 atlas、`UI_006` 与 overhaul 对应一致（每项 sha 相同）；不据此扩到其他 variant。构建成功与素材核对**不等于真机或像素验收**，视觉结论仍为 **NOT_ACCEPTED**。（输出目录内其他 release 产物为既有文件，本批未重建、未改动，也未做 Release 运行。）
本批按压修复的当前证据：`.slim/deepwork/ui-craft-regression-20261015/pressed-fix.md` 记录的 **4/4**（`ui_adapter_tests`、`native_toggle_render_tests`、`native_text_button_render_tests`、`core_ui_components_tests`，日志 `.tmp/custom-ui-components-20261015/pressed/final-ctest.log`），以及主代理当前整树的 UI 相关 target 通过（`141/143`，红项与本批无关）；本批 Debug 构建成功（`output/inotia4_qol_lsposed_debug_2610151556_ed9081c4c54e.apk`，SHA-256 `ed9081c4c54e4bd5e0829ae8749d58d6a70e5101a0428f131cb299e953d29fd2`，`.tmp/ui-craft-regression-20261015/validation/build-debug.log:272-276`），**未集成打包、未安装**，不得用前轮 7/7 或旧 APK 作为本批新源码验证。

## 3. 使用的模块功能、依赖与消费者

- **Requires / optional**：`ui.state` 1.0 为可选依赖；Debug 构建对 `http.registry`/`http.dispatch` v1.1 的依赖也为可选，非 Debug descriptor 不声明 HTTP 依赖（`plugin.cpp:70-79,247-253`；版本见 `contracts/http.h:96-99`）。provider 缺失只使 Debug HTTP 探针无法接线，不影响共享 UI 功能；provider 恢复后按可选依赖生命周期重新接线。选择框向 `ui.state` 发布自定义 dialog snapshot；缺少该 capability 时 native choice 本身仍按本插件路径工作（`feature.cpp:70-99`）。
- **Required port**：`QOL_PORT_CHOICE` 1.0，提供原生选择框状态/选项/标题/焦点/弹出等受控操作（`plugin.cpp:134-137`；版本见 `contracts/game_ports/ports.h:17,47`；接口见 `contracts/game_ports/choice_port.h:11-30`）。
- **Host SDK**：使用 Host 注入的 `sdk->ui` Adapter（契约版本 1.20；它不是本插件提供的 capability），以及 `panels`、`hooks.text_subscribe/run_text`、descriptor endpoint handlers、scheduler/log（`contracts/ui.h:3-6,15-19`；`plugin.cpp` 中的 `kHttpEndpoints`；`text_mechanism.cpp:27-46`）。
- **已核实消费者**：`ui_components` 与 `ui_panels` 由当前 `plugins/settings/` 设置页消费；`ui_panels` 还由 `save_backup`、`autosell` 使用；`ui_choice` 被 `world_teleport` 作为可选能力消费；`text_layer` 被 `custom_recipe`、`character_panel_i18n` 要求（`plugins/settings/plugin.cpp:32-38`、`settings/panels/settings.cpp:816-821`、`save_backup/plugin.cpp:31-35`、`autosell/plugin.cpp:196-204`、`world_teleport/plugin.cpp:46-61`、`custom_recipe/plugin.cpp:168-178`、`character_panel_i18n/plugin.cpp:33-36`）。
- **真实设置页边界**：页面实现位于 `plugins/settings/`，descriptor ID 为 `settings`，只要求 `ui_panels` 与 `ui_components`；`core.custom_ui/settings_contribution.cpp` 当前不注册设置项（`plugins/settings/plugin.cpp:19-20,32-38,238-255`；`core.custom_ui/settings_contribution.cpp:4-8`）。

## 4. 职责范围

- `core.custom_ui` 提供组件、面板/导航、游戏原生选择框和文本机制；具体页面、页面状态与业务文案由各自插件拥有，设置页归注册 ID 为 `settings` 的 `plugins/settings/`（`plugin.cpp:139-140`；`ARCHITECTURE.md` §7.2）。
- `core.ui_state` 拥有游戏界面状态/操作；`core.custom_ui` 只通过可选 `ui.state` 发布自定义 dialog 状态，不接管该领域 owner（`feature.cpp:70-99`；架构边界见 `ARCHITECTURE.md` §7.2）。
- UI Adapter/Game Adapter 承担底层游戏控件、绘制与资源原语；本插件通过 contracts/Host SDK 使用受控接口，不承载通用游戏语义（`contracts/ui.h:3-6,304-315`；`ARCHITECTURE.md` §4.3、§7.2）。
- Debug 面板与 endpoint 是 Debug 构建探针，不等同于功能页面或 release 对外业务接口（`CMakeLists.txt:11-19`；`plugin.cpp:74-79,220-241`）。

## 5. 对外提供与依赖影响

四项 capability 随普通插件启停发布/撤销。当前外部 consumers 如 §3 所列；`ui_choice` 的可选消费者 `world_teleport` 不把它作为 install 前提。停用时关闭选择控件门禁、停用文本分派，由 Host binder 撤销该 descriptor 的 Debug routes，并解除面板/SDK 绑定；组件或面板能力不因此变成具体页面的 owner（`plugin.cpp:166-202`）。

## 6. 读写线程与生命周期机制

- **UI 与选择控件**：`contracts/ui.h` 明确 Host UI Adapter 接口仅可在游戏主线程调用；`UiChoiceSpec.on_select` 约定在主线程回调。选择操作使用 mutex 保护 active/spec/generation 并持有选项/标题副本，底层 read/write 通过 `QOL_PORT_CHOICE`；port contract 本身未另行规定所有函数调用的线程上下文（`contracts/ui.h:3-6`；`contracts/ui_choice.h:25-34`；`contracts/game_ports/choice_port.h:11-30`；`feature.cpp:23-46,130-221,223-307`）。
- **Debug HTTP 写路径**：descriptor command handler 只验证请求并写原子请求状态；UI 面板开关交由 scheduler task 执行。三个 Debug 面板都有该“HTTP 请求→调度任务→面板”路径（`panels/debug_components.cpp`；`debug_scroll.cpp`；`debug_help.cpp`）。
- **文本读路径**：wrapper 先调用原 `MEMORYTEXT_GetText`，若文本机制 active 再同步调用 Host `run_text` 分派，失败/空输出回退原文；订阅由 Host hooks API 登记/撤销（`text_mechanism.cpp:27-64`）。源码没有为该 hook 回调另行声明一个通用线程保证，本说明不把它扩写为 UI 主线程接口。
- **生命周期**：install 绑定 UI/面板/文本 API 并验证 required choice port；enabled/disabled 分别打开/关闭 choice 与 text gate，Debug 构建同步启停调试面板；deactivate 关闭 choice、撤销 Debug 路由/面板、停用 text dispatch 并清理绑定（`plugin.cpp:141-180,182-202`）。

## 7. 文件地图

| 子职责 | 文件 | 关键事实 |
|---|---|---|
| descriptor、provides/requires/port、资源与生命周期 | `module/app/src/main/cpp/plugins/core.custom_ui/plugin.cpp` | ID/version/capabilities：21-79；resources/port：81-137；install/lifecycle：141-202；Debug entry：220-267 |
| 组件接口实现 | `module/app/src/main/cpp/plugins/core.custom_ui/ui_components.cpp`、`ui_components.h` | 组件对象/原生 adapter 绑定：`ui_components.cpp:20-78,124-130`；接口：`ui_components.h:13-45`；文本按钮/主菜单绘制 rect：`785-812`；help 双栏 ornament：`1484-1499` |
| 原生 toggle 绘制 | `module/app/src/main/cpp/game/access/ui_native_toggle.cpp` | 分层源画布居中与 halo：`52-111` |
| 原生文本按钮绘制 | `module/app/src/main/cpp/game/access/ui_native_text_button.cpp` | 比例裁切/切片判定：`157-200`；按压层避开烘焙 dash：`291-395` |
| 原生选择框 | `module/app/src/main/cpp/plugins/core.custom_ui/feature.cpp`、`feature.h` | 锁状态、port 读写、dialog publish：`feature.cpp:23-46,70-99,130-221,223-334` |
| 文本共享机制 | `module/app/src/main/cpp/plugins/core.custom_ui/text_mechanism.cpp`、`text_mechanism.h` | subscribe/unsubscribe 与原文 fallback dispatch：`text_mechanism.cpp:27-64` |
| Debug panels/endpoints | `module/app/src/main/cpp/plugins/core.custom_ui/plugin.cpp`、`panels/debug_components.cpp`、`debug_scroll.cpp`、`debug_help.cpp` | descriptor endpoint facts/handlers 位于 plugin.cpp；各面板源码实现 command callback 与 scheduler task |
| 跨插件契约 | `module/app/src/main/cpp/contracts/ui.h`、`panels.h`、`ui_choice.h`、`text_layer.h`、`game_ports/choice_port.h` | adapter/components 版本与线程说明：`ui.h:3-19`；其余 capability ID/version 与接口见各文件开头定义 |
| 当前设置页消费者 | `module/app/src/main/cpp/plugins/settings/plugin.cpp`、`plugins/settings/panels/settings.cpp` | descriptor requires：`plugin.cpp:32-38`；运行时 require ui_components：`settings.cpp:816-821` |
