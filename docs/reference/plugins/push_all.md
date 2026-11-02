# `push_all` 插件参考

## 摘要

功能插件（纯消费者、叶子）：通过两条 `CHAR_Move` 调用点 patch 放开推动分支的 blocker 资格限制——① 队伍闸门（`CHAR_Move+0x168`）把怪物/主控召唤物视为队伍成员，使其可被推；② 站立闸门（`CHAR_Move+0x180`）把条件分支置 NOP，使非站立（战斗中）单位也可被推。不提供能力、无 HTTP 路由、不写游戏数据。

除特别说明，文中 `plugins/...`、`contracts/...`、`game/...`、`host/...` 路径均相对 `module/app/src/main/cpp/`。

## 定位与版本

`push_all`，插件版本 `0.1.0`（`plugins/push_all/plugin.cpp:12-13`）。descriptor：1 项 required 依赖 `character.facts` v1.0（`plugin.cpp:19-22`，`plugin.cpp:137`）；`optional_requires` 空；`provides_count=0`；2 条资源；1 条 JNI 绑定；1 个端口 `QOL_PORT_CHARACTER_EXTRAS` v1.2（`plugin.cpp:24-28`，`contracts/game_ports/ports.h:23,53`）；1 个 feature。无 `routes.inc`、无 HTTP query/endpoint。注册于 `host/assembly.cpp:56,226`。

## 公开能力与路由

无对外能力：不 `provides` 任何 capability，不注册 HTTP 路由、快照、事件或命令。

唯一用户可见 feature 为 `push_all.push`，版本 0，默认 `0`，分类 `kGameplay`，类型 `kToggle`，名称「推怪」（`plugin.cpp:128-132`）。`on_toggle` 回调为 no-op：运行时门禁由 Host 的 feature 布尔值 → `enable` 派生驱动生命周期（`ARCHITECTURE.md` §8.2），回调本身不需动作。

开关通道：`POST /api/config/set`，请求体 `{"plugin_id":"push_all","field":"feature","key":"push_all.push","value":<bool>}`（公共契约见 `docs/reference/api-reference.md` §提交单 key feature/config/desired）；feature 值派生插件 `enable`。遗留绑定 `nativeSetPushAllEnabled`（`plugin.cpp:65-71`、`NativeBridge.kt:92`）为**冻结兼容空实现**：`bridge/native/gamebridge_push_all.cpp:9-14` 恒返回 `JNI_FALSE` 且不写配置，Kotlin 侧无调用点；旧配置键 `pushAllEnabled`（`host/settings_config_bridge.cpp:37`、`host/settings_config_bridge.cpp`→`core_settings_config_store.cpp:454`）属遗留桥，不是当前公开开关语法。

## 游戏内行为

两条 `CHAR_Move` 调用点 patch（`CHAR_Move` VMA=`0xe9808`，`game/symbols/game_symbols.h:637`），均声明为 `private / required / fail_closed`，`resumable=true`，`step_budget=120`（`plugin.cpp:33-62`）：

| 资源 id | 符号（site） | 机制 | 职责 |
|---|---|---|---|
| `patch:CHAR_Move+0x168` | `P_CHAR_MOVE_PUSH_PARTY`（`game/access/patches.cpp:39-41`，`CHAR_MOVE_PUSH_PARTY_BL_OFF=0x168`，`game_symbols.h:648`） | `ResourceMechanism::kCallPatch`，站点层 `PatchKind::kBlRedirect`，BL 重定向到 `push_all_party_filter` | 原版 `CHAR_GetPartyIndex` 队伍闸门的替代值 |
| `patch:CHAR_Move+0x180` | `P_CHAR_MOVE_PUSH_STAND`（`patches.cpp:42-45`，`CHAR_MOVE_PUSH_STAND_CBZ_OFF=0x180`，`game_symbols.h:642`） | `ResourceMechanism::kCallPatch`，站点层 `PatchKind::kWordReplace`（cbz 置 NOP），无 handler | 站立条件分支置 NOP，非站立单位也可被推 |

> 说明：`ResourceMechanism` 枚举只区分 `kEntryHook/kCallPatch/kPtrSlot/kOwnership`（`contracts/plugin.h:26-31`）；BL 重定向与 cbz 置 NOP 在站点层由 `PatchKind` 区分（`game/access/patches.cpp:39-45`）。第二条 reason 与 handler 注释中的「kWordReplace」指站点层机制。

队伍闸门判定（`rules.cpp:5-16`，纯逻辑、不触内存）：
- `type==1`（怪物）→ 0（本功能新增语义，伪装为队伍成员使其可推）；
- `is_active_player_group`（主控本人/主控召唤物）→ 0；
- 其余 → 透传 `party_index`（队友 ≥0 可推、场景设施 -1 不可推）。

字符类型与阵营查询在 `feature.cpp`（需读游戏内存、调游戏函数），返回值为原版 `CHAR_GetPartyIndex` 的替代。

## 使用的 Host SDK 与 Game 面

- `ctx.sdk->capabilities.require`：install 期 require `character.facts`（`plugin.cpp:88-98`）。
- `ctx.sdk->log.write`：门禁变更与首帧 wrapper 记录（`feature.cpp:18-30,36,49`）。
- `contracts::CharacterFactsApi`：`type` / `is_active_player_group`（`feature.cpp:57-62`；`contracts/character_facts.h:19-21`）。
- `QolCharacterExtrasPort::read.party_index`：原版队伍索引读取（`feature.cpp:62`；`contracts/game_ports/character_extras_port.h:12`）。返回 `CHAR_GetPartyIndex` 原值，**含 `-1`（不在队伍）**；`not_ready` 仅表示符号未解析。该取值语义变更见 `docs/refactor/adr/0003-character-extras-party-index-semantics.md`。
- `ctx.game`（`QolGameHost`）：install 期保存并做 null 校验，经 `game_host()` 暴露（`plugin.cpp:76-85,158`）。wrapper 侧 `game_host()` 仅用于空指针保护，未解引用任何成员（`feature.cpp:51-56`）。

## 消费者与职责边界

本插件不作为 provider，**无下游消费者**；级联影响面仅在依赖方向：

- required 依赖 `character.facts`（provider=`core.character`）：provider 缺失时本插件 `blocked`；provider 撤销时作为 required consumer 被级联停用，provider 恢复后按 Host 生命周期重新激活（`docs/refactor/verification.md` VM-K2-03）。
- required 端口 `QOL_PORT_CHARACTER_EXTRAS`：`port_count != 1` 或端口不可用 → install 失败（`plugin.cpp:79,85`），按依赖矩阵置 `blocked`。
- 本插件停用不影响任何其他插件（叶子）。

边界归其他 owner：`CHAR_Move` 站点登记、近址 thunk 与回跳归 `patch/` 与 `game/access/patches.cpp`；`character.facts` / `character_extras` 的实现归 `core.character`；启停状态机归 Host 命令队列与 registry。

## 失败语义（fail-closed）

- `plugin_install`（`plugin.cpp:73-102`）：校验 `struct_size`/`sdk_major`/`g_sdk`/`g_game`/`ports` 与 `character.facts` require，任一失败清空全局并返回 1。
- `on_resource_ready`（`feature.cpp:76-82`）：调用点 patch 无原函数入口需缓存，恒返回 0，本插件无失败路径。
- wrapper `push_all_party_filter`（`feature.cpp:43-70`）：`ch==nullptr` 或任一依赖指针为空或任一查询失败（`.ok != kQolOk`）→ 返回 -1（不可推）；门禁关闭时透传 `party_index`（资源应已物理还原，此为竞态窗口内的 fail-safe）。
- 资源声明 `kFailClosed`：安装失败即按还原释放该资源并把插件置 failed。

## 生命周期

`install` → 保存 sdk/game、require 能力、`set_character_facts`、`set_gate(true)`；`deactivate` → `set_gate(false)`、`set_character_facts(nullptr)`、清端口；`on_enabled` / `on_disabled` → `set_gate(...)`；`fill_status` 输出 `gate_enabled()`（`plugin.cpp:73-125`）。门禁为 `std::atomic<bool>`（`feature.cpp:13,34-41`）：配置事件（主线程）写、游戏线程读；wrapper 在游戏主线程 `CHAR_Move` 调用链内执行，只读 C_TYPE 与 game/access 纯读查询。

## 文件地图

| 文件 | 事实位置 |
|---|---|
| `plugins/push_all/plugin.cpp:12-13,19-28,33-62,65-71` | 版本、required 依赖、端口、两条资源声明、JNI 绑定。 |
| `plugins/push_all/plugin.cpp:73-125,128-152` | install/deactivate/on_enabled/on_disabled/fill_status、feature、descriptor。 |
| `plugins/push_all/feature.h:28-40` | 资源 id 常量、门禁与 wrapper/on_resource_ready 声明。 |
| `plugins/push_all/feature.cpp:13-30,34-41` | 门禁原子量、日志、set_gate/gate_enabled。 |
| `plugins/push_all/feature.cpp:43-70,76-82` | 队伍闸门 wrapper、on_resource_ready。 |
| `plugins/push_all/rules.cpp:5-16` | `party_decision` 纯逻辑。 |
| `plugins/push_all/settings_contribution.cpp:6` | 旧目录贡献已退役，`register_settings` 空实现。 |
| `game/access/patches.cpp:39-45` | 两条站点登记（BL 重定向 / cbz 置 NOP）。 |
| `game/symbols/game_symbols.h:637,642,648` | `CHAR_Move` VMA 与两处偏移。 |
| `game/adapter_ports/character_extras_port.cpp:21-30` | `read.party_index`（原值含 -1）。 |
| `host/settings_config_bridge.cpp:37`、`host/core_settings_config_store.cpp:454` | 遗留 `pushAllEnabled` → `push_all`/`enable` 映射。 |
| `tests/test_push_all_rules.cpp`、`tests/CMakeLists.txt:936-945` | `party_decision` 矩阵 host target `push_all_rules_tests`。 |
