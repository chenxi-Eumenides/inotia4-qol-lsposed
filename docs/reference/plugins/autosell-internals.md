# `autosell` 内部实现

本文记录 `autosell` 的目录职责、装配、扫描与持久化机制、失败恢复和验证边界。对外行为、规则语义、HTTP 契约与调用方保证见 [`autosell.md`](autosell.md)。本文与对外契约是 `autosell` 的仅有两份权威文档；`docs/archive/` 与 `docs/refactor/audits/` 下的自动出售材料仅作历史追溯。

源码路径相对 `module/app/src/main/cpp/`；插件内文件（含 `ui/`）省略 `plugins/autosell/` 前缀。Kotlin 路径为完整仓库路径。

## 分层与目录

| 层/职责 | 主要位置 | 说明 |
|---|---|---|
| 纯规则与类型 | `contracts/autosell.h`、`plugins/autosell/rules.{h,cpp}` | `Config` 纯类型；阈值/分档判定与硬保护；不依赖宿主。 |
| sidecar 编解码 | `plugins/autosell/config_codec.h` | tier/旧 special 名映射、JSON 解析/序列化、字段钳制；inline 实现，供 store/config 与 JNI 共用。 |
| 运行时配置与状态 | `plugins/autosell/config.{h,cpp}` | `g_config_mtx` 保护的配置快照、进程内计数、slot-data 状态原子量、类型化 `StatusSnapshot`。 |
| 存档会话 | `plugins/autosell/scan_session.cpp` | slot/epoch/active 状态机；save-enter/exit 与加载完成时推进。 |
| 持久化 | `plugins/autosell/store.{h,cpp}` | slot-data participant、revision/dirty、写请求、60 帧重试、注销。 |
| Host 只读适配 | `plugins/autosell/read_only.{h,cpp}` | 转调 `item.facts`、Game Host、inventory extras port；不向业务层暴露游戏指针。 |
| 物品视图 | `plugins/autosell/view.{h,cpp}` | 物品引用 → 规则视图；宝石质量由调用方注入 provider。 |
| 扫描门禁 | `plugins/autosell/scan_gates.{h,cpp}` | 纯门禁顺序，依赖以函数指针注入，可 host 单测。 |
| 扫描与任务 | `plugins/autosell/scan.{h,cpp}` | 20 帧 tick、扫描体、轮次取消、任务同步、save 事件回调。 |
| 装配 | `plugins/autosell/plugin.{h,cpp}`、`routes.inc` | descriptor、能力获取、资源声明、feature、面板 provider、生命周期。 |
| 游戏内 UI | `plugins/autosell/ui/panel_*.cpp`、`ui/panel.h` | 入口锚定、几何、控件、事件草稿、provider；共享声明在 `panel_internal.h`。 |
| 状态 JSON | `plugins/autosell/status_json.{h,cpp}` | 只负责将 `StatusSnapshot` 组装为固定 JSON。 |
| JNI / HTTP 传输 | `bridge/native/gamebridge_autosell.cpp`、`module/app/src/main/java/com/inotia4/qol/api/controller/AutoSellFeatureController.kt` | 参数转换、严格校验、状态 JSON 返回。 |

依赖方向以“规则/类型 → 编解码/配置 → 持久化/扫描 → 装配/UI/传输”为主。`store.cpp` 在 load 回调中调用 `autosell_apply_config` 并同步扫描任务，属于持久化回调的编排入口；`config.cpp` 只依赖 `config_codec.h`，不再依赖 `store.h`。

## 装配

### Descriptor

| 项目 | 当前值 |
|---|---|
| id / version | `autosell` / `0.1.0`（`plugins/autosell/plugin.cpp`） |
| required capabilities | 6 项；id/版本/用途见对外契约的“能力与开关”。 |
| optional capabilities | 3 项：`inventory.bag_source`、`http.registry`、`ui_panels`。 |
| provides | 0 项。 |
| resources | 1 项：背包入口 call patch。 |
| ports | 2 项：`QOL_PORT_INVENTORY_EXTRAS` 1.1（保留的只读 extras 入口）、`QOL_PORT_ITEM` 3.8（`for_each_space_slot` 扫描 `space=0` 原版袋与 `space=1` C18 追加袋）。 |
| features | 1 项：`autosell.auto_sell`，默认 `0`，`kGameplay`/`kToggle`，`display_priority=-1`，callback 为 no-op。 |
| HTTP route metadata | 2 条：GET/POST `/api/feature/autosell/config`，owner `autosell`，`op_gated=false`，`lease_required=true`。 |
| JNI | descriptor `jni_count=0`；实际存在 4 个 JNI 导出（见“已知限制与风险”）。 |

required capabilities 在 `plugin.cpp` 声明；`inventory.bag_source` 1.2 是可选能力，只用于注册源枚举，缺失时原版袋与 C18 追加袋仍扫描。descriptor 定义见 `plugins/autosell/plugin.cpp`；路由元数据见 `plugins/autosell/routes.inc`。

### 资源 claim

| 项目 | 当前值 |
|---|---|
| id | `patch:SCENE_DRAW_EQUIP+0x1cc` |
| symbol | `P_SCENE_DRAW_EQUIP_DESC` |
| mechanism / policy | `kCallPatch` / `kPrivate` |
| requirement / failure | `kRequired` / `kFailClosed` |
| resumable | `true` |
| owner | `autosell` |
| step_budget | `120` |
| handler | `autosell_equip_desc_draw_wrapper` |

资源在 `plugin.cpp` 声明。宿主安装成功后在装备页绘制点调用 wrapper：先复刻原 `UIDesc_Draw`，再绘制入口按钮。资源失败会阻止插件进入可用态。

### 生命周期

| 入口 | 行为 |
|---|---|
| `plugin_install` | 保存 SDK/Game Host 指针；要求 2 个 port（inventory extras + item）；获取 `world.facts`/`item.facts`/`save.slot_data`/`ui.state`；注册 UI state 面板；注册 save-enter/save-exit；`apply_gate(true)`；注册面板 provider。任一步失败回滚指针并返回 1（`plugin.cpp`）。 |
| `plugin_on_enabled` | 重新获取运行时能力、注册 UI state、`apply_gate(true)`；失败保持门禁关闭（`plugin.cpp`）。 |
| `plugin_on_disabled` | 注销 slot-data participant、注销 UI state、清空能力指针、`apply_gate(false)`（`plugin.cpp`）。 |
| `plugin_deactivate` | 注销 slot-data、撤销 `ui_panels` owner、注销 save 回调、清 Game Host/port、`apply_gate(false)`、注销面板 provider（`plugin.cpp`）。 |
| `apply_gate` | 更新 `g_gate`，同步扫描全局开关与 UI 门控（`plugin.cpp`）。 |

`attribute.quality` 与 `inventory.action` 不缓存全局指针，而是在扫描/视图构建时按需 `require`；这样能力撤销后不会继续使用旧接口。

## 关键机制

### 扫描 tick、门禁与取消

- `infra::scheduler` 以 `kRenderPre`、间隔 20 帧注册 `autosell_tick`（`scan.h`、`scan.cpp`）。
- tick 入口先读取 `g_scan_cancel_epoch` 并构造 `ScanRound`；随后按顺序检查：feature 全局开启、slot-data session 活跃、存档配置 `enabled`、纯门禁（`scan.cpp`）。
- 纯门禁在 `scan_gates.cpp:18-52`：世界状态、游戏状态、自动出售面板 active、UI busy；`scan.cpp` 将 `read_only`/`panels.active`/`ui_busy` 适配为 `ScanGateDeps`。
- `ui_busy()` 只把 `kPanel`/`kDialog` 且 id 为 `inventory` 的层视为占用；其他 UI 不阻塞扫描（`scan.cpp`）。
- `remove_task_if_any()` 先递增取消纪元，再在 `g_task_mtx` 内移除任务；扫描在每个槽和每次出售前检查纪元，已进入 `inventory.action.sell` 的单笔事务允许完成（`scan.cpp`）。
- `sync_task()` 在 `g_task_mtx` 内二次复核 global/session/config，避免检查与注册之间的迟到窗口；重复调用不会重复注册（`scan.cpp`）。
- `save_enter` 校验 slot 后同步任务；`save_exit` 调 `autosell_store_reject_session()` 并移除任务（`scan.cpp`）。
- 原版物理袋：`scan_physical_bags` 通过 `QOL_PORT_ITEM.for_each_space_slot(space=0)` 枚举 `bag 0..4`；不再使用 legacy `for_each_bag_slot`，避免关闭背包面板后枚举为空。
- 注册源扫描：`scan_body` 在物理袋后先调用 `scan_variant_bags`（`space=1` C18 原生追加袋，经 `QOL_PORT_ITEM.for_each_space_slot`），再调用 `scan_registered_sources`（`inventory.bag_source` 1.2 的 `source_count/source_at`，`space>=2`）。每个引用校验句柄/category、`refresh_item_count` 后进入 `process_ref`；每类遍历错误按 code 变化限速 WARN，`kQolErrNotReady` 静默。取消纪元同样在回调内检查。

### 配置快照与计数

- `g_config_mtx` 保护 `autosell::Config g_config`；`autosell_set_runtime_config`/`autosell_get_runtime_config` 做拷贝读写（`config.cpp:13-34`）。
- `g_sold_total`、`g_failed_total`、`g_last_scan_frame`、`g_host_installed`、`g_store_slot`、`g_store_loaded_slot`、`g_store_persisted` 为原子量；状态查询通过 `autosell_status_snapshot()` 一次性读取（`config.cpp:36-63`）。
- `sold`/`failed`/`lastScanFrame` 是进程内累计，不随存档切换清零；与 [API 参考 §9.1](../api-reference.md) 一致。

### slot-data 持久化

- section 名/版本：`autosell` v2，固定 3 个存档槽；section IO 由 `save.slot_data` 执行（`store.cpp:20-22`）。
- `sd_load` 先做 4096 字节上限、JSON 语法与 codec 校验；再在 `g_config_commit_mtx` + `g_store_mtx` 下处理 pending intent/已有 dirty，校验 `(slot, epoch)` 后设置 `g_load_ready`，再调用 `autosell_apply_config` 并标记 session 已加载（`store.cpp:266-326`）。
- `sd_capture` 在 `g_store_mtx` 下复制当前槽配置并记录 `captured_revision`；`sd_complete` 只有 `committed && captured_revision == revision` 才清 dirty，且不在回调内重入 provider API（`store.cpp:329-383`）。
- `autosell_store_commit_config` 要求 `g_loaded_slot >= 0`、`g_epoch != 0`、`g_load_ready`；更新当前槽配置/revision/dirty，应用运行时配置，再发起写请求；无 session 或未加载直接拒绝且不修改运行时配置（`store.cpp:571-608`）。
- 写请求失败时保留 dirty；`store_flush` 每 60 帧由重试任务补发；`sd_complete` 只按 revision 清 dirty（`store.cpp:386-520`）。
- `autosell_store_unregister` 失败会恢复 handle 并确保重试任务继续运行（`store.cpp:610-653`）。

### 游戏内 UI

- 入口使用 `QolUiAnchoredButton`；每帧 `anchored_button_ensure` 由 `core.custom_ui` 解析活袋容器、校验句柄身份并在失效时只注销记录后重建，入口侧不删除游戏 native（`ui/panel_entry.cpp:77-104`）。
- 入口几何：68×68；横向对齐扩展页签列中心，纵向对齐原版任务袋中心，整体下移 3px（`ui/panel_internal.h:26-31`、`ui/panel_entry.cpp:25-75`）。
- 入口点击只通过 `ui_panels` 能力打开 `autosell` 面板；`panels.open` 只做注册表门禁，不建立 popup 栈（`ui/panel_ui.cpp`、`ui/panel_entry.cpp:17-23`）。
- 面板 provider 注册在 `plugin.cpp`；PopupState 与面板栈由 `core.custom_ui` 的 PopupHost 托管。面板 enter 创建控件，process 绘制，event 处理 `0x17/0x18`（`ui/panel_events.cpp`）。
- 草稿 `g_draft` 在 enter 时从运行时配置复制；返回/关闭时 `autosell_commit_draft` 调 `autosell_store_commit_config`。面板打开期间 `scan_gates` 的 `panels.active("autosell")` 会阻塞扫描（`ui/panel_events.cpp`、`scan.cpp`）。
- 面板实现已拆为 `panel_ui.cpp`（适配器/能力查找）、`panel_layout.cpp`（几何/命中）、`panel_entry.cpp`（入口/门控）、`panel_components.cpp`（控件/绘制）、`panel_events.cpp`（草稿/事件/provider）。
- 面板规则为 8 行：左列 3 行装备规则；右列 5 行依次为宝石等级、宝石属性、骰子、背包、徽章。右列不再绘制独立宝石分组框，未启用文字使用灰色。

### 物品视图与宝石质量

- `autosell_build_view` 通过 `item.facts.is_real_equip` 填 `is_equip`，不再使用 `item.facts.view.equip`（后者是“计数未编码/不可堆叠”，不是真实装备）。`read_only::item_is_real_equip()` 直接调用 `ItemFactsApi.is_real_equip`。
- 读取 `item.facts.view`，缺 `HasRarity` 时 rarity 记 `-1`；先按 category 判定宝石/背包/徽章/骰子，已知非装备不再查询真实装备。宝石 category 28..32 才要求 Jewel 字段；谓词不可用时按 category 28..32 回退。背包 1..4、骰子 52..56 直接按 category 识别，避免谓词未接线导致整类不卖。
- 分类字段：`category`、`is_backpack`、`badge_rank`（42..47→1、48→2、50→3、49/928..933→4）、`dice_tier`（52..56→1..5）。
- 宝石质量通过 `AttributeQualityApiProvider` 注入；provider 为空、struct_size 不合法、查询失败、结果不完整或状态非 Ok 时保持 `jewel_percentile=-1`，`gemRange` 规则不命中（fail-closed）。
- 硬保护在规则入口先于一切规则执行，但只对真实装备：`is_equip && (enhance_level > 0 || socket_filled > 0)` 才返回 `kHardProtected`；`enhance_level` 由 view 从 `facts.enchant_level` 复制。

### 编解码与状态 JSON

- `config_codec.h` 是 tier 字段与旧 `special` 名映射的唯一实现：sidecar v2 新增 `backpackTier`/`badgeTier`/`diceTier`，保留 `special` 兼容输出；旧 `special` 名字回退为 `backpack→4`、`normalSeal→1`、`dice→4`；显式 tier 优先。
- `status_json.cpp` 只读 `StatusSnapshot` 并输出固定字段顺序；新增 tier 字段，`special` 由 tier 派生；缓冲区 768 字节，字段值均由配置上限约束。

## 数据流

### 读取

```text
HTTP GET
  → Kotlin AutoSellFeatureController
  → JNI nativeAutoSellStatusJson
  → status_json
  → autosell_status_snapshot (config snapshot + atomics)
```

### 写入

```text
HTTP POST / 游戏内面板
  → autosell_store_commit_config
  → 校验 loaded session / epoch
  → 更新当前槽 config/revision/dirty
  → autosell_apply_config
  → sync_task
  → request_dirty_write → slot-data Immediate 写
  → sd_complete 按 captured_revision 清 dirty
```

### 扫描

```text
scheduler (每 20 帧)
  → autosell_tick: global/session/config + scan_gates
  → for_each_space_slot(space=0, bag 0..4)
  → scan_variant_bags (QOL_PORT_ITEM.for_each_space_slot space=1)
  → scan_registered_sources (inventory.bag_source 1.2 source_count/source_at → for_each_item, space>=2)
  → refresh_item_count
  → autosell_build_view (item.facts + 注入的 attribute.quality)
  → should_sell_with_reason
  → inventory.action.sell({space_id,bag,slot}, handle, count, "autosell")
       space=0/1: 原生 item_at + remove/add_money
       space>=2: QolBagSource.item_at 预读 + export/remove/import
  → 统计 sold/failed + lastScanFrame
```

### 存档

```text
save_enter → scan_session 标记 session → sync_task
sd_load → 校验 sidecar → 应用配置 → 标记 load_ready
sd_capture → 复制当前槽配置
sd_complete → 按 revision 清 dirty / 保留重试
save_exit → reject session → remove task
```

## 失败与恢复

| 情况 | 处理 |
|---|---|
| 无活动存档或 session 未 load | 拒绝配置写入，不修改运行时配置；HTTP 返回 409。 |
| sidecar 不可读/版本不支持 | load 返回错误，不应用配置。 |
| JSON 非法、超过 4096 字节、未来版本 | load 返回错误。 |
| 写请求失败/未受理 | 保留 dirty，60 帧重试任务补发。 |
| unregister 失败 | 恢复 handle，确保重试任务继续。 |
| 扫描被关闭/退出存档 | 递增取消纪元，逐槽/出售前停止；在途出售允许完成。 |
| 注册源枚举失败 | `kQolErrNotReady` 静默；其他错误按 `(space_id, code)` 变化限速 WARN，不影响物理袋扫描。 |
| 能力不可用 | 必需能力缺失由 Host 判 `blocked`；扫描内 `inventory.action` 不可用按状态变化限速 WARN。 |
| 单件出售失败 | `stale`/`empty_slot`/`no_item_handle` 静默跳过；其他失败计入 `failed` 并 WARN。 |
| 面板组件创建/清理失败 | 销毁已创建部分并请求宿主关闭面板。 |
| UI 关闭 | 隐藏可证明归属的入口；面板打开时请求关闭。 |

## 已知限制与风险

1. **扩展袋分三类空间**：原版 `space=0 bag 0..4`；C18 原生追加袋 `space=1`；`inventory.bag_source` 注册源 `space>=2`。`inventory.bag_source` 是可选能力，缺失时只跳过注册源；注册源回调失败只跳过该源，不影响前两类。任务袋 `space=0 bag=5` 始终不扫描。
2. **AS-13**：feature 关闭时不注销面板 provider；`autosell_provider_block_reason` 恒返回 `nullptr`。provider 没有枚举接口，当前依靠 `provider_open` 门禁拒绝。
3. **AS-16**：descriptor `jni_count=0`，但 bridge 保留 4 个 JNI 导出；JNI 三向绑定表尚未落地。
4. **AS-15 测试缺口**：已补 scan gates/task 的 host 覆盖；仍无真实 panel enter/process/event、JNI/Kotlin、HTTP 集成用例，也没有 `space=1`/注册源扫描的独立 host 用例。
5. **真机覆盖**：本轮未做真机验证。上一轮 C18 v520 的入口/面板证据不覆盖本次 real-equip、tier、`space=1`/注册源扫描和 8 行 UI 改动。
6. **业务互斥量保留**：`g_config_mtx`、`g_store_mtx`、`g_config_commit_mtx`、`g_task_mtx`、`g_entry_mtx` 仍存在。配置提交允许来自 HTTP/JVM 线程，因此没有按“全部业务状态只在主线程”模型改成命令队列；AS-02/06/07 已修掉已知重入/持锁跨插件调用路径。
7. **装配注释漂移**：`plugin.cpp` 的资源注释仍写“2 条常驻”，descriptor 实际为 1 条资源；以 descriptor 为准。
8. **面板关闭重复提交**：返回按钮与 provider close 都会调用 `autosell_commit_draft`；第二次提交在配置未变化时无写入，仅为一次幂等提交和日志。
9. **pending 状态残留**：`g_pending_intent`/`g_pending_config`/`g_pending_revision` 仍保留在 `sd_load` 分支，但唯一写入者 `autosell_store_persist` 已删除，当前不可达；后续可删除该分支及相关状态。
10. **无调用点的公开函数**：`autosell_ui_enabled()` 只有 `panel.h` 声明与 `panel_entry.cpp` 定义，仓库内无调用点；实际门控直接读 `g_enabled`。
11. **徽章档位与骰子顶级**：教团徽章（50）为 `badgeTier=3`，排在普通/被祝福之后、英雄之前；英雄（49/928..933）为 `badgeTier=4`。骰子顶级（55）是 `diceTier=4`，混沌（56）是 `diceTier=5`。若产品语义要求不同，需要再裁决。
12. **UI 无 host 测试**：面板已改为 8 行（左 3、右 5），没有 panel enter/process/event 的 host 覆盖，只有编译与静态检查。

## 测试与验证映射

| Target | 覆盖 |
|---|---|
| `autosell_rules_tests` | 纯规则：装备 AND、宝石 OR、背包/徽章/骰子分档、硬保护、档位边界、fail-closed。 |
| `autosell_view_diagnostic_tests` | 物品视图字段、真实装备判定、rarity 缺失、宝石档位、背包/徽章/骰子分类、注入的 quality provider 成功/失败/畸形 API。 |
| `autosell_store_tests` | sidecar JSON 编解码、tier 字段、旧 `special` 回退、字段钳制、版本回退。 |
| `autosell_slot_data_tests` | slot-data load/capture/complete、revision/dirty、无 session 拒绝、重试路径。 |
| `autosell_plugin_tests` | descriptor/资源/feature、required/optional 能力、2 个 port、install/deactivate 对面板 provider 的登记/反登记；扫描/UI 以桩替代。 |
| `autosell_scan_gates_tests` | 纯门禁顺序、短路、面板查询失败软处理、reason 名称。 |
| `autosell_scan_task_tests` | global/session/config 门控、scheduler add/remove 去重、重复 sync 幂等；`for_each_space_slot` 以桩替代。 |
| `inventory_action_sell_tests` | core.item 原生出售、provider 空间预读/export/remove/import、回滚与 stale 语义。 |

以上 target 的源文件为 `tests/test_autosell_*.cpp`，CMake 注册见 `tests/CMakeLists.txt`。

静态与构建：

- Host 全量 `155/155` 通过（本轮改动后）。
- `check_arch.py`：layers/offsets/jni/resources 全部通过。
- Android Debug 构建：`output/inotia4_qol_lsposed_debug_2610261921_ccddcac32d7e.apk`，sha256 `ccddcac32d7ec2c7faf69f5b1cac1266c0ea054cac2238e4f5ff7de9a946b7d3`。
- C18 v520 真机（LSPatch 集成包）验证：`refs=45 view_skipped=0 no_rule=33 no_sell=0 sold=12 failed=0 stale=0`；原版背包/白装、EB 空间的白/蓝装、宝石、背包、英雄徽章均已出售；剩余为药水/卷轴/材料等未命中规则物品。当前存档无骰子样本，骰子分类仅由 host 测试覆盖。

## 已删除旧结构

| 旧结构 | 现状 |
|---|---|
| `ui/panel.cpp`（833 行） | 删除，拆为 `panel_internal.h` + 5 个实现文件。 |
| `store.h` 内的 JSON 编解码 | 移到 `config_codec.h`；`store.h` 只保留 participant/提交/会话接口。 |
| `autosell_store_persist` | 删除；无生产调用者（AS-17）。 |
| `contracts/autosell.h` 的运行时/持久化声明 | 删除；契约头只保留 `Config`。 |
| `autosell_status_json` 在 `config.cpp` | 移到 `status_json.cpp`；`config.cpp` 只提供快照。 |
| `special_mask`/`SpecialType` | 删除；改为 `backpack_tier`/`badge_tier`/`dice_tier`，旧 `special` 仅作 JSON 兼容。 |
| `item.facts.view.equip` 作为装备判定 | 改为 `item.facts.is_real_equip`；`view.equip` 仅表示“计数未编码/不可堆叠”。 |
| EB 逻辑袋扫描与 `module_view_installed` 门禁 | 删除；改为 `space=1` C18 原生追加袋 + `inventory.bag_source` 注册源 `space>=2`。 |
| `CandidateDiagnostics`/fingerprint/`emit_candidate_diagnostic` | 删除；EB 逻辑袋移除后无调用者。 |
| 特殊类型 chips、宝石分组框 | 删除；面板改为 8 行，右列 5 行与宝石同列。 |

公开契约见 [`autosell.md`](autosell.md)。
