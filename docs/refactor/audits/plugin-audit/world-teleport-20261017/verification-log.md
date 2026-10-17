# `world_teleport` 插件审阅登记（2026-10-17）

- 审阅对象：功能插件 `world_teleport`（`module/app/src/main/cpp/plugins/world_teleport/`，6 文件）。
- 基线参考说明（阶段 0.75，本批新建）：`docs/reference/plugins/world_teleport.md`，已加入 `docs/INDEX.md`。
- 修复提交：`a26a97a7`（WT-01/02/03/04/05/07）与 `2140e9d3`（WT-06）。
- 现场基线（阶段 0）：设备 `192.168.3.56:5555`，`/api/health` = `ready`、variant `monster v29`。

## §1 本批登记问题

Findings（阶段 1 @oracle 只读静态审计，A–G 七维）：

| 编号 | 严重度 | 文件:行 | 问题 | 处置 |
|---|---|---|---|---|
| WT-01 | P2 | `plugins/world_teleport/feature.cpp:194-265`；`contracts/game_host.h:150-157,192-217` | 插件直接调用聚合的 `QolGameHost::world_action`（确认框/扣钱/切图/弹窗）；与 `ARCHITECTURE.md` §4.3 排除跨域聚合的边界不一致，公开面过宽 | 已修复（`a26a97a7`，选 B）：新增私有 `QOL_PORT_WORLD_ACTION`（契约 + provider + registry 私有约束），插件不再使用 `ctx.game`；`extension_bag` 的 `cancel_tutorial` 仍走聚合表（残留，另行裁决） |
| WT-02 | P2 | `plugins/world_teleport/plugin.cpp:72-75` | install 只校验 `g_game != nullptr`，未校验 `g_game->abi` 的 size/major/minor；ABI 不匹配时可能按错误布局调用 | 已被 WT-01 B 取代：插件不再使用 `ctx.game`，`g_game->abi` 校验随依赖一并删除；版本保障由端口 `abi_version` 承担 |
| WT-03 | P2 | `plugins/world_teleport/feature.cpp:245-254` | 扣费成功后忽略 `request_map_change` 的 `QolResult`（适配器在地图切换符号未就绪时返回 `not_ready`）；`create_yesno` 返回值亦被忽略却仍记录「confirmation opened」 | 已修复（`a26a97a7`，选 B）：切图失败经 `add_money(cost)` 退款并提示；退款失败单独提示；`create_yesno` 失败不再误记「confirmation opened」 |
| WT-04 | P2 | `plugins/world_teleport/feature.cpp:97-103,226-255,258-290,295-319`；`plugin.cpp:131-134` | `schedule` 取得任务 ID 后不保存；`on_disabled` 只清 `g_installed`，业务回调不检查该状态 → 停用后已排队任务仍可能打开 UI/扣费/切图 | 已修复（`a26a97a7`）：`invalidate_operations()` 停用代次使已排队帧任务与游戏回调失效；停用后不再开面板/扣费/切图 |
| WT-05 | P2 | `plugins/world_teleport/rules.cpp:12-17`；`contracts/world_facts.h:18` | `target_map_id` 对 `int` 直接 `current_id + delta`，未限制输入范围，极端/异常 ID 有符号溢出风险（正常状态未证实可达） | 已修复（`a26a97a7`）：`int64_t` 计算；越界 `current_id`/`upper` fail-closed 返回 -1；补极值/负值用例 |
| WT-06 | P2 | `plugins/world_teleport/plugin.cpp`；`feature.cpp` | `QOL_PORT_CHOICE` 是必需端口，但唯一用途只是读取 UI 状态写日志；诊断依赖会阻断 install，选择框实际使用可选 `ui_choice` 能力 | 已修复（`2140e9d3`）：`core.ui_state` 新增 `ui.state` v1.1 `read_layers`，程序化读取 UI 栈及每层 id/type；`world_teleport` 移除必需端口并改用该接口记录顶层诊断。 |
| WT-07 | P2 | `tests/test_world_teleport_status.cpp:6-19`；`tests/test_world_teleport_install.cpp:36-65`；`tests/CMakeLists.txt:1103-1120` | 状态 target 只测未被真实插件调用的 `InstallDisposition` helper；install target 未验证成功后 `on_enabled/fill_status`；规则测试未覆盖负 count、无效 target、余额不足、切图失败；host target 未编译 `feature.cpp` | 已修复（`a26a97a7`）：状态测试驱动真实 descriptor；补退款、停用失效、余额不足、无效目标用例；未接入的 `InstallDisposition` 已删除 |

基础设施观察（G，非本插件缺陷，已登记）：patch 双版本指纹与尾部回跳元数据符合预期；scheduler 不绑定 owner、不自动清理插件任务（消费者自负）；`game/access/world.cpp:150-156` 的 `world_teleport_patch_info` 在当前检索范围无调用点，与 `patches.cpp` 登记表存在漂移风险。

## §2 无法验证项

本批无经用户确认后永久不再主动验收的 UNVER 项。WT-03/WT-04 的退款与停用时序已由 host 用例覆盖，真机回归见 §3。

## §3 已完成验收登记

| 验收项 | 结果 | 证据 |
|---|---|---|
| 真机装配（WT-01/03/04 批） | **PASS**：`world_teleport` `enabled`、`degraded=false`、`patch:UIPlay_CallMapName` `live`；私有 `QOL_PORT_WORLD_ACTION` 注入成功（install 通过 `port_count=2`）；模块 sha256 `a044632ac450…`（`a26a97a7` 构建） | `GET /api/system/features`；设备 lspatch cache 哈希 |
| 行为（点击小地图传送） | **PASS（用户真机确认，2026-10-17）**：点击小地图触发传送选择框，功能正常。该入口在原版不可触发，仅大修及后续改版提供 | 用户确认；回归 VM-45 |
| Host CTest | **PASS（145/145）**：含 `world_teleport_feature_tests`（退款/停用失效/余额不足/无效目标）、`world_teleport_install_tests`、`world_teleport_status_tests` | `ctest --test-dir module/app/src/main/cpp/tests/build` |
| 架构 / 路由 / 构建 | **PASS**：`check_arch.py`；`check_http_routes.py` 147；`build-debug.sh`；`git diff --check` | 同上 |
| WT-06 批次（`2140e9d3`） | **Host / 静态 / Debug 构建通过；真机待验**：Host 145/145；`check_arch.py` 全过；`check_http_routes.py` 147 无漂移；`git diff --check` 通过；`scripts/build-debug.sh` 成功，APK `output/inotia4_qol_lsposed_debug_2610171444_36baf50ae8ea.apk`，SHA-256 `36baf50ae8ea…`。WT-06 行为尚未真机实测，不作 PASS 结论 | `ctest --test-dir module/app/src/main/cpp/tests/build`；`scripts/maintenance/check_arch.py`；`scripts/maintenance/check_http_routes.py`；`scripts/build-debug.sh`；构建输出 |
