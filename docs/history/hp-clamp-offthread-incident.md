# HP 莫名扣血/死亡事故：离线程 `CHAR_GetAttr` 钳制写回

历史归档（2026-09-14）：仅供追溯，不得作为当前实现依据；当前线程规则见 `docs/development/architecture.md` §9.6，当前只读原语见 `module/app/src/main/cpp/game_state.{h,cpp}`。

> 状态：已定位并修复（真机 frida 复现根因 + 修复后观察）
> 范围：记录事故经过、根因定位、同类排查与修复取舍；**不复制当前 API 契约**。
> 关联：`docs/development/architecture.md` §2.1（帧任务消费者）、§9.6（线程纪律）；`docs/development/features/frame-dispatch-host.md`（`kFramePointLogicPre` 经验缓存）。

## 1. 现象

扩展背包 / 堆叠上限 / 拖拽合并三开关开启态下长时间挂机（无战斗、无操作），角色 HP 缓慢下降，幅度小但单调，偶发死亡面板 `dialog_wipeout`。表现为「被悄悄扣血」，而非某次操作后的跳变。

## 2. 调查过程（时间线）

1. 先排除游戏自身逻辑：当前地图无怪物、角色无 DOT/buff，HP 仍下降。
2. 缩小到模块侧写回：帧缓存预取线程（`core/native/game_cache.cpp` 的 `cache_prefetch_thread_fn`）每帧帧计数驱动构造 `interval>0` 槽（player/party/units/gamestate/snapshot/drops）。
3. 反汇编 `CHAR_GetAttr`（`game_access` 的 `fn_get_attr`，VMA `0xdfd18`）：`attr=0x1e`（`ATTR_MAX_HP`）分支在 `HP > maxHP` 时执行 `str w0,[x20,#0x1f0]`，**把 HP 写为 maxHP**。即「读 maxHP」自带 HP 钳制副作用。
4. 竞争窗口：游戏主线程 `CHAR_UpdateAttr` 的属性重算与预取线程的 `CHAR_GetAttr(0x1e)` 交错，预取线程以旧值/中间态 maxHP 把 HP 钳低；反复交错可把 HP 钳到 0。
5. 真机 frida 高频调用实验（在非游戏线程高频调 `CHAR_GetAttr(ch, 0x1e)`）稳定复现 HP 下降直至 `dialog_wipeout`，确认根因。

## 3. 根因

预取线程的 JSON 构造（`build_player_json` / `member_json` / `data_op_set_hp` 等）为取 HP/MP 上限调用了 `CHAR_GetAttr`；该 getter 在 `attr=0x1e` 时写回 HP，与游戏线程属性重算竞争，把角色 HP 永久钳低甚至钳成 0。

## 4. 修复

新增 data 层只读原语（`game_state.{h,cpp}`），写入点全部改用：

- `char_max_hp` / `char_max_mp`：直读 `[ch+C_MAX_HP]` / `[ch+C_MAX_MP]`（= `[ch+0x9c]` / `[ch+0xa0]`，属性数组 attr 0x1e/0x1f 的缓存槽），不调用 `CHAR_GetAttr`；缓存值 ≤0 时回退当前 `C_HP` / `C_MP`，保证世界未就绪时输出合理。
- 常量 `C_MAX_HP` / `C_MAX_MP` 入 `data/native/game_symbols.h`（由 `C_ATTR + ATTR_MAX_HP/MP * 4` 推得，与 `CHAR_GetAttr` 同偏移体系）。
- 写入点：`member_json`（`api/native/game_character.cpp`）、`build_player_json`（`game_system.cpp`）、`data_op_set_hp` / `data_op_set_mp` 全部改用该原语。

## 5. 同类排查（同一次调查）

- `fn_get_stat`（`CHAR_GetStat` `0xdf8d0`，经 `CHAR_GetStatSub` `0xdf888`）：动态派生脏位 `C_STAT_CALC_FLAG`（`[ch+0x270]`）置位时会调 `CHAR_CalculateStatus` 重算并写回 `[ch+0x266]`、状态位 `[ch+0x270]`、`SV_MainCharacterSet`，属写操作 → 改为 `char_stat_total` 直读 `C_STAT_BASE/MAIN/BONUS/SUB` 求和（= `CHAR_GetStat` 的 Base+Main+Bonus+Sub，无 clamp）。
- `fn_get_next_exp`（`CHAR_GetNextExperience` `0xd9b68`）：`[ch+0x320]==0` 时用 `CAL_Calculate`（**全局计算器栈**）现算并写回，且 `CHAR_SetLevel`（`0xe05a0`）会清零失效 → 改为**游戏线程帧缓存**：`char_next_exp_tick` 注册在 `kFramePointLogicPre` 每帧对 3 名队员调用一次并写 atomic 快照，`game_state.cpp` 提供 `char_next_exp_cache_start`（`nativeInit` 注册）与 `char_next_exp_cached`；JSON 只读快照，未命中回退直读游戏自身惰性缓存 `[ch+0x320]`。
- `fn_get_exp`（`CHAR_GetExperience` `0xd9b54`）为 `ldr w0,[x0,#0x318]` 纯读，保持直用。

## 6. 影响与验证

- `game_access.h` 对 `fn_get_attr` / `fn_get_stat` / `fn_get_next_exp` 增加 `⚠️ 禁止在非游戏线程调用` 注释，指向替代原语。
- 取证与回归脚本：`scripts/verification/hp_watch_session.py`（三开关开启态挂机采样，检测任意角色 HP 下降 / 死亡事件并留取证快照）。
- 规则沉淀：本事故直接催生 `docs/development/architecture.md` §9.6「线程纪律」。

## 7. 遗留

- `char_stat_total` 在动态派生脏位刚置位、游戏尚未重算时，`sub` 为上一次缓存值——这是「宁肯短时读旧值也不在离线程写回」的取舍，属有意保留。
- `char_next_exp_cached` 未进 world 时不缓存，回退直读 `[ch+0x320]`；帧宿主未安装时任务注册成功但不派发（惰性无效）。
