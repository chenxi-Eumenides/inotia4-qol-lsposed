# 商店扩展物品出售（W1）反汇编证据

> 归档资料：记录 W1「商店扩展物品选择数量与部分出售」冻结时的原版链反汇编与数据槽证据，
> 供追溯。基线为反汇编冻结文件（原 `.tmp/push-teammate/game_full.asm`，关键段已迁入本文）。
> 版本：大修版（`apk/decoded/overhaul`）VMA；三变体符号名一致、VMA 不同，实现一律走
> `fn_resolve` 符号解析，禁止裸 VMA。

## 1. 原版链总览

```
UIStore_ButtonSellExe@0xd1818          商店出售按钮
  ├─ 可堆叠(类表 bit0) 且 b>1 → UIInputItemCount_Create(1, item, 1, max=b,
  │                              cb=UIStore_SellOKInputItemCount)
  └─ 否则 → UIPopupMsg_CreateYesNoFromTextData(payload=ITEM_GetSellPrice(item))
UIStore_SellOKInputItemCount@0xd1948   数量面板 OK
  └─ 二次确认 payload = ITEM_GetSellPrice(item) * count(全局 0x2f5000+0x238)
UIStore_SellItem@0xd25f0               确认 OK（只收总价 x0）
  └─ 重读 cursor item：可堆叠且 b>1 → ITEMSYSTEM_Divide(item, 选中数量)
                        否则 → INVEN_RemoveItem(item)
     → INVEN_AddMoney(price) → UIStore_RefreshInvenItem → 弹窗 0x1f
```

W1 采用 `UIStore_SellItem` 函数入口 hook：模块物品在入口层按 `x0 / ITEM_GetSellPrice`
反推数量接管；原版面板/确认/取消全保留，原版物品逐指令不变。

## 2. `UIStore_ButtonSellExe@0xd1818`

```
d182c  bl  ControlObject_GetCursor
d1830  bl  ControlItem_GetItem                 ; x19 = cursor item
d1844  ldrh w0,[x19,#0x8]                       ; I_TYPE
d1848  bl  UTIL_GetBitValue(_,15,6)             ; category
d184c  bl  ITEMDATABASE_IsNoSell
d1854  cbnz w0, 0xd1924                         ; 禁售 → 弹窗 0x21
d1868  ldr x3,[0x2f4000+0xcf0]                  ; item 类表基址
d1874  ldr x1,[0x2f5000+0x308]                  ; 表 stride
d1880  mul w0,cat,stride
d188c  add x0,base,+0x6
d1890  bl  MEM_ReadUint8                         ; 类表 +0x6 字节
d1894  tbnz w0,#0x0, 0xd18d0                     ; bit0 = 可堆叠
d1898  mov x0,x19
d189c  bl  ITEM_GetSellPrice                     ; 不可堆叠/单件：payload=unit
d18bc  bl  UIPopupMsg_CreateYesNoFromTextData     ; OK 槽 [0x2f5000+0x318]
; --- 可堆叠分支 ---
d18d0  ldr w0,[x19,#0x10]                         ; +0x10 数量字段
d18d4  bl  UTIL_GetBitValue(_,31,25)              ; b 段（R-51 重定向下限，见 §5）
d18e0  cmp w0,#0x1 ; b.le 0xd1898                 ; b<=1 走单件 YesNo
d18e8  ldr w0,[x19,#0x10]
d18f4  bl  UTIL_GetBitValue(_,31,25)
d18fc  adrp x4,0x2f4000 ; ldr x4,[x4,#0xd80]      ; UIInputItemCount OK 回调槽
d1904  mov x1,x19                                 ; item
d1910  bl  UIInputItemCount_Create(1,item,1,max,cb)
d1924  mov w0,#0x21 ; ... ; bl UIPopupMsg_CreateOKFromTextData   ; 禁售提示
```

## 3. `UIStore_SellOKInputItemCount@0xd1948`

```
d1958  bl  ITEM_GetSellPrice
d1964  mul x5,x0,x19            ; payload = unit * count
d1974  mov x0,#0x9 ; mov x2,#0x2 ; ldr x3,[0x2f5000+0x318]
d1980  b   UIPopupMsg_CreateYesNoFromTextData(x3=OK回调槽)
```

## 4. `UIStore_SellItem@0xd25f0`（W1 接管点）

```
d25fc  mov x20,x0                          ; x0 = 弹窗载荷（总价）
d2608  ldr x0,[0x307000+0x3f0]             ; 商店物品控件（G_UISTORE_ITEM_CTRL）
d260c  bl  ControlObject_GetCursor
d2610  bl  ControlItem_GetItem             ; x19 = cursor item
d2618  cbz x0, 0xd2698                     ; 无物品 → 弹窗 0xc
d2634  bl  UTIL_GetBitValue(_,15,6)        ; category
d2654  bl  MEM_ReadUint8                    ; 类表 +0x6 bit0 可堆叠
d2658  tbnz w0,#0x0, 0xd26b8
; --- 非堆叠 / 单件 ---
d265c  mov x0,x19
d2660  bl  INVEN_RemoveItem
d2668  cbz w0, 0xd2698                      ; 删除失败 → 弹窗 0xc
d266c  mov w0,w20
d2670  bl  INVEN_AddMoney
d2674  bl  UIStore_RefreshInvenItem
d2684  mov w0,#0x1f ; ... ; b UIPopupMsg_CreateOKFromTextData   ; 成功弹窗
; --- 可堆叠且 b>1 ---
d26b8  ldr w0,[x19,#0x10]
d26c4  bl  UTIL_GetBitValue(_,31,25)
d26c8  cmp w0,#0x1 ; b.le 0xd265c
d26d0  ldr x1,[0x2f5000+0x238]             ; 选中出售数量全局
d26d8  ldr w1,[x1]
d26e0  bl  ITEMSYSTEM_Divide(item,count)
d26e4  cbz x0, 0xd265c                      ; 拆分失败回落整删
d26e8  cmp x19,x0 ; b.ne 0xd266c            ; 拆分产物 ≠ 源 → 走加钱
```

要点：
- `UIStore_SellItem` **只收总价 x0**，数量在函数内部由 cursor item 的 `+0x10` 自行读取。
- 模块物品若不进入口接管，会落到 `INVEN_RemoveItem` / `ITEMSYSTEM_Divide` 处理逻辑对象，
  因此 W1 在入口 `count = x0 / ITEM_GetSellPrice(item)`，整除且 `1<=count<=view` 才接管。

## 5. `ITEMSYSTEM_Divide@0x1083f8`（模块物品不再走此路径）

```
108458  ldr w0,[x19,#0x10] ; UTIL_GetBitValue(_,31,25)   ; R-51 重定向点（0x108458）
108470  bl  ITEMSYSTEM_CopyAsNewUID
108490  bl  UTIL_GetBitValue(_,31,25)
1084a4  bl  UTIL_SetBitValue(_,31,25,old-count)          ; 源堆只写 b
1084c0  bl  UTIL_SetBitValue(_,31,25,count)              ; 新对象写 b
```

W1 部分出售改用 `decrement_extension_item_view_locked`（`effective_write_count` 模式感知
写回 + payload 重序列化），不调用原版 `ITEMSYSTEM_Divide` 处理扩展对象。

## 6. 数据槽证据

| 槽 | VMA | 含义 | 证据 |
|---|---|---|---|
| 商店物品控件 | `0x307000+0x3f0`（`G_UISTORE_ITEM_CTRL_VMA`） | `ControlObject_GetCursor` 的宿主控件 | `d2608` |
| 选中出售数量 | `0x2f5000+0x238` | 数量面板选中的 count，供 `ITEMSYSTEM_Divide` | `d26d0`/`d26d8` |
| 类表 stride | `0x2f5000+0x308` | `category*stride` 索引类表 | `d187c`/`d263c` |
| YesNo OK 回调槽 | `0x2f5000+0x318` | `UIPopupMsg_CreateYesNoFromTextData` 的 OK 回调 | `d18b8`/`d1970` |
| item 类表基址 | `0x2f4000+0xcf0` | 指向类表基址指针 | `d1868`/`d262c` |
| 数量面板 OK 回调槽 | `0x2f4000+0xd80` | `UIInputItemCount_Create` 第 4 参 | `d190c` |

## 7. 取证范围与后续

- 重定向安装日志与 S2 getter 行为归 `VM-37`（R-51）；本文只固化原版链与数据来源。
- 真机回归（W1）：原版物品商店出售不变；扩展物品整堆/部分出售、数量面板上限按 canonical、
  部分出售后剩余与金额一致。缺该真机证据前 W1 不得判通过。
