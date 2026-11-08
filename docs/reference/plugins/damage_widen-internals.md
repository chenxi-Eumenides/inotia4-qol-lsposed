# `damage_widen` 内部实现

## 装配与资源

一期 4 个 wrapper 与二期物防 wrapper 是私有、可选、fail-soft entry hook；六个新增指令区域和 2a 三个既有区域为 gated、optional、fail-soft `kInsnPatch`。`damage_widen.enabled` 关闭时资源物理归还，原生函数/指令恢复。符号和指令点位分别由 `game/access/symbols.cpp` 与 `game/access/insn_sites.cpp` 解析。

## 安全千分比运算

`safe_scale(X,N)` 统一计算 `(X / D) * N + ((X % D) * N) / D`（D 为点位除数，100 或 1000）；两次除法均为有符号向零截断，与 `trunc0(X*N/D)` 精确等价。输入为游戏 int32 值及其有限和；分解后的中间值在 int64 范围内。普通/魔法攻击、暴击、魔防与物防 wrapper 使用该 inline helper。kind4 thunk 对全部千分比/百分比点执行对应的 `sxtw`、`sdiv`、`msub`、低余数乘法与加和（除数参数化，÷100 与 ÷1000 同构），不做原始 32 位乘积，也不使用 64 位完整乘积。区域重放分两组：非标志指令先于分解式（不得写分解式源寄存器），置标志指令在写回后最后重放（区域外分支消费最终标志）。

## 二期 B 组点位

审计报告：`.tmp/damage-widen/audit-act_dd.md`、`audit-attr_attack.md`、`audit-calc_physical_defense.md`。以下 5 个变体（c18、c18-v514、c18-v520、monster、overhaul）逐点机器码一致，均启用；monster `0xef028/0xef038` mod 门不与区域重叠。

| 函数 | 点位 | 处理 |
|---|---|---|
| `CHAR_Act_DD` | `0xef20c` | kind4：×1000，重放 `ldrb`、`ldr`、独立索引 `mul` |
| `CHAR_Act_DD` | `0xef4d0` | kind4：×1000 |
| `CHAR_Act_DD` | `0xef530` | kind4：×1000，重定位并重放 `CHAR_GetAttr` BL |
| `CHAR_Act_DD` | `0xef560` | kind4：×1000，返回原后续分支前 |
| `CHAR_Act_DD` | `0xef5d8` | kind4：×1000，重放后续 `orr` |
| `CHAR_AttrAttackDamage` | `0xeb1b4` | kind4：×100 分解式（除数参数化），结果写入消费寄存器 `w26`；重放原属性 ID 比较以维持 NZCV |
| `CHAR_CalcPhysicalDefense` | `0xe07d8` | entry-hook 整函数替换，保留原 CAL 文本公式及参数 |

kind4 验证乘法起始指令，生成近址 thunk，按点位重放已审计的内存加载/调用/比较等副作用；无法编码的分支/范围按 fail-closed 拒绝。结果钳制到 `[0, INT32_MAX]`。物防 wrapper 仍通过 defender 属性选择原公式文本，并以 `[scaled(attr17, attr18+1000), defender[14]]` 调 `CAL_Calculate`；再按原顺序叠加 attr7 与正 attr81 的千分比项，计算 `(1000-R)*damage/1000`。公式中间保持有符号向零截断，最终输出钳制。属性或公式读取不可用时透传原函数。

## 明确豁免

- `CHAR_AddDamage`：全函数没有 32 位乘除，不加 hook。
- `CHAR_AddLife`：只有 `0xea1bc` 一条 32-bit add；上游钳制后该点不可达，不加 hook。
- `CHAR_AttrAttackDamageByType`、DOT、`UTIL_Calculate`：本批不实现，另行审计/后续批次处理。

## 限制与验证

本批静态代码验证不等同真机验收。VM-DW-02 真机仍需验证伤害钳制、低伤害回归及功能开关的物理还原。公共契约见 [`damage_widen.md`](damage_widen.md)。
