# `world_tuning` 插件参考

## 摘要

功能插件「游戏世界调整」，插件版本 `0.1.0`。设置页只有一个入口行；点击后启用插件并打开自有面板，面板内每行一个倍率，左右箭头在固定档位间调整。改动经 Host 配置 Store 持久化，写入 `config.json` 的 `world_tuning` namespace。

当前真正接线的倍率只有三项（其余四项只在面板中保存数值，不产生游戏效果）：

- 敌怪血量倍率：缩放怪物最大生命；
- 伤害倍率：缩放玩家侧对怪物造成的伤害；
- 承伤倍率（配置键 `damage_in`）：缩放玩家侧受到的伤害。

金币倍率、经验倍率、玩家血量倍率、移速倍率在面板中与其它行完全一致（可调整、可持久化），只是尚未接线到游戏逻辑。面板行只显示名称、数值与箭头，不显示副标题。

插件 id 由旧 `simple_mode` 改为 `world_tuning`，旧 `simple_mode.combat` feature 与旧配置不再迁移。除特别说明，文中路径均相对 `module/app/src/main/cpp/`。

## 定位与 descriptor

- plugin id `world_tuning`，注册于 `host/assembly.cpp`。
- `requires` 1 项：`character.facts` v1.0。
- `optional_requires` 2 项：`ui_panels` v1.3、`ui_components` v1.22；界面工具被停用时倍率仍生效，仅面板不可用。
- `ports` 1 项：`QOL_PORT_CHARACTER_EXTRAS` v1.2。
- `provides` 0、JNI 0、HTTP route/endpoint 0。
- `resources` 2 个入口 hook，均为 `kOptional` + `kFailSoft`：单个落点不可用只影响对应倍率，不会把插件置 `failed`。
- `validate_config` = `validate_rates_config`，只接受 `rates` 键。
- 唯一 feature 为 entry（`plugins/world_tuning/plugin.cpp`）：

| feature id | 默认 | 分类 | 类型 | 名称 | 描述 | 按钮 |
|---|---:|---|---|---|---|---|
| `world_tuning.panel` | 0 | Gameplay | entry | 游戏世界调整 | 调整游戏世界的属性 | 调整 |

entry 默认 0：首次点击时设置页先提交 feature=true、等待 Host actual `enabled`，再调用 entry 回调打开面板；之后 feature 保持 true，插件常驻。

## 配置

单键 `rates`，值为整数百分比对象：

```json
{"gold":100,"exp":100,"player_hp":100,"enemy_hp":50,"damage_out":150,"damage_in":50,"move_speed":100}
```

- 字段：`gold`、`exp`、`player_hp`、`enemy_hp`、`damage_out`、`damage_in`、`move_speed`；缺字段按 100 处理。
- 取值只允许档位：`10/30/50/80/100/120/150/200/300/500`（即 ×0.1 … ×5.0）；未知字段、重复字段、非整数、档位外取值一律拒绝。
- `validate_config` 在写入与 Store 加载两条路径生效（`host/features/controller.cpp`、`host/core_settings_config_store.cpp`）。
- 读取通道：插件经 `QolFeatureApi::read_config` 读自己的 namespace；写入经 `request_set_config`，面板每帧轮询一次 receipt。`/api/config/set` 改同一键，下次打开面板或插件重新启用时生效。
- 插件未订阅 `runtime_effects`；外部改动不会即时推送。

## 面板

面板 id `world_tuning`，provider 在 `plugin_install` 内注册（需要 `ui_panels` + `ui_components` 同时可用）。

- 960×640 root，单栏容器 `{28,100,920,444}`；与设置页相同的 `kPanelBg` 底、`draw_container_ornamented` 边框和左右装饰图（`draw_image_part` unit `0x4f`/`0x50`）；不绘制标题与说明行。
- 7 行，行高 71，使用 `scroll_list`（关闭选中态）；每行：名称、数值（`×1.0`）、左右 `arrow_button`；行底 2 逻辑单位金色分割线，与设置页行样式一致。
- 列表视口高 428，内容高 497，可上下拖动滚动；滚动条沿用原生 `scroll_list` 自带的一条，不额外自绘。
- 全部 6 行的箭头都可用、样式一致；未接线行只保存数值，不产生游戏效果。
- 点击箭头 → 生成完整 `rates` JSON → `request_set_config` → 进入 pending（全部箭头禁用）→ 持久化确认后 `apply_rates`；成功不显示提示，失败/状态未知才在底部显示错误并回滚。
- 面板打开时重新读取配置，覆盖外部编辑；关闭/插件停用销毁全部控件与回调。

## 游戏内行为

两个入口 hook 与旧 `simple_mode` 相同：

| 资源 id | 符号 | 用途 |
|---|---|---|
| `hook:CHAR_AddDamage` | `F_CHAR_ADD_DAMAGE_VMA` | 伤害唯一汇合点，改写 `w2` 入参 |
| `hook:CHAR_UpdateAttrFromMonster` | `F_CHAR_UPDATE_ATTR_FROM_MONSTER_VMA` | 怪物最大生命重算层，`attr==0x1e` |

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

## 生命周期与线程模型

- `plugin_install`：校验 context/port/SDK → require `character.facts` → 缓存 `sdk/game/port/facts/owner/plugin_id` → 可选注册面板 provider → 读取 `rates`。
- `plugin_on_enabled`：开门禁并重新读取 `rates`。
- `plugin_on_disabled`：关门禁。
- `plugin_deactivate`：关门禁 → 注销面板 owner → 解绑 UI → 保留 `sdk/game`（残留 wrapper 仍需要 in-flight 与 backup），清空 facts/port。
- 门禁与倍率为原子变量，游戏线程读、主线程写；HP 账本仅游戏线程访问。
- 面板未绑定或 `ui_panels` 不可用时，entry 返回 `kQolErrNotReady`，倍率不受影响。

## 文件地图

| 文件 | 职责 |
|---|---|
| `plugins/world_tuning/plugin.cpp` / `plugin.h` | descriptor、feature、install/deactivate、面板注册、配置读取 |
| `plugins/world_tuning/feature.cpp` / `feature.h` | 两个 wrapper、阵营判定、HP 账本、倍率原子值、配置读写 |
| `plugins/world_tuning/rates.cpp` / `rates.h` | 档位、字段/文案表、JSON 编解码与校验、缩放函数 |
| `plugins/world_tuning/panel.cpp` / `panel.h` | 面板 provider、列表/箭头/文本绘制、receipt 轮询 |
| `plugins/world_tuning/settings_contribution.cpp` / `.h` | 旧 catalog contribution 空实现 |
| `plugins/world_tuning/CMakeLists.txt` | `qol_plugin_world_tuning` 静态库 |

## 测试

- `world_tuning_rates_tests`（`tests/test_world_tuning_rates.cpp`）：档位、显示文本、JSON 往返、非法输入、`validate_config`、缩放边界。
- `world_tuning_feature_tests`（`tests/test_world_tuning_feature.cpp`）：descriptor、安装失败出口、门禁时序、伤害矩阵、HP 缩放与幂等/倍率变更、阵营失败、残留 wrapper、entry 不可用分支。
- `settings_contributions_tests` 编译本插件的空 contribution。

面板绘制/命中与 entry 联动已有真机观察（见下）；未覆盖：真实设备 hook ABI（伤害/敌怪血量实际生效）、资源安装时序。

## 验证状态

Host 测试与构建通过；2026-10-29 真机（C18L v589）已验证：设置页入口 → 面板打开、6 行箭头显示与点击、数值写入 `config.json`（`rates.move_speed` 1.0→1.2）、无 FATAL。三项倍率的游戏内实际效果尚未复测，按 `docs/refactor/verification.md` 的口径登记为 `PARTIAL / NOT_ACCEPTED`。
