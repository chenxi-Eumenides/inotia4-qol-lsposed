# item.facts 类型与使用路径

本文记录 `item.facts` 4.0 的权威 `item_type`，以及各类物品的原生使用路径。目标是让扩展背包只识别 UI 身份，类型判断和动作一律由 core.item 提供。

## 1. 权威类型接口

`ItemFactsApi::item_type(handle, out)` 是唯一类型判断入口。

```text
kUnknown
kConsumable
kEquipment
kBackpack
kJewel
kEnchantScroll
kMercenarySeal
kSkillBook
kItemBox
kSynthSet
kPotion
kDice
kReviveScroll
kLamp
kUseAfterConfirm
kOther
```

旧的 `is_real_equip`、`is_jewel_category` 已从 `ItemFactsApi` 和 `QolItemPort` 移除，不得再新增基于 category 的平行类型判断。

## 2. 识别依据

识别顺序固定，先特殊类型，再通用类型：

| 类型 | 判断依据 |
|---|---|
| `kBackpack` | `ITEMCLASSBASE + 2 == 0x1F` |
| `kEquipment` | `ITEM_IsRealEquip` |
| `kJewel` | `ITEMSYSTEM_IsJewel` |
| `kEnchantScroll` | `ITEMSYSTEM_IsEnchantScroll` |
| `kMercenarySeal` | `ITEMSYSTEM_IsMercenarySeal` |
| `kSkillBook` | category 51，或 `ITEMSYSTEM_IsSealed` |
| `kItemBox` | `ITEMSYSTEM_IsItemBox` 或 `ITEMSYSTEM_IsPackItem` |
| `kDice` | `ITEMSYSTEM_IsDice` |
| `kReviveScroll` | category 26/27；`CHAR_UseItemEx` 内对应 `CHAR_ProcessReviveScroll` |
| `kLamp` | category 62；`CHAR_UseItemEx` 内对应 category `0x3e` 分支 |
| `kUseAfterConfirm` | `ITEMSYSTEM_IsUseAfterConfirm` |
| `kSynthSet` | category 706..717 且 `ITEMCLASSBASE +2 == 0x21` |
| `kPotion` | `ITEMDATABASE_IsUse` 或 use-effect `0x16/0x17` |
| `kConsumable` | 其他 `ITEMCLASSBASE +2 > 0` 的使用效果物品 |
| `kOther` | 无使用效果且不属于以上类型 |

`ITEMDATABASE_IsUse` 只读 `ITEMCLASSBASE +2` 的 `0x16/0x17`，是普通可使用标记，不是完整物品类型。技能书、合成套装、神灯、骰子、徽章、开箱、装备、卷轴都不能用它判断。

## 3. 使用路径

### 共用 `CHAR_UseItemEx`

以下类型交给同一个原生入口，由游戏内部按类别/使用效果分流：

- `kPotion`
- `kReviveScroll`
- `kLamp`
- `kSkillBook`
- `kSynthSet`
- `kConsumable`

`CHAR_UseItemEx` 内部已有：

- 复活卷轴 → `CHAR_ProcessReviveScroll`
- 合成套装 → `CHAR_ProcessRecipe`
- 技能书 → 技能书分支
- 礼盒/包裹 → `ITEMSYSTEM_ProcessUnpack`
- 神灯 → category `0x3e` 特殊分支

这条路径不依赖 `INVEN_pItem` 指针表；物品指针只用于传入、消耗和 hook。

### 独立路径

| 类型 | 原生路径 |
|---|---|
| `kDice` | `STATUSDICE_Roll` + pending 状态/确认 |
| `kMercenarySeal` | `MERCENARYSYSTEM_MakeMercenary` |
| `kItemBox` (1007..1009) | `ITEMSYSTEM_OpenItemBox` |
| `kItemBox` (pack，如 924) | `CHAR_UseItemEx` → `ITEMSYSTEM_ProcessUnpack` |
| `kUseAfterConfirm` | 确认 UI，然后 `CHAR_UseItemEx` |
| `kEquipment` / `kBackpack` | 装备/背包路径 |
| `kJewel` / `kEnchantScroll` | apply 路径，不调用 use |

## 4. 入口规则

`game/ops/item_inventory_state_write.cpp::use_item` 不再用 `ITEMDATABASE_IsUse` 提前拒绝特殊使用类型：

```text
sealed / item box / dice / mercenary 先分流
否则允许 ITEMCLASSBASE +2 > 0 的物品进入 CHAR_UseItemEx
```

因此技能书、合成套装、神灯等不会被误判为“不可使用”。

## 5. 快捷栏

`CHAR_ProcessShortcut` 的条目是“类型 + 类别”，不是物品句柄。

- 原生物品数量 > 0 时走原版路径。
- 原生数量为 0 时，core.item 通过 `QolBagSource` 按类别查找注册源物品，调用源的 `use`。
- 这使扩展袋药水、复活卷轴等已登记快捷栏物品可以触发，不需要写 `INVEN_pItem`。
- 快捷栏条目读取只暴露类别，不暴露可变物品指针。

## 6. 扩展背包边界

- EB 只保存扩展侧 sidecar、捕获 UI 详情身份、转发动作。
- 类型判断必须调用 `item.facts.item_type`；EB 不得再用 category 或 `ITEM_*` 原语自行判断。
- 背包类装备来源按 `QolBagRef` 的 space/bag/slot 处理，目标空槽由 core `find_empty_slots` 返回，原生空槽优先；C18 改版没有背包类物品逻辑，不参与该分支。
- 旧 `read_item_is_jewel`、`read_item_is_enchant_scroll`、`read_item_is_mercenary_seal`、`item_is_equip`、`category_is_extension_backpack` 等包装不得继续扩散。

## 6. 后续扩展

其他改版独立物品的分类器应在 native 识别之前接入 `item_type` 分类链；当前先把原生类型收口，后续新增分类器时保持同一顺序和单一返回类型。
