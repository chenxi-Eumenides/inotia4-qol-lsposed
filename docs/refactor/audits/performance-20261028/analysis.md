# 性能审计与优化方案（2026-10-28）

> 状态：分析完成；Batch A 已实现（待真机复测），Batch B–D 待排期。
> 证据目录：`.tmp/perf-sampling/20261028-094320-main_menu/`、`...-094449-world_idle/`、`...-094610-bag_open/`、`...-094832-bag_drag/`（本地产物，不提交）。
> 采样脚本：`scripts/verification/perf_sample.sh`。

## 1. 方法与场景

设备：192.168.3.54:5555（arm64-v8a，Android 12，root）。
工具：simpleperf（CPU 采样，1kHz，带调用链）+ perfetto（sched/freq/binder/atrace）+ SurfaceFlinger 帧间隔 + gfxinfo + `qol.log`。

场景与操作：

| 场景 | 操作 |
|---|---|
| `main_menu` | 主菜单静置 |
| `world_idle` | 进游戏后站在地图上不动 |
| `bag_open` | 打开背包页静置 |
| `bag_drag` | 背包页持续拖动物品 |

`bag_scroll` 不存在：游戏背包页没有滚动交互，`bag_open` + `bag_drag` 已覆盖静态与拖拽路径。

## 2. 测量结果

### 2.1 模块 CPU 占比（simpleperf，按 dso）

| 场景 | 模块 `libgamebridge.so` | 游戏 `libgame.so` | 内核 | 帧间隔 p50 / p90 |
|---|---:|---:|---:|---|
| `main_menu` | 11.7% | — | 36.0% | 47.7 / 95.4ms |
| `world_idle` | **39.2%** | 10.2% | 16.6% | 47.7 / 95.4ms |
| `bag_open` | **32.1%** | 2.7% | 34.7% | 31.9 / 79.5ms |
| `bag_drag` | **30.4%** | 2.8% | 36.7% | 31.9 / 95.4ms |

帧间隔均为 16.67ms 的整数倍，说明是 vsync 掉帧（20fps / 30fps），不是自由帧率。

### 2.2 模块内热点（调用链子树占比，存在嵌套，不可相加）

| 场景 | 热点 | 占比 | 代码位置 |
|---|---|---:|---|
| `main_menu` | `FeatureReconciler::step` | 6.5% | `host/features/reconciler.cpp:151` |
| `main_menu` | `snapshot_handle::publish` | 5.3% | `infra/snapshot_handle.cpp:45` |
| `world_idle` | `FeatureReconciler::step` | 18.6% | 同上 |
| `world_idle` | `FeatureReconciler::notify_toggles` | 10.2% | `host/features/reconciler.cpp:125` |
| `world_idle` | `snapshot_handle::publish` | 16.2% | 同上 |
| `world_idle` | `make_shared<vector<uint8_t>>` / shared_ptr release | 8.6% / 7.6% | `infra/snapshot_handle.cpp:50` |
| `world_idle` | `prepare_direction_overlay` | 2.1% | `game/input/eight_way_draw.cpp:123` |
| `bag_open` | `control_readable` | 24.2% | `host/ui_adapter.cpp:482` |
| `bag_open` | `child_at` | 18.9% | `host/ui_adapter.cpp:512` |
| `bag_open` | `install_extension_tab_buttons_locked` | 14.5% | `plugins/extension_bag/core/render.cpp:150` |
| `bag_open` | `FeatureReconciler::step` | 11.2% | 同上 |
| `bag_drag` | `control_readable` | 26.7% | 同上 |
| `bag_drag` | `child_at` | 20.9% | 同上 |
| `bag_drag` | `projection_item_for_control` | 12.0% | `plugins/extension_bag/core/item_display_hook.cpp:83` |
| `bag_drag` | `install_extension_tab_buttons_locked` | 12.2% | 同上 |

### 2.3 关键根因

1. **Reconciler 每逻辑帧全量运行**
   - `host/assembly.cpp:350` 每帧调用 `production_feature_frame_step()`。
   - `FeatureReconciler::step()` 无论是否有配置变更，都遍历约 30 个插件，执行 `notify_toggles()`、`std::map<std::string,...>` 查找和 `feature_registry_active()`（字符串查找 + 事务槽扫描）。
   - 采样中 `std::map`/字符串比较相关符号合计约 18%，与 `step()` 18.6% 吻合。

2. **快照发布全量分配 + 零初始化 + 拷贝**
   - `infra/snapshot_handle.cpp:50` 每次 `make_shared<vector<uint8_t>>(byte_size)`，vector 构造会零初始化，再 memcpy；旧 buffer 的 shared_ptr 释放也占 7.6%。
   - main_menu 下所有 provider 每 15 帧发布「not in game」状态，但仍按 73KB/112KB/165KB 全量拷贝。
   - 空闲时没有 HTTP 消费者，快照仍然重建/发布。

3. **背包页每次控件校验都发 `process_vm_readv`**
   - `host/ui_adapter.cpp:482-499` 对每个控件、每个链表节点调用 `process_vm_readv`；`child_at` 每次从链表头遍历，`valid_child_locked` 对 16 个槽逐个调用 → O(n²) 遍历 + 每节点系统调用。
   - 采样确认 `control_readable` 的 96.85% 时间在 `process_vm_readv` 内核路径（`get_user_pages_remote` / `follow_page_mask`）。
   - `bag_open`/`bag_drag` 的内核占比 34–37% 主要来自这里。

4. **背包页每帧全量 ensure 页签**
   - `render.cpp:150` 每帧调用 `install_extension_tab_buttons_locked`；`tabs_ui.cpp:313-350` 对 5 个页签逐个 `anchored_control_ensure`，每次包含 `find_root`、子控件列表校验、布局、`get_rect`、`set_control_show` 写。
   - `refresh_tab_bag_items_locked` 每帧扫 128 袋。

5. **次要项**
   - `eight_way_move` 的 `prepare_direction_overlay` 每帧移动/写箭头控件，即使没有触摸（world_idle 2.1%）。
   - 文本分派 `dispatch_run_text` 自身约 0.05%，加 libc `__strcmp` 部分总成本约 1–2%，不是主瓶颈。
   - 调度器 `scheduler_dispatch` 自身约 0.05%，成本都在任务体（reconciler/snapshot）。
   - autosell 的 no-rule 日志按 category 去重，只在首次发现时输出；采样中约 44 条 WARN/25s，属一次性诊断，不是持续热点。
   - 本次采样没有 HTTP 流量，`frame_cache` 任务未注册，缓存失效不在帧内热点中；只有 API 使用时才付 3ms/帧 × 约 4 帧的代价。

## 3. 优化方案

### P0-1 Reconciler 空闲门禁

- `FeatureReconciler::step()` 增加 dirty 门禁：无 pending revision、无 dependency dirty、无 entry、无在途生命周期事务/HTTP stop 时直接返回。
- `notify_toggles()` 只在 `toggles_dirty_` 时执行；保留每 300 帧一次兜底全量 step。
- 后续可把 `desired_/eligible_/notified_` 从 `std::map<std::string,...>` 换成按插件下标访问的扁平数组（P0-1b）。
- 预期：main_menu -6%，world_idle -18%，bag -11%。

### P0-2 背包 UI 校验缓存

- `control_readable` 增加帧/UI epoch 校验缓存；未命中才 `process_vm_readv`；所有 `delete_control`、屏幕/root 变化时失效。
- `child_at` 增加 `root → vector<void*>` 缓存，每 epoch 只遍历一次链表。
- `projection_item_for_control` 建立 `control → (bag, slot)` 表，O(1) 查找。
- 页签 ensure 用 `root + child_count + inventory_generation + screen` 做 epoch，未变化跳过；`set_control_show` 只在可见状态变化时调用。
- 预期：bag 页模块 32% → 10–15%，内核 GUP 大幅下降。

### P0-3 快照发布去重 + 去零初始化

- `snapshot_handle::publish` 先与当前 buffer `memcmp`，相同直接返回；不同才分配。
- buffer 改为 `unique_ptr<uint8_t[]>`（`new uint8_t[size]`，不零初始化）或 2–3 buffer 池。
- 后续增加消费者门控：provider 记录 `last_consumer_frame`，空闲超过 300 帧不重建；请求到来时标 dirty，下一帧重建。
- 预期：main_menu -5%，world_idle -10~16%。

### P1 其余项

- `eight_way_move`：无触摸时只隐藏一次箭头，不每帧写控件。
- 文本分派：无消费者时原子快速路径；消费者表 RCU/双缓冲去 mutex。
- 调度器：迁移 5 个 legacy `frame_task_*` 消费者，删除 `infra/frame/frame_task.*`；按 point 分桶 + swap-remove。
- 缓存：`frame_cache_invalidate(mask)` 按写操作影响面精准失效。
- HTTP/JNI：每请求栈缓冲、路由哈希、byte[] 返回、distance 零拷贝、去掉请求线程等待。

### 日志

当前日志量 100–200 行/30 秒，**不建议**为此加写线程。先做宏级别短路；autosell no-rule 日志按 category 去重已足够，暂不改。等 debug logging 开启或日志量上千行/秒再上 MPSC 环形队列 + 单写者。

## 4. 按需快照与票据（fresh read）

现有票据机制（`infra/command_queue`、`GET /api/system/tickets/{id}`）可用于「请求返回重建后的数据，且请求线程不等待」，但**不能直接改既有 query 路由**：`ARCHITECTURE.md` §2.2 冻结 query 成功响应。

推荐做法：

1. 保留现有 `GET /api/...` 语义：返回当前快照；请求 handler 调用 `provider.mark_consumer()`，若 provider 空闲则标 dirty，下一帧重建。允许最多 20 帧的陈旧度。
2. 新增显式 fresh-read 协议：`POST /api/system/snapshot/refresh`，body 指定 kind（如 `{"kinds":["world","character"]}`），返回 `{"ok":true,"ticket":N}`。
3. 主线程 handler 重建指定 kind，票据终态 result 返回 `{"ok":true,"generation":N}`（不返回大 JSON；票据结果预算 16KB）。
4. 客户端轮询票据到 `done` 后，再调用原 `GET`；provider 保证当前快照 generation ≥ 请求 generation，因此不会返回旧数据。
5. 若某类快照体积小，可在票据 result 中直接返回该 JSON；world/character 等大快照只返回 generation。

这样既不改既有 query 契约，也不让 HTTP 线程等待主线程。

## 5. Batch A 范围

本批只做：

- P0-1：Reconciler 空闲门禁 + `notify_toggles` dirty 门控。
- P0-3：`snapshot_handle` 发布去重 + 去零初始化。
- P1 的 autosell 日志：采样显示为一次性去重日志，暂不修改；如后续日志量增长再处理。

不在本批：P0-2（背包 UI）、消费者门控、fresh-read 路由、调度器迁移。

### 5.1 实施记录

- `FeatureReconciler::step()` 增加空闲门禁：无 revision/dependency/entry、无在途事务、无待处理 HTTP stop 时直接返回；首次 step 强制全量；每 300 次空闲 step 兜底全量。
- `notify_toggles()` 由 `toggles_dirty_` 门控，避免每帧遍历全部插件。
- 能力注册表新增 `capability_revision()`；reconciler 用 O(1) 计数变化触发依赖边重算，替代每帧全图扫描。
- 生命周期事务排空后只触发一次全量 step（`full_step_pending_`），不 bump retry batch，避免失败插件被反复重试。
- owner token 未就绪时最多重试 8 帧（`activation_retry_`），避免异步 owner 恢复被空闲门禁饿死。
- `snapshot_handle::publish` 先与当前 buffer `memcmp`，相同内容直接返回；buffer 改为 `unique_ptr<uint8_t[]>` + `new uint8_t[size]`，去掉 vector 的零初始化。
- 新增 `infra_snapshot_handle_tests` 用例：相同内容发布复用同一 buffer。
- 验证：`scripts/build-debug.sh` 通过；`check_arch.py` 全部通过；Host CTest 154/157，通过项包含 `feature_reconciler_tests`、`production_feature_facade_tests`、`custom_recipe_owner_fixture_tests`、`infra_snapshot_handle_tests`；3 个失败为既有、与本批 diff 无关：`custom_recipe_tests`（recipe 索引期望）、`core_api_lifecycle_tests`（core.api feature description 期望）、`item_handle_contract_redline`（`contracts/game_ports/item_port.h` 既有裸指针违规）。
- 产物：`output/inotia4_qol_lsposed_debug_2610281022_05ec913db97f.apk`（SHA-256 `05ec913db97f50e847bec24ebc3ff4ee8633fc72c3311ef6fe3a44d96242ae08`）。

### 5.2 复测结果（2026-10-28 10:34 / 10:37）

部署：C18L v589 集成包覆盖安装，包内模块 APK SHA-256 `05ec913d…` 与新构建一致；游戏重启后采样 20 秒。

| 指标 | 基线 | Batch A 后 | 变化 |
|---|---:|---:|---:|
| `main_menu` 模块 CPU 占比 | 11.7% | **6.5%** | -5.2pp |
| `world_idle` 模块 CPU 占比 | 39.2% | **12.6%** | -26.6pp |
| `FeatureReconciler::step` | 6.5% / 18.6% | 不在热点前 14 | 已消除 |
| `snapshot_handle::publish` | 5.3% / 16.2% | 不在热点前 14 | 已消除 |
| `world_idle` 总 CPU cycles/s | 0.605B | 0.315B | **-48%** |
| `world_idle` p99 帧间隔 | 111.3ms | 95.7ms | 改善 |
| `main_menu` / `world_idle` p50 | 47.7ms | 47.7ms | 不变（20fps 上限/vsync） |

说明：基线采样 25 秒、复测 20 秒；`main_menu` 的绝对 cycles/s 受启动阶段影响，不作为精确对比。帧率未提升说明当前场景受 vsync/GPU 或引擎上限约束，Batch A 的收益主要是 CPU 余量、功耗和后续批次的可优化空间。

Batch A 后新的模块热点：

- `world_idle`：autosell 扫描（`inventory_for_each_space_slot` / `native_bag_layout` / `game_memory_accessible`）约 13.6%；`eight_way_move` 的 `prepare_direction_overlay` 约 4.2%；`scheduler_dispatch` 任务体仍占 10.8%（其中大部分是游戏原绘制）。
- `main_menu`：`settings_main_menu_more_games_draw` 约 5.6%，`ui_main_menu_live_tree_snapshot` 约 3.1%，`game_memory_accessible` 约 3.2%（部分与前者重叠）。
- `bag_open` / `bag_drag` 未变，仍以 `control_readable` / `child_at` / 页签 ensure 为主。

### 5.3 Batch 计划（修订）

Batch B 已按用户选择先做背包 UI，见 §5.4。后续批次调整为：

- **Batch C（全局剩余热点）**：
  - `eight_way_move` 空闲 overlay 门控：无触摸时只隐藏一次箭头，不每帧 `control_rect`/`set_control_rect`。
  - autosell 扫描优化：复用已缓存的 native bag layout；在单次扫描内缓存 `game_memory_accessible` 结果或只校验袋槽表一次，减少逐物品 `/proc/self/maps` 查询。
  - `settings` 主菜单按钮绘制门控：`settings_main_menu_more_games_draw` / `ui_main_menu_live_tree_snapshot` 只在树签名变化时完整 ensure，其余帧只绘制。
  - 目标：`world_idle` 再降约 10–15% 总 CPU，`main_menu` 模块占比从 6.5% 降到约 2–3%；帧率仍可能受 vsync/引擎上限约束。
- **Batch D（深度校验与机制）**：
  - `control_readable` / `process_vm_readv` 的帧级正缓存或已知游戏内存范围快路径，继续压背包/商店/合成页内核 26–31%。
  - 快照消费者门控（无 HTTP/UI 消费者时不重建）。
  - 文本分派快速路径、legacy scheduler 迁移、HTTP/JNI、日志。

Batch C 不解决：背包页剩余 22.5% 的 `control_item_get_item` / `valid_child_locked` 成本（归 Batch D）、HTTP 延迟、帧率上限。

### 5.4 Batch B 实施记录（背包 UI）

提交：

- `5b69550b`：`child_count`/`child_at` 子控件列表缓存（root + epoch）、背包页签 10 帧 ensure 门控。
- `5ddb8823`：修复 `create_control` 后未失效子控件缓存导致页签反复重建/崩溃。
- `868955fc`：`native_bag_count_locked()` 按 EB revision 缓存，消除商店/合成页每帧逐袋扫描。
- `433f70f3`：商店/合成页签 10 帧 ensure 门控 + 逐帧存活校验；`projection_item_for_control` 增加 `control→slot` 缓存。
- `31836974`：`native_bag_layout()` 1 秒 TTL 缓存，避免 `set_current_bag` 每帧触发 `/proc/self/maps` 解析。

复测（20 秒采样，模块 CPU 占比）：

| 场景 | 基线 | Batch B 后 | 变化 |
|---|---:|---:|---:|
| `bag_open` | 32.1% | 22.5% | -9.6pp |
| `bag_drag` | 30.4% | 18.5% | -11.9pp |
| `store_open` | 54.2%（缓存前） | 19.3% | -34.9pp |
| `mix_open` | 58.3%（缓存前） | 17.5% | -40.8pp |

剩余热点：`process_vm_readv` / `/proc/self/maps` 校验仍占内核 26–31%；`control_item_get_item` / `valid_child_locked` 仍是背包页主要用户态成本。帧间隔 p50 仍为 47.7ms（20fps），CPU 下降尚未转化为帧率提升，需排除场景状态/引擎上限影响。

`31836974` 的布局缓存已构建并安装，但尚未单独采样；最终包 SHA-256 `34a5972327e5761d4409f2fa03515f1fd5c7724340ca9085c5cfc43c1ffb1bb3`。

## 6. 验证

- 构建：`scripts/build-debug.sh`。
- 静态检查：`uv run python scripts/maintenance/check_arch.py`。
- Host CTest：154/157；3 个既有失败见 §5.1（与本批 diff 无关）。重点 `feature_reconciler_tests`、`production_feature_facade_tests`、`custom_recipe_owner_fixture_tests`、`infra_snapshot_tests`、`infra_snapshot_handle_tests` 均通过。
- 真机：同一脚本复测 `main_menu`、`world_idle`，对比模块占比与 `FeatureReconciler::step`、`snapshot_handle::publish` 的子树占比。
- 行为回归：配置开关、插件 activate/deactivate、依赖级联、HTTP query（143 路由 smoke）。

## 7. 限制

- 帧间隔量化到 16.67ms，CPU 降低不一定线性提升 FPS；若引擎/GPU 上限为 20fps，收益体现在帧稳定性和功耗。
- 采样期间没有 HTTP 轮询；消费者门控的收益需在 API 使用场景下单独验证。
- `control_readable` 是防止 stale 控件的安全机制，缓存必须保留 fail-closed 语义，不能直接删除校验。
