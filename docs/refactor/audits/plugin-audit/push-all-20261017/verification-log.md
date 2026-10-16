# `push_all` 插件审阅登记（2026-10-17）

- 审阅对象：功能插件 `push_all`（`module/app/src/main/cpp/plugins/push_all/`，8 文件）。
- 基线参考说明（阶段 0.75，本批新建）：`docs/reference/plugins/push_all.md`，已加入 `docs/INDEX.md`。
- 修复提交：`0cabd5fc`（`read.party_index` 语义修正 + 端口 minor 1.2 + host 用例）；决策记录 `docs/refactor/adr/0003-character-extras-party-index-semantics.md`。
- 现场基线（阶段 0）：设备 `192.168.3.56:5555`，`/api/health` = `ready`、variant `monster v29`、`/api/ui/screen` = `world`（进入后）。

## §1 本批登记问题

Findings（阶段 1 @oracle 只读静态审计，A–G 七维；本缺陷由主代理静态定位）：

| 编号 | 严重度 | 文件:行 | 问题 | 处置 |
|---|---|---|---|---|
| PSH-04 | P1 | `game/adapter_ports/character_extras_port.cpp:21-30`；`plugins/push_all/feature.cpp:60-64` | `read.party_index` 把「不在队伍」（`CHAR_GetPartyIndex == -1`）判成 `not_ready`；push_all 的 fail-closed 查询据此 `return -1`（不可推）→ 怪物恒不可推，推怪功能失效。回归引入 `9cdc3d27`（L4 胶囊化），`b9ef63cc` 迁到 extras 端口后保持；VM-P4-01 的 2026-09-17 真机确认早于该回归 | 已修复（`0cabd5fc`，方案 A）：`-1` 作为合法值以 `ok` 返回，`not_ready` 仅表示符号未解析；端口 minor 1.1→1.2；ADR 0003 |
| PSH-01 | P2 | `bridge/native/gamebridge_push_all.cpp:9-14`；`docs/reference/plugins/push_all.md` | `nativeSetPushAllEnabled` 是冻结兼容空实现（恒返回 `JNI_FALSE`、不写配置，Kotlin 侧无调用点）；参考说明与旧台账曾把它描述为有效开关通道 | 已修复：参考说明改正为「遗留兼容空实现」并指向 `POST /api/config/set` 的 feature 契约 |
| PSH-02 | P2 | `plugins/push_all/plugin.cpp:73-96` | install 早期失败分支（`capabilities.require == nullptr`，第 86 行）未清理已写入的 `g_sdk`/`g_game`；相邻失败分支均清理，语义不一致 | 开放 |
| PSH-03 | P2 | `tests/CMakeLists.txt:935-945`；`tests/test_push_all_rules.cpp` | 仅覆盖 `party_decision` 结果矩阵；未覆盖 wrapper 的空指针/函数表缺失/查询失败/门禁透传路径与 descriptor↔站点绑定 | 开放（本批仅补 `read.party_index` 取值用例；wrapper 级用例待补） |

基础设施观察（G，非本插件缺陷，已分别登记）：`patch/` 对本插件第二条资源（`kWordReplace`、空 handler）的处理自洽；config 兼容桥与 feature 契约并存；`fill_status` 零调用点属 `ENG-04`。

## §2 无法验证项

本批无经用户确认后永久不再主动验收的 UNVER 项。

## §3 已完成验收登记

| 验收项 | 结果 | 证据 |
|---|---|---|
| Host CTest | **PASS（143/143）** | `cd module/app/src/main/cpp/tests && cmake -B build && cmake --build build && ctest --test-dir build` |
| 架构 / 路由 / 构建 / 空白 | **PASS**：`check_arch.py`；`check_http_routes.py` 147 不漂移；`scripts/build-debug.sh`；`git diff --check` | 同上 |
| 真机部署与运行身份 | **PASS**：集成包 `output/Inotia4_v1.3.2_monster_v29_lspatched.apk` sha256 `4d834442…`；运行模块 sha256 `10b218e703f9…`，与本机 Debug 构建逐字一致 | 设备 `/data/user/0/<pkg>/cache/lspatch/com.inotia4.qol/434958478.apk` |
| 装配 | **PASS**：`push_all` `enabled`、`degraded=false`、`patch:CHAR_Move+0x168` 与 `+0x180` 均 `live` | `GET /api/system/features` |
| 行为（推怪） | **PASS（用户真机确认，2026-10-17）**：走向怪物，推怪恢复正常 | 用户确认；回归 VM-P4-01 / VM-K2-03 |

> 运行期佐证（只读 frida 探针，修复前）：`libgame.so` base `0x795c42d000`，`+0x168` = `0x94238dcc`（已重定向入模块）、`+0x180` = `0xd503201f`（NOP）——两条 patch 确已生效，缺陷在 wrapper 判定而非安装。
