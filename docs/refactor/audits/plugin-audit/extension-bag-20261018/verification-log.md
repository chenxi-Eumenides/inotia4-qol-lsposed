# `extension_bag` 插件审阅登记（2026-10-18）

- 审阅对象：功能插件 `extension_bag`（`module/app/src/main/cpp/plugins/extension_bag/`，约 1.64 万行；含测试 1.78 万行）。
- 参考说明（阶段 0.75）：`docs/reference/plugins/extension_bag.md`（本批新建）。
- 静态审计：阶段 1 @oracle 只读，A–G 七维，按 4 个文件域 + 规则映射拆分；去重后 39 条 findings（P1 8、P2 25、P3/观察 6）。
- 本批提交：
  - `0df29b1b` 延迟释放越界与 custody 释放期重绑
  - `59ebda74` ext→orig 回滚与 pending 恢复
  - `5eb16096` 装备失败阻断原版回退并校验袋表写入
  - `e3243d83` 读档保留 canonical 数量并修正持久化/保管失败语义
  - `8e006db0` 收口运行时锁、页签 generation 与三宿主 teardown
  - `22901794` 装配失败回滚与 HTTP 参数/票据语义
  - `adc614fb` 商店买入容量/合并预检与 Store/Mix 清理
  - `6dcd9cb6` 合并数量模式快照与移除身份复核
  - `b208c15a` 明确 write port 双通道与截断 body 契约
  - `ccf17c41` Host SDK V2 详情按钮身份接口
  - `5aae1cdd` EB 详情按钮 hook 迁到 V2
- 证据底稿：`.tmp/extension-bag-audit/{baseline.md,findings.md,summary.md,0.5-A.md,d03-host-sdk-design.md}`（本地工作区，不入提交）。

## §1 本批登记问题

### P1

| 编号 | 文件:行 | 问题 | 处置 |
|---|---|---|---|
| A-01 | `plugin.cpp:799-816`；`extension_bag_public_runtime.inc:415-435`；`host/features/reconciler.cpp:125-149` | ITM-15 设置页开关无效；静态 Host feature 路径已连通，设置页实际写入路径不在插件仓库，无法静态归因 | **未修**：后置设备批取证（feature receipt/config、actual、resource、设置页写入入口） |
| C1-01 | `extension_bag_transaction.inc:238-300,445-470` | 宿主容量读取/写回失败被忽略，事务继续；失败出口可能写 0 | 本批修复：读取+写回成功才继续，guard 仅在成功读取后启用，出口检查恢复 |
| C1-02 | `extension_bag_transaction.inc:191-225,400-470` | ext→orig 回滚只按 handle 重找；源释放失败会留下“目标已入原版、扩展源保留” | 本批修复：回滚复用已复核槽位并复核 handle/对象身份 |
| C1-03 | `model/virtual_bag_transaction_rules.inc:435-457`；`extension_bag_runtime.inc:761-885` | pending 恢复只清票据，不核验/移除已入库的原版目标 | 本批修复：新增 `ext2orig_recovery_action` 四态；回滚目标或完成提交 |
| C2-01 / D-02 | `game_ui_virtbag.cpp:2000-2145`；`native_inventory_hook.cpp:1235-1292` | 已识别扩展源的装备失败/无空扩展袋仍返回 `NotExtension`，原版 backup 可能继续处理 | 本批修复：已识别扩展对象失败统一 `Blocked` |
| C2-03 | `game_ui_virtbag.cpp:2061-2090` | `set_original_bag_item` 结果未检查就清扩展源 | 本批修复：写入成功后才清源/移交；失败保持源并 `Blocked` |
| D-01 | `extension_bag_ownership.inc:110-160`；`extension_bag_tabs.inc:14-44` | 延迟释放排空把 `queue+custody` 当队列长度，custody 非空时重复读、越界写，可能双重释放 | 本批修复：队列 16 / custody 128 分段遍历；失败保留条目；新增 `extension_bag_deferred_free_tests` |
| D-03 | `extension_bag_equip.inc:393-450,771-862,1139-1160` | 详情按钮 hook 按固定 index 绑定，不核验按钮/data/execute-slot 身份，违反 R-64 | 本批修复：Host SDK v1.27 `QolDescHookApi` V2（resolve/bind/validate/original/unbind，按 execute slot 去重）；EB 改按绑定 id 校验，身份不符直接 stale 返回；真机 R-64 PASS |

### P2

| 编号 | 文件:行 | 问题 | 处置 |
|---|---|---|---|
| A-02 / C3-05 | `plugin.cpp:485-540,700-820` | install 后半段失败不回滚；启用时 slot_data 重注册失败仍置 active/enabled | 本批修复：可撤销登记回滚；provider/participant set-once 常驻但 gate fail-closed |
| A-03 | `plugin.cpp:280-470`；`plugin.h:19-250` | `equip`/`item` 缺字段默认 -1；`strtol` 不检查 errno/范围/尾字符 | 本批修复：顶层 JSON 严格 int32；必填字段 400 |
| A-04 | `plugin.cpp:370-385` | ticket 用子串 `"ok":false` 判失败 | 本批修复：顶层 `ok` 布尔；解析失败按 failed |
| A-08 | `docs/reference/api-reference.md:946,2180-2245` | 容量 16/8/4/0/0 过时；status 缺 `save_panel_hook`；`unequip` 缺 bag | 本批修复文档 |
| B-01 | `extension_bag_persistence.cpp:92-183` | 注释称“未知版本尽力解析/回退空状态”，实际阻断 | 本批修复注释；v4-only fail-closed；版本矩阵测试 |
| B-02 | `model/virtual_bag_state_json.inc:273-306` | 读档把 canonical 截到运行时上限，违反 R-48 | 本批修复：保留合法 canonical；超 int/编码非法 fail-closed |
| B-03 | `extension_bag_ownership.inc:12-30,110-160` | 16+128 容量契约不清；满载失败语义不明 | 本批明确固定上限与 fail-closed；不扩数组 |
| B-04 | `extension_bag_ownership.inc:120-150,200-230,470-500` | `extension_free_item` 返回值被忽略，失败仍移除队列项 | 本批修复：失败保留并记录 |
| B-05 | `model/item_handle_custody.h:28-39,69-75` | `kReleasing` 条目被 `bind/rebind` 清零 | 本批修复：拒绝并补测试 |
| B-06 | `extension_bag_claims.inc:74-200` | Popup restore 失败后清本地镜像 | 本批修复：副本 restore，失败保留镜像可重试 |
| C1-04 | `extension_bag_runtime.inc:761-885` | 恢复后投影未重装 | 本批修复：退出前按原宿主/袋重装；失败切回原版模式 |
| C1-05 | `extension_bag_transaction.inc:864-960` | 合并数量视图与上限模式不同源 | 本批修复：一次模式快照；数量视图/上限同源 |
| C1-06 | `game_ui_virtbag.cpp:646-710` | 解锁调用原版后只复读源槽为空，未复核身份 | 本批修复：保存/复核 handle、generation、对象 |
| C1-07 | `extension_bag_transaction.inc:995-1030` | borrow 失败仍释放源并提交 | 本批修复：失败回滚逻辑事务 |
| C2-02 | `game_ui_virtbag.cpp:1704-1725`；`extension_bag_equip.inc:151-260` | 持锁调用原版装备交换；物理槽恢复结果被忽略 | 本批修复：锁内装临时值→解锁调用→重锁复核/恢复 |
| C2-04 | `extension_bag_store.inc:724-820` | 商店投影容量下查原版空位；槽 0 被当无槽 | 本批修复：临时恢复真实容量；用成功状态判定 |
| C2-05 | `extension_bag_store.inc:724-820` | 无空槽但可合并堆时买入 gate 先拒绝 | 本批修复：复用 `place_plan` 合并/新建判据 |
| C2-06 | `extension_bag_store.inc:256-310`；`extension_bag_mix.inc:272-320` | Store/Mix 持锁调用原版 UI 刷新 | 本批修复：锁外刷新，锁内复核 generation/root |
| C2-07 | `extension_bag_equip.inc:943-990` | 佣兵徽章持锁弹窗 | 本批修复：离锁后弹窗 |
| C2-08 / C3-03 | `extension_bag_store.inc:86-200`；`extension_bag_mix.inc:131-200`；`extension_bag_runtime.inc:130-256` | 控件创建失败遗留半成品；部分安装被当完成 | 本批修复：失败清理；全部成功才提交 root/generation |
| C3-01 | `extension_bag_runtime.inc:13-108` | 页签事件持锁调用 observation post 再锁同 mutex | 本批修复：锁内记录、锁外 finish |
| C3-02 / D-05 | `extension_bag_runtime.inc:130-256,628-690`；`extension_bag_public_runtime.inc:415-435` | root 重建不递增 generation；禁用清 pending 延迟；提交不复核 live root | 本批修复：重建/禁用立即失效；提交复核 enabled/root/控件 |
| C3-04 | `extension_bag_runtime.inc:269-285`；`extension_bag_lifecycle.inc:497-520` | 禁用/主菜单未统一三宿主 teardown | 本批修复：统一 store/mix/equip teardown |
| C3-06 | `extension_bag_runtime.inc:516-628` | pending tab 提交持锁调用 MakeDesc/SetBag/SetDesc | 本批修复：锁外 UI 回调，返回后复核 generation/root/袋状态 |
| C3-07 | `extension_bag_runtime.inc:1194-1200` | `persist_state_locked` 恒真语义不清 | 本批注释：表示内存变更已接受，不表示 durable staging |
| C3-08 | `extension_bag_lifecycle.inc:147-153,201-203,331-338` | 模块事件入口缺 session admission | 本批修复：未准入只走原版事件 |
| D-04 | `extension_bag_render.inc:450-610` | 投影安装未要求 root/child/data/绑定完整就置 installed | 本批修复：全部成功才提交；失败回滚；刷新核对 live root |
| D-05 | 同 C3-02 | 禁用/重启用窗口旧 pending 可提交 | 本批修复 |

### P3 / 观察

| 编号 | 处置 |
|---|---|
| A-05 | 本批明确 write port 双通道契约；`test_host_adapters` 覆盖 0/非 0/未就绪/参数错误 |
| A-06 | descriptor `jni_count=0` 注释为全局 bridge 有意归属；不改冻结签名 |
| A-07 | 删除仅 include 的 `gamebridge_extension_bag.cpp` 并移出 CMake |
| B-07 | `extensionbags.journal` v1 model-only；未接入生产，保持 ext→ext pending |
| C1-08 | 成功 body 截断语义写入契约：成功 body 可忽略；`out_len` 报完整长度；不得以缓冲不足改报失败 |

## §2 无法验证项

- **A-01 ITM-15**：设备复验 PASS（2026-10-18）：模块设置页 `扩展背包` 开关 off→inactive/0 live，on→enabled/20 live；当前构建不再复现。证据见 §3。
- **FIX-07**：正常跨袋移动已在设备复验 PASS（bag6 slot0→bag0 slot9，源清空、count13、保存重进保持）；“目标已入原版、扩展源保留”的失败注入仍未复现，需后续受控复现后关闭。
- **C1-01**：容量读写失败注入、pending 两侧同时存在、投影重装失败未在 host/设备复现。
- **D-01**：真实触摸窗口内连续退役、延迟释放饱和未做压力复现；模型/纯逻辑测试通过，常规面板开关无崩溃。
- **D-03**：已实现并真机 PASS；真实 UI 控件重建/地址复用 ABA 仍按设计保留为残余风险。
- **P2 UI 项**：Store/Mix 控件创建失败、UI 刷新重入、root 重建、MakeDesc 回调重入仅模型/源码守卫，未真机注入。
- **EB-01**：用户确认已修复，本批未复核；相关真机卡是否恢复由用户在设备批决定。

## §3 已完成验收登记

| 验收项 | 结果 | 证据 |
|---|---|---|
| Host 全量 CTest | **PASS 146/146** | `cmake --build build -j`；`ctest --test-dir module/app/src/main/cpp/tests/build --output-on-failure` |
| 流程校验 | **PASS** | `git diff --check` |
| 架构校验 | **PASS**：layers/offsets/jni/resources 全绿 | `uv run python scripts/maintenance/check_arch.py` |
| 路由漂移 | **PASS**：native 147 / baseline 147 / duplicates 0 / missing 0 / extra 0 | `uv run python scripts/maintenance/check_http_routes.py` |
| Android Debug 构建 | **PASS**：`BUILD SUCCESSFUL` | `scripts/build-debug.sh` |
| APK（P1/P2 中间构建） | `output/inotia4_qol_lsposed_debug_2610181337_4a8e48f41472.apk`，SHA-256 `4a8e48f414729b118a47af396a7f8b7d89857e1b46e986360661f9c14e79d8af`；最终 D-03 构建见下方 | 构建输出 |
| 契约/参数测试 | **PASS**：`extension_bag_contract_tests`（版本/布局/JSON int32/票据 `ok`） | host ctest |
| 持久化测试 | **PASS**：`extension_bag_slot_data_tests`（load 矩阵、canonical 保留、capture/complete、staging） | host ctest |
| 出售/事务测试 | **PASS**：`extension_bag_sale_transaction_tests`（sale removal、store sell plan、ext2orig 恢复判定） | host ctest |
| 运行时/UI 测试 | **PASS**：`extension_bag_draw_lock_tests`（锁重入、stale pending、部分创建、投影安装模型） | host ctest |
| 延迟释放测试 | **PASS**：`extension_bag_deferred_free_tests`（队列/custody 分段、失败保留、容量边界） | host ctest |
| move-merge shim | **PASS**：`extension_bag_move_merge_shim_tests` | host ctest |
| status query | **PASS**：`extension_bag_status_query_tests` | host ctest |

| 设备部署 | **PASS**：集成包 `output/Inotia4_v1.3.2_monster_v29_lspatched.apk`，SHA-256 `48983632aff867dc5c5bc202b6ced1d9de675d367b0551da094b651a697fa588`；install -r 成功；health ready、variant monster v29 | `evidence/device-health.json` |
| A-01 / ITM-15 设置页开关 | **PASS**：模块设置→更多优化功能→扩展背包；off → `inactive`/0 live，on → `enabled`/20 live；不再复现 | `evidence/eb-toggle-off-final.json`、`eb-toggle-on-final.json`、`eb-settings-on.png`/`off.png` |
| 扩展视图进入 | **PASS**：打开 inventory 面板后 `enter_view bag=6` → ticket done，`mode=module`、`selected=0`、`extension_tab_button=true`；无崩溃 | `evidence/fix07-eb-status.json` |
| FIX-07 正常跨袋移动 | **PASS**：`bag6 slot0 count13 → bag0 slot9`，ticket 9 done；源槽清空、目标 count13；保存/重进仍为 count13 | `evidence/fix07-move-ticket.json`、`fix07-bag0-slot9.json` |
| 生命周期 deactivate/activate | **PASS**：deactivate → `inactive`/0 live；activate → `enabled`/20 live、`degraded=false`；无 registration failure | 设备 API 记录（§3 文本） |
| 日志 | **PASS**：2015 行 logcat 无 FATAL/SIGSEGV/tombstone、无 EB ERROR | `evidence/device-logcat.txt` |

| Host SDK V2 | **PASS**：minor 27；`desc_hook_v2` offset 848；size 856；`host_adapter_tests` 覆盖 resolve/bind/validate/同槽冲突/解绑恢复 | `test_host_adapters`、`test_settings_catalog_abi`、`test_lifecycle_participants` |
| D-03 R-64 真机 | **PASS**：详情打开安装 index=1/7；index=8 因同 `execute_slot` 被去重；点击出售 `index=1` 正确路由 `bag=0 slot=2`；无 stale/FATAL/SIGSEGV | `evidence/d03-r64-summary.txt`、`d03-r64-logcat.txt`、`d03-detail-buttons.png`、`d03-sell-confirmation.png` |
| D-03 构建 | **PASS**：Debug APK `output/inotia4_qol_lsposed_debug_2610181444_b3370ebf8d0c.apk`，SHA-256 `b3370ebf8d0c…`；集成包 monster `d7bb70fc…` | `evidence/d03-apk-sha256.txt` |

## §4 用户裁决记录

1. 商店卖出可见数量 `<=0`：**拒绝出售**，不按 1 件结算。
2. ITM-15 取证：后置到设备批；**2026-10-18 设备复验 PASS，不再复现**。
3. FIX-07：**重新真机复核后关闭**，不沿用既有证据直接关闭。
4. B-01：**保持非 v4 拒绝**，不实现 v1–v3 迁移。
5. B-02：**保留有效 canonical，非法值阻断**，不静默截断。
6. A-05：**保留 `QolResult` / `out_result` 双通道**并写清契约。
7. A-06：**JNI 保持全局 bridge**，descriptor 0 为有意；冻结签名不动。
8. EB-01：**用户确认已修复**，移出本次审阅范围。
9. D-03：**采用方案 A**，实现 Host SDK V2 并迁移 EB 详情 hook。

## §5 未完成 / 后续批次

- 设备批剩余：FIX-07 失败注入、C1-01 容量失败注入、D-01 触摸压力、P2 UI 注入项。
- D-03 已完成：Host SDK v1.27 + EB V2 迁移；证据见 §3。
- 结构整理：审阅后按方案 A 分批拆 TU；先 move-merge shim，再运行时/UI；不与本批行为修复混合。
