# inventory_hooks 审阅现场基线（2026-10-17）

## 设备
- 序列号：192.168.3.56:5555（Phh_Treble_with_GApps, arm64_ab）
- 设备时间：Sat Oct 17 14:47:31 CST 2026
- 游戏 pid：10935
- `/api/health`：`{"ok":true,"ready":true,"variant":{"series":"monster","version":"v29","known":true,"capabilities":3}}`
- `/api/ui/screen`：`{"screen":"world"}`

## 运行模块身份
- logcat `module identity`（最新）：`versionCode=192 versionName=0.7.13 sha256=a044632ac450c9b5cb96a588ea126e9e70a64be1ff4c5183b7adbe3ff00582a6`
- 集成包 `output/Inotia4_v1.3.2_monster_v29_lspatched.apk` 内 `assets/lspatch/modules/com.inotia4.qol.apk` 的 sha256 = `a044632a…`，**与运行身份一致**。
- 最新 debug 模块 APK：`output/inotia4_qol_lsposed_debug_2610171444_36baf50ae8ea.apk`（sha256 `36baf50ae8…`），**不是**当前运行版本；集成包为 14:25 重打。

## inventory_hooks 运行态（/api/system/features）
- `state=enabled`、`enabled=true`、`degraded=false`、`error=null`
- `requires=[world.facts 2.0, item.facts 3.3]`
- `provides=[inventory_refresh 1.0, inventory.action 2.0]`
- 24 条 hook 资源全部 `live`：INVEN_FindItem, INVEN_HaveItem, INVEN_GetItemCount, ITEM_GetCumulateCount, INVEN_ConsumeItem, INVEN_RemoveItem, CHAR_EquipItemFromInvenToSlot, ITEMSYSTEM_PutJewel, INVEN_IsHavingEmptySlot, CHAR_UnequipItemToInven, UIEquip_ButtonEquipExe, UIEquip_ButtonUnequipExe, UIEquip_ButtonDestroyExe, UIEquip_OKConfirmUseItem, UIEquip_OKDestroyItem, UIEquip_ButtonUseMercenarySealExe, INVEN_SaveItem, INVEN_MoveItem, UIEquip_EquipControlEventProc, UIEquip_RefreshItemArea, INVEN_SaveItemDirect, ITEMSYSTEM_Divide, ITEMSYSTEM_MakeItem, INVEN_RemoveItemData

## 观察（待阶段 1 核实，不作结论）
- 启动日志 14:47:12：`W core core.item extension inventory omitted: provider unavailable`（core.item 加载早于 inventory_hooks 的疑似装配顺序现象）。
- ARD 审计证据中曾出现 optional 环：`inventory_hooks→extension_bag`、`inventory_hooks→move_merge`（inventory_refresh）、`extension_bag→inventory_hooks`（extension_bag），需在阶段 1 依赖方向维度核实。
