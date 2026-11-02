# `autosell` 插件审阅登记（2026-10-20）

- 审阅对象：功能插件 `autosell`（`module/app/src/main/cpp/plugins/autosell/`，19 文件约 3577 行）。
- 基线参考说明（阶段 0.75）：`docs/reference/plugins/autosell.md`（本批新建）。
- 静态审计：阶段 1 @oracle 只读，A–G 七维，findings AS-01…AS-17；阶段 4 真机追加 AS-18。
- 本批提交：`527a2a5b`（无存档拒绝/slot-data 去重入）、`c4b9a971`（POST 严格校验）、`a5314a46`（能力重取/扫描取消/诊断）、`665f9ebd`（rarity fail-closed/档位注释/文案）、`685c80e4`（ui.state 门禁/save-exit 失效）。
- 现场基线：设备 `192.168.3.56:5555`、monster v29、health ready、screen main_menu；运行模块最终 sha256 `108a3d1e50bdcf405918cb871b024f2aa6bf54be21887e2afd32d0a9e460a37a`。

## §1 本批问题与处置

| 编号 | 严重度 | 文件:行 | 问题 | 处置 |
|---|---|---|---|---|
| AS-01 | P1 | `plugins/autosell/store.cpp:609-650`、`scan.cpp:612-617`、`gamebridge_autosell.cpp:56-58` | 无活动存档时 POST 配置仍应用运行态并暂存 pending，下一次加载覆盖槽 sidecar；回主菜单后 `g_load_ready` 仍为 true，配置继续被接受 | 本批修复：无 session/未 load 直接拒绝且不 apply；JNI 返回实际结果，Kotlin 返回 409；save-exit 调 `autosell_store_reject_session()` 失效 session，下一次 `sd_load` 恢复 |
| AS-02 | P1 | `plugins/autosell/store.cpp:349-383`、`plugins/core.save/slot_data_provider.cpp:473-490` | `slot_data_save_finish` 持 `g.mutex` 调 `sd_complete`；complete 内立即 `request_dirty_write → query` 重入同一锁，保存完成路径可自死锁 | 本批修复：complete 只清 dirty/记录日志，较新 revision 由 60 帧 retry task 补发；host fake 用例断言 complete 内 query/request 为 0 |
| AS-03 | P1 | `plugins/autosell/plugin.cpp:50`、`scan.cpp:339-378` | UI 占用检查依赖 optional EB；EB 关闭时 fail-open；EB 启用时 `inventory_state_id` 世界常态返回 20，扫描恒 `ui_busy` | 本批修复：新增 required `ui.state` 1.1，用 `read_layers` 检测非 `none` 面板/对话框；独立于 EB；EB 关闭时物理袋继续、逻辑袋跳过 |
| AS-04 | P2 | `plugins/autosell/plugin.cpp:53-63,231-252` | `plugin_deactivate` 动态 require `ui_panels` 但 descriptor 未声明 | 本批修复：`ui_panels` 1.3 加入 optional_requires |
| AS-05 | P2 | `plugins/autosell/plugin.cpp:143-201,254-269` | world.facts/item.facts/slot_data 接口指针跨能力撤销缓存；on_enabled 不重新获取 | 本批修复：install/on_enabled 统一重新 acquire 并校验；on_disabled/deactivate 清空指针 |
| AS-06 | P2 | `plugins/autosell/scan.cpp:505-536` | tick 持 `g_scan_mtx` 跨越 item/world/attribute/inventory 插件调用 | 本批修复：删除私有扫描锁，依赖主线程 scheduler 串行；跨插件调用不持锁 |
| AS-07 | P2 | `plugins/autosell/scan.cpp:505-547,549-582` | 关闭/退出后在途扫描不取消；sync_task 检查与注册之间存在迟到注册窗口 | 本批修复：tick 入口捕获取消纪元，remove 任务递增纪元，逐槽/出售前检查；任务锁内复核 global/session/config |
| AS-08 | P2 | `AutoSellFeatureController.kt:37-106` | HTTP 字段容错转换/钳制，非法类型或数组元素不报错 | 本批修复：类型/整数范围/special 元素严格校验返回 400；native 返回 false 时 409 |
| AS-09 | P2 | `contracts/autosell.h:32-34` | 契约注释写 gemRange 档 5=100，实现/面板为 99 | 本批按裁决统一为 99（保留满分宝石），仅改注释 |
| AS-10 | P2 | `plugins/autosell/ui/panel.cpp:727` | 面板文案暗示所有装备都不会出售 | 本批改为「按规则定时出售物品。已强化或已镶嵌物品不会被出售。」 |
| AS-11 | P2 | `plugins/autosell/view.cpp:40-46`、`rules.cpp:13-17` | 所有物品强制要求 rarity 字段，变体缺 getter 时特殊/宝石物品被整件跳过 | 本批修复：缺 HasRarity 记 `-1`；装备 rarity 规则开启时未知值 fail-closed |
| AS-12 | P2 | `plugins/autosell/scan.cpp:443-450,394-410` | 遍历失败、sell API 不可用静默；failed 只统计已调用出售 | 本批补按状态变化限速 WARN；failed 口径保持出售事务失败数 |
| AS-13 | P2 | `plugin.cpp:254-269`、`ui/panel.cpp:868-892` | feature 关闭不注销 panel provider；`block_reason` 恒 nullptr | 登记开放：provider 无枚举接口，`open` 已由门禁拒绝；是否停用时撤销待后续裁决 |
| AS-14 | P2 | `docs/reference/api-reference.md` §9.1 | autosell 路由缺请求/响应 schema 与错误语义 | 本批补 GET/POST schema、400/409、计数作用域 |
| AS-15 | P2 | `tests/CMakeLists.txt`、`tests/test_autosell_*` | 无真实 scan/tick、panel、JNI/HTTP、生产 provider 并发用例 | 部分修复：补无 session、完整回调去重、能力重取、rarity 用例；scan/panel/JNI 集成仍缺 |
| AS-16 | P3 | `plugin.cpp:283-306`、`NativeBridge.kt` | descriptor `jni_count=0`，但存在 4 个 autosell JNI 导出 | 登记开放：JNI 三向绑定表 P5 未落地 |
| AS-17 | P3 | `contracts/autosell.h:46`、`store.cpp:570-607` | `autosell_store_persist` 无生产调用者，保留旧 pending 写路径 | 登记开放，未删除 |
| AS-18 | P1 | `plugins/autosell/scan.cpp:339-378` | 真机追加：EB 启用时 `inventory_state_id()` 世界常态=20，`!=5` 使 autosell 永不扫描（`lastScanFrame=-1`） | 本批随 AS-03 修复；真机 EB 开/关两态扫描均推进 |

审阅裁决（阶段 2，用户）：

- 无活动存档 POST：拒绝且不改运行态。
- gemRange 档 5：99，保留满分宝石。
- EB 不可用：仅跳过 EB 逻辑袋，物理袋继续扫描；不把 autosell 变成 EB 附属。
- 关闭/save-exit：停止并取消当前轮次；已进入 `inventory.action.sell` 的单笔允许完成。
- POST 类型/范围/special：严格校验并返回 400。
- 修复范围：P1 + 可直接修 P2；AS-13/16/17 只登记。

## §2 无法验证项

- AS-02 真实 `save_finish` 并发保存路径未在真机复现（需在保存事务中并发改配置）；已由 host fake 用例覆盖「complete 内不重入 provider」。
- AS-06/07 的在途出售取消未在真机逐帧捕获；已验证关闭 config 后任务停止、`lastScanFrame` 不再推进，未构造「扫描中途关闭且仍有待售物品」的时序。
- AS-10/AS-11 的面板文案与 entry 按钮视觉未由用户目视确认；需阶段 5 手工卡。
- AS-12 的遍历失败/action 不可用 WARN 未在真机触发。
- AS-15 的 scan/tick、panel、JNI/HTTP 集成测试仍未补齐。
- 全量 `smoke_all.py` 未运行；本批只执行目标 API 与真机路径。
- `check_log_policy.py` 唯一 error 为既有 `special_equip` 缺 Kotlin LogDomain（非本批）；`test_core_feature_exposure.py` 的分类顺序断言为既有基线差异。

## §3 已完成验收登记

| 项 | 结果 | 证据 |
|---|---|---|
| host 全量 | **147/147 PASS** | `.tmp/autosell-audit/host-final`；autosell_rules 132、store 102、slot_data 213+、plugin 64 断言 |
| 架构/路由/JNI | **PASS** | `check_arch.py` 全绿；`check_http_routes.py` 147/147、owner `autosell=2`；`check_jni.py` 163/163 |
| Debug 构建 | **PASS** | `output/inotia4_qol_lsposed_debug_2610201558_108a3d1e50bd.apk`，sha256 `108a3d1e50bdcf…`；设备运行模块 identity 同 sha |
| AS-01 无存档拒绝 | **PASS** | 主菜单 POST 返回 409 `config rejected (no active save session)`，GET 配置字段保持不变（enabled=false/rarity=0） |
| AS-08 严格校验 | **PASS** | `enabled:"yes"`/`rarity:6`/`gemTier:1.5`/`special:["bad"]`/`special:"dice"`/坏 JSON 全部 400；合法 POST 200 且 `persisted=true` |
| AS-18/AS-03 EB 两态 | **PASS** | EB 关闭：物理袋扫描、`lastScanFrame` 推进、出售计数增加；EB 开启：`lastScanFrame` 继续推进（337→397→457），不再 `ui_busy` |
| AS-05 生命周期 | **PASS** | deactivate → `inactive`、资源 free；activate → `enabled`、资源 live、扫描恢复（frame 670→730） |
| 配置持久化 | **PASS** | POST 合法配置后 `persisted=false→true`；重进档后 sidecar 配置恢复（enabled=false/rarity1/enhance1/socket1） |
| 存档回滚 | **PASS** | 测试前导出 slot0 `0b7136443c74`、slot2 `39d1dd5b86da`；测试后导入 slot0，物品与配置恢复 |
| 设备恢复 | **PASS** | autosell feature 恢复关闭、EB feature 开启、screen main_menu |

## §4 G 基础设施观察

- `core.save/slot_data_provider.cpp:473-490` 持 `g.mutex` 调用参与者 `complete`；消费者在回调内重入 provider API 会自锁。本批已在 autosell 侧规避，provider 锁纪律需另行处理。
- `infra/scheduler` 的 `remove` 不等待已开始 callback；插件不能把 remove 当 quiescence。本批用取消纪元处理。
- `PanelProviderSpec.block_reason` 不参与 `ui_panels_open` 拒绝路径；provider 无枚举接口，AS-13 的可见性无法静态断言。

## §5 设备操作摘要

1. 基线：health ready、monster v29、screen main_menu；autosell inactive、资源 free。
2. 构建/集成/安装：`scripts/build-debug.sh` → `scripts/patch-apk.sh` → `adb install -r output/Inotia4_v1.3.2_monster_v29_lspatched.apk`；identity sha 与 APK 一致。
3. 进档：关闭 agreement → `POST /api/system/enter_slot {"slot":0}` → screen world。
4. 功能开关：`POST /api/config/set {"plugin_id":"autosell","field":"feature","key":"autosell.auto_sell","value":true/false}`。
5. 配置/校验/出售：`POST /api/feature/autosell/config`；EB 开/关两态观察 `lastScanFrame`。
6. 回滚：`POST /api/system/backup/import {"checksum":"0b7136443c74","slot":0}`；恢复后重进档核对配置。

## §6 AS-19 入口按钮句柄失效（2026-10-21）

**根因证据**：tombstone 2026-10-21 00:24:31（monster v29），`#00 LINKEDLISTITEM_getData+0` → `#01-#05` 本模块帧；符号化 `pc 0x359074/0x3e3d18/0x3e52d8/0x3c5328/0x3c50b4` = `ui_adapter delete_control` ← `core.custom_ui destroy` ← `autosell_on_equip_draw`（旧 `panel.cpp:511`）← `autosell_equip_desc_draw_wrapper`。同秒 EB 日志 `extension tabs installed generation=7 root=...`：游戏销毁袋容器控件子树后，旧入口句柄已失效，`components->destroy` 命中已释放控件。

**重合证据**：失效句柄地址被 EB 页签复用后，旧实现对复用控件 `set_rect`/`icon_button_draw`，页签被移到入口坐标且触摸归页签（`.tmp/autosell-audit/gear-after-switch.png`）。

**修复**：`panel.cpp` 每帧 `autosell_entry_is_live()`：先容器子链表纯指针比对，命中后校验安装时写入的 user type 标记 `0x51` + 控件类型 + data 指针；失效/不符经 `ui_components.forget_native`（契约 1.19 尾追加：只注销组件记录、不删 native）丢弃记录并重建，绝不 delete。

**地址复用实证**（终版真机日志）：4 次切页均为 `entry handle stale ... in_tree=1 -> reinstall`——失效句柄仍是容器子控件，说明其地址已被扩展背包页签复用；旧实现会把该页签当入口 `set_rect`，新实现由 user type/data 元组拒绝。

**真机验证（2026-10-21，monster v29，模块 `c913d2108270`，集成包 `700b3eff…`）**：

| 项 | 结果 |
|---|---|
| 切页（技能↔背包）3 轮 + 5 轮 | **PASS**：每次重建日志 `entry handle stale ... in_tree=0 -> reinstall`，齿轮保持底部原位，EB 5 页签列完整 |
| 关闭/重开背包页 3 轮 | **PASS**：容器指针不变（`0x793bceccac`），句柄仍失效并被检出重建 |
| 进入 EB bag6 视图并返回 | **PASS**：布局与页签高亮正常，入口不移动 |
| 入口点击 | **PASS**：`provider open panel state=20`，面板打开/返回关闭正常（user type 标记未影响触摸） |
| 崩溃 | **PASS**：进程 pid 18752 全程存活，logcat FATAL/SIGSEGV/SIGABRT = 0，`/api/health` ready |
| 重建频率 | **PASS**：每次页面重建恰好一条 stale 日志，无逐帧抖动 |
| 组件记录 | **PASS**：stale 路径经 `forget_native` 注销；`core_ui_components_tests` 覆盖不删 native、注销后 destroy/set_rect 返回 NotFound（211 断言） |
| Host | **PASS**：148/148；`check_arch`、`check_http_routes`、`git diff --check` 通过 |

## §7 AS-19 后续：锚定按钮机制（2026-10-21）

**实现**：`ui_components` minor 20 尾追加 `anchored_button_ensure/set_visible/release/draw`；Host adapter minor 25 尾追加 `click_binding_is_current`；autosell 入口只保留点击回调与布局回调（`panel.cpp:385-457`），句柄存活/身份校验/失效重建由框架统一处理。`unbind` 跳过 `native_externally_owned` 组件，不删除游戏拥有的 native。

**身份判据**：`click_binding_is_current` = 主线程 + 句柄与 data 内存可读 + 绑定表 (callback, owner) 匹配 + `ExecuteProc` 仍是 adapter trampoline。命中返回 live=1；内存不可读返回 NotReady（不得据此删除）；其余 live=0（允许注销记录并重建，不允许 delete）。

**Host 覆盖**：`core_ui_components_tests` 230 断言（锚定 ensure 创建一次、子树重建后只注销+重建且 `delete_control` 计数不变、root 消失时清状态、release 不删 native）；`ui_adapter_tests` 覆盖 `click_binding_is_current` 的命中/owner 不符/回调不符/未绑定/内存不可读；Host 148/148。

**真机（monster v29，模块 `9402c676cd6d`，集成包 `6c0aa81d…`）**：

| 项 | 结果 |
|---|---|
| 首次打开背包页 | **PASS**：齿轮在底部原位，EB 5 页签列完整 |
| 切页（技能↔背包）4 轮 + 3 轮 | **PASS**：`entry installed via anchored` 总数 = 1 + 重建次数（5→10），每次重建恰好一条，无逐帧抖动 |
| 关闭/重开背包页 2 轮 | **PASS**：每次重建一条，位置不变 |
| 进入 EB bag6 并返回 | **PASS**：入口不移动，页签高亮正常 |
| 入口点击/面板关闭 | **PASS**：`panel_in_app` → 返回 `panel_inventory` |
| 崩溃 | **PASS**：进程 pid 21512 全程存活，logcat FATAL/SIGSEGV = 0 |

## §8 全部游戏树挂载迁移（2026-10-21）

**范围**：`ui_components` 1.21 追加 `anchored_control_ensure/set_visible/release/detach`；Adapter 1.26 追加 `control_proc_matches`（读 `CO_CONTROL_PROC=+0x98`，即 `set_control_proc` 写入值）。`extension_bag` 装备页/商店/合成器三处页签全部改为锚定（optional 依赖 `ui_components`），不再手写 root/generation 存活判断；autosell 入口沿用 `anchored_button_*`。主菜单替换保留既有 `main_menu_text_button_is_live` 等价证明（见 `docs/reference/plugins/core.custom_ui.md` §控件挂载归属与生命周期规则）。

**Host**：148/148（含新增 `anchored_control` 用例：同 proc/data 判活、地址被复用成「别的 proc」时注销重建且 `delete_control` 计数不变、`detach` 只在存活时交出句柄）。构建在 HEAD + 本批改动的独立 worktree 完成（主工作区当时有另一批未完成改动）。

**真机（monster v29，模块 `449962e14d72`，集成包 `d4ffd8a6…`）**：

| 项 | 结果 |
|---|---|
| 首次打开背包页 | **PASS**：EB 5 页签列与齿轮入口同时存在；`complete=1 created=1` |
| 切页（技能↔背包）4 轮 | **PASS**：`re-anchored` 25 条 = 5 页签 × 5 次挂载，无逐帧抖动；无 `anchor ensure failed` |
| EB 页签点击/返回 | **PASS**：`extension tab selected bag=0`，进入逻辑袋视图并切回原版袋 |
| autosell 齿轮点击/关闭 | **PASS**：`panel_in_app` → 返回 `panel_inventory` |
| 崩溃 | **PASS**：pid 25478 全程存活，FATAL/SIGSEGV = 0 |
| 商店/合成器页签 | **NOT_ACCEPTED（真机）**：未进入商店/合成器面板，仅有静态/编译覆盖；行为待补验 |
