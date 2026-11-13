# `item_lab` 物品构造测试

`item_lab.construct` 启用后，在背包页自动出售按钮左侧显示齿轮按钮。点击一次固定构造 5 件**非法装备**；若当前原生背包 slot 0 有物品且额外有 1 个空位，再复制 1 件副本。

| # | 测试点 | 构造 |
|---|---|---|
| 1 | 白装 4 词条 + 最大孔/强化字段 | category 462，rarity=0（raw=0），原生词条 4，`socket=15/0`，`enchant_id=1`、`enchant_level=31`、`enchant_remaining=15` |
| 2 | 孔 1/镶 4 | category 462，rarity=0，原生词条 0，`socket=1/4`，宝石节点 4 |
| 3 | 孔 2/镶 2 + 4 附魔 | category 462，rarity=2（raw=7），原生词条 2，`socket=2/2`，宝石节点 2，附魔节点 4 |
| 4 | 白装 + 前缀秘银 | category 462，请求 rarity=0，但 raw=9（秘银），原生词条 0 |
| 5 | 紫装 + 前缀生锈的 | category 462，请求 rarity=4，但 raw=0（生锈的），原生词条 4 |

## 附魔/强化的固定值

- 4 个附魔节点依次为：`type=2 id29 value=100`、`type=2 id0 value=500`、`type=2 id31 value=50`、`type=2 id30 value=10`。
- id29 是百分比类型却写 `5`（显示为 0.5%）；id0 是整数类型却写 `500`，用于交叉验证 `percent` 与数值语义。
- 第 1 件写入 `I_ENCHANT`：`id=1`（bits11-15）、`level=31`（bits6-10）、`remaining=15`（bits2-5）。

## 字段上限与解析现状

| 字段 | 位宽/范围 | 游戏实际限制 |
|---|---|---|
| 总孔数 `socket_total` | bits4-7，0..15 | 常规最多 4 孔 |
| 已镶嵌 `socket_filled` | bits0-3，0..15 | 不超过总孔数才合法 |
| 强化耐久 `enchant_remaining` | bits2-5，0..15 | 15 |
| 强化等级 `enchant_level` | bits6-10，0..31 | 当前版本强化上限 12；旧系统理论上限 15 |
| 强化 ID `enchant_id` | bits11-15，0..31 | 游戏维护 |

- `enchant_level`/`enchant_remaining`/`enchant_id` 已由 `item.facts` view 解析，但常规 Item JSON 只输出 type=2 的 `enchant` 数组，未输出这三个标量；`/api/debug/item/raw` 可查看 `enchant_count`、`enhance_low4`、`enchant_id`。
- `GetRarity` 从 `I_TYPE` bits2-5 经 `ITEMGRADEBASE +4` 映射，因此前缀 raw 会决定实际 rarity：raw=9 映射为蓝（2），raw=0 映射为白（0）。第 4/5 件的“请求 rarity”与“实际 rarity”会不一致，属于预期非法状态。
- 标记位：`I_COUNT` bits25-31 固定写 100，保证 `ITEM_IsRealEquip` 不把装备判为未鉴定/损坏。

## 容量与日志

点击前检查当前袋剩余容量：至少 5 个空位才执行 5 件构造；slot 0 有物品且额外还有 1 个空位（共 6 个）时才复制副本，否则跳过副本并 WARN。不足 5 个空位会 WARN 并跳过整批。

日志域为 `item_lab`，逐件记录 `expected` 和实际落位 `space/bag`，可直接与上表对照。插件仅用于测试解析、生成、构造链路，避免在重要存档中使用。
