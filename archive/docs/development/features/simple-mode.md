# 简单模式（simple-mode）

> 状态：已实现并真机验证（monster v23；三项行为 + 队伍负对照全部拿到真机对照数据）｜位置：`module/app/src/main/cpp/feature/simple_mode/`
> 开关：模块设置页「简单模式」（page 1）／`POST /api/config/set {"simpleModeEnabled":true}`

## 1. 语义

开启后：

| 行为 | 规则 |
|---|---|
| 怪物最大生命 | 减半（×0.5） |
| 玩家侧打敌人 | 伤害 +50%（×1.5） |
| 玩家侧受到伤害 | −50%（×0.5） |
| 怪物互殴 / 涉及 NPC 与装饰物 | 不变（×1） |

「怪物最大生命减半」是**永久性属性语义**：不只在生成瞬间改一次，而是在属性重算的每条路径上保持（生成、升级、装备、buff、难度缩放）。关闭开关后不做主动回滚，下一次属性重算会写回满值。

## 2. 落点与机制

两个落点都用 **LSPosed 官方 Native Hook API**（architecture §2.2.1 第五档）单点包裹，因为前四档都覆盖不了这两个控制点：

- `CHAR_AddDamage` 只被直接 `bl` 调用（15 个静态调用点，无函数指针槽）→ `PtrHook` 不适用；逐点指令 patch 需要维护 15 个点，其中 `CHAR_AddDamageByDefaultCheck` 内的 2 处是**尾调用 `b`**，现有 `call_patch_install_bl` 不支持。
- `CHAR_UpdateAttrFromMonster` 同理无槽位可用。

### 2.1 伤害倍率：包裹 `CHAR_AddDamage` 入口

`CHAR_AddDamage(x0=攻击者, x1=受害者, w2=最终伤害, w3=标记(1=暴击/8=DOT), w4=0物理/非0法术)`

选它的理由是它是**全部伤害路径的唯一汇合点**：函数体第 3 条语句即 `neg w1,w2; mov x0,受害者; bl CHAR_AddLife`（唯一的扣血点，VMA 0xea330），因此入口即「生效前最后一刻」。普攻 / 物理技能 / 法术 / DOT / 怪物动作 / 防御计算兜底共 15 处静态调用点全部经此单点，缩放后的值会一致流入扣血、承伤记录（`CHAR_SetAttribute(ch,0x63,伤害)`）、仇恨与飘字。

只改 `w2` 一个入参，其余 4 个参数原样透传。

### 2.2 最大生命：包裹 `CHAR_UpdateAttrFromMonster`

`CHAR_UpdateAttr` 对 `C_TYPE==1` 必然分派到 `CHAR_UpdateAttrFromMonster`（VMA 0xe0048），而属性脏位（`CHAR_SetAttrUpdated`/`CHAR_IsAttrUpdated`）驱动的是**惰性重算**，所以任何触发重算的路径都会经过这里。包裹后先调 backup，返回后对 `attr==0x1e`（最大生命）做减半，并同步把当前 HP 钳到新上限。

生成路径的时序也吻合：`GENSYSTEM_ProduceMonster`/`CHARSYSTEM_Produce` 之后会调 `CHAR_UpdateAttr(ch,0x1e)`，其「生成结算」步再把 `C_HP = CHAR_GetAttr(ch,0x1e)`，于是新生成的怪自动带半血上限。

### 2.3 幂等账本（必须，否则累积减半）

`CHAR_UpdateAttrFromMonster` 是「**读旧槽值 → 变换 → 写回**」的幂等调整层，不是从默认值重算：函数体内唯一属性槽写点 `e011c: str w20,[x19,#0x24]`，而 `w20` 的初值读自同一槽 `e008c: ldr w20,[x0,#0x24]`，之后只经过召唤继承、难度缩放、上限钳制三类变换。

因此对槽值再做一次非幂等变换会随每次重算累积（1/2 → 1/4 → 1/8）。`simple_mode.cpp` 用**按角色池槽的账本**保证幂等：

- 槽值 == 账本记录的上次半值 ⇒ 本次只是把我们的结果原样写回 ⇒ 跳过；
- 槽值不同 ⇒ 游戏给出了新的满值 ⇒ 重新减半一次；
- 槽指针变化（槽被新角色占用）⇒ 重置该槽账本。

无法定位池槽时**不改写**（fail-safe 方向是「怪物保持满血」，而不是冒累积减半的风险）。

## 3. 阵营判定

判定式（`simple_mode.cpp` 的 `is_player_side`）：

```
is_player_side(ch) =
  ch != null
  && ( CHAR_GetPartyIndex(ch) != -1          // 主角 + 2 名队友
    || CHAR_IsActivePlayerGroup(ch) != 0     // 主控本人 + 主控的召唤物
    || (s = CHAR_GetSummoner(ch)) && s != ch && is_player_side(s) )   // 队友的召唤物（深度 ≤ 4）
```

三个原语各自的缺口与互补关系：

| 原语 | 覆盖 | 缺口 |
|---|---|---|
| `CHAR_GetPartyIndex` | 主角 + 2 名队友 | 不含任何召唤物 |
| `CHAR_IsActivePlayerGroup` | 主控本人 + 主控的召唤物 | 不含队友 |
| `CHAR_GetSummoner` | 反查召唤者 | 需递归才能补齐「队友的召唤物」 |

`C_TYPE(ch+0x09)` 语义已定案：**1 = 怪物**（`GENSYSTEM_ProduceMonster` 尾部扫池以 `ldrsb w1,[ch+0x9]; cmp w1,#1` 筛怪；`CHAR_UpdateAttr` 对 type==1 分派到 `CHAR_UpdateAttrFromMonster`），`0 = 玩家侧角色`，`2 = NPC/装饰物`。`game_symbols.h` 原注释「0=英雄 1=佣兵」是错误口径，已纠正。

`C_TYPE==1` 用在两处：伤害侧限定「玩家打怪物」才加伤（打 NPC/装饰物不加），半血侧限定只处理怪物。

## 4. mod 跳板跟随（全版本通用的关键）

`CHAR_AddDamage` 的入口指令形态逐版不同：

| 构建 | 入口首指令 |
|---|---|
| 原版 v1.3.2、大修 20260704 / 20260830 | `stp x29,x30,[sp,#-0x40]!`（干净序言，可直接 hook） |
| monster v20 | `b 0x14e280`（跳板，目标在 `.text` 内） |
| monster v23 / v25 / v26 / v27 | `b 0x7450b8`（跳板，目标在第二可执行段） |

inline hook 若需重定位一条 26 位 `b`，跨 ±128MB 的偏移会失效。因此 `hookable_entry()` 在安装前读入口首字：若是无条件 `b`（`(w & 0xFC000000) == 0x14000000`），解码 26 位有符号立即数并跟随一次，目标须 4 字节对齐且落在已加载的可执行映射内（`game_memory_accessible(..., 'x')`）。

跟随是语义等价的：跳板入口先 `stp x0..x8` 保存 x0-x7 再修改寄存器，故跳板目标处 ABI 与函数入口完全一致；且该跳板**专用于 `CHAR_AddDamage`**（`b 0x7450b8` 在 `.text` 全量反汇编中仅 1 处引用）。

`CHAR_UpdateAttrFromMonster` 在**全部 8 个已核对构建**中入口都是干净序言，无需跟随。

下表 5 个符号在 原版 v1.3.2、monster v20/v23/v25/v26/v27、大修 20260704/20260830 共 8 个构建中 **VMA 与 size 逐字节一致**（`llvm-objdump -T/-d` 核对），运行期仍优先按 `.dynsym` 符号名解析：

| 宏 | 符号 | VMA |
|---|---|---|
| `F_CHAR_ADD_DAMAGE_VMA` | `CHAR_AddDamage` | 0xea2d8 |
| `F_CHAR_UPDATE_ATTR_FROM_MONSTER_VMA` | `CHAR_UpdateAttrFromMonster` | 0xe0048 |
| `F_CHAR_GET_PARTY_INDEX_VMA` | `CHAR_GetPartyIndex` | 0xdca10 |
| `F_CHAR_IS_ACTIVE_PLAYER_GROUP_VMA` | `CHAR_IsActivePlayerGroup` | 0xe6d1c |
| `F_CHAR_GET_SUMMONER_VMA` | `CHAR_GetSummoner` | 0xdb730 |

## 5. 开关链路

```
设置页「简单模式」（feature/ui/game_ui_settings.cpp 的 kSettingsItems，page 1）
  → ModuleConfigUiBridge.toggleConfig("simpleModeEnabled")
  → ModuleConfig.apply → ConfigApiService.applyOnChange
  → NativeBridge.nativeSetSimpleModeEnabled(bool)
  → gamebridge_simple_mode.cpp（JNI 薄层）
  → simple_mode_set_enabled(std::atomic<bool>)   ← 热路径只读该原子，不读文件、不反调 Kotlin
```

`POST /api/config/set {"simpleModeEnabled":true}` 与设置页走的是同一条链路。

其余接入点：`NativeBridge.kt` external 声明、`ModuleConfig.kt`（默认值 / 读取 / apply / toJson / 缺字段补写检查）、`ConfigController.kt`（old 值捕获）、`core/native/qol_log.{h,cpp}` 的日志域 `kSimpleMode`（token `simple_mode`）+ Kotlin `LogDomain.SIMPLE_MODE`。

## 6. 失败与降级语义

- 符号未解析或 hook 安装失败：**fail-closed，不安装、不半装**，并记 `ERROR` 日志；开关仍可切换但不会有效果。
- 阵营判定依赖（`CHAR_GetPartyIndex`/`CHAR_IsActivePlayerGroup`/`CHAR_GetSummoner`）缺失：一律判为「非玩家侧」，方向是**不发加成**（绝不把敌人误判成友方，否则会变成「玩家挨打不掉血」）；只记一次 `WARN`。
- 无法定位角色池槽：跳过该次半血改写（见 §2.3）。
- 开关关闭：不主动回滚，下一次属性重算写回满值。

## 7. 线程模型

两个 wrapper 只在游戏线程被调用（伤害结算与属性重算都在游戏主循环路径上）。wrapper 内会调游戏查询函数做阵营判定——这三个函数是纯读、无写回副作用，但**本域任何入口都不得在非游戏线程调用**（与 `CHAR_GetAttr(0x1e)` 的禁用理由同类，见 architecture §9.6 / `docs/history/hp-clamp-offthread-incident.md`）。

开关由 JVM 线程写、游戏线程读，用 `std::atomic<bool>`。

## 8. 验收证据（真机，monster v23）

设备：`192.168.3.54:5555`；游戏 APK md5 `d5583f2f7806e2433f6bff9453afc3c7` = `Inotia4_v1.3.2_monster_v23.apk`。

### 8.1 hook 安装

```
simple_mode simple_mode.cpp:274 hooks installed: damageTarget=trampoline maxHpTarget=entry
```

`damageTarget=trampoline` 证明跳板跟随在真机上被正确触发（v23 的 `CHAR_AddDamage` 入口确实是 `b 0x7450b8`）；`maxHpTarget=entry` 与版本矩阵预测一致。

### 8.2 ① 怪物最大生命减半

练习01（map 19）Lv5 黏液怪，同一目标类型：

| simpleMode | 场上目标 hp |
|---|---|
| ON | **300** |
| OFF（离开重进图重新刷怪） | **600** |

精确 1/2。箱子（`C_TYPE==1` 但 hp=1）不受影响（下限保 1）。

### 8.3 ② 打敌人伤害 +50%

由较弱队友「奥罗贝」(role=1) 出手（凯恩单次伤害 ≥300，会一击清空已被减半到 300 血的目标，读不出数值）。

**同批对比**（关键方法）：倍率作用在**最终伤害**（已过防御）上，换一种怪就等于换了防御、两轮不可比——首轮探针在 ON 阶段靠自然刷怪换成了另一种怪（hp 1156），测出的 76 与 OFF 的 124 无法比较。因此改为两轮打**同一批**怪：开关翻转不会让已存在的怪重算最大生命，故目标保持一致。

| simpleMode | 单次普攻伤害样本（黏液怪 Lv5） |
|---|---|
| OFF | 124, 124, 124, 124（另有 1 次 62 的样本，来自采样窗口内的其它来源命中，不影响判定） |
| ON | **186, 186, 186, 186, 186**（同批的 Lv8 目标亦为 186） |

精确 ×1.5（124 × 1.5 = 186）。

### 8.4 ③ 受到伤害 −50%

Lv5 黏液怪普攻直击「凯恩」：

| simpleMode | 单次受击 |
|---|---|
| OFF | 86, 124 ×6 |
| ON | **62** ×7（样本中的 124 是两次 62 落在同一采样间隔内） |

精确 ×0.5。

### 8.5 负对照：玩家侧不受影响

同一会话内，`simpleModeEnabled=true` 时队伍快照不变：凯恩 8280/8280、奥罗贝 3960/3960、马雷斯 3576/3576。证明阵营判定正确地把主角/队友排除在「怪物最大生命减半」之外。

### 8.6 host tests

`simple_mode_rules_tests`：阵营 3×3 全矩阵、倍率、取整与边界钳制（含 `scale_by_percent` 溢出保护与 `halved_max_hp` 下限 1）。全部 16 个 host test 目标通过。

## 9. 已知限制

- 伤害加成的叠加次序：monster 版的 mod 自身已经在 `CHAR_AddDamage` 路径上改伤害（其 seg2 跳板在 `bl 0x503c` 后用返回值覆写 w2）。我们的缩放发生在**其之前**（作为该函数的输入），真机实测最终伤害确为 ×1.5（§8.3），即该 mod 变换保留了我们的缩放。
- 半血账本的槽复用边界：若新怪在同一槽上恰好拥有「等价于上一只怪半血值」的满血值，该次会被跳过而保持满血；方向是怪略强，不影响正确性。
- 「受到伤害 −50%」同样作用于玩家侧召唤物（它们被判为玩家侧）；「打敌人 +50%」同样作用于玩家侧召唤物造成的伤害。

## 10. 相关文件

| 文件 | 作用 |
|---|---|
| `feature/simple_mode/simple_mode.cpp` | 两个 wrapper、阵营判定、跳板跟随、幂等账本、安装 |
| `feature/simple_mode/simple_mode_rules.{h,cpp}` | 纯逻辑：阵营 → 倍率、整数缩放与钳制（host 可测） |
| `feature/simple_mode/simple_mode.h` | 域入口：开关、状态 JSON、安装 |
| `bridge/native/gamebridge_simple_mode.cpp` | JNI 薄层 |
| `data/native/game_symbols.h` / `symbol_registry.h` / `game_access.{h,cpp}` / `game_access_globals.inc` | 5 个新符号的登记四处 |
