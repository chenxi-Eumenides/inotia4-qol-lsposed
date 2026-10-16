# `world_teleport` 插件审阅登记（2026-10-17）

- 审阅对象：功能插件 `world_teleport`（`module/app/src/main/cpp/plugins/world_teleport/`，6 文件）。
- 基线参考说明（阶段 0.75，本批新建）：`docs/reference/plugins/world_teleport.md`，已加入 `docs/INDEX.md`。
- 修复提交：无（本批未改 `world_teleport` 代码；P2 findings 待裁决）。
- 现场基线（阶段 0）：设备 `192.168.3.56:5555`，`/api/health` = `ready`、variant `monster v29`。

## §1 本批登记问题

Findings（阶段 1 @oracle 只读静态审计，A–G 七维）：

| 编号 | 严重度 | 文件:行 | 问题 | 处置 |
|---|---|---|---|---|
| WT-01 | P2 | `plugins/world_teleport/feature.cpp:194-265`；`contracts/game_host.h:150-157,192-217` | 插件直接调用聚合的 `QolGameHost::world_action`（确认框/扣钱/切图/弹窗）；与 `ARCHITECTURE.md` §4.3 排除跨域聚合的边界不一致，公开面过宽 | 开放（需裁定是否作为迁移期低层接口保留；新增能力不得再扩此表） |
| WT-02 | P2 | `plugins/world_teleport/plugin.cpp:72-75` | install 只校验 `g_game != nullptr`，未校验 `g_game->abi` 的 size/major/minor；ABI 不匹配时可能按错误布局调用 | 开放（install 期 fail-closed ABI 校验 + 错配 Host 用例） |
| WT-03 | P2 | `plugins/world_teleport/feature.cpp:245-254` | 扣费成功后忽略 `request_map_change` 的 `QolResult`（适配器在地图切换符号未就绪时返回 `not_ready`）；`create_yesno` 返回值亦被忽略却仍记录「confirmation opened」 | 开放（动作结果检查；扣费后切图失败的补偿/错误语义需裁决） |
| WT-04 | P2 | `plugins/world_teleport/feature.cpp:97-103,226-255,258-290,295-319`；`plugin.cpp:131-134` | `schedule` 取得任务 ID 后不保存；`on_disabled` 只清 `g_installed`，业务回调不检查该状态 → 停用后已排队任务仍可能打开 UI/扣费/切图 | 开放（保存并移除任务 ID 或用操作代次失效；补「确认后立即停用」用例） |
| WT-05 | P2 | `plugins/world_teleport/rules.cpp:12-17`；`contracts/world_facts.h:18` | `target_map_id` 对 `int` 直接 `current_id + delta`，未限制输入范围，极端/异常 ID 有符号溢出风险（正常状态未证实可达） | 开放（宽整数计算 + 范围 fail-closed + 极值用例） |
| WT-06 | P2 | `plugins/world_teleport/plugin.cpp:59-62,72-81`；`feature.cpp:179-183` | `QOL_PORT_CHOICE` 是必需端口，但唯一用途只是读 `state_id` 写日志且忽略结果；开关选择框实际用可选 `ui_choice` 能力 | 开放（需裁定是否保留该诊断依赖；建议移除或降为不阻断 install） |
| WT-07 | P2 | `tests/test_world_teleport_status.cpp:6-19`；`tests/test_world_teleport_install.cpp:36-65`；`tests/CMakeLists.txt:1103-1120` | 状态 target 只测未被真实插件调用的 `InstallDisposition` helper；install target 未验证成功后 `on_enabled/fill_status`；规则测试未覆盖负 count、无效 target、余额不足、切图失败；host target 未编译 `feature.cpp` | 开放（改为驱动真实 descriptor 回调 + 补业务失败用例） |

基础设施观察（G，非本插件缺陷，已登记）：patch 双版本指纹与尾部回跳元数据符合预期；scheduler 不绑定 owner、不自动清理插件任务（消费者自负）；`game/access/world.cpp:150-156` 的 `world_teleport_patch_info` 在当前检索范围无调用点，与 `patches.cpp` 登记表存在漂移风险。

## §2 无法验证项

本批无经用户确认后永久不再主动验收的 UNVER 项。WT-03/WT-04 的部分时序需真机专项，属待验而非 UNVER。

## §3 已完成验收登记

| 验收项 | 结果 | 证据 |
|---|---|---|
| 真机装配 | **PASS**：`world_teleport` `enabled`、`degraded=false`、`patch:UIPlay_CallMapName` `live`；模块 sha256 `10b218e703f9…`（`0cabd5fc` 构建） | `GET /api/system/features`（部署批，2026-10-17） |
| 行为（地图传送选择框） | **PASS（用户真机确认，2026-10-17）**：世界传送选择框功能正常 | 用户确认；回归 VM-45 |
| Host 定向 CTest | 本批未单独执行（未改 `world_teleport` 代码）；Host 全量已于 `0cabd5fc` 验证批跑通 143/143 | 同 push_all 登记 §3 |
