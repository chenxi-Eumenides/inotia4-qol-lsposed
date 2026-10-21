# `simple_mode` 插件参考

## 摘要

功能插件（纯消费者、默认关闭的叶子）：开启后对怪物侧应用三项战斗缩放——怪物最大生命减半、玩家侧对怪物的伤害 ×1.5、玩家侧受到的伤害 ×0.5。插件通过两个入口 hook 实现：`CHAR_AddDamage` 单点改写伤害入参，`CHAR_UpdateAttrFromMonster` 在最大生命重算后写回半值。

插件不提供 capability、无 HTTP 路由，唯一的对外副作用是 Kotlin 侧根据它的 runtime effect 改变屏幕水印颜色。`nativeSetSimpleModeEnabled` 只剩零调用的 JNI 兼容导出，不参与启停。

除特别说明，文中 `plugins/...`、`contracts/...`、`game/...`、`host/...` 路径均相对 `module/app/src/main/cpp/`。

## 定位与版本

`simple_mode`，插件版本 `0.1.0`（`plugins/simple_mode/plugin.cpp:12-13`）。descriptor（`plugin.cpp:133-151`）：

- `requires` 1 项：`character.facts` v1.0（`plugin.cpp:19-22`；`contracts/character_facts.h:11-14`）。
- `optional_requires` 0、`provides` 0、HTTP route/endpoint 0（`plugin.cpp:137-139,149`）。
- `resources` 2 个入口 hook（`plugin.cpp:32-61`）。
- `jni` 1 项：`nativeSetSimpleModeEnabled`（`plugin.cpp:64-70`）。
- `ports` 1 项：`QOL_PORT_CHARACTER_EXTRAS` v1.2（`plugin.cpp:24-28`；`contracts/game_ports/ports.h:54`）。
- 生命周期回调：`plugin_install`、`plugin_deactivate`、`plugin_on_enabled`、`plugin_on_disabled`、`plugin_fill_status`、`on_resource_ready`（`plugin.cpp:141-146`）。

注册位于 `host/assembly.cpp:224`。插件是功能插件叶子：只有 `character.facts` 一个必需 capability，没有消费者依赖它。

## 公开能力与路由

- 无 capability `provides`，无 `routes.inc`，无 HTTP query/endpoint/route metadata。因此没有属于本插件的 HTTP 路由；启停只能经 Host 常驻控制面（§7.6 配置/feature，§7.7 状态）。
- 唯一 feature 为 `simple_mode.combat`：默认 `0`、分类 `kGameplay`、类型 `kToggle`、名称「简单模式」、描述「降低怪物生命并增强玩家伤害与承伤。」（`plugin.cpp:127-131`）。回调为空操作，忽略 `enabled` 参数并返回 `ok()`；插件行为由 lifecycle 与 `g_gate` 驱动，不由 feature 回调驱动。
- 插件只有 1 个 feature，`want_run = enable && any(feature)`（`docs/reference/api-reference.md` §7.6）因此 feature 值实际等价于插件 desired 状态：开启即安装插件与两个资源，关闭即完整 deactivate。
- JNI 元数据登记 `nativeSetSimpleModeEnabled`，签名哈希为 `nullptr`（`plugin.cpp:63-70`）；对应导出是实现固定的兼容 stub，永远返回 `JNI_FALSE` 且不写配置（`bridge/native/gamebridge_simple_mode.cpp:9-14`），Kotlin 声明在 `java/com/inotia4/qol/NativeBridge.kt:91`，仓库内无调用点。

## 游戏内行为

### 入口 hook

两个资源均为 `kEntryHook`、`private`、`required`、`failClosed`、`resumable=true`、`step_budget=120`、`gate=nullptr`（`plugin.cpp:32-61`）。`gate=nullptr` 表示资源跟随插件生命周期：插件安装时由宿主安装，插件 deactivate 时释放；handler 为业务层 wrapper。

| 资源 id | 符号 / VMA | wrapper |
|---|---|---|
| `hook:CHAR_AddDamage` | `F_CHAR_ADD_DAMAGE_VMA` = `0xea2d8`（`game/symbols/game_symbols.h:673`） | `char_add_damage_wrapper`（`feature.cpp:149-161`） |
| `hook:CHAR_UpdateAttrFromMonster` | `F_CHAR_UPDATE_ATTR_FROM_MONSTER_VMA` = `0xe0048`（`game/symbols/game_symbols.h:679`） | `char_update_attr_from_monster_wrapper`（`feature.cpp:163-212`） |

资源 id 常量在 `feature.h:39-40`；符号解析表映射见 `game/access/symbols.cpp:22-23` 与 `game/access/game_access.cpp:428-430`。

`CHAR_AddDamage` 声明为全部伤害路径的汇合点，`feature.h:16-22` 记录的反汇编结论为：函数体第 3 条语句即 `CHAR_AddLife(受害者, -伤害)`，因此入口改写 `w2` 后扣血、承伤记录、仇恨、飘字与音效一致缩放。`CHAR_UpdateAttrFromMonster` 是 `C_TYPE==1` 角色属性重算的唯一分派目标，也是最大生命槽唯一的幂等调整层（`feature.h:20-21`；`feature.cpp:125-131`）。

### 阵营判定

`classify(ch)`（`feature.cpp:109-119`）：

1. `ch == nullptr` → `kNeutral`。
2. 先调 `is_player_side_impl`，玩家侧优先于类型判定——玩家召唤物的 `C_TYPE` 也是 1。
3. 再读 `CharacterFactsApi::type`：`1` → `kMonster`，其余（含读取失败）→ `kNeutral`。`C_TYPE` 语义为 0=玩家侧角色、1=怪物、2=NPC/装饰物（`game/symbols/game_symbols.h:15`）。

`is_player_side_impl(ch, depth)`（`feature.cpp:75-105`）依次读取三个原语：

- `QolCharacterExtrasPort::read.is_party_member`：主角与 2 名队友；
- `CharacterFactsApi::is_active_player_group`：主控本人与主控召唤物；
- `QolCharacterExtrasPort::read.summoner`：反查召唤者，递归补齐「队友的召唤物」，深度上限 4 且拒绝 `summoner == ch` 自环（`feature.cpp:67-68,102-104`）。

任一依赖指针缺失或任一读取返回非 `kQolOk` 时返回 `false`（fail-safe：不把敌人误判为友方），并通过 `note_faction_deps_missing_once` 只记一次告警（`feature.cpp:58-65,96-99`）。

### 伤害倍率

`char_add_damage_wrapper(attacker, victim, damage, damage_flag, magic_flag)`（`feature.cpp:149-161`）：

1. acquire 读取 `g_backup_add_damage`；为空时直接返回，不调用原函数（fail-closed）。
2. `g_gate` 为真且 `damage > 0` 时，对攻击者与受害者各做一次 `classify`，交给 `damage_percent`。
3. 百分比非 100 时按 `scale_by_percent` 缩放 `damage`，其余入参原样透传。

`damage_percent(attacker, victim)`（`rules.cpp:7-18`）的判定顺序固定：

| 条件 | 百分比 |
|---|---|
| `victim == kPlayer`（玩家侧受到的伤害，与攻击者身份无关） | 50 |
| `attacker == kPlayer && victim == kMonster` | 150 |
| 其余（怪物互殴、涉及 NPC/装饰物、无法判定） | 100 |

`scale_by_percent`（`rules.cpp:24-36`）：

- `value <= 0` 或 `percent == 100` → 原样返回；
- 用 `int64_t` 计算 `value * percent / 100`，整数除法向下取整；
- 大于 `INT32_MAX` → 钳到 `INT32_MAX`；小于 1 → 钳到 1（伤害 ≤0 会被游戏侧丢弃）；
- 因此 1 点伤害 ×150% 仍为 1（文档与用例均固定此取整语义，`rules.h:17-23`）。

### 最大生命减半

`char_update_attr_from_monster_wrapper(ch, attr)`（`feature.cpp:163-212`）：

1. 读取 backup；为空直接返回。
2. 先调用原函数，再检查 `g_gate`——关闭时只做透传。
3. 只处理 `attr == 0x1e`（`ATTR_MAX_HP`，`game/symbols/game_symbols.h:70`）；其余属性直接返回。
4. 依赖检查：`game_host`、`character.facts.type`、port 的 `pool_slot`/`max_hp`/`hp`、`game->character_write.max_hp`/`hp` 全部非空，否则返回（不改写）。
5. 只对 `type == 1` 且非玩家侧（`is_player_side_impl`）的角色处理；玩家召唤物虽有 `C_TYPE==1` 也不减半。
6. 用 `read.pool_slot` 定位角色池槽，要求 `0 <= slot < 100`；定位失败时不改写并只记一次告警（fail-safe 方向：怪物保持满血）。
7. 读当前最大生命 `raw`，按槽维护 `HalfHpMark{ch, halved}` 账本：
   - 槽主变化（`mark.ch != ch`）→ 重置账本；
   - `mark.halved != 0 && raw == mark.halved` → 判定为游戏原样写回上次半值，跳过（幂等）；
   - 否则计算 `halved_max_hp(raw)` 并写回 `character_write.max_hp`，再记录 `mark.halved`。
8. 若当前 HP `> half`，同步写 `character_write.hp(ch, half)`，避免血条超上限。

`halved_max_hp`（`rules.cpp:38-43`）：`max_hp <= 2` → 1；其余 `max_hp / 2`（向下取整，下限 1）。

幂等账本的存在理由记录在 `feature.cpp:125-136`：该函数是「读旧槽值 → 变换 → 写回」的调整层（`game_symbols.h:674` 记录唯一写点 `str w20,[x19,#0x24]`，`w20` 初值读自同一槽），无条件重复除 2 会按 1/2 → 1/4 → 1/8 累积；账本以「槽值等于上次半值」识别原样写回。

### 数据来源

- `C_TYPE` 偏移 `0x09`（int8）：`game/symbols/game_symbols.h:15`，读取经 `CharacterFactsApi::type`（provider 实现 `plugins/core.character/plugin.cpp:177-183`，底层 `game/adapter_ports/character_port.cpp`）。
- `C_MAX_HP` = `C_ATTR(0x24) + 0x1e*4` = `ch+0x9c`；`C_HP` = `ch+0x1f0`（`game/symbols/game_symbols.h:19-20,76`）。读取/写入由 Game Adapter 的 `char_read_max_hp`/`char_write_max_hp`/`char_read_hp`/`char_write_hp` 收口（`game/access/char.cpp:18-37`），经 port `read.max_hp`/`read.hp`（`game/adapter_ports/character_extras_port.cpp:96-108`）与 `QolGameHost::character_write`（`contracts/game_host.h:117-121`）暴露。
- 角色池：`C_OBJ_SIZE = 0x430`、`C_CHARSYSTEM_POOL_SLOTS = 100`（`game/symbols/game_symbols.h:37-38`）。`char_pool_slot` 校验对象地址位于池内、按对象步长对齐且槽号在容量内（`game/access/char.cpp:49-62`）；`kCharPoolSlots = 100` 与符号常量一致（`feature.cpp:138`）。
- 阵营原语由 Host 的 `QolCharacterExtrasPort` 与 `character.facts` 提供；port 的 `is_party_member` 依赖 `CHAR_GetPartyIndex` 等符号已解析（`game/adapter_ports/character_extras_port.cpp:34-48`），`character.facts` 由 `core.character` 提供 v1.0（`plugins/core.character/plugin.cpp:195-201`）。

### 行为边界

- 只缩放「怪物最大生命」与「伤害数值」。伤害标志（暴击/DOT/物理/法术）、命中、仇恨、飘字、音效不单独改写，随 `CHAR_AddDamage` 入参一致缩放。
- 玩家侧受到的伤害固定 ×0.5，包括怪物、NPC、环境来源（`victim` 判定优先），以及玩家侧互击的理论路径。
- 玩家侧打 NPC/装饰物不加伤；怪物打 NPC 也不改倍率。
- 最大生命只在 `attr == ATTR_MAX_HP` 且 `C_TYPE == 1` 时处理；玩家、队友、玩家召唤物、NPC 不处理。定位不到池槽的怪物保持满血（不改写）。
- 关闭插件不会把已经减半的角色恢复为满血：deactivate 只释放 hook 与门禁，账本与已写入的槽值保留；后续游戏自身重算给出新满值时才会再次减半（若插件重新启用）。VM-P3-01 曾以「关门禁后刷图 `hp=270`」验证新刷出的怪物为满值，而不是既有怪物回满。
- 不写 ITEMDATABASE、不修改生成表；只经 Game Adapter 写 `C_MAX_HP` 与 `C_HP` 两个字段。

## 使用的 Host SDK 与 Game 面

- `ctx.sdk->capabilities.require`：安装期 require `character.facts` v1.0，并缓存 `CharacterFactsApi*`（`plugin.cpp:85-97`）。运行时 `type` 与 `is_active_player_group` 均经该缓存调用（`feature.cpp:114,180`）。
- `ctx.game`（`QolGameHost`）：`character_write.max_hp` / `character_write.hp`（`feature.cpp:175,204,210`）。
- `QolCharacterExtrasPort::read`：`is_party_member`、`summoner`（阵营判定，`feature.cpp:80-95`）、`pool_slot`、`max_hp`、`hp`（半血账本与钳制，`feature.cpp:174-209`）。
- `ctx.sdk->log.write`：告警（阵营符号缺失、池槽定位失败、未知 resource id）与 enable 状态日志（`feature.cpp:29-52,214-217,239`）。
- `ctx.ports[0]`：`QOL_PORT_CHARACTER_EXTRAS` v1.2，安装期要求 `port_count == 1`（`plugin.cpp:76-84`）。
- 不消费 `hooks.set_intercept`、命令、票据、面板或 UI 能力。

## 职责边界

- 本插件拥有：两个 wrapper 的伤害/生命缩放策略、阵营判定编排、半血幂等账本、插件门禁与资源声明。
- `core.character` 拥有：`character.facts` provider（`type`、`is_active_player_group`）与角色状态读取（`plugins/core.character/plugin.cpp:177-206`）。
- Game Adapter 拥有：`C_TYPE`/`C_MAX_HP`/`C_HP` 偏移读取、池槽计算、`CHAR_AddDamage`/`CHAR_UpdateAttrFromMonster` 符号解析与 hook 安装机制（`game/access/char.cpp`、`game/adapter_ports/character_extras_port.cpp`、`host/plugin_registry.cpp`）。
- Host 拥有：feature 持久化与 desired/actual 状态机、配置控制端点、资源仲裁与生命周期（`docs/reference/api-reference.md` §7.6/§7.7）。
- Kotlin 侧拥有：`simple_mode` runtime effect 订阅与屏幕水印呈现；插件不通过 JNI 主动更新 UI（`java/com/inotia4/qol/PluginRuntimeEffects.kt:58,94-97,197-208`）。

## 生命周期与线程模型

- `plugin_install`（`plugin.cpp:72-101`）：写入 `g_sdk`/`g_game`，清空 port；校验 `PluginContext` 尺寸、SDK major、`ports` 数量与指针；缓存 port；require `character.facts`，成功则写 `g_character_facts` 并 `set_gate(true)`。所有失败出口都清空已写入的全局（`plugin.cpp:79-95`）。
- `plugin_deactivate`（`plugin.cpp:103-108`）：`set_gate(false)`、清空 facts 指针与 port。不释放资源——资源由宿主事务释放。
- `plugin_on_enabled` / `plugin_on_disabled`（`plugin.cpp:111-119`）：只切换 `g_gate`，不做资源操作。
- `plugin_fill_status`（`plugin.cpp:121-124`）：`enabled = gate_enabled()`，`error = nullptr`。
- `on_resource_ready`（`feature.cpp:228-241`）：按 resource id 把 backup 存入 `std::atomic<void*>`；未知 id 记一次告警后仍返回 0；本插件无失败路径。
- feature 回调是空操作；`simple_mode.combat` 的开关通过 Host reconciler 改写 `want_run`，进而推进插件安装/完整 deactivate（`host/features/reconciler.cpp:186-215`；`docs/reference/api-reference.md` §7.6）。
- 线程模型：门禁 `std::atomic<bool>`、两个 backup `std::atomic<void*>` 可跨线程读写；`g_sdk`/`g_game`/`g_character_facts`/`g_character_extras` 为普通全局指针，安装与 deactivate 在宿主生命周期线程写，wrapper 在游戏线程读（`feature.h:23-25`）。半血账本 `g_half_marks[100]` 声明为仅游戏线程访问、无同步（`feature.cpp:125-140`）。

## 外部联动

### 屏幕水印

Kotlin 的 `PluginRuntimeEffects` 在启动时订阅 `simple_mode`（`java/com/inotia4/qol/PluginRuntimeEffects.kt:58,94-97`），只接受 `core.api` 与 `simple_mode` 两种 plugin id 的 effect（`:197-199`）。`simple_mode` 的效果把 `desiredEnabled` 交给 `ConfigPlatformEffects.onSimpleModeRuntimeEffect`（`:205-208`），后者按 `eventRevision` 去重并调用 `WatermarkOverlay.updateSimpleMode`（`java/com/inotia4/qol/ConfigPlatformEffects.kt:6-13`）。水印配色表命中「简单模式」时使用亮绿 `0xCC00FF00`（`java/com/inotia4/qol/patch/WatermarkOverlay.kt:65,99,102-106`）。

该联动使用 desired 状态而非 actual 状态，且只是平台侧视觉指示，不参与游戏内资源门禁。

### 旧配置键与 JNI

- 配置投影 `simpleModeEnabled` 由 `feature_enabled("simple_mode", false)` 派生（`host/core_settings_config_store.cpp:965`），可经 `POST /api/config/set` 写入（`:1595,1628`）；它是配置平台兼容投影，不直接驱动本插件运行态。
- `nativeSetSimpleModeEnabled` 只保留符号与固定返回值 `JNI_FALSE`（`bridge/native/gamebridge_simple_mode.cpp:9-14`），Kotlin 侧仅声明、无调用点（`java/com/inotia4/qol/NativeBridge.kt:91`）。

## 文件地图

| 文件 | 职责 |
|---|---|
| `plugins/simple_mode/plugin.cpp` | descriptor、requires/resources/ports/features、install/deactivate/启停/status |
| `plugins/simple_mode/feature.cpp` | 两个入口 wrapper、阵营判定、半血账本、门禁与 backup、诊断日志 |
| `plugins/simple_mode/feature.h` | 业务层声明、机制与线程模型说明、resource id 常量 |
| `plugins/simple_mode/rules.cpp` / `rules.h` | 纯规则：阵营→伤害百分比、最大生命百分比、百分比缩放、半血取整 |
| `plugins/simple_mode/settings_contribution.cpp` / `.h` | 旧 settings catalog contribution 的空实现（`register_settings` 恒返回 `ok()`） |
| `plugins/simple_mode/CMakeLists.txt` | `qol_plugin_simple_mode` 静态库 |
| `bridge/native/gamebridge_simple_mode.cpp` | `nativeSetSimpleModeEnabled` 兼容 stub |
| `java/com/inotia4/qol/PluginRuntimeEffects.kt` | `simple_mode` runtime effect 订阅与分派 |
| `java/com/inotia4/qol/ConfigPlatformEffects.kt` | desired 状态 → 水印效果 |
| `java/com/inotia4/qol/patch/WatermarkOverlay.kt` | 简单模式水印配色规则 |

## 测试

Host target `simple_mode_rules_tests`（`tests/CMakeLists.txt:497-505`）只链接 `rules.cpp` 与 `tests/test_simple_mode_rules.cpp`：

- 伤害矩阵：3×3 阵营组合逐条固定（受害者玩家侧一律 50；玩家打怪 150；其余 100）。
- 最大生命矩阵：怪物 50、玩家/中立 100。
- `scale_by_percent`：整数取整方向、下限 1、非法/零值原样返回、`INT32_MAX` 钳制。
- `halved_max_hp`：偶数、奇数、0/1/2、负数下限 1。

`settings_contributions_tests` 编译 `plugins/simple_mode/settings_contribution.cpp`（`tests/CMakeLists.txt:2065`），覆盖范围是该文件返回 `ok()` 的空实现。

未覆盖：两个 wrapper 本体、`classify` 的真实 port/游戏调用、hook 安装与 backup 交还、半血账本的槽复用/幂等/关闭再开启路径、插件 install/deactivate 失败出口、runtime effect 与水印联动。历史设备验收记录见 `docs/refactor/verification.md` VM-P2-01、VM-P3-01、VM-P3-03、VM-K2-03、VM-K4-02；这些记录早于当前实现，不能替代当前真机验证。
