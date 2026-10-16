# ADR 0003：`character_extras` 端口 `read.party_index` 的取值语义

状态：已接受（用户裁决，2026-10-17）
适用范围：`QOL_PORT_CHARACTER_EXTRAS` 端口方法 `read.party_index`（`contracts/game_ports/character_extras_port.h:12`，实现 `game/adapter_ports/character_extras_port.cpp:21-30`）及其消费者。不改变端口其余方法与成员。

## 裁决原文

> read.party_index把-1视为合法值，是否更改了其语意？如果该函数被其他地方调用，是否会导致问题？
>
> 按A落地。在独立说明文档中记录。如果后续有其他调用方，再进行决策。

## 背景

`plugins/push_all`（推怪）对怪物不生效，静态定位为端口取值语义缺陷：

1. 游戏侧 `CHAR_GetPartyIndex`（`game/symbols/game_symbols.h` 注释；VMA `0xdca10`）：命中队伍返回 `0/1/2`，**否则 -1**；怪物恒为 -1。
2. `read_party_index` 原实现（`character_extras_port.cpp:21-30`）把 `not_ready` 复用于两种情形：① 符号未解析（`!char_party_index_resolved()`）；② 单位不在队伍（`value < 0`）。
3. `plugins/push_all/feature.cpp:60-64` 的 fail-closed 查询把任一 `.ok != kQolOk` 视为致命：
   ```
   if (… || character_extras()->read.party_index(ch, &party_index).ok != kQolOk) return -1;
   ```
   于是怪物（-1）在抵达 `party_decision`（`rules.cpp:7-9`，`type==1 → 0`）之前就被 `return -1` 拦住 → 推怪失效。

回归来源：`9cdc3d27`（L4 插件胶囊化）把 push_all 从直接调用 `game::char_party_index(ch)`（原值透传，`574e19ee` 原型）改为经宿主适配器 + `.ok != kQolOk` 判定；该适配器在 `9cdc3d27` 起就把 `value < 0` 判为 `not_ready`。均晚于 VM-P4-01（2026-09-17）的真机确认，此后只做过装配级核验。

**消费者盘点（决策依据）**：

- `read.party_index` 方法调用者：**仅 `plugins/push_all`**（`feature.cpp:54,62`）；它需要的正是原值 `-1`。
- 其余 required `QOL_PORT_CHARACTER_EXTRAS` 的插件（`extension_bag`、`special_equip`、`move_merge`、`simple_mode`）调用的是端口**其它方法**，全仓无一处调用 `party_index`；`simple_mode` 用 `is_party_member`（`feature.cpp:80,92`）。
- 测试：`tests/test_host_adapters.cpp:1237-1238` 只断言 `value == 0 → ok`，**无** “负数 → not_ready” 用例。
- 同型重复实现：`game/adapter_ports/game_host_port.cpp:202 read_char_party` 采用同一 `value < 0 → not_ready` 写法，但全仓无引用（死代码）。

端口内已有 `is_party_member`（`character_extras_port.cpp:32-38`）在“不在队伍”时返回 `ok(0)`，即“用 `ok` 表达不在队伍”已有先例；`party_index` 的 `not_ready`-for-negative 属异类。

候选方案：**A**（端口侧：`-1` 作为合法值 `ok` 返回，`not_ready` 只留给未解析）；**B**（插件侧：push_all 不把 `party_index` 的 `not_ready` 当致命，回退 `-1`）。用户裁决按 **A**。

## 决策

- **D1 取值语义**：`read.party_index` 返回 `ok` 并以 `*out` 给出 `CHAR_GetPartyIndex` 原值（含 `-1` = 不在队伍）；`not_ready` 仅表示符号未解析。删除原 `value < 0 → not_ready` 分支。
- **D2 版本**：该端口 `kQolPortCharacterExtrasVersion` 由 `{1, 1}` 升为 `{1, 2}`（`contracts/game_ports/ports.h:53`），作为契约行为变更信号；端口成员签名与布局不变。
- **D3 后续调用方**：若将来出现新的 `read.party_index` 调用方，按用户口径**另行决策**，不得默认沿用本次语义。
- **D4 同型重复登记**：`game_host_port.cpp:202 read_char_party`（死代码、旧语义）本次不改，登记为后续清理候选，避免遗留漂移。
- **D5 不改插件**：本次不修改 `plugins/push_all` 代码；生效路径为端口语义修正 + 既有 `party_decision`。

## 关联

- 缺陷定位与设备佐证：push_all 插件审阅（`docs/refactor/audits/plugin-audit/`，2026-10-17）；静态审计（@oracle）无 P0/P1，本缺陷由主代理定位。
- 契约面：`contracts/game_ports/character_extras_port.h`、`contracts/game_ports/ports.h`、`game/adapter_ports/character_extras_port.cpp`。
- 受影响验收卡：VM-P4-01、VM-K2-03（真机行为回归待补）。
- 回归引入：`9cdc3d27`；持久化：`b9ef63cc`（迁到 extras 端口）。
