# IH-08 逐变体 24 hook 符号矩阵（2026-10-18）

方法：从 `apk/game-apk/` 的四个变体 APK 提取 `lib/arm64-v8a/libgame.so`，运行
`uv run python scripts/maintenance/check_symbols.py <libgame.so>`，按
`plugins/inventory_hooks/native_inventory_hook.cpp` 的 `INVENTORY_HOOK_CLAIM` 表与
`game/symbols/symbol_registry.h` 的 `SYM(宏, 符号)` 映射逐条比对。

覆盖：`check_symbols.py` 只校验 A 类（已注册、可按 .dynsym 解析的地址常量）；24 条 hook 符号全部属于该类，因此矩阵覆盖完整 24 条。B 类静态地址与布局常量不在该校验范围。

| 资源 | 宏 | original | overhaul | monster | c18 |
|---|---|---|---|---|---|
| `hook:INVEN_FindItem` | `F_FIND_ITEM_VMA` | ✅ | ✅ | ✅ | ✅ |
| `hook:INVEN_HaveItem` | `F_INVEN_HAVE_ITEM_VMA` | ✅ | ✅ | ✅ | ✅ |
| `hook:INVEN_GetItemCount` | `F_INVEN_GET_ITEM_COUNT_VMA` | ✅ | ✅ | ✅ | ✅ |
| `hook:ITEM_GetCumulateCount` | `F_GET_CUMULATE_COUNT_VMA` | ✅ | ✅ | ✅ | ✅ |
| `hook:INVEN_ConsumeItem` | `F_CONSUME_ITEM_VMA` | ✅ | ✅ | ✅ | ✅ |
| `hook:INVEN_RemoveItem` | `F_REMOVE_ITEM_VMA` | ✅ | ✅ | ✅ | ✅ |
| `hook:CHAR_EquipItemFromInvenToSlot` | `F_EQUIP_ITEM_FROM_INVEN_TO_SLOT_VMA` | ✅ | ✅ | ✅ | ✅ |
| `hook:ITEMSYSTEM_PutJewel` | `F_PUT_JEWEL_VMA` | ✅ | ✅ | ✅ | ✅ |
| `hook:INVEN_IsHavingEmptySlot` | `F_INVEN_IS_HAVING_EMPTY_SLOT_VMA` | ✅ | ✅ | ✅ | ✅ |
| `hook:CHAR_UnequipItemToInven` | `F_UNEQUIP_VMA` | ✅ | ✅ | ✅ | ✅ |
| `hook:UIEquip_ButtonEquipExe` | `F_UIEQUIP_BUTTON_EQUIP_EXE_VMA` | ✅ | ✅ | ✅ | ✅ |
| `hook:UIEquip_ButtonUnequipExe` | `F_UIEQUIP_BUTTON_UNEQUIP_EXE_VMA` | ✅ | ✅ | ✅ | ✅ |
| `hook:UIEquip_ButtonDestroyExe` | `F_UIEQUIP_BUTTON_DESTROY_EXE_VMA` | ✅ | ✅ | ✅ | ✅ |
| `hook:UIEquip_OKConfirmUseItem` | `F_UIEQUIP_OK_CONFIRM_USE_ITEM_VMA` | ✅ | ✅ | ✅ | ✅ |
| `hook:UIEquip_OKDestroyItem` | `F_UIEQUIP_OK_DESTROY_ITEM_VMA` | ✅ | ✅ | ✅ | ✅ |
| `hook:UIEquip_ButtonUseMercenarySealExe` | `F_UIEQUIP_BUTTON_USE_MERCENARY_SEAL_EXE_VMA` | ✅ | ✅ | ✅ | ✅ |
| `hook:INVEN_SaveItem` | `F_INVEN_SAVE_ITEM_VMA` | ✅ | ✅ | ✅ | ✅ |
| `hook:INVEN_MoveItem` | `F_INVEN_MOVE_ITEM_VMA` | ✅ | ✅ | ✅ | ✅ |
| `hook:UIEquip_EquipControlEventProc` | `F_UIEQUIP_EQUIP_CONTROL_EVENT_PROC_VMA` | ✅ | ✅ | ✅ | ✅ |
| `hook:UIEquip_RefreshItemArea` | `F_UIEQUIP_REFRESH_ITEM_AREA_VMA` | ✅ | ✅ | ✅ | ✅ |
| `hook:INVEN_SaveItemDirect` | `F_INVEN_SAVE_ITEM_DIRECT_VMA` | ✅ | ✅ | ✅ | ✅ |
| `hook:ITEMSYSTEM_Divide` | `F_ITEMSYSTEM_DIVIDE_VMA` | ✅ | ✅ | ✅ | ✅ |
| `hook:ITEMSYSTEM_MakeItem` | `F_MAKE_ITEM_VMA` | ✅ | ✅ | ✅ | ✅ |
| `hook:INVEN_RemoveItemData` | `F_INVEN_REMOVE_ITEM_DATA_VMA` | ✅ | ✅ | ✅ | ✅ |

结论：四个变体的 24 条 hook 符号全部可解析且 VMA 与 `game_symbols.h` 一致；未发现变体缺符号。`kRequired/kFailClosed` 的「任一符号缺失整插件回滚」风险在当前四个变体上不成立；新增变体时仍需先跑本矩阵。

原始输出：`.tmp/inventory-hooks-audit/libs/<variant>/check.txt`。
