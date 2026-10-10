# core.ui_state custom panel/dialog 验证记录

## 当前范围

实现提交：`039c4043`；P1 修复：`41c2f473`。三个既有 GET route、响应字段集和 `NativeBridge` external 签名未变；`/api/ui` 仍走 Kotlin `gamestateJson()` legacy 路径。未运行扩展背包 UI 测试或操作。

## 构建与 Host

- Debug 模块 SHA-256：`51dae2d00e617351e45a7fb44c3551f3b1af2a9422ac5d737980e3d3314e16f2`。
- LSPatch monster v31 集成包运行时 `module identity` SHA 与模块一致；`firstInstallTime=2025-06-13` 保持。
- UI targets：`core_ui_state_tests`、`core_ui_panels_tests`、`native_choice_tests`、`ui_state_port_tests`、`settings_contributions_tests`、`plugin_registry_tests` 通过；`check_arch.py` 新增违规 0；`check_http_routes.py` 146；Debug build 成功。
- **测试范围偏差**：fixer报告另执行了全量 CTest 128/128，而派发范围要求仅 UI targets、避免 EB tests。由于全量可能包含 EB Host targets，此记录不把该全量结果计作 EB 验收；没有执行 EB 真机测试。

## 非 EB 真机子项

设备：`192.168.3.54:5555`，monster v31，实际模块 SHA 如上。

### 自定义 settings panel

1. 进入 slot0 world 后，通过 `/api/debug/settings-ui/open-panel` 打开 `core.settings` provider。
2. `GET /api/ui/panel` 返回 `{"panel":"panel_settings"}`；`GET /api/ui/screen` 返回底层 `panel_in_app`，不被自定义 panel 值替换；`GET /api/ui/dialog` 保留原生 in-app dialog 内容。
3. `POST /api/ui/close_panel` ticket `done`；随后 screen 恢复 `world`，panel 返回 `null`。

初次从主菜单调用 native `/api/ui/open_panel {"panel":"settings"}` 得到 `not in game`，属于原生 UI port 的游戏态守卫；自定义 PanelProvider 使用 settings 专用 debug/action 入口验证，不把该失败误判为自定义 stack 失败。

### 自定义 world-teleport dialog

用户手动打开 world teleport 选择框；本代理只读查询，不选择传送动作：

- `GET /api/ui/screen`：`{"screen":"dialog_choice"}`。
- `GET /api/ui/panel`：`{"panel":null}`。
- `GET /api/ui/dialog`：顶层 `type="world_teleport.confirm"`、`active=true`；native 原有 `displayed`、`speaker`、`options` 字段和值保留，未新增字段。

这是 `ui_choice` type ID 的设备侧例证。没有 route/API 可以安全请求打开 world-teleport choice；本次不以 `/api/ui/open_panel world_map` 替代（原生 API 明确拒绝该事件驱动面板）。

## 结论与限制

- `core.ui_state` custom panel stack 映射与 custom dialog type 映射在上述非 EB 场景可观察到；P1 的 dialog 原字段保留、pending close 不提前清栈、native snapshot 读取失败不回旧成功体另有 Host 回归。
- 未测 nested panel、其他 custom provider、close pending 的真机边界；这些仍不能从本日志宣称完整行为验收。
- EB panel、商店、背包以及任何 EB 操作均未测试，保持用户暂停要求。Fixer 全量 CTest 的范围偏差如上记录，不作为 EB 证据。
