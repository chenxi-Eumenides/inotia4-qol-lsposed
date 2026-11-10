# `world_tuning` 插件参考

## 摘要

功能插件「游戏世界调整」，插件版本 `0.1.0`。设置页只有一个入口行；点击后启用插件并打开自有面板，面板内每行一个倍率，左右箭头在档位间调整；OP 关闭时使用基础档位，OP 开启时使用扩展档位。改动经 Host 配置 Store 持久化，写入 `config.json` 的 `world_tuning` namespace。

当前真正接线的倍率有九项：

- 金币倍率：缩放 `INVEN_AddMoney` 的增量；
- 经验倍率：缩放玩家侧 `CHAR_AddExperience` 的增量；
- 敌怪血量倍率：缩放怪物最大生命；
- 伤害倍率：缩放玩家侧对怪物造成的伤害；
- 承伤倍率（配置键 `damage_in`）：缩放玩家侧受到的伤害；
- 掉落数量倍率（配置键 `drop_quantity`）：整数 0..INT32_MAX，重复整次 `CHARSYSTEM_DropItem` 结算；
- 掉落重抽次数（配置键 `drop_reroll`）：整数 0..INT32_MAX；已核验比较点的概率阈值按 K² 放大，槽内造物仍按 K 候选重掷取最优；
- 药水冷却倍率（配置键 `potion_cooldown`）：缩放玩家药水冷却；
- 技能冷却倍率（配置键 `skill_cooldown`）：缩放玩家技能冷却。

插件 id 由旧 `simple_mode` 改为 `world_tuning`，旧 `simple_mode.combat` feature 与旧配置不再迁移。除特别说明，文中路径均相对 `module/app/src/main/cpp/`。

## 定位与 descriptor

- plugin id `world_tuning`，注册于 `host/assembly.cpp`。
- `requires` 1 项：`character.facts` v1.0。
- `optional_requires` 3 项：`ui_panels` v1.3、`ui_components` v2.23、`item.use` v1.0；界面工具被停用时倍率仍生效，仅面板不可用；`item.use` 缺失时只影响药水冷却。
- `ports` 1 项：`QOL_PORT_CHARACTER_EXTRAS` v1.3。
- `provides` 0；JNI 0、HTTP route/endpoint 0。
- `resources` 9 个入口 hook（含动作冷却最终写入）与 4 个掉落 CMP `kInsnPatch` claims，均为 `kOptional` + `kFailSoft`：单个落点不可用只影响对应行为，不会把插件置 `failed`。`hook:CHAR_AddDamage` / `hook:CHAR_SetActionNextTime` 声明 `follow_trampoline=true`；monster v29 的 mod 会周期性重写伤害入口，冷却入口则落到隐藏段目标。
- `validate_config` = `validate_rates_config`，只接受 `rates` 键。
- 唯一 feature 为 entry（`plugins/world_tuning/plugin.cpp`）：

| feature id | 默认 | 分类 | 类型 | 名称 | 描述 | 按钮 |
|---|---:|---|---|---|---|---|
| `world_tuning.panel` | 0 | Gameplay | entry | 游戏世界调整 | 调整游戏世界的属性 | 调整 |

entry 默认 0：首次点击时设置页先提交 feature=true、等待 Host actual `enabled`，再调用 entry 回调打开面板；之后 feature 保持 true，插件常驻。

## 配置

单键 `rates`，值为整数对象：

```json
{"gold":100,"exp":100,"enemy_hp":50,"damage_out":150,"damage_in":50,"drop_quantity":1,"drop_reroll":1,"potion_cooldown":100,"skill_cooldown":100}
```

- 字段：`gold`、`exp`、`enemy_hp`、`damage_out`、`damage_in`、`drop_quantity`、`drop_reroll`、`potion_cooldown`、`skill_cooldown`；缺字段按各行默认值处理（百分比行默认 100，`drop_quantity`/`drop_reroll` 为 1）。
- 百分比配置范围为 `0..INT32_MAX`（100 = 不修改），掉落数量和重抽范围均为 `0..INT32_MAX`。配置不限制档位上限；面板档位仅为操作预设，手写的合法非档位值原样生效。
- `gold`、`exp`、`damage_out`、`damage_in` 的 0 表示 ×0；`enemy_hp` 的 0 按 1e-6 缩放，结果下限为 1。`drop_quantity` 或 `drop_reroll` 为 0 时均不掉落；重抽值 1 不改变阈值，品质候选也只取首抽。
- 单字段的非整数、溢出或负数只令该字段回退默认值，其余字段继续生效；未知字段忽略。重复字段、JSON 语法错误及结尾多余内容仍拒绝整份配置。
- 旧配置兼容：历史 `move_speed` 键解析时跳过并忽略；`drop_quality` 作为 `drop_reroll` 别名，显式 `drop_reroll` 优先；重复旧键仍拒绝。
- 旧配置里的 `player_hp` 键作为未知字段忽略。
- `validate_config` 仍校验 `rates` 对象结构；越界字段已回退默认值，不会导致 Host 删除 `rates` 键。写入与 Store 加载两条路径均执行该校验（`host/features/controller.cpp`、`host/core_settings_config_store.cpp`）。
- `drop_quantity` 或 `drop_reroll` 超过 16 次时写一次 warning；不设置硬上限，仍按配置次数执行。概率阈值乘 K² 使用饱和运算，避免中间乘法溢出。
- 读取通道：插件经 `QolFeatureApi::read_config` 读自己的 namespace；写入经 `request_set_config`，面板每帧轮询一次 receipt。`/api/config/set` 改同一键，下次打开面板或插件重新启用时生效。
- 插件未订阅 `runtime_effects`；外部改动不会即时推送。

## 面板

面板 id `world_tuning`，provider 在 `plugin_install` 内注册（需要 `ui_panels` + `ui_components` 同时可用）。

- 960×640 root，单一装饰框 `{44,104,872,390}`；背景与设置页一致，先绘制 `draw_image_part` unit `0x4f`/`0x50`（中线左右各 300），再用 `draw_container_styled`（`ornamented=1`、`fill_rgb565=0x0000`、`alpha=60`，即黑色 60%）绘制边框与四角装饰；不绘制标题或说明行。迁移前使用的 `draw_container_ornamented` 已随 ui_components Major 2 移除。
- 框内左右两列各 400 宽、列间距 24；每列 5 行、每页最多 10 项，先填满左列再进右列。行高 76，框内上下各留 5；每行：名称、数值、左右 `arrow_button`；行间 2 逻辑单位金色分割线，行本身不绘制底色，每列最后一个可见行不画下分隔线。
- 框内不使用 `scroll_list` 或原生滚动。框下方居中放置左右翻页箭头与页码；当前 9 项单页（左 5、右 4），翻页按钮存在但禁用；项数超过 10 时自动分页，只创建当前页的箭头。
- OP 关闭时使用基础档位：百分比 `{10,30,50,80,100,120,150,200,300,500}`，掉落数量/重抽 `{1,2,3,4,5}`，冷却倍率 `{10,30,50,80,100,200,300,500}`。因此基础档位不提供 0。
- OP 开启时使用扩展档位：百分比从 0、10 开始，500 后为 1000、10000……100000000（最高显示 `×1000000.0`）；掉落数量/重抽从 0、1、2、3、4、5 开始，之后按 10 递增至 200；冷却倍率追加 0 与 1000（10x）。
- 百分比行显示 `×1.5` 形式（0 显示 `×0`）；掉落数量/重抽行显示整数。配置值无上限（int32 范围内），面板档位上限为百分比 100000000、掉落 200。
- 点击箭头 → 生成完整 `rates` JSON → `request_set_config` → 进入 pending（全部箭头禁用）→ 持久化确认后 `apply_rates`；成功不显示提示，失败/状态未知才在底部显示错误并回滚。
- 面板打开时重新读取配置，覆盖外部编辑；关闭/插件停用销毁全部控件与回调。

## QOL 水印

Kotlin 侧 `PluginRuntimeEffects` 订阅 `world_tuning` 的 Host runtime effect，从 `config_json` 的 `rates` 读取五项参与难度判定的倍率（缺字段按 100）：

- 偏简单：`damage_in < 100` 或 `enemy_hp < 100` 或 `damage_out > 100` 或 `potion_cooldown < 100` 或 `skill_cooldown < 100`；
- 偏困难：`damage_in > 100` 或 `enemy_hp > 100` 或 `damage_out < 100` 或 `potion_cooldown > 100` 或 `skill_cooldown > 100`。

承伤倍率、敌怪血量与两项冷却倍率越低越简单，伤害倍率越高越简单。插件 desired 启用时，任意一项偏简单显示绿色 `0xCC00FF00`；否则任意一项偏难显示紫色 `0xCCAA00FF`；五项均为 100 或插件未启用时保持默认半透明白色。模块健康异常（红色）优先于以上颜色。其余倍率（金币/经验/掉落数量/掉落重抽）不参与水印判定。面板写入与 `/api/config/set` 都经同一 runtime effect 刷新水印。

## 游戏内行为

| 资源 id | 符号 | 用途 |
|---|---|---|
| `hook:CHAR_AddDamage` | `F_CHAR_ADD_DAMAGE_VMA` | 伤害唯一汇合点；入口是 `b` 时跟随到隐藏段目标再改写 `w2` 入参 |
| `hook:CHAR_UpdateAttrFromMonster` | `F_CHAR_UPDATE_ATTR_FROM_MONSTER_VMA` | 怪物最大生命重算层，`attr==0x1e` |
| `hook:CHAR_AddExperience` | `F_ADD_EXP_VMA` | 加经验统一入口 `int(void* ch, int32 amount, uint8 flag)` |
| `hook:INVEN_AddMoney` | `F_ADD_MONEY_VMA` | 加金币增量入口 `int(int64 amount)` |
| `hook:CHARSYSTEM_DropItem` | `F_CHARSYSTEM_DROP_ITEM_VMA` | 死亡掉落整次结算入口 `void(void* ch)`；数量倍率重复调用 |
| `hook:ITEMSYSTEM_MakeItem` | `F_MAKE_ITEM_VMA` | 掉落槽内造物 `void*(category, lookup_key, flag)`；重抽按类别过滤 |
| `hook:ITEMSYSTEM_MakeEquipDetail` | `F_MAKE_EQUIP_DETAIL_VMA` | 装备候选选取 `void*(kind,min,max,item,option)`；重抽只按稀有度 |
| `hook:CHAR_GetActionCoolTime` | `F_CHAR_GET_ACTION_COOLTIME_VMA` | 技能冷却时长查询 `int(void* ch, void* action)`；不在 `SetActionNextTime` 链内时返回缓存的已缩放总时长，供快捷栏饼使用 |
| `hook:CHAR_SetActionNextTime` | `F_CHAR_SET_ACTION_NEXT_TIME_VMA` | 动作冷却结束时间写入 `void(void* ch, void* action)`；唯一缩放点，按最终 `nextTime - now` 缩放并缓存总时长，覆盖 monster 的固定值覆盖 |

经验：入口对 `ch` 做阵营判定，只缩放玩家侧（主角/队友/召唤物）；负数原样透传，倍率 0 结果为 0。

金币：`INVEN_AddMoney` 是增量入口（出售、商店/网络商店、存档相关回补等）；`EVTSYSTEM_Process` 的任务奖励走 `INVEN_SetMoney` 写绝对值，不在本倍率覆盖范围内。

阵营判定沿用三态：主角/队友/主控或队友的召唤物 → 玩家侧；`C_TYPE==1` 且明确非玩家侧 → 怪物；其余或依赖失败 → 中性（不缩放）。

伤害：

| 条件 | 倍率 |
|---|---|
| 受害者玩家侧 | `damage_in` |
| 攻击者玩家侧且受害者怪物 | `damage_out` |
| 其余 | 100 |

`scale_by_percent`：`value<=0` 或倍率 100 原样返回；倍率 0 返回 0；其他正倍率以 int64 计算并钳到 `[1, INT32_MAX]`。

冷却倍率：

- 药水：订阅 `core.item` 的 `item.use`。回调只处理 `result != 0` 且 `character == 当前主控`；读取 `g_nGlobalCoolTimeCount` 与 `CHECKTIME_nPotionTick`，当 `tick > now` 时把差值按 `potion_cooldown` 缩放后写回。这样不会影响 Buff 道具的 15 tick 冷却，也不会误伤佣兵/怪物。
- 技能：只有一个缩放数据源 `CHAR_SetActionNextTime`。wrapper 让原函数完整跑完所有分支/跳转后，读取节点 `action+0x10` 的最终 `nextTime`，按 `nextTime - now` 缩放并写回；同时把最终总时长缓存到 `action -> total`。`CHAR_GetActionCoolTime` 不再参与缩放：在 `SetActionNextTime` 调用链内返回原值，供真实计算使用；其他调用（快捷栏冷却饼）返回缓存的总时长，保证实际冷却与饼一致。monster 变体在原始计算后直接覆盖固定冷却值时，也由最终值兜底缩放。`CHAR_SetActionNextTime` 声明 `follow_trampoline=true`，入口是 `b` 时落到隐藏段目标。
- 两者倍率 0 表示无冷却；正倍率结果下限为 1 tick。`scale_cooldown_ticks` 用于 int64 tick 差值，`scale_by_percent` 用于技能 int32 返回值。

monster v29 的 `CHAR_AddDamage` 入口是 `b <隐藏段目标>`，且 mod 会在 hook 安装后约 5 秒内把入口字写回同一个 `b`。本插件对该资源声明 `follow_trampoline=true`：安装时解码入口 `b` 的 imm26，把 hook 落在隐藏段目标上，入口被还原不影响已落在目标上的 hook。目标首条是短桩类控制流（`b`/`bl`/`b.cond`/`cbz`/`tbz`/`br`/`blr`/`ret`）时解析层拒绝跟随并回退入口短补丁（DropItem 形态），避免覆盖「桩返回入口+8」的落点。

敌怪最大生命：

- 只处理 `kMonster`，且池槽可定位（0..99）；定位失败只告警一次并保持游戏原值。
- 账本按池槽保存 `{ch, raw, applied}`。进入时若槽值仍等于上次 `applied`，先把 `raw` 写回槽再调用原函数；原函数返回后读取游戏结果，按 `enemy_hp` 缩放并记录新的 `raw/applied`。这样倍率变化在下一次属性重算时生效且不会累积缩放。
- 缩放后若当前 HP 高于新上限，同步写回 HP 上限值。
- 倍率 100 时不缩放；若上次有缩放，先把 raw 写回槽再透传，之后清空 applied。
- `enemy_hp` 倍率 0 按 raw / 1,000,000 计算，正数结果下限为 1。

掉落数量：

- 入口 `CHARSYSTEM_DropItem` 是整次结算（12 个槽逐个独立判定，可同时命中多个槽）。数量倍率 `N` 表示同步调用原函数 `N` 次；`0` 表示直接返回、不掉落。
- `drop_reroll=0` 同样在入口直接返回、不掉落；该检查先于槽内 maker 调用。
- 倍率是**相对该变体原版行为**：C18/C18L 自带每击 3 轮槽扫描，`×2` 实际为 6 轮；大修/monster 的阈值补丁同样保留。
- `drop_reroll=K` 还有独立的概率阈值语义：在 `CHARSYSTEM_DropItem` 函数偏移 `+0x258/+0x3b8/+0x400/+0x460` 四个已核验 MATH_GetRandom 后 CMP 点，roll 原样保留，只在比较前把 threshold 变换为 `min(threshold×K², max_roll+1)`；threshold≤0 与 K=1 原样透传。乘法饱和，概率最多 100%；CMP 原字重放后由原 `b.cond` 消费 NZCV。
- 四条 CMP claim 只对明确 allowlist 生效：原版 v1.3.2、C18 v586/v514/v520/v603、monster v1.3.2/v23/v29、盗版大修 20260830。原版/C18 roll 上限为 9999，threshold cap 为 10000；monster/大修前三点 roll 上限为 2999（cap 3000），装备点为 5999（cap 6000）。unknown、fantasy 与未核验版本保持原版比较，不猜测地址/上限；每条 claim 独立 fail-soft。
- monster 变体的入口被改写成 `b 0x1b58f8`，宿主的 `resolve_hook_entry` 会跟随该跳转后再装 hook，专用掉落（id 554/558）仍会执行。

掉落重抽：

- 只在掉落上下文内生效（`DropItem` wrapper 置 thread_local 计数），商店/合成/开箱等同一批 maker 调用不受影响。
- 上述阈值 K² 与品质重掷 K 是两种同时生效的语义：每个实际掉落槽仍取 K 个候选（首抽 + K−1 次重掷），保留既有类别/稀有度比较；阈值扩张不改 roll，也不新增随机源调用。
- `MakeItem`（槽内造物）：首抽为**药水（subtype2）/材料（subtype4）**时直接确定；首抽为**卷轴/宝石（subtype3）**或**背包/骰子（subtype5）**时，重掷 `K-1` 次，只接受**同一小类别**的结果并按物品等级取最高；没有同类别结果时保留首抽。徽章/技能书/神灯等 subtype5 其余物品不参与。
- `MakeEquipDetail`（槽 2 普通装备）：`kind` 与 `[min,max]` 等级段由上游固定，重掷只按**稀有度**取最高；同稀有度保留首抽。不使用装备需求等级作为评分，避免把掉落推向玩家穿不上的装备。
- 类别读取走 `QolGameHost.item_category`：C18 按 12bit 物品 id（`bits[6..16) | bits[1:0]<<10`）解码，其余变体（含 unknown）按原版 10bit。
- 重掷产生的非保留物品经 `inventory_write.free_item` 立即释放；`read_only.item.*` 或 `free_item` 不可用时直接透传（fail-soft）。

## 生命周期与线程模型

- `plugin_install`：校验 context/port/SDK → require `character.facts` → 缓存 `sdk/game/port/facts/owner/plugin_id` → 可选注册面板 provider → 可选订阅 `item.use` → 读取 `rates`。
- `plugin_on_enabled`：开门禁、重试面板/`item.use` 绑定并重新读取 `rates`。`item.use` 未就绪时注册 30 帧重试任务，capability 发布后自动订阅并停止任务。
- `plugin_on_disabled`：关门禁并解绑 `item.use`。
- `plugin_deactivate`：关门禁 → 解绑 `item.use` → 注销面板 owner → 解绑 UI → 保留 `sdk/game`（残留 wrapper 仍需要 in-flight 与 backup），清空 facts/port。
- 门禁与倍率为原子变量，游戏线程读、主线程写；HP 账本与掉落上下文（thread_local）仅游戏线程访问。
- 面板未绑定或 `ui_panels` 不可用时，entry 返回 `kQolErrNotReady`，倍率不受影响。

## 文件地图

| 文件 | 职责 |
|---|---|
| `plugins/world_tuning/plugin.cpp` / `plugin.h` | descriptor、feature、install/deactivate、面板注册、配置读取 |
| `plugins/world_tuning/feature.cpp` / `feature.h` | 九个入口 wrapper、CMP helper、`item.use` 回调、阵营判定、HP 账本、掉落上下文与重抽、倍率原子值、配置读写 |
| `plugins/world_tuning/rates.cpp` / `rates.h` | 档位、字段/文案表、JSON 编解码与校验、常规及 K² 阈值缩放 |
| `plugins/world_tuning/panel.cpp` / `panel.h` | 面板 provider、双列分页布局、箭头/文本绘制、receipt 轮询 |
| `plugins/world_tuning/settings_contribution.cpp` / `.h` | 旧 catalog contribution 空实现 |
| `plugins/world_tuning/CMakeLists.txt` | `qol_plugin_world_tuning` 静态库 |

## 测试

- `world_tuning_rates_tests`（`tests/test_world_tuning_rates.cpp`）：基础/扩展档位、显示文本、JSON 往返、0 值缩放与 1e-6 敌怪血量、配置非负范围、越界值分字段回退、未知字段忽略、重复/语法错误拒绝、旧配置 `move_speed`/`drop_quality` 键兼容、`validate_config`；另覆盖 K=1、K²、100% cap、负 threshold 与高 K 饱和无溢出。
- `world_tuning_feature_tests`（`tests/test_world_tuning_feature.cpp`）：descriptor（13 资源、无 provides）、安装失败出口、门禁时序、伤害/经验/金币/血量 0 倍率、伤害矩阵、HP 缩放与幂等/倍率变更、阵营失败、残留 wrapper、entry 不可用分支；掉落数量 0/1/3/17 次调用与 warning、CMP helper 的 gate/TLS 隔离、重抽 0 不掉落、品质候选仍按 K 次调用及既有过滤/取最高/释放规则。
- `insn_patch_tests`（`tests/test_insn_patch.cpp`）：kind5 trampoline 保存/恢复寄存器与 LR、精确重放 CMP 后用 B 返回原 `b.cond`、expected-word 拒绝与物理还原。
- `settings_contributions_tests` 编译本插件的空 contribution。

面板绘制/命中与 entry 联动已有真机观察（见下）；未覆盖：真实设备 hook ABI（伤害/敌怪血量实际生效）、资源安装时序。

## 验证状态

Host 测试与 Debug 构建状态见 `docs/refactor/verification.md` 的 VM-WT-01；2026-10-29 真机（C18L v589）只覆盖设置页/面板/配置持久化，不覆盖新增 CMP ABI。概率阈值与品质候选的游戏内效果尚无新真机证据，按验收口径登记为 `PARTIAL / NOT_ACCEPTED`。QOL 水印配色由 `world_tuning` runtime effect 驱动，本批只覆盖 Debug 构建与纯函数判定，真机颜色切换未验收。
