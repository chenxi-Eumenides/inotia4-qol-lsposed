# `simple_mode` 插件审阅登记（2026-10-22）

- 审阅对象：功能插件 `simple_mode`（`module/app/src/main/cpp/plugins/simple_mode/`，8 文件 598 行；审计时另含已删除的 `bridge/native/gamebridge_simple_mode.cpp`）。
- 现场基线：审计开始 HEAD `31e523ab`，工作区 clean；参考说明提交 `31e523ab`。
- 修复提交：`f41a48bf`（正确性批）、`804f9669`（兼容面删除批）；文档在本登记同批更新。
- 阶段 1 由 @oracle 按 A–G 七维完成，共 12 条 finding（P1 4 / P2 4 / P3 4）；阶段 2 用户裁决 5 项；阶段 3 全部可直接修项与裁决项已落地。设备面未验收。

## §1 本批登记问题

Findings 与处置以本节为准。已修条目同时汇总到 `docs/refactor/issues.md` §16；开放/设备欠账见 §2 与 `issues.md` §10。

| 编号 | 级别 | 发现 | 处置 / 状态 |
|---|---|---|---|
| SM-01 | P1 | 阵营查询失败时 `is_player_side_impl` 返回 false，随后 `classify` 仍把 `C_TYPE==1` 判为怪物，玩家侧召唤物可能被加伤/减半。 | **已修** `f41a48bf`：阵营判定改三态（玩家/非玩家/未知）；未知 → `kNeutral`，不缩放、不减半；player/active-group 命中后短路。新增 host 用例覆盖 party/active/summoner 失败、自环、超深链。 |
| SM-02 | P1 | `plugin_install` 在资源安装事务前 `set_gate(true)`；两个 hook 非原子安装，存在部分生效窗口。 | **已修** `f41a48bf`：install 进入即关门禁，成功后仍保持 false；只在 `plugin_on_enabled` 开门。Host 用例覆盖 install 返回后门禁为 false。 |
| SM-03 | P1 | 无 in-flight guard；quarantine 后 Host 仍调用 deactivate 清空 port，残留 wrapper 可空指针。 | **已修** `f41a48bf`：两个 wrapper 建立 `InFlightGuard`；sdk/game/port/facts 取局部指针并判空；deactivate 后残留 hook 用例通过。 |
| SM-04 | P2 | backup 为空时 wrapper 直接丢弃整次伤害事件，且发生在分类之前。 | **已修** `f41a48bf`：backup 判空提前到分类之前；保留 fail-closed 不调用原函数。测试断言无分类读取、无 backup 调用、InFlight 仍配对。 |
| SM-05 | P2 | feature 关闭后到 drain 完成之间 gate 仍为 true；水印按 desired 先变色。 | **已修** `f41a48bf`：feature `on_toggle(0)` 立即关门禁；`on_toggle(1)` 不开门。显式 lifecycle deactivate 仍由 on_disabled/deactivate 关门（异步一帧）。 |
| SM-06 | P1 | wrapper 内同步写 `C_MAX_HP`/`C_HP`，与 Game Host「write 仅由 owner 主线程命令处理器调用」的书面契约冲突。 | **用户裁决：认可为受限例外**。`contracts/game_host.h` 头注释更新为：入口 hook wrapper 在游戏主线程同步执行时，可为该 wrapper 语义所需字段直接调用 write 面，不得跨线程或扩大为通用写入口。ABI 未改；真机语义未验收。 |
| SM-07 | P2 | 停用不清半血账本，既有怪物保持半血；同址同值槽复用可能误判幂等。 | **用户裁决：接受保持半血**，不增加恢复逻辑；参考说明已写明。槽复用判据保留现状（裸指针无法区分 incarnation），未做设备验证。 |
| SM-08 | P2 | 只有 rules 测试，wrapper/账本/生命周期/故障路径零覆盖。 | **已修** `f41a48bf`：新增 `simple_mode_feature_tests`，覆盖 descriptor、install 失败出口、门禁时序、伤害/最大生命矩阵、未知阵营、幂等、池槽越界、backup 为空、残留 wrapper 与 InFlight 计数。设备行为仍 NOT_ACCEPTED。 |
| SM-09 | P3 | `capabilities.require == nullptr` 失败出口未清理已写入全局（当前 Host 不可达）。 | **已修** `f41a48bf`：统一 `clear_install_state()`，覆盖所有失败出口；测试覆盖 require 指针为空。 |
| SM-10 | P3 | 伤害热路径无条件分类双方；阵营判定不短路。 | **已修** `f41a48bf`：party/active 命中即返回；victim 为玩家时不再分类 attacker。测试断言调用次数。 |
| SM-11 | P3 | `max_hp_percent` 仅测试使用；空 settings contribution；池容量常量重复。 | **部分修** `f41a48bf`：删除 `max_hp_percent` 与对应测试。空 contribution 与 `kCharPoolSlots` 重复保留（P3 维护项，登记 `issues.md` §16）。 |
| SM-12 | P2 | API §7.7 的 `simple_mode` 示例 `requires: []` 与 descriptor 不符；参考说明有 5 处事实漂移。 | **已修（文档）**：`api-reference.md` 示例改为 `character.facts` v1.0；`docs/reference/plugins/simple_mode.md` 已按最终实现同步（三态、门禁时序、JNI/旧键删除、测试与文件地图）。 |

### 兼容面删除（用户裁决）

- 删除 `nativeSetSimpleModeEnabled`：Kotlin `external` 声明、native 导出、`bridge/native/gamebridge_simple_mode.cpp`、descriptor `kJni` 项；当前 `jni_count == 0`。
- 删除旧配置键 `simpleModeEnabled`：`ModuleConfig`/`ModuleConfigPatch` 字段、`ConfigStore` 迁移/投影/更新、`settings_config_bridge` 公共 bool 键、相关测试。`kSettingsConfigBoolCount` 10→9、`kSettingsConfigMinor` 1→2。
- 旧 `config.json` 中的 `simpleModeEnabled` 按未知键处理；当前入口只有 feature `simple_mode.combat` 与 `/api/op/features/lifecycle`。

## §2 无法验证项

用户决定（2026-10-22）：本轮执行「修复 + Host 验证」，不进行真机验证。以下设备项全部保持 **NOT_ACCEPTED**，此后不再主动验收；仅保留静态与 Host/构建证据。

| 项目 | 无法验证内容 | 结果/限制 |
|---|---|---|
| VM-P2-01 / VM-P3-01 / VM-P3-03 行为面 | 启停后资源 live/free、半血 A/B、票据终态、`/api/system/resources` 一致性 | **NOT_ACCEPTED**：本批未部署、未执行真机卡。卡内 `simpleModeEnabled` 命令已失效，仅属历史证据。 |
| SM-01 阵营失败分支 | 目标变体上 party/active/summoner 符号缺失或读取失败时，玩家召唤物是否确实不缩放/不减半 | 静态与 fake Host 覆盖通过；无真实缺失符号环境。 |
| SM-02 部分安装窗口 | 两个 hook 分帧安装期间单侧生效、第二资源失败后的回滚 | install 已不再提前开门，Host 用例覆盖门禁状态；但资源软失败/degraded 路径仍由 Host 决定是否调用 `on_enabled`，本批未改 Host 语义。未做安装失败注入与真机时序。 |
| SM-03 隔离残留 | hook 释放失败 → quarantine → deactivate → 残留 wrapper 再调用 | 静态守卫与 fake 用例通过；未做 unhook 失败注入或真机复现。 |
| SM-05 关闭窗口 | feature 关闭到资源释放之间的实际帧数与游戏内表现 | on_toggle 立即关门禁已静态覆盖；未测真实 drain 帧数。 |
| SM-06 hook-time 写 | 写 `C_MAX_HP`/`C_HP` 是否影响属性脏位、后续重算、血条/死亡通知 | 用户批准受限例外；语义影响必须真机确认。 |
| SM-07 停用半血 | 已减半怪物停用后保持半血、重开再减半的体验 | 用户接受该行为；未做真机 A/B。 |
| 深层召唤链 | 队友/主控召唤物的多层召唤在目标变体上的实际归属 | 反汇编注释与 fake 链覆盖；未枚举全部变体。 |
| runtime effect/水印 | desired 与 actual 不一致时的水印表现 | 仅静态核对 Kotlin 路径。 |
| 构建产物部署 | 本批 Debug APK 的设备身份与运行日志 | 未安装；模块 SHA 与真机证据未取得。 |

## §3 已完成验收登记

本节只登记静态、Host 与构建证据，不代表真机行为通过。

| 验收项 | 结果 | 证据 |
|---|---|---|
| 阶段 1 静态审计 A–G | **完成** | 12 条 finding；P0 0、P1 4、P2 4、P3 4。 |
| Host CTest（全量） | **PASS（153/153）** | `cd module/app/src/main/cpp/tests && cmake -B build && cmake --build build -j && ctest --test-dir build --output-on-failure`；含 `simple_mode_rules_tests` 与 `simple_mode_feature_tests`。见 `evidence/host-tests.txt`。 |
| 架构检查 | **PASS** | `uv run python scripts/maintenance/check_arch.py`：全部通过（含 check_layers/check_offsets/check_jni/check_resources）。 |
| HTTP 路由检查 | **PASS** | `check_http_routes.py`：duplicates 0、controller/native/api-reference 无缺失或多余；无路由变更。 |
| JNI 校验 | **PASS** | `check_jni.py`：Kotlin external 158 = native 导出 158，违规 0（删除 1 对后仍一致）。 |
| 资源安装点检查 | **PASS** | `check_resources.py`：违规 0、重复安装 0。 |
| 偏移/符号越权检查 | **PASS** | `check_offsets.py`：违规 0。 |
| Debug 构建 | **PASS** | `scripts/build-debug.sh`：BUILD SUCCESSFUL；APK `output/inotia4_qol_lsposed_debug_2610220450_2f83929c2141.apk`；SHA-256 `2f83929c21417090497713d8cc72fec3d59e84b92228d8e94a26a6acc5d5f188`。见 `evidence/build-debug.txt`。构建成功不代表已部署或行为验收。 |
| `git diff --check` | **PASS** | 两批提交前均通过。见 `evidence/static-checks.txt`。 |

## §4 阶段 2 用户裁决

| 议题 | 用户决定 |
|---|---|
| SM-06 hook 内同步写游戏字段 | 认可为受限例外：在 `contracts/game_host.h` 明确入口 hook wrapper（游戏主线程）可为 wrapper 语义所需字段同步调用 write 面；补真机证据（本批未做）。 |
| SM-05 关闭语义 | 立即关 gate：feature `on_toggle(false)` 先关门禁，资源随后释放。 |
| SM-07 停用后的半血 | 接受既有怪物保持半血，直到游戏自身重算；不增加恢复逻辑。 |
| SM-01 阵营未知态 | 查询失败/超深度归未知，不改写。 |
| 兼容面（JNI + 旧配置键） | 两者都删，接受对旧集成包/旧配置的兼容破坏。 |
| 本轮执行范围 | 修复 + Host 验证（不真机），完成后提交。 |

## §5 提交与变更

| 提交 | 内容 |
|---|---|
| `31e523ab` | 新增 `docs/reference/plugins/simple_mode.md` 并更新 `docs/INDEX.md`（阶段 0.75）。 |
| `f41a48bf` | 阵营三态 fail-closed、install/feature 门禁时序、InFlightGuard 与空指针守卫、backup 提前判空、install 失败统一清理、热路径短路；新增 `simple_mode_feature_tests`；`game_host.h` 受限例外注释。 |
| `804f9669` | 删除零调用 JNI（Kotlin/native/descriptor/bridge）与旧 `simpleModeEnabled` 配置键；`kSettingsConfigBoolCount` 10→9、`kSettingsConfigMinor` 1→2；同步测试。 |
