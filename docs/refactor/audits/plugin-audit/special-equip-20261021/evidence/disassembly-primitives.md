# special_equip 原语反汇编证据（monster v29）

- 目标：`apk/game-apk/艾诺迪亚4_monster_v29.apk` → `lib/arm64-v8a/libgame.so`（3,445,576 bytes，MD5 `4c332e9b508b35fe1e97f1f26e56f04d`）。
- 工具：NDK `llvm-objdump`；首个 LOAD 的文件偏移与 VMA 均从 0 起，低地址 VMA 可直接用于反汇编。v29 另有 overlay 段：文件偏移 `0x310000` → VMA `0x740000`（减 `0x430000`）。
- 结论用于 SE-01/SE-02 的修复方向；不替代真机行为验收。

## 1. `ITEM_IsRealEquip`（`0x105ab8`）

关键指令：

```asm
105ac8: cbz     x0, 0x105b3c
105af4: bl      UTIL_GetBitValue       ; category = item+8 bits 6..15
105b14: bl      MEM_ReadUint8          ; ITEMCLASSBASE[category].+2
105b34: bl      MEM_ReadUint8          ; 装备槽表[index].+2
105b38: tbnz    w0, #0, 0x105b50       ; bit0=0 → false
105b5c: bl      UTIL_GetBitValue       ; item+0x10 bits 25..31
105b60: cmp     w0, #0x63
105b64: cset    w0, gt                 ; 有符号 >99
```

判据：空指针返回 0；类别装备槽表记录 `+2` bit0 未置位返回 0；否则读 `item+0x10` bits25–31，按有符号 `>99` 返回布尔。**不读取** `ITEMCLASSBASE[category].+6 bit0`。

`ITEMSYSTEM_CreateItem`（`0x10c02c..0x10c048`）对满足槽表 bit0 的类别用 `UTIL_SetBitValue(..., 31, 25, 100)` 写 marker；`ITEM_IsRealEquip` 接受 100–127。

## 2. `ITEM_GetAbilityLevel`（`0x1091f4`）

- 空指针返回 0；非损坏分支经 `ITEMSYSTEM_GetAbilityLevel(category)` 读 `ITEMCLASSBASE[category].+3`，`sxtb` 有符号 int8 返回。
- 损坏分支读同一记录 `+3`，同样有符号返回。
- 无装备等级补偿或上限裁剪。

## 3. `ITEM_GetEquipLevel`（`0x1097ac`）

- 空指针返回 0；基础值同样读 `ITEMCLASSBASE[category].+3`（有符号 int8）。
- 读 `item+0x1a` 的 16 位值，`(value | value >> 1) & 1` 置位时加 `ITEM_GetChaosLevel(item)`（`item+0x10` bits0–7，有符号 int8）。
- 负值归零；以 GOT 表中 `13 * stride` 位置的 int16 为上限裁剪；100 附近另有分支裁剪。

AbilityLevel 与 EquipLevel 可能不同（chaos 加成、负值归零、上限裁剪）。

## 4. `CHAR_CanEquipItem`（`0xe4eb4`）的 ④

```asm
e4f50: b       0x74cd28             ; v29 overlay 跳板：读 [x19+0xe] 到 w22
e4f54: mov     x0, x20              ; item
e4f58: bl      ITEM_GetEquipLevel
e4f5c: cmp     w22, w0
e4f60: b.lt    0xe4ee8              ; 角色等级 < 装备等级 → 拒绝
```

函数体无 `ITEM_GetAbilityLevel` 调用。其余顺序：空指针拒绝 → `ITEM_IsRealEquip` → `+7 bit4` → `CHAR_CanChangeEquip` → 等级比较。

`CHAR_CanUnequipItem`（`0xe4e2c`）：空指针拒绝 → `+7 bit4` 拒绝 → 返回 `CHAR_CanChangeEquip(ch)`。

## 5. 结论

- 「数量未编码」与 `ITEM_IsRealEquip` 读不同字段，**不等价**。差异条件：
  - `+6 bit0==0` 但槽表 bit0 未置位或 marker ≤99 → 前者真、后者假。
  - 槽表 bit0 置位且 marker >99 但 `+6 bit0==1` → 前者假、后者真。
- 原版 ④ 的真实比较值是 `ITEM_GetEquipLevel`；`need_level` 使用 `ITEM_GetAbilityLevel`，在 chaos 加成或裁剪时可能偏离。
- 修复采用 `item.facts.is_real_equip` 与 `game->read_only.item.equip_level`（与原版同函数）。

## 6. 证据强度

- **确证**：APK 成员路径、MD5、VMA、五个函数反汇编、`ITEMSYSTEM_CreateItem` marker 写入、④ 调用 `ITEM_GetEquipLevel`、`CHAR_CanUnequipItem` 判据。
- **部分证据**：v29 overlay 回调是否动态改写等级读值；装备等级上限表身份。
- **未证实**：目标静态类别表中不一致判定组合的实际存在数量；真机行为。
