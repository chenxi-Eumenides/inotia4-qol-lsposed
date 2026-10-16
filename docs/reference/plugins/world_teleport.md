# `world_teleport` 插件参考

## 摘要

功能插件（纯消费者、叶子）：拦截世界态地图名横幅入口（`UIPlay_CallMapName` 尾部），改为原生 `ui_choice` 选择面板，提供 ±1/±10 四个方向的地图跳转与金币费用确认，确认后扣费并请求切图。不提供能力、无 HTTP 路由、不直接写游戏内存（经 Game Host 动作面与能力契约）。

除特别说明，文中 `plugins/...`、`contracts/...`、`game/...`、`host/...` 路径均相对 `module/app/src/main/cpp/`。

## 定位与版本

`world_teleport`，插件版本 `0.1.0`（`plugins/world_teleport/plugin.cpp:16-17`）。descriptor（`plugin.cpp:148-172`）：2 项 required 依赖 `world.facts` v2.0 与 `item.facts` v3.3（`plugin.cpp:52-57`）；1 项 optional 依赖 `ui_choice` v1.1（`plugin.cpp:46-50`）；`provides_count=0`；1 条资源；`jni_count=0`；1 个端口 `QOL_PORT_CHOICE` v1.0（`plugin.cpp:59-62`，`contracts/game_ports/ports.h:17,47`）；1 个 feature；`on_resource_ready=nullptr`。无 `routes.inc`、无 HTTP query/endpoint。注册于 `host/assembly.cpp:68,234`。

## 公开能力与路由

无对外能力：不 `provides` 任何 capability，不注册 HTTP 路由、快照、事件或命令。

唯一用户可见 feature 为 `world_teleport.map_jump`，版本 1，默认 `1`，分类 `kBasicOptimization`，类型 `kToggle`，名称「更好的地图传送对话框」（`plugin.cpp:142-146`）。`on_toggle` 回调为 no-op：运行时门禁由 Host 的 feature 布尔值 → `enable` 派生驱动生命周期（`ARCHITECTURE.md` §8.2）。

## 游戏内行为

入口为 `UIPlay_CallMapName` 尾部的声明式入口 patch（`plugin.cpp:29-44`），声明为 `private / required / fail_closed`，`resumable=true`，`step_budget=120`：

| 资源 id | 符号（site） | 机制 | 职责 |
|---|---|---|---|
| `patch:UIPlay_CallMapName` | `P_UI_PLAY_CALL_MAP_NAME`（`game/access/patches.cpp:46-55`，`F_UI_PLAY_CALL_MAP_NAME_VMA=0xc6664`，`game_symbols.h:826`） | `kCallPatch`，站点层 `PatchKind::kBranchRedirectWithReturn`（原版 `mov x0,#1` = `0xd2800020`，含尾部回跳元数据），重定向到 `world_teleport_call_map_name` | 世界态地图名入口改为传送选择框 |

业务编排（`feature.cpp`）：

1. `world_teleport_call_map_name`（`feature.cpp:328-340`）：读 `world.facts::state`；非世界态（`state != 5`，`kWorldState`）忽略；世界态调用 `request_open_choice`。
2. `open_choice_panel`（`feature.cpp:131-185`）：读 `current_map_id`/`map_record_count`，由 `max_map_id_from_record_count` 推导上限；构造 4 项传送目标（delta `{+1,+10,-1,-10}`，费用 `{300,3000,300,3000}`）+ 1 项「关闭」（共 `kChoiceCount=5`，`feature.cpp:18-22`）；以 `UiChoiceSpec`（`dialog_type_id="world_teleport.confirm"`，`feature.cpp:166-169`）调 `ui_choice::open`。
3. `teleport_on_select`（`feature.cpp:295-322`）：越界/依赖缺失 → 关面板；无效目标 → 横幅提示；有效 → 写确认文本、关面板、`g_pending_target=target`、调度 `delayed_open_confirmation`。
4. `delayed_open_confirmation`（`feature.cpp:192-205`）：经 `world_action.create_yesno` 弹确认框（ok=`teleport_confirmed`、cancel=`teleport_cancelled`、param=`cost`）。
5. `teleport_confirmed`（`feature.cpp:272-286`）：调度 `delayed_close_choice`，关 popup、restore HUD。
6. `delayed_close_choice` → `delayed_transfer`（`feature.cpp:226-270`）：读 `item.facts::money` 与 `world.facts::active_player_direction`；余额不足 → 横幅「金币不足，无法传送」；否则 `world_action.minus_money` → `world_action.request_map_change(map_id,0,0,direction)`。
7. `teleport_cancelled`（`feature.cpp:288-293`）：调度重开选择面板。

地图 id 回绕（`rules.cpp`）：`kMaxMapId=414`（`rules.h:6`）；`target_map_id(current,delta,max)` 在 `[0,max]` 循环（`rules.cpp:12-18`）；`max_map_id_from_record_count(count)`：`count<=0` 回退静态上限，否则 `min(count-1, kMaxMapId)`（`rules.cpp:7-10`）。

## 使用的 Host SDK 与 Game 面

- `ctx.sdk->capabilities.require`：install 期 require `world.facts` / `item.facts`（`plugin.cpp:83-111`）；运行期每次现取 `ui_choice`（`feature.cpp:105-115`），不缓存。
- `ctx.sdk->scheduler.add`（帧任务，interval=1）：`delayed_open_choice`/`delayed_open_confirmation`/`delayed_close_choice`/`delayed_transfer` 的延迟调度（`feature.cpp:97-103`）。
- `ctx.sdk->log.write`：业务日志（`feature.cpp:44-56`）。
- `QolWorldActionApi`（`contracts/game_host.h:150-157`）：`create_yesno`、`minus_money`、`request_map_change`、`close_popup`、`restore_hud`、`show_banner`。
- `WorldFactsApi`（`contracts/world_facts.h:10-11,23-31`，v2.0）：`state`、`map_record_count`、`current_map_id`、`map_name`、`active_player_direction`。
- `ItemFactsApi`（`contracts/item_facts.h:10-14`，v3.3）：`money`（FIX-05 起金钱归 `item.facts`，`docs/refactor/issues.md`）。
- `UiChoiceApi`（`contracts/ui_choice.h:21-22,36`，v1.1，optional）：`open`/`close`。
- `QolChoicePort::read.state_id`（`contracts/game_ports/choice_port.h`；`feature.cpp:179-183`）：仅用于日志记录当前 choice 状态。

## 消费者与职责边界

本插件不作为 provider，**无下游消费者**；级联影响面仅在依赖方向：

- required 依赖 `world.facts`（provider=`core.world`）与 `item.facts`（provider=`core.item`）：任一 provider 缺失 → `blocked`；provider 撤销 → 作为 required consumer 被级联停用，provider 恢复后按 Host 生命周期重新激活（`docs/refactor/verification.md` VM-K2-01、VM-K4-04a/05）。
- optional 依赖 `ui_choice`（provider=`core.custom_ui`）：缺失时选择面板打开失败（`feature.cpp:170-178` 记录并返回），不阻塞插件启用。
- required 端口 `QOL_PORT_CHOICE`：`port_count != 1` → install 失败（`plugin.cpp:74`）。
- 本插件停用不影响任何其他插件（叶子）。

边界归其他 owner：`UIPlay_CallMapName` 站点登记、近址 thunk 与回跳归 `patch/` 与 `game/access/patches.cpp`；世界读写数据面归 `core.world`（阶段 5 批 5-6b 已将世界写路由并入 `core.world`，见 `docs/refactor/verification.md` VM-K5-06g；部分 Kotlin controller 注释仍写「由 world_teleport owner 承接」，属历史引用）；金钱归 `core.item`；选择控件实现与能力提供归 `core.custom_ui`。

## 失败语义（fail-closed）

- `plugin_install`（`plugin.cpp:64-113`）：校验 `struct_size`/`sdk_major`/`g_sdk`/`g_game`/`ports` 与两 required 能力（`item.facts` 额外做 `item_facts_api_size_valid`），失败清空全局、写 `g_install_error` 文案并返回 1。
- 各业务函数在 `game()==nullptr` 或所需 `world_action` 槽为空时记录日志并返回（`feature.cpp:194,218,230-234,259-261,274`）。
- `feature.h:11-31` 定义 `InstallDisposition`（`kInstalled/kUnsupported/kFailed`）与 `install_error()` 文案，供安装结果语义核对（`tests/test_world_teleport_install.cpp`）。
- 资源声明 `kFailClosed`：入口 patch 安装失败即按还原释放并把插件置 failed。注意 `on_resource_ready=nullptr`（`plugin.cpp:166`），资源备份无插件侧缓存。

## 生命周期

- `install`（`plugin.cpp:64-113`）：装配检查与能力裁决；**成功返回 0 但不置 `g_installed`**。
- `on_enabled`（`plugin.cpp:125-129`）：`g_installed=true`、清 `g_install_error`。
- `on_disabled`（`plugin.cpp:131-134`）：`g_installed=false`。
- `deactivate`（`plugin.cpp:115-123`）：清 `g_installed` 与全部接口指针。
- `fill_status`（`plugin.cpp:136-139`）：`enabled=g_installed`、`error=g_install_error`。

业务全局状态 `g_targets`/`g_pending_target`/`g_confirm_text` 为文件内 `std::array`/POD（`feature.cpp:35-38`）；面板与确认流程均经 `QolHostSdk::scheduler` 帧任务与游戏主线程回调推进。

## 文件地图

| 文件 | 事实位置 |
|---|---|
| `plugins/world_teleport/plugin.cpp:16-17,29-62` | 版本、资源声明、required/optional 依赖、端口。 |
| `plugins/world_teleport/plugin.cpp:64-172` | install/deactivate/on_enabled/on_disabled/fill_status、feature、descriptor。 |
| `plugins/world_teleport/feature.h:11-40` | `InstallDisposition`/`install_error`、业务入口与端口声明。 |
| `plugins/world_teleport/feature.cpp:17-38` | 常量、目标/确认缓冲全局状态。 |
| `plugins/world_teleport/feature.cpp:58-115` | facts 读取、调度、`ui_choice` 现取。 |
| `plugins/world_teleport/feature.cpp:131-185,187-215` | 选择面板构造与（可延迟）打开。 |
| `plugins/world_teleport/feature.cpp:226-322` | 确认、关闭、转账（扣费+切图）、取消、选择回调。 |
| `plugins/world_teleport/feature.cpp:328-340` | `world_teleport_call_map_name` 入口。 |
| `plugins/world_teleport/rules.cpp:7-22` | `max_map_id_from_record_count` / `target_map_id` 回绕。 |
| `game/access/patches.cpp:46-55` | `P_UI_PLAY_CALL_MAP_NAME` 站点（branch-return、原版 `mov x0,#1`、epilogue）。 |
| `game/symbols/game_symbols.h:826` | `UIPlay_CallMapName` VMA。 |
| `contracts/game_host.h:150-157` | `QolWorldActionApi` 成员。 |
| `tests/test_world_teleport_status.cpp`、`tests/test_world_teleport_install.cpp`、`tests/CMakeLists.txt:1103-1120` | 安装/状态语义 host target。 |
