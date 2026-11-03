# `world_tuning` 插件参考

## 摘要

功能插件「游戏世界调整」，插件版本 `0.1.0`。设置页只有一个入口行；点击后启用插件并打开自有面板，面板内每行一个倍率，左右箭头在固定档位间调整。改动经 Host 配置 Store 持久化，写入 `config.json` 的 `world_tuning` namespace。

当前真正接线的倍率有七项：

- 金币倍率：缩放 `INVEN_AddMoney` 的增量；
- 经验倍率：缩放玩家侧 `CHAR_AddExperience` 的增量；
- 敌怪血量倍率：缩放怪物最大生命；
- 伤害倍率：缩放玩家侧对怪物造成的伤害；
- 承伤倍率（配置键 `damage_in`）：缩放玩家侧受到的伤害；
- 掉落数量倍率（配置键 `drop_quantity`）：整数 0..5，重复整次 `CHARSYSTEM_DropItem` 结算；
- 掉落品质倍率（配置键 `drop_quality`）：整数 1..5，槽内造物重掷取同类别最优。

玩家血量倍率尚未接线（玩家/佣兵不走 `CHAR_UpdateAttrFromMonster`），面板行只保存数值，不产生游戏效果。面板行只显示名称、数值与箭头，不显示副标题。

插件 id 由旧 `simple_mode` 改为 `world_tuning`，旧 `simple_mode.combat` feature 与旧配置不再迁移。除特别说明，文中路径均相对 `module/app/src/main/cpp/`。

## 定位与 descriptor

- plugin id `world_tuning`，注册于 `host/assembly.cpp`。
- `requires` 1 项：`character.facts` v1.0。
- `optional_requires` 2 项：`ui_panels` v1.3、`ui_components` v2.23；界面工具被停用时倍率仍生效，仅面板不可用。
- `ports` 1 项：`QOL_PORT_CHARACTER_EXTRAS` v1.2。
- `provides` 0；JNI 0、HTTP route/endpoint 0。
- `resources` 7 个入口 hook（伤害、怪物最大生命、加经验、加金币、掉落结算、MakeItem、MakeEquipDetail），均为 `kOptional` + `kFailSoft`：单个落点不可用只影响对应倍率，不会把插件置 `failed`。
- `validate_config` = `validate_rates_config`，只接受 `rates` 键。
- 唯一 feature 为 entry（`plugins/world_tuning/plugin.cpp`）：

| feature id | 默认 | 分类 | 类型 | 名称 | 描述 | 按钮 |
|---|---:|---|---|---|---|---|
| `world_tuning.panel` | 0 | Gameplay | entry | 游戏世界调整 | 调整游戏世界的属性 | 调整 |

entry 默认 0：首次点击时设置页先提交 feature=true、等待 Host actual `enabled`，再调用 entry 回调打开面板；之后 feature 保持 true，插件常驻。

## 配置

单键 `rates`，值为整数对象：

```json
{"gold":100,"exp":100,"player_hp":100,"enemy_hp":50,"damage_out":150,"damage_in":50,"drop_quantity":1,"drop_quality":1}
```

- 字段：`gold`、`exp`、`player_hp`、`enemy_hp`、`damage_out`、`damage_in`、`drop_quantity`、`drop_quality`；缺字段按各行默认值处理（前六项 100，`drop_quantity`/`drop_quality` 为 1）。
- 百分比行只允许档位 `10/30/50/80/100/120/150/200/300/500`（即 ×0.1 … ×5.0）。
- `drop_quantity` 只允许 `0/1/2/3/4/5`（0 = 不掉落）；`drop_quality` 只允许 `1/2/3/4/5`（1 = 不重掷）。
- 未知字段、重复字段、非整数、档位外取值一律拒绝。
- 旧配置兼容：历史版本写入的 `move_speed` 键（已移除的移速倍率）解析时读取并忽略，不使整份 rates 非法；其取值仍须为合法档位，重复出现仍拒绝。
- `validate_config` 在写入与 Store 加载两条路径生效（`host/features/controller.cpp`、`host/core_settings_config_store.cpp`）。
- 读取通道：插件经 `QolFeatureApi::read_config` 读自己的 namespace；写入经 `request_set_config`，面板每帧轮询一次 receipt。`/api/config/set` 改同一键，下次打开面板或插件重新启用时生效。
- 插件未订阅 `runtime_effects`；外部改动不会即时推送。

## 面板

面板 id `world_tuning`，provider 在 `plugin_install` 内注册（需要 `ui_panels` + `ui_components` 同时可用）。

- 960×640 root，单栏容器 `{28,100,920,444}`；与设置页相同的 `kPanelBg` 底、`draw_container_styled`（`ornamented=1`）边框/内底和左右装饰图（`draw_image_part` unit `0x4f`/`0x50`）；不绘制标题与说明行。迁移前使用的 `draw_container_ornamented` 已随 ui_components Major 2 移除。
- 8 行，行高 71，使用 `scroll_list`（关闭选中态）；每行：名称、数值、左右 `arrow_button`；行底 2 逻辑单位金色分割线，与设置页行样式一致。
- 列表视口高 428，内容高 568（8 行 × 71），超出部分由 `scroll_list` 滚动；滚动条沿用原生 `scroll_list` 自带的一条，不额外自绘。
- 百分比行显示 `×1.5` 形式；掉落数量/品质行显示 `×0`..`×5` 整数。全部 8 行的箭头都可用、样式一致；未接线行（玩家血量）只保存数值，不产生游戏效果。
- 点击箭头 → 生成完整 `rates` JSON → `request_set_config` → 进入 pending（全部箭头禁用）→ 持久化确认后 `apply_rates`；成功不显示提示，失败/状态未知才在底部显示错误并回滚。
- 面板打开时重新读取配置，覆盖外部编辑；关闭/插件停用销毁全部控件与回调。

## 游戏内行为

| 资源 id | 符号 | 用途 |
|---|---|---|
| `hook:CHAR_AddDamage` | `F_CHAR_ADD_DAMAGE_VMA` | 伤害唯一汇合点，改写 `w2` 入参 |
| `hook:CHAR_UpdateAttrFromMonster` | `F_CHAR_UPDATE_ATTR_FROM_MONSTER_VMA` | 怪物最大生命重算层，`attr==0x1e` |
| `hook:CHAR_AddExperience` | `F_ADD_EXP_VMA` | 加经验统一入口 `int(void* ch, int32 amount, uint8 flag)` |
| `hook:INVEN_AddMoney` | `F_ADD_MONEY_VMA` | 加金币增量入口 `int(int64 amount)` |
| `hook:CHARSYSTEM_DropItem` | `F_CHARSYSTEM_DROP_ITEM_VMA` | 死亡掉落整次结算入口 `void(void* ch)`；数量倍率重复调用 |
| `hook:ITEMSYSTEM_MakeItem` | `F_MAKE_ITEM_VMA` | 掉落槽内造物 `void*(category, lookup_key, flag)`；品质重掷按类别过滤 |
| `hook:ITEMSYSTEM_MakeEquipDetail` | `F_MAKE_EQUIP_DETAIL_VMA` | 装备候选选取 `void*(kind,min,max,item,option)`；品质重掷只按稀有度 |

经验：入口对 `ch` 做阵营判定，只缩放玩家侧（主角/队友/召唤物）；负数/零原样透传。

金币：`INVEN_AddMoney` 是增量入口（出售、商店/网络商店、存档相关回补等）；`EVTSYSTEM_Process` 的任务奖励走 `INVEN_SetMoney` 写绝对值，不在本倍率覆盖范围内。

阵营判定沿用三态：主角/队友/主控或队友的召唤物 → 玩家侧；`C_TYPE==1` 且明确非玩家侧 → 怪物；其余或依赖失败 → 中性（不缩放）。

伤害：

| 条件 | 倍率 |
|---|---|
| 受害者玩家侧 | `damage_in` |
| 攻击者玩家侧且受害者怪物 | `damage_out` |
| 其余 | 100 |

`scale_by_percent`：`value<=0` 或 100 原样返回；int64 计算后钳到 `[1, INT32_MAX]`。

敌怪最大生命：

- 只处理 `kMonster`，且池槽可定位（0..99）；定位失败只告警一次并保持游戏原值。
- 账本按池槽保存 `{ch, raw, applied}`。进入时若槽值仍等于上次 `applied`，先把 `raw` 写回槽再调用原函数；原函数返回后读取游戏结果，按 `enemy_hp` 缩放并记录新的 `raw/applied`。这样倍率变化在下一次属性重算时生效且不会累积缩放。
- 缩放后若当前 HP 高于新上限，同步写回 HP 上限值。
- 倍率 100 时不缩放；若上次有缩放，先把 raw 写回槽再透传，之后清空 applied。

掉落数量：

- 入口 `CHARSYSTEM_DropItem` 是整次结算（12 个槽逐个独立判定，可同时命中多个槽）。数量倍率 `N` 表示同步调用原函数 `N` 次；`0` 表示直接返回、不掉落。
- 倍率是**相对该变体原版行为**：C18/C18L 自带每击 3 轮槽扫描，`×2` 实际为 6 轮；大修/monster 的阈值补丁同样保留。
- monster 变体的入口被改写成 `b 0x1b58f8`，宿主的 `resolve_hook_entry` 会跟随该跳转后再装 hook，专用掉落（id 554/558）仍会执行。

掉落品质：

- 只在掉落上下文内生效（`DropItem` wrapper 置 thread_local 计数），商店/合成/开箱等同一批 maker 调用不受影响。
- `MakeItem`（槽内造物）：首抽为**药水（subtype2）/材料（subtype4）**时直接确定；首抽为**卷轴/宝石（subtype3）**或**背包/骰子（subtype5）**时，重掷 `K-1` 次，只接受**同一小类别**的结果并按物品等级取最高；没有同类别结果时保留首抽。徽章/技能书/神灯等 subtype5 其余物品不参与。
- `MakeEquipDetail`（槽 2 普通装备）：`kind` 与 `[min,max]` 等级段由上游固定，重掷只按**稀有度**取最高；同稀有度保留首抽。不使用装备需求等级作为评分，避免把掉落推向玩家穿不上的装备。
- 类别读取走 `QolGameHost.item_category`：C18 按 12bit 物品 id（`bits[6..16) | bits[1:0]<<10`）解码，其余变体（含 unknown）按原版 10bit。
- 重掷产生的非保留物品经 `inventory_write.free_item` 立即释放；`read_only.item.*` 或 `free_item` 不可用时直接透传（fail-soft）。

## 生命周期与线程模型

- `plugin_install`：校验 context/port/SDK → require `character.facts` → 缓存 `sdk/game/port/facts/owner/plugin_id` → 可选注册面板 provider → 读取 `rates`。
- `plugin_on_enabled`：开门禁并重新读取 `rates`。
- `plugin_on_disabled`：关门禁。
- `plugin_deactivate`：关门禁 → 注销面板 owner → 解绑 UI → 保留 `sdk/game`（残留 wrapper 仍需要 in-flight 与 backup），清空 facts/port。
- 门禁与倍率为原子变量，游戏线程读、主线程写；HP 账本与掉落上下文（thread_local）仅游戏线程访问。
- 面板未绑定或 `ui_panels` 不可用时，entry 返回 `kQolErrNotReady`，倍率不受影响。

## 文件地图

| 文件 | 职责 |
|---|---|
| `plugins/world_tuning/plugin.cpp` / `plugin.h` | descriptor、feature、install/deactivate、面板注册、配置读取 |
| `plugins/world_tuning/feature.cpp` / `feature.h` | 七个 wrapper、阵营判定、HP 账本、掉落上下文与品质重掷、倍率原子值、配置读写 |
| `plugins/world_tuning/rates.cpp` / `rates.h` | 档位、字段/文案表、JSON 编解码与校验、缩放函数 |
| `plugins/world_tuning/panel.cpp` / `panel.h` | 面板 provider、列表/箭头/文本绘制、receipt 轮询 |
| `plugins/world_tuning/settings_contribution.cpp` / `.h` | 旧 catalog contribution 空实现 |
| `plugins/world_tuning/CMakeLists.txt` | `qol_plugin_world_tuning` 静态库 |

## 测试

- `world_tuning_rates_tests`（`tests/test_world_tuning_rates.cpp`）：档位、显示文本、JSON 往返、非法输入、旧配置 `move_speed` 键兼容、`validate_config`、缩放边界。
- `world_tuning_feature_tests`（`tests/test_world_tuning_feature.cpp`）：descriptor（7 资源、无 provides）、安装失败出口、门禁时序、伤害矩阵、HP 缩放与幂等/倍率变更、经验/金币缩放、阵营失败、残留 wrapper、entry 不可用分支；掉落数量 0/1/3 次调用、品质重掷的类别过滤/取最高/释放次数/掉落上下文隔离。
- `settings_contributions_tests` 编译本插件的空 contribution。

面板绘制/命中与 entry 联动已有真机观察（见下）；未覆盖：真实设备 hook ABI（伤害/敌怪血量实际生效）、资源安装时序。

## 验证状态

Host 测试与构建通过；2026-10-29 真机（C18L v589）已验证：设置页入口 → 面板打开、行箭头显示与点击、数值写入 `config.json`、无 FATAL（该证据时点面板为 7 行且含已移除的 `rates.move_speed` 键）。各倍率的游戏内实际效果尚未复测，按 `docs/refactor/verification.md` 的口径登记为 `PARTIAL / NOT_ACCEPTED`。
