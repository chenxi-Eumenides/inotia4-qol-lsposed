# VM-K5-03：阶段 5 批 3——debug/feature 路由 owner 审计

## 变更范围

- 审计 18 条已有真实 native 落点的路由：autosell 2 条、extension_bag 8 条、core.ui 5 条、legacy debug 3 条。
- `/api/debug/ui`、`/api/debug/path`、`/api/debug/item/raw` 的 owner 从 `api.debug` 改为 `legacy.debug`；三条路由仍通过原有 Kotlin `InfoApiServiceCore` / `InfoSystemQueries` / `NativeBridge` 调用链。
- core.ui、extension_bag、autosell 的 owner、HTTP 方法、路径、`op_gated`、`lease_required` 均保持不变。
- 路由映射修正为实际逻辑入口：debug 查询使用 `api/native/*_read.inc`，扩展背包测试入口使用 `plugins/extension_bag/extension_bag_public_runtime.inc`。
- Controller、Service、NativeBridge external 方法名与签名、成功响应和业务语义未修改。

## 18 条审计表

| 路由 | owner | Kotlin 链 | JNI / native 逻辑入口 |
|---|---|---|---|
| `GET /api/feature/autosell/config` | `autosell` | `AutoSellFeatureController.getConfig:26-27` → `nativeAutoSellStatusJson` | `bridge/native/gamebridge_autosell.cpp:101-107` → `plugins/autosell/config.cpp:57` |
| `POST /api/feature/autosell/config` | `autosell` | `AutoSellFeatureController.setConfig:29-44` → `nativeSetAutoSellConfig` + status | `bridge/native/gamebridge_autosell.cpp:29-67` → `plugins/autosell/config.cpp`, `scan.cpp`, `store.cpp` |
| `GET /api/debug/ui` | `legacy.debug` | `DebugController.ui:22-23` → `InfoApiServiceCore:375` → `InfoSystemQueries:8` | `gamebridge_data.cpp:115-118` → `api/native/game_ui_read.inc:1` |
| `GET /api/debug/path` | `legacy.debug` | `DebugController.path:25-27` → `InfoApiServiceCore:376` → `InfoSystemQueries:9` | `gamebridge_data.cpp:150-153` → `api/native/game_world_navigation.inc:67` |
| `GET /api/debug/item/raw` | `legacy.debug` | `DebugController.itemRaw:29-35` → `InfoApiServiceCore:377` → `InfoSystemQueries:10` | `gamebridge_data.cpp:155-158` → `api/native/game_inventory_read.inc:162` |
| `POST /api/debug/settings-ui/inject` | `core.ui` | `DebugController:37-38` → `InfoApiServiceCore:378` → `InfoSystemQueries:11` | `gamebridge_settings.cpp:18-24` → `plugins/core.ui/panels/settings.cpp:287` |
| `GET /api/debug/settings-ui/status` | `core.ui` | `DebugController:40-41` → `InfoApiServiceCore:379` → `InfoSystemQueries:12` | `gamebridge_settings.cpp:27-33` → `settings.cpp:296` |
| `POST /api/debug/settings-ui/restore` | `core.ui` | `DebugController:43-44` → `InfoApiServiceCore:380` → `InfoSystemQueries:13` | `gamebridge_settings.cpp:36-42` → `settings.cpp:332` |
| `POST /api/debug/settings-ui/open-option` | `core.ui` | `DebugController:46-47` → `InfoApiServiceCore:381` → `InfoSystemQueries:14` | `gamebridge_settings.cpp:45-51` → `settings.cpp:353` |
| `POST /api/debug/settings-ui/open-panel` | `core.ui` | `DebugController:49-50` → `InfoApiServiceCore:382` → `InfoSystemQueries:15` | `gamebridge_settings.cpp:54-60` → `settings.cpp:348` |
| `GET /api/debug/extension_bag/status` | `extension_bag` | `DebugController:52-53` → `InfoApiServiceCore:383` → `InfoSystemQueries:16` | `gamebridge_extension_bag.cpp:4-10` → `extension_bag_api.cpp:4-6` |
| `POST /api/debug/extension_bag/enter_view` | `extension_bag` | `DebugController:55-61` → `ActionApiServiceCore` → `nativeOpExtensionBagEnterView` | `gamebridge_extension_bag.cpp:13-21` → `extension_bag_api.cpp:8-10` |
| `POST /api/debug/extension_bag/exit_view` | `extension_bag` | `DebugController:64-65` → `ActionApiServiceCore` → `nativeOpExtensionBagExitView` | `gamebridge_extension_bag.cpp:35-42` → `extension_bag_api.cpp:12-14` |
| `POST /api/debug/extension_bag/unequip` | `extension_bag` | `DebugController:67-73` → `ActionApiServiceCore` → `nativeOpExtensionBagUnequip` | `gamebridge_extension_bag.cpp:24-32` → `extension_bag_api.cpp:24-26` |
| `POST /api/debug/extension_bag/select_bag` | `extension_bag` | `DebugController:76-82` → `ActionApiServiceCore` → `nativeOpExtensionBagSelectBag` | `gamebridge_extension_bag.cpp:45-53` → `extension_bag_api.cpp:16-18` |
| `POST /api/debug/extension_bag/click_item` | `extension_bag` | `DebugController:85-93` → `ActionApiServiceCore` → `nativeOpExtensionBagClickItem` | `gamebridge_extension_bag.cpp:56-65` → `extension_bag_api.cpp:20-22` |
| `POST /api/debug/extension_bag/equip` | `extension_bag` | `DebugController:96-102` → `InfoApiServiceCore:384` → `InfoSystemQueries:17` | `gamebridge_settings.cpp:72-80` → `extension_bag_public_runtime.inc:619` |
| `POST /api/debug/extension_bag/item` | `extension_bag` | `DebugController:105-116` → `InfoApiServiceCore:385` → `InfoSystemQueries:18-19` | `gamebridge_settings.cpp:83-96` → `extension_bag_public_runtime.inc:631` |

## R-xx / B-xx 影响

- 本批只修改 HTTP route owner 元数据和映射文档，不修改扩展背包、自动出售或 UI 行为；因此无新增或改变的 `R-xx` / `B-xx` 编号。
- 扩展背包既有语义保持由 `VM-K4-03`、`VM-P4-07` 及既有 R-02/R-15/R-16/R-25/R-31 证据覆盖；自动出售保持由 `VM-44` 覆盖；core.ui 生命周期保持由 `VM-D1` 覆盖。

## 验证结果

- `uv run python scripts/maintenance/check_http_routes.py`：143/143，owner 分布含 `legacy.debug=3`，重复/遗漏均为 0。
- `uv run python scripts/maintenance/check_arch.py`：全部通过，无新增违规。
- `git diff --check`：通过。
- Host CTest：58/58 通过。
- Debug 构建：`output/inotia4_qol_lsposed_debug_2609222040_f5d76deb0f02.apk`，`BUILD SUCCESSFUL`。
- LSPatch：3/3 成功；本次设备使用 `output/Inotia4_v1.3.2_monster_v29_lspatched.apk`。
- 真机：`variant=monster v29`、`ready=true`、进档后 `screen=world`；`/api/system/routes` 为 `registered=143`、`expected=143`、`duplicates=0`、`omitted=0`、`accepting=true`，三条 legacy debug 路由 owner 均为 `legacy.debug`。
- 全量 smoke：`total=143、pass=120、warn=23、fail=0`；warn 数未增加。
- 清空 logcat 后装配：`registered=23 install_failed=0 kernel_only=false`；未发现 FATAL、SIGSEGV 或 UnsatisfiedLinkError。

证据文件：本目录的 `build-debug.log`、`lspatch.log`、`host-ctest.log`、`http-routes.log`、`check-arch.log`、`smoke.json`、`smoke.log`、`routes.json`、`health.json`、`enter-slot.json`、`screen.json` 与 `logcat-selected.txt`；冻结基线为 `scripts/verification/smoke_baseline_v0.7.13.json`。

## 补行为证据：owner 生命周期绑定（2026-09-22）

### 前置与构建确认

- 设备：`192.168.3.54:5555`，monster `v29`。
- 重启后 `/api/health`：HTTP 200，`ok=true`、`ready=true`、`variant=monster/v29/known=true/capabilities=3`。
- logcat 模块身份：`versionCode=192`、`versionName=0.7.13`、`sha256=f5d76deb0f02710cc377c9fd26c785272b4637a0dc91627ad6330eb3b39b009b`，与批 3 Debug 构建一致。
- 同意页 `select ok`：HTTP 200，`{"ok":true,"result":"close_dispatched"}`；`enter_slot slot=0`：HTTP 200；随后 `GET /api/ui/screen` 为 `{"screen":"world"}`。

### extension_bag

只读查询：`GET /api/debug/extension_bag/status`。

| 阶段 | 查询响应摘要 | owner/资源状态 |
|---|---|---|
| 正常 | HTTP 200；`enabled=true`、`injected=true`、`save_panel_hook=true`，`state.mode=original`，背包容量 `[16,16,12,12,12]` | `extension_bag=enabled`，29/29 `live`，`error=null`，`in_flight=0` |
| 停用后 | HTTP 200；`enabled=false`、`injected=false`、`extension_tab_button=false`、`inventory_frame_active=false`、`state={}`；无错误体 | `extension_bag=inactive`，29/29 `free`，`in_flight=0`；deactivate ticket=7 |
| 恢复后 | HTTP 200；`enabled=true`、`injected=true`、`save_panel_hook=true`，`state.mode=original`，查询恢复完整背包状态 | `extension_bag=enabled`，29/29 `live`，`error=null`，`in_flight=0`；activate ticket=8 |

结论：资源物理生命周期明确绑定到 `extension_bag` owner；但该只读 native 处理函数没有 feature gate，停用态仍返回 HTTP 200，而不是明确错误。

### autosell

只读查询：`GET /api/feature/autosell/config`。停用窗口在查询完成后立即恢复。

| 阶段 | 查询响应摘要 | owner/资源状态 |
|---|---|---|
| 正常 | HTTP 200；`enabled=false`、`rarity=1`、`enhance=1`、`socket=1`、`sold=0`、`failed=0`、`hostInstalled=true` | `autosell=enabled`，2/2 `live`，`error=null`，`in_flight=0` |
| 停用后 | HTTP 200；响应仍为配置对象，`enabled=false`、计数保持 `sold=0/failed=0`、`hostInstalled=true`；无错误体 | `autosell=inactive`，2/2 `free`，`in_flight=0`；deactivate ticket=9 |
| 恢复后 | HTTP 200；响应与正常查询一致，配置仍为 `enabled=false`、`hostInstalled=true` | `autosell=enabled`，2/2 `live`，`error=null`，`in_flight=0`；activate ticket=10 |

结论：资源生命周期明确绑定到 `autosell` owner；该配置查询也没有停用态 HTTP/error gate，停用时照常成功返回配置。这里的 `enabled=false` 是用户自动出售配置，不是 owner 生命周期状态；owner 状态由 features/resources 端点确认。

### 总结与证据索引

- 两个 owner 均完成「正常查询 → deactivate → 确认资源释放 → 停用态查询 → activate → 确认资源恢复 → 复查」闭环，最终 `screen=world`，两者均回到 `enabled`，EB 29/29、autosell 2/2 资源均为 `live`。
- 路由行为对 owner 生命周期的体现为**资源与 owner 状态绑定**，不是 HTTP 路由 gate：本次两条只读 native 查询在停用态均 HTTP 200，未返回明确错误；该无 gate 行为如实记录。
- R-xx/B-xx：本次仅补真机行为证据，不新增或改变既有编号；扩展背包沿用 R-02/R-15/R-16/R-25/R-31 与 VM-K4-03/VM-P4-07，自动出售沿用 VM-44。本补证对应 VM-K5-03 的 owner 生命周期行为缺口，受影响 VM 卡为 VM-K4-03、VM-44；已执行真机查询、生命周期切换、资源核验和恢复复查。
- logcat：全程清空后采集至 `logcat-all.txt`，`FATAL=0`、`SIGSEGV=0`、`UnsatisfiedLinkError=0`。
- 原始响应、features/resources 快照、摘要和完整 logcat：见本目录 `*-normal.txt`、`*-disabled-*`、`*-restored-*`、`feature-summary.json`、`resource-summary.json`、`logcat-all.txt`。
