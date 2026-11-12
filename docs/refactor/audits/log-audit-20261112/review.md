# 功能级日志覆盖审计：裁定与补全任务单

依据：各组件 `coverage-*.md` 与 `docs/reference/logging-standards.md`。本文件只裁定"补什么、由谁记、记成什么样"，不重复矩阵内容。

## 1. 全局裁定

| 编号 | 裁定 | 说明 |
|---|---|---|
| G1 | 生命周期唯一所有者 | 插件启用/停用/降级/恢复只由 `host/plugin_registry.cpp` 记一次；插件内不得再记启停。驳回 eb-core #6、eb-outer #4、world-teleport #1、custom-recipe 停用项、save-backup #6。 |
| G2 | 资源安装唯一所有者 | hook/patch/资源组安装与恢复由 host 装配层记结果；插件只记"本功能因此不可用"的一次 W 或降级 D。驳回 core.item #3 的逐 hook 记账，改为整体不可用 W。 |
| G3 | 本轮不动 Kotlin | public export、异步导出、设置 receipt 的 Kotlin 侧完成回执延后；native 只记入队拒绝与 enqueue 结果，禁止宣称"文件已导出"。 |
| G4 | 面板生命周期按面板归属 | 每个面板只由其 UI 所有者记一次 open/close；同一面板不得在 core 与 ui 两层双记。 |
| G5 | 快照类不重复 | core.system/character/world/ui_state 已有 `op=snapshot_read result=failed keep=previous`；只补"恢复"边沿，不补逐字段。 |
| G6 | INFO 只记状态变化 | 允许：启用/停用、安装/恢复、打开/关闭、提交/拒绝、恢复/降级。禁止：查询、读取、轮询、逐项、逐帧。 |
| G7 | 热路径只用 D | 战斗/逐帧/逐对象诊断保持静默或门控 D；`damage_widen` 现有 INFO 轨迹改 D。 |
| G8 | 存疑先核实 | 功能不存在或未注册的，不为假设补日志。 |

## 2. 组件任务单

### host（coverage-host.md）
批准 #1-#5、#7；#6 仅 native 入队拒绝（G3）。
- 存档事务：最终不可恢复 E，gate 拒绝 W `op=save_transaction result=rejected slot= reason=`；成功只在会话级完成 I。
- `save_route`：`op=save_route owner= result=bound|unbound reason=`。
- feature 生命周期：`op=feature_transition plugin= state= result=ok|degraded reason=`，只此一处。
- 配置：`op=config_load result=fallback reason=` / `op=config_write result=failed keep=previous`。
- slot gate：`op=slot_operation result=rejected slot= reason=`。
- HTTP provider：仅 `op=http_provider state=degraded reason=`，请求级由端点负责。

### extension_bag core（coverage-extension-bag-core.md）
批准 #1、#2、#3、#4、#5、#7；驳回 #6（G1）。
- #1 SlotBound 写失败：重试 W 带 `attempt=i/n reason=`，最终失败 E。
- #2 bag_source mutation：仅 import/remove/replace 提交后一条 I，`op=bag_source action= result=committed count=`。
- #3 投影视图：`op=module_view result=installed|restored bag=`。
- #4 bagCount：`op=bag_count result=applied limit= source=`；receipt 由 outer 记。
- #5 swap：`apply op=bag_swap result=committed src_bag= src_slot= dst_bag= dst_slot=`。
- #7 slot_data：staged/committed/cancelled/degraded 由现有事务结果承载；只补缺的终态，不新增阶段噪音。

### extension_bag ui（coverage-extension-bag-ui.md）
批准 #1-#7，受 G4 约束。
- store_buy：唯一交易结果点 `op=store_buy result=committed|rejected reason=`。
- 装备页/商店页：`open|close panel=equip|store result=entered|closed`，仅状态变化。
- store_detail：打开/失败各一条。
- 拖拽 `handled=0`：I→W，补 source/target/reason。
- item_use：事务提交一条 I `op=item_use result=consumed`。
- 混装 adapter/root 缺失：I→W + reason。
- 卸下与确认取消：`op=bag_unequip result=`、`close panel=confirmation result=cancelled`。

### extension_bag outer（coverage-extension-bag-outer.md）
批准 #2、#3、#5、#6；驳回 #1 安装成功（G1）、#4 启停（G1）、#7 袋源变更（归 core #2）。
- pending 恢复：`op=pending_recovery result=recovered|isolated reason=`。
- slot_data commit：`op=slot_data result=committed|cancelled`；失败与回滚留给 core #7，避免双记。
- 配置 receipt：`op=config_receipt section= key= result=applied|failed reason=`。
- projection 回调注册：`op=projection_callback result=registered|failed reason=`。

### core.item + core.inventory（coverage-core-item-inventory.md）
批准 #1、#2、#4、#5、#6；#3 改为整体不可用 W（G2）。
- 使用/丢弃最终失败：`reject op=item_use|item_drop bag= slot= item= reason=`。
- 容量拒绝：`reject op=pickup reason=inventory_full count= capacity=`。
- 扩容：`op=bag_limit result=applied limit= source=`。
- 传输转换：最终边界 W `op=transfer from= to= item= reason=`；回滚失败 E。
- 装备提交：`apply op=equip result=committed slot=` / `apply op=unequip ...`。

### core.save（coverage-core-save.md）
批准 #1-#5；#6 维持边界不补。
- 重试：`retry op=save attempt=i/3 slot= tx= reason=`；耗尽 `fail op=save attempts=3 reason=retries_exhausted`。
- 读档：按 token 唯一终态 `op=load result=ready|failed|cancelled reason=`。
- journal 恢复：`restore op=recover slot= tx= result=committed`。
- 降级：首次 `degrade op=save slot= tx= reason=`；后续拒绝 W 不重复宣告。

### save_backup + autosell（coverage-save-backup-autosell.md）
批准 #1、#2、#4、#5、#7；#3 仅 native 侧（G3）；驳回 #6（G1）。
- 完整性校验：checksum mismatch 边界 E `op=verify reason=checksum_mismatch`。
- 自动备份：成功后一条 I `op=backup result=created source=autosave`。
- slot-data 重试：`attempt=i/n reason=`，耗尽 E。
- 扫描注册失败：W `op=scan_scheduler result=failed reason=`。
- 轮转：核实后若有清理，一条删除汇总 I；无则不补。

### custom_recipe（coverage-custom-recipe.md）
批准：配方表恢复成功 I、升阶成功 I、规则命中进入确认 I、放料拒绝细节 D。
驳回停用项（G1）；未实现的 CRUD/导入导出/持久化不补（G8）。

### world_teleport（coverage-world-teleport.md）
驳回 #1（G1）；#2 地图缓存终态归 `core.data` 所有者，本插件不记；#3 先核实解锁判定归属，属于上游则不补。

### small plugins（coverage-small-plugins.md）
批准 #1-#7。
- world_tuning：保存并 apply 后一条 I；receipt 失败唯一 W；面板打开失败 W。
- character_panel：快照整体失败 W（不逐字段）；面板开关 I。
- craft_lottery：一次处理终态 I；fallback 收敛点一条 W/I。
- core.craft：install 失败 W。
- attribute_range_display：可选 hook 缺失 D；整体不提供时 W。

### core.custom_ui + settings（coverage-custom-ui-settings.md）
批准 #1-#7。
- 设置请求失败 W：`feature= section= reason=`。
- 设置页关闭/分类切换 I：仅状态变化。
- 特性联动：按 owner 一条，不逐 feature。
- provider 打开失败 W、action 终态 I/W、重装恢复 I/W、确认框 open/confirm/cancel 一条。

### 未审插件（coverage-uncovered-plugins.md）
批准 #1、#2、#3、#6、#7；#4/#5 先核实 custom_map 是否已注册。
- 资源组失败：`op=install resource= reason=`；成功不在插件 wrapper 重复。
- attribute_quality：服务不可用边界 W，正常查询不记。
- damage_widen：热路径 I→D，门控前不求值。
- module_health：scheduler 最终失败一次 W。
- push_all/better_move：启停/模式/步长变化 I；拒绝 W；首个 hook 命中仅一次 D。

### core 基础设施插件（coverage-core-infra-plugins.md）
批准 #1-#8，受 G5、G1 约束。
- core.ui_state：快照失效/恢复边沿；发布拒绝 W。
- core.data：导入失败/整批完成/目录不可用；可恢复 W，整体不可用 E。
- core.api：服务启停与 lifecycle 命令拒绝；请求细节不重复。
- core.feature：开关状态变化与资源绑定失败；热路径只 D。
- core.quest：快照失败/恢复与命令结果；补 disable/deactivate。
- core.world/core.character/core.system：结构化既有日志，补恢复与写结果，不新增逐帧。

## 3. 写作约束（全部组件）

1. 正文 `verb key=value`，不重复头部的级别/域/src。
2. 失败必须带 `reason=`；有重试必须带 `attempt=i/n`；最终失败才 E。
3. INFO 只在状态变化或提交结果处出现，一次事件一行。
4. DEBUG 必须在宏内门控，未开启时不得构造参数或读取额外状态。
5. 已有日志能表达的事实不新增；同一事实只在一个所有者处记录。
6. 不新建日志宏或日志框架；使用组件现有宏。

## 4. 验证结果

- Host 全量 `ctest`：**164/164 通过**（`module/app/src/main/cpp/tests`）。
- `check_log_policy.py`：R1–R4 = 0，R5 = 29（旧日志缺 `key=value` 的软告警），PASS。
- `git diff --check`：通过。
- `scripts/build-debug.sh`：成功，APK `output/inotia4_qol_lsposed_debug_2611121709_82f6e5c62c9b.apk`，SHA-256 `82f6e5c62c9b1cbfabddde24587583e3ae7803019d7e6e99bf53c771b382438c`。
- 测试契约同步：`test_feature_reconciler.cpp` 断言改为 `fail op=feature_transition ... state=failed`；`test_world_facts_dispatch.cpp` 快照失败告警由 2 次改为 1 次（边沿记录）。

## 5. 遗留与边界

- Kotlin 侧完成回执（public export、异步导出、设置 receipt）按 G3 延后。
- `custom_map` 未在 `host/assembly.cpp:262-264` 注册；注册后需补加载失败 `fallback=original` W 与转换结果等级规范。
- `store_buy` 在扩展袋侧无可观测提交终态；原版完成点不归本模块，未补。
- `world_teleport` 无场景/章节解锁过滤；不为不存在的判定补日志。
- `custom_recipe` 的 CRUD/导入导出/持久化未实现，不补日志。
