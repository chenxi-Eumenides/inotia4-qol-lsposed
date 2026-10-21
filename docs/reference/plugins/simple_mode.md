# `simple_mode` 插件参考

## 摘要

功能插件（纯消费者、默认关闭的叶子）：开启后对怪物侧应用三项战斗缩放——怪物最大生命减半、玩家侧对怪物的伤害 ×1.5、玩家侧受到的伤害 ×0.5。插件通过两个入口 hook 实现：`CHAR_AddDamage` 单点改写伤害入参，`CHAR_UpdateAttrFromMonster` 在最大生命重算后写回半值。

插件不提供 capability、无 HTTP 路由，也不提供 JNI 导出。唯一的对外副作用是 Kotlin 侧根据它的 runtime effect 改变屏幕水印颜色。

除特别说明，文中 `plugins/...`、`contracts/...`、`game/...`、`host/...` 路径均相对 `module/app/src/main/cpp/`。

## 定位与版本

`simple_mode`，插件版本 `0.1.0`（`plugins/simple_mode/plugin.cpp:12-13`）。descriptor（`plugin.cpp:134-152`）：

- `requires` 1 项：`character.facts` v1.0（`plugin.cpp:19-22`；`contracts/character_facts.h:11-14`）。
- `optional_requires` 0、`provides` 0、JNI 0、HTTP route/endpoint 0（`plugin.cpp:138-141,150`）。
- `resources` 2 个入口 hook（`plugin.cpp:32-61`）。
- `ports` 1 项：`QOL_PORT_CHARACTER_EXTRAS` v1.2（`plugin.cpp:24-28`；`contracts/game_ports/ports.h:54`）。
- 生命周期回调：`plugin_install`、`plugin_deactivate`、`plugin_on_enabled`、`plugin_on_disabled`、`plugin_fill_status`、`on_resource_ready`（`plugin.cpp:142-147`）。

注册位于 `host/assembly.cpp:224`。插件是功能插件叶子：只有 `character.facts` 一个必需 capability，没有消费者依赖它。

## 公开能力与路由

- 无 capability `provides`，无 `routes.inc`，无 HTTP query/endpoint/route metadata，也无 JNI。因此没有属于本插件的 HTTP 路由；启停只能经 Host 常驻控制面（§7.6 配置/feature，§7.7 状态）。
- 唯一 feature 为 `simple_mode.combat`：默认 `0`、分类 `kGameplay`、类型 `kToggle`、名称「简单模式」、描述「降低怪物生命并增强玩家伤害与承伤。」（`plugin.cpp:125-132`）。回调在 `enabled == 0` 时立即 `set_gate(false)`，开启方向不直接开门（开门由 lifecycle 的 `plugin_on_enabled` 负责）。
- `want_run = enable && any(feature)`（`docs/reference/api-reference.md` §7.6）。本插件只有 1 个 feature，设置页关闭 feature 会在同一次 feature 提交中派生 `enable=false`，随后推进完整 deactivate；显式 lifecycle 写入可以不按 feature 聚合归一化，因此 feature 布尔值不保证在所有时刻都等于插件 desired 状态。
- 旧 `nativeSetSimpleModeEnabled` JNI 导出与旧配置键 `simpleModeEnabled` 已于 2026-10-22 删除；两者都不再属于当前实现。

## 游戏内行为

### 入口 hook

两个资源均为 `kEntryHook`、`private`、`required`、`failClosed`、`resumable=true`、`step_budget=120`、`gate=nullptr`（`plugin.cpp:32-61`）。`gate=nullptr` 表示资源跟随插件生命周期：插件安装时由宿主安装，插件 deactivate 时释放；handler 为业务层 wrapper。

| 资源 id | 符号 / VMA | wrapper |
|---|---|---|
| `hook:CHAR_AddDamage` | `F_CHAR_ADD_DAMAGE_VMA` = `0xea2d8`（`game/symbols/game_symbols.h:673`） | `char_add_damage_wrapper`（`feature.cpp:157-179`） |
| `hook:CHAR_UpdateAttrFromMonster` | `F_CHAR_UPDATE_ATTR_FROM_MONSTER_VMA` = `0xe0048`（`game/symbols/game_symbols.h:679`） | `char_update_attr_from_monster_wrapper`（`feature.cpp:181-234`） |

资源 id 常量在 `feature.h:39-40`；符号解析表映射见 `game/access/symbols.cpp:22-23` 与 `game/access/game_access.cpp:428-430`。

`CHAR_AddDamage` 声明为全部伤害路径的汇合点，`feature.h:16-22` 记录的反汇编结论为：函数体第 3 条语句即 `CHAR_AddLife(受害者, -伤害)`，因此入口改写 `w2` 后扣血、承伤记录、仇恨、飘字与音效一致缩放。`CHAR_UpdateAttrFromMonster` 是 `C_TYPE==1` 角色属性重算的唯一分派目标，也是最大生命槽唯一的幂等调整层（`feature.h:20-21`；`feature.cpp:133-147`）。

### 阵营判定

`faction_impl(ch, depth, extras, facts)`（`feature.cpp:78-113`）返回三态：玩家侧、明确非玩家侧、未知。

判定顺序与短路：

1. `ch == nullptr`、`depth > 4`、任一依赖指针缺失、任一读取返回非 `kQolOk`、或 `summoner == ch` 自环 → 未知。
2. `read.is_party_member` 为真 → 玩家侧（主角与 2 名队友），不再调用后续原语。
3. `is_active_player_group` 为真 → 玩家侧（主控本人与主控召唤物），不再调用 summoner。
4. `read.summoner` 为空 → 非玩家侧；否则递归调用，深度上限 4（`feature.cpp:67-68`）。
5. 任一步失败只通过 `note_faction_deps_missing_once` 记一次告警（`feature.cpp:59-65`）。

`classify(ch, extras, facts)`（`feature.cpp:116-127`）：

1. `ch == nullptr` → `kNeutral`。
2. 阵营为玩家侧 → `kPlayer`；未知 → `kNeutral`（不缩放、不减半，不再按 `C_TYPE` 补判为怪物）；明确非玩家侧才继续读 `CharacterFactsApi::type`。
3. `type == 1` → `kMonster`，其余（含读取失败）→ `kNeutral`。`C_TYPE` 语义为 0=玩家侧角色、1=怪物、2=NPC/装饰物（`game/symbols/game_symbols.h:15`）。

未知态 fail-closed 的目的：阵营原语未解析或读取失败时，玩家侧召唤物不能被误判为怪物。代价是此时伤害不做倍率、最大生命不减半。

### 伤害倍率

`char_add_damage_wrapper(attacker, victim, damage, damage_flag, magic_flag)`（`feature.cpp:157-179`）：

1. 建立 `InFlightGuard`（resource id `hook:CHAR_AddDamage`），Host 释放资源前可等待在途 wrapper。
2. acquire 读取 `g_backup_add_damage`；为空时直接返回，不调用原函数，也不做任何分类。
3. `g_gate`、`damage > 0`、sdk/game/port/facts 均可用时：先分类 victim；victim 为玩家侧直接取 50，否则再分类 attacker 并交给 `damage_percent`。
4. 百分比非 100 时按 `scale_by_percent` 缩放 `damage`，其余入参原样透传。

`damage_percent(attacker, victim)`（`rules.cpp:7-18`）的判定顺序固定：

| 条件 | 百分比 |
|---|---|
| `victim == kPlayer`（玩家侧受到的伤害，与攻击者身份无关） | 50 |
| `attacker == kPlayer && victim == kMonster` | 150 |
| 其余（怪物互殴、涉及 NPC/装饰物、无法判定） | 100 |

`scale_by_percent`（`rules.cpp:20-32`）：

- `value <= 0` 或 `percent == 100` → 原样返回；
- 用 `int64_t` 计算 `value * percent / 100`，整数除法向下取整；
- 大于 `INT32_MAX` → 钳到 `INT32_MAX`；小于 1 → 钳到 1（伤害 ≤0 会被游戏侧丢弃）；
- 因此 1 点伤害 ×150% 仍为 1（文档与用例均固定此取整语义，`rules.h:17-23`）。

### 最大生命减半

`char_update_attr_from_monster_wrapper(ch, attr)`（`feature.cpp:181-234`）：

1. 建立 `InFlightGuard`（resource id `hook:CHAR_UpdateAttrFromMonster`）。
2. 读取 backup；为空直接返回。
3. 先调用原函数，再检查 `g_gate`——关闭时只做透传；port 为空时同样直接返回。
4. 只处理 `attr == 0x1e`（`ATTR_MAX_HP`，`game/symbols/game_symbols.h:70`）；其余属性直接返回。
5. 依赖检查：sdk、`game_host`、`character.facts`、port 的 `pool_slot`/`max_hp`/`hp`、`game->character_write.max_hp`/`hp` 全部可用，否则返回（不改写）。
6. `classify(ch) == kMonster` 才继续；玩家、队友、玩家召唤物、NPC 与未知阵营都不减半。
7. 用 `read.pool_slot` 定位角色池槽，要求 `0 <= slot < 100`；定位失败时不改写并只记一次告警（fail-safe 方向：怪物保持满血）。
8. 读当前最大生命 `raw`，按槽维护 `HalfHpMark{ch, halved}` 账本：
   - 槽主变化（`mark.ch != ch`）→ 重置账本；
   - `mark.halved != 0 && raw == mark.halved` → 判定为游戏原样写回上次半值，跳过（幂等）；
   - 否则计算 `halved_max_hp(raw)` 并写回 `character_write.max_hp`，再记录 `mark.halved`。
9. 若当前 HP `> half`，同步写 `character_write.hp(ch, half)`，避免血条超上限。

`halved_max_hp`（`rules.cpp:34-39`）：`max_hp <= 2` → 1；其余 `max_hp / 2`（向下取整，下限 1）。

幂等账本的存在理由记录在 `feature.cpp:133-147`：该函数是「读旧槽值 → 变换 → 写回」的调整层（`game_symbols.h:674` 记录唯一写点 `str w20,[x19,#0x24]`，`w20` 初值读自同一槽），无条件重复除 2 会按 1/2 → 1/4 → 1/8 累积；账本以「槽值等于上次半值」识别原样写回。

### 数据来源

- `C_TYPE` 偏移 `0x09`（int8）：`game/symbols/game_symbols.h:15`，读取经 `CharacterFactsApi::type`（provider 实现 `plugins/core.character/plugin.cpp:177-183`，底层 `game/adapter_ports/character_port.cpp`）。
- `C_MAX_HP` = `C_ATTR(0x24) + 0x1e*4` = `ch+0x9c`；`C_HP` = `ch+0x1f0`（`game/symbols/game_symbols.h:19-20,76`）。读取/写入由 Game Adapter 的 `char_read_max_hp`/`char_write_max_hp`/`char_read_hp`/`char_write_hp` 收口（`game/access/char.cpp:18-37`），经 port `read.max_hp`/`read.hp`（`game/adapter_ports/character_extras_port.cpp:96-108`）与 `QolGameHost::character_write`（`contracts/game_host.h:119-123`）暴露。
- 角色池：`C_OBJ_SIZE = 0x430`、`C_CHARSYSTEM_POOL_SLOTS = 100`（`game/symbols/game_symbols.h:37-38`）。`char_pool_slot` 校验对象地址位于池内、按对象步长对齐且槽号在容量内（`game/access/char.cpp:49-62`）；`kCharPoolSlots = 100` 与符号常量一致（`feature.cpp:146`）。
- 阵营原语由 Host 的 `QolCharacterExtrasPort` 与 `character.facts` 提供；port 的 `is_party_member` 依赖 `CHAR_GetPartyIndex` 等符号已解析（`game/adapter_ports/character_extras_port.cpp:34-48`），`character.facts` 由 `core.character` 提供 v1.0（`plugins/core.character/plugin.cpp:195-201`）。

### 行为边界

- 只缩放「怪物最大生命」与「伤害数值」。伤害标志（暴击/DOT/物理/法术）、命中、仇恨、飘字、音效不单独改写，随 `CHAR_AddDamage` 入参一致缩放。
- 玩家侧受到的伤害固定 ×0.5，包括怪物、NPC、环境来源（`victim` 判定优先），以及玩家侧互击的理论路径。
- 玩家侧打 NPC/装饰物不加伤；怪物打 NPC 也不改倍率。
- 最大生命只在 `attr == ATTR_MAX_HP` 且 `classify == kMonster` 时处理；玩家、队友、玩家召唤物、NPC、未知阵营都不处理。定位不到池槽的怪物保持满血（不改写）。
- 阵营依赖失败时整次伤害/最大生命都不缩放（未知态），不会退化为「按 C_TYPE 当怪物」。
- 关闭插件不会把已经减半的角色恢复为满血：deactivate 只释放 hook 与门禁，账本与已写入的槽值保留；后续游戏自身重算给出新满值时才会再次减半（若插件重新启用）。2026-10-22 用户裁决接受该行为，不增加恢复逻辑。VM-P3-01 曾以「关门禁后刷图 `hp=270`」验证新刷出的怪物为满值，而不是既有怪物回满。
- 不写 ITEMDATABASE、不修改生成表；只经 Game Adapter 写 `C_MAX_HP` 与 `C_HP` 两个字段。该同步写发生在入口 hook wrapper 内、游戏主线程，属于 `contracts/game_host.h` 头注释记录的受限例外：只允许 wrapper 语义所需字段。

## 使用的 Host SDK 与 Game 面

- `ctx.sdk->capabilities.require`：安装期 require `character.facts` v1.0，并缓存 `CharacterFactsApi*`（`plugin.cpp:83-97`）。运行时 `type` 与 `is_active_player_group` 均经该缓存调用（`feature.cpp:122,204`）。
- `ctx.game`（`QolGameHost`）：`character_write.max_hp` / `character_write.hp`（`feature.cpp:200-201,226,232`）。
- `QolCharacterExtrasPort::read`：`is_party_member`、`summoner`（阵营判定，`feature.cpp:82-108`）、`pool_slot`、`max_hp`、`hp`（半血账本与钳制，`feature.cpp:198-231`）。
- `ctx.sdk->log.write`：告警（阵营依赖缺失、池槽定位失败、未知 resource id）与 enable 状态日志（`feature.cpp:29-52,236-239,263`）。
- `ctx.sdk->in_flight`：两个 wrapper 的 `InFlightGuard`（`feature.cpp:159-161,182-184`）。
- `ctx.ports[0]`：`QOL_PORT_CHARACTER_EXTRAS` v1.2，安装期要求 `port_count == 1`（`plugin.cpp:74-82`）。
- 不消费 `hooks.set_intercept`、命令、票据、面板或 UI 能力。

## 职责边界

- 本插件拥有：两个 wrapper 的伤害/生命缩放策略、阵营三态判定编排、半血幂等账本、插件门禁、在途保护与资源声明。
- `core.character` 拥有：`character.facts` provider（`type`、`is_active_player_group`）与角色状态读取（`plugins/core.character/plugin.cpp:177-206`）。
- Game Adapter 拥有：`C_TYPE`/`C_MAX_HP`/`C_HP` 偏移读取、池槽计算、`CHAR_AddDamage`/`CHAR_UpdateAttrFromMonster` 符号解析与 hook 安装机制（`game/access/char.cpp`、`game/adapter_ports/character_extras_port.cpp`、`host/plugin_registry.cpp`）。
- Host 拥有：feature 持久化与 desired/actual 状态机、配置控制端点、资源仲裁与生命周期（`docs/reference/api-reference.md` §7.6/§7.7）。
- Kotlin 侧拥有：`simple_mode` runtime effect 订阅与屏幕水印呈现；插件不通过 JNI 主动更新 UI（`java/com/inotia4/qol/PluginRuntimeEffects.kt:58,94-97,197-208`）。

## 生命周期与线程模型

- `plugin_install`（`plugin.cpp:71-99`）：进入即 `set_gate(false)`；写入 `g_sdk`/`g_game`，清空 port 与 facts；校验 `PluginContext` 尺寸、SDK major、`ports` 数量与指针；缓存 port；require `character.facts`。成功也只缓存 facts，门禁保持关闭；所有失败出口经 `clear_install_state()` 清空全部全局与门禁（`plugin.cpp:63-69,74-97`）。
- `plugin_on_enabled`（`plugin.cpp:109-112`）：`set_gate(true)`。宿主在两个必需资源安装事务完成后调用；因此 install 返回后、资源未就绪期间 wrapper 不会生效。
- `plugin_on_disabled` / `plugin_deactivate`（`plugin.cpp:101-106,114-117`）：`set_gate(false)`；deactivate 还清空 facts 与 port。资源由宿主事务释放。
- feature 回调（`plugin.cpp:128-131`）：`enabled == 0` 时立即关门禁，覆盖 feature 关闭到资源 drain 完成之间的窗口；`enabled == 1` 时不在这里开门。
- `plugin_fill_status`（`plugin.cpp:119-122`）：`enabled = gate_enabled()`，`error = nullptr`。
- `on_resource_ready`（`feature.cpp:252-265`）：按 resource id 把 backup 存入 `std::atomic<void*>`；未知 id 记一次告警后仍返回 0；本插件无失败路径。
- 线程模型：门禁 `std::atomic<bool>`、两个 backup `std::atomic<void*>` 可跨线程读写；`g_sdk`/`g_game`/`g_character_facts`/`g_character_extras` 为普通全局指针，安装与 deactivate 在宿主生命周期线程写，wrapper 在游戏线程读（`feature.h:23-25`）。wrapper 入口先把这些指针取到局部并判空，避免 deactivate 清空后残留 hook 解引用空 port。半血账本 `g_half_marks[100]` 声明为仅游戏线程访问、无同步（`feature.cpp:133-148`）。

## 外部联动

### 屏幕水印

Kotlin 的 `PluginRuntimeEffects` 在启动时订阅 `simple_mode`（`java/com/inotia4/qol/PluginRuntimeEffects.kt:58,94-97`），只接受 `core.api` 与 `simple_mode` 两种 plugin id 的 effect（`:197-199`）。`simple_mode` 的效果把 `desiredEnabled` 交给 `ConfigPlatformEffects.onSimpleModeRuntimeEffect`（`:205-208`），后者按 `eventRevision` 去重并调用 `WatermarkOverlay.updateSimpleMode`（`java/com/inotia4/qol/ConfigPlatformEffects.kt:6-13`）。水印配色表命中「简单模式」时使用亮绿 `0xCC00FF00`（`java/com/inotia4/qol/patch/WatermarkOverlay.kt:65,99,102-106`）。

该联动使用 desired 状态而非 actual 状态，且只是平台侧视觉指示，不参与游戏内资源门禁；关闭 feature 时水印可能早于 hook 释放变色。

### 已删除的兼容面

2026-10-22 起：`nativeSetSimpleModeEnabled` JNI 导出、Kotlin `external` 声明、descriptor `kJni` 项，以及旧配置键 `simpleModeEnabled` 全部删除。旧 `config.json` 中的该键按未知键处理；当前启停/配置入口只有 feature `simple_mode.combat` 与 `/api/op/features/lifecycle`。

## 文件地图

| 文件 | 职责 |
|---|---|
| `plugins/simple_mode/plugin.cpp` | descriptor、requires/resources/ports/features、install/deactivate/启停/status |
| `plugins/simple_mode/feature.cpp` | 两个入口 wrapper、阵营三态判定、半血账本、门禁与 backup、在途保护、诊断日志 |
| `plugins/simple_mode/feature.h` | 业务层声明、机制与线程模型说明、resource id 常量 |
| `plugins/simple_mode/rules.cpp` / `rules.h` | 纯规则：阵营→伤害百分比、百分比缩放、半血取整 |
| `plugins/simple_mode/settings_contribution.cpp` / `.h` | 旧 settings catalog contribution 的空实现（`register_settings` 恒返回 `ok()`） |
| `plugins/simple_mode/CMakeLists.txt` | `qol_plugin_simple_mode` 静态库 |
| `java/com/inotia4/qol/PluginRuntimeEffects.kt` | `simple_mode` runtime effect 订阅与分派 |
| `java/com/inotia4/qol/ConfigPlatformEffects.kt` | desired 状态 → 水印效果 |
| `java/com/inotia4/qol/patch/WatermarkOverlay.kt` | 简单模式水印配色规则 |

## 测试

Host target `simple_mode_rules_tests`（`tests/CMakeLists.txt:497-505`）只链接 `rules.cpp` 与 `tests/test_simple_mode_rules.cpp`：

- 伤害矩阵：3×3 阵营组合逐条固定（受害者玩家侧一律 50；玩家打怪 150；其余 100）。
- `scale_by_percent`：整数取整方向、下限 1、非法/零值原样返回、`INT32_MAX` 钳制。
- `halved_max_hp`：偶数、奇数、0/1/2、负数下限 1。

Host target `simple_mode_feature_tests`（`tests/CMakeLists.txt:2186-2196`，`tests/test_simple_mode_feature.cpp`）编译 `feature.cpp`/`plugin.cpp`/`rules.cpp`，覆盖：

- descriptor：requires/resources/feature，`jni_count == 0`。
- install 失败出口（port 数量、require 失败、require 指针为空、require 返回空）与成功路径门禁时序。
- 伤害 wrapper：门禁透传、玩家/怪物/中立矩阵、未知阵营不缩放、backup 为空、非正伤害、InFlight enter/leave。
- 最大生命 wrapper：半血写回、HP 钳制、幂等、raw 变化重算、玩家召唤物、未知阵营、非最大生命属性、池槽越界、关闭门禁、backup 为空。
- deactivate 后残留 wrapper 不崩溃；feature `on_toggle(0)` 立即关门禁。

`settings_contributions_tests` 编译 `plugins/simple_mode/settings_contribution.cpp`（`tests/CMakeLists.txt:2066`），覆盖范围是该文件返回 `ok()` 的空实现。

未覆盖：真实设备 hook ABI/符号、资源安装/释放的真实 drain 时序、属性脏位与血条/死亡通知、深层召唤链在目标变体上的实际归属、runtime effect 与水印联动。历史设备验收记录见 `docs/refactor/verification.md` VM-P2-01、VM-P3-01、VM-P3-03、VM-K2-03、VM-K4-02；这些记录早于当前实现，不能替代当前真机验证。
