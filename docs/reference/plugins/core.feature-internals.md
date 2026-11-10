# `core.feature` 内部实现

迁移期插件实现参考。公开行为见 [`core.feature.md`](core.feature.md)。

## 分层与目录

| 路径 | 职责 |
|---|---|
| `module/app/src/main/cpp/plugins/core.feature/plugin.cpp` | 描述符、HTTP 占位 handler、三项入口 hook wrapper、feature 回调 |
| `module/app/src/main/cpp/plugins/core.feature/routes.inc` | 501 占位路由声明，供路由映射校验 |
| `module/app/src/main/cpp/tests/test_core_feature_plugin.cpp` | HTTP 占位与描述符回归 |
| `module/app/src/main/cpp/tests/test_core_feature_crit_fx.cpp` | 暴击特效拦截 wrapper 回归 |

## 装配

### descriptor

- id：`core.feature`；version：`0.1.0`。
- optional requires：HTTP registry、HTTP dispatch 两项 capability。
- resources：3 条私有 `kEntryHook`，全部 `kOptional` + `kFailSoft` + `resumable`。
- features：2 条 toggle。
- http endpoints：1 条 POST 占位路由。

### resources

| 资源 id | 符号 | 机制 | 用途 |
|---|---|---|---|
| `hook:CHAR_AddActEffect` | `F_CHAR_ADD_ACT_EFFECT_VMA` | EntryHook | 按特效 ID 拦截暴击闪白 `0x5a` 与横向打击条 `0x1f..0x22`，其余透传 |
| `hook:MAP_SetShake` | `F_MAP_SET_SHAKE_VMA` | EntryHook | 拦截同一暴击分支内的屏幕震动 |
| `hook:VIBRATION_Play` | `F_VIBRATION_PLAY_VMA` | EntryHook | 拦截同一暴击分支内的设备震动 |

符号常量与符号名登记位于 `game/symbols/game_symbols.h`、`game/symbols/symbol_registry.h` 和 `game/access/symbols.cpp`。插件只声明资源，不持有裸地址。

### 生命周期

- `plugin_install`：缓存 Host SDK 与 Game Host；缓存 `thread.frame_count`；把开关初始化为开启。
- `plugin_deactivate`：清空 backup、帧计数器和 pending 状态，关闭开关。
- `on_resource_ready`：按资源 id 缓存三个原函数入口。
- `mask_crit_effects_toggle`：写插件原子开关；关闭时清空 pending，避免遗留状态影响下一次暴击。

## 关键机制

### 闪白与打击条拦截

`CHAR_AddActEffect` wrapper 在开关开启且 `effect_id == 0x5a` 或 `0x1f <= effect_id <= 0x22` 时直接返回 0，不创建特效节点；其他 ID 调用 backup。全游戏只有 `CHAR_AddDamage` 的暴击分支传入这些 ID，因此该判断不会影响其他动作特效。暴击光效 `0x52/0x1df` 不在拦截集合内。

### 震屏与震动的同帧关联

暴击分支顺序固定：

1. `0x52` / `0x1df` 暴击光效；
2. `0x5a` 闪白；
3. 随机打击特效；
4. `MAP_SetShake(1)`（仅攻击者为当前主控时）；
5. `VIBRATION_Play(3)`。

wrapper 在拦截 `0x5a` 时记录当前帧号并设置 `g_crit_feedback_pending`。`MAP_SetShake` wrapper 在同一帧内看到 pending 时直接返回，但保留 pending；`VIBRATION_Play` wrapper 拦截后清除 pending。若当前帧号与记录不一致，pending 自动失效，避免跨帧误拦其他来源的震屏或震动。

`frame_count` 不可用时退化为仅依赖 pending；生产路径由 Game Host 始终提供帧计数。

### 帧号来源

`contracts::QolGameHost::thread.frame_count` 由 Game Adapter 提供，和插件开关一样只读。wrapper 不调用其他游戏函数。

## 数据流

```text
暴击伤害结算
  -> CHAR_AddActEffect(0x52/0x1df) 透传
  -> CHAR_AddActEffect(0x5a)       开关开启则丢弃并标记 pending
  -> CHAR_AddActEffect(0x1f..0x22) 开关开启则丢弃
  -> MAP_SetShake(1)               同帧 pending 则丢弃
  -> VIBRATION_Play(3)             同帧 pending 则丢弃并清除 pending
```

## 失败与恢复

- 三个资源均为 optional + fail-soft：符号缺失或安装失败只影响对应反馈的拦截，插件保持可用。
- backup 未就绪时 `CHAR_AddActEffect` wrapper 返回 0，属于 fail-closed；正常的 `on_resource_ready` 会在资源进入 live 前写入 backup。
- 帧号不可用时不会跨帧残留 pending。
- 开关关闭或插件停用会清空 pending，不留下跨生命周期状态。

## 已知限制与风险

- 同帧关联依赖暴击分支的固定调用顺序。若未来变体在该分支中插入其他震屏/震动来源，同一帧内可能被一并拦截。
- 只识别 `0x5a` 闪白与 `0x1f..0x22` 打击条；新增暴击全屏特效 ID 需要同步 `is_masked_crit_effect`。
- `core.feature` 是迁移期插件，长期应把用户功能迁移到独立功能插件后再删除本插件。

## 测试与验证映射

| 验证 | 证据 |
|---|---|
| 0x5a 与 0x1f..0x22 被拦截、0x52/0x23 透传 | `tests/test_core_feature_crit_fx.cpp` |
| 同帧震屏/震动被拦截，下一帧恢复 | 同上 |
| 开关关闭后全部透传 | 同上 |
| HTTP 501 占位不回归 | `tests/test_core_feature_plugin.cpp` |
| 全量构建 | `scripts/build-debug.sh` |
| 真机暴击 | 待验证：安装 Debug 模块后触发物理/法术暴击，确认无闪白、无震屏、无震动，且暴击光效和伤害数字仍在 |
