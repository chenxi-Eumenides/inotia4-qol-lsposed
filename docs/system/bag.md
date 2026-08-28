# 背包与袋对象机制（逆向记录）

> 本文记录原版背包/袋对象机制的逆向结论，是扩展背包容量派生契约（control-plane §8.4）的证据基础。
> 证据来源：libgame.so arm64 反汇编（`apk/decoded/lib/arm64-v8a/libgame.so`，带符号）+ 真机 frida 实测（`scripts/analyze/bag_object_probe.js`，2026-08-28）。

## 1. 三个数据结构

| 结构 | 地址/符号 | 布局 |
|---|---|---|
| 物品槽表 | `INVEN_pItem` @ `0x7131c0`（.bss 0x300） | 6 袋 × 0x80 步长，每槽 8B 物品指针（96 槽） |
| 袋对象表 | `INVEN_pBagSlot` @ `0x7134c8`（.bss 0x30） | 6 个 8B 指针：`pBagSlot[slot]` = 该袋**已装备的背包物品对象** |
| 当前选中袋 | `INVEN_nBagSlotSelected` @ `0x7134f8`（u32） | UI 当前袋号 |

GOT 引用：`*(0x2f3bc0)` = `INVEN_pBagSlot`；`*(0x2f3bc8)` = `INVEN_pItem`（G_BAG_TABLE_VMA/G_INVEN_VMA 已在 symbol_registry）。

## 2. 容量位域（INVEN_GetBagSize @ 0x103250，反汇编还原）

```c
int INVEN_GetBagSize(int bag) {
    if ((unsigned)bag > 5) return 0;
    void** table = *(void***)(base + 0x2f3bc0);   // INVEN_pBagSlot
    void* bag_item = table[bag];
    if (bag_item == NULL) return 0;
    return UTIL_GetBitValue(*(uint32_t*)((uint8_t*)bag_item + 0x10), 24, 0);  // bit0..24
}
```

- **袋容量 = 已装备背包物品对象 +0x10 的 bit0..24（25 位）**。`+0x10` 与普通物品的 I_COUNT 同址——背包物品装备后该位域被写为容量值（4/8/12/16），库存中未装备的背包物品该位域为堆叠数（1）。
- 模块 `kCapacityMask = (1u<<25)-1` 与此一致（`bag_size_word_locked` 读/写同一位域）。

## 3. 真机实测（2026-08-28，save0）

| 袋 | 容量 | 袋对象 |
|---|---|---|
| 0 | 16 | 0x6f14807310 |
| 1 | 8 | 0x6f148075b8 |
| 2 | 8 | 0x6f14807720 |
| 3 | 8 | 0x6f14807770 |
| 4 | 4 | 0x6f148077c0 |
| 5（任务袋） | 16 | 0x6f148077e8 |

对象 0x20 字节样例（bag0）：`+0x00` 4B 小数值（0x0b89e74c，非指针）、`+0x08` 4×u16（0x0108/0x03ed/0x040c/0x04ad）、`+0x10` 容量位域、`+0x1c` 2×u16。+0x08/+0x1c 的 u16 语义未定（不像指针，疑似格子/图标元数据）。

## 4. 存档流（SAVE_LoadInventory @ 0x127ea4，反汇编还原）

对每个袋槽（6 × 8B）：

```c
bag_item = SAVE_LoadItem(cursor, ...);        // 从存档流重建背包物品对象
if (!bag_item) continue;
INVEN_pBagSlot[slot] = bag_item;              // 袋表[slot] = 物品指针
capacity = UTIL_GetBitValue(bag_item->w10, 24, 0);
if (capacity > 0)
    for (i = 0; i < capacity; i++)            // 恰好填充 capacity 个槽
        INVEN_pItem[slot][i] = SAVE_LoadItem(cursor, ...);
```

**结论**：原版存档流按「先袋物品、后 capacity 个袋内物品」编码；袋容量持久化由原版存档天然承载（无需 sidecar 重复存储容量值）。`SAVE_SaveInventory`（0x127d8c）为对称写入方。

## 5. 背包物品静态记录（ITEMDATABASE.json）

| cat | item_id | 名称 | 价格(u16[6]) |
|---|---|---|---|
| 1 | 31 | 手提包 | 90 |
| 2 | 32 | 背包（小） | 540 |
| 3 | 33 | 背包（中） | 3300 |
| 4 | 34 | 背包（大） | 4300 |

live category = ITEMDATABASE 记录下标 = item_id − 30。容量（4/8/12/16，见 `docs/stack-limit-fixed-split-plan.md:15`）**不在静态记录的可见字段中**——由装备逻辑写入物品 +0x10。

## 6. 已确认与未决

**已确认**：
- 容量派生源 = 装备物品对象 +0x10 bit0..24（运行时唯一真源）
- 袋对象即背包物品对象；袋槽与物品一一对应
- 存档按容量循环编码袋内物品；容量随原版存档持久化
- 实测容量 16/8/8/8/4（+任务袋 16）；等级序列 4/8/12/16 见 stack-limit 文档

**未决（后续实验）**：
- 装备时初值写入方：背包物品装备到袋槽的代码路径（UI 层，CHAR_EquipItem 0xe51c0 无背包分支）；动态实验：装备一个背包（中）观察 +0x10 变化，确认 4/8/12/16 与物品等级的映射
- 袋对象 +0x08（4×u16）与 +0x1c（2×u16）字段语义
- 解除/出售已装备背包、超容溢出的原版行为
