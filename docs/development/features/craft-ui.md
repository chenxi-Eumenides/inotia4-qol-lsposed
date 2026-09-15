# 合成器界面层（craft-ui）

## 1. 范围

本域是**合成器（UIMix 面板）的界面层**：所有 UIMix 控件操作、全部函数级 hook 的挂载与分派、自动选中格、弹窗与文案决策。

配方知识**不在本域**。本域只做界面判断与呈现，任何「这三格是什么结果」都问配方层（`feature/custom_recipe`，见 `custom-craft-recipe.md`），并按它回传的枚举决定弹什么文案、要不要清空填入格。

## 2. 分层与依赖方向（用户裁决 2026-09-16）

> 「选中、弹窗、恢复界面，都是 UI 相关操作，放在一起；配方、是否合成成果、产出结果、消耗材料，都是自定义配方，放在一起。**界面归界面，结果归结果**。」
> 「因为本质上，**宝石合成也是一个隐式配方**。」「**统一走隐式配方**，**两者公用同一套开关**。」

```
craft_ui  ──问──▶  custom_recipe     「这三格是什么结果？」
craft_ui  ◀─答──   custom_recipe     四个结果枚举 + 产物类别 + 要扣什么
```

- **依赖单向**：`craft_ui → custom_recipe`。配方层不认识界面，一行弹窗/选中/控件动作都没有。
- **没有策略注册机制**：只有一家配方层，craft_ui 直接问它。
- 配方层的对外接口与枚举定义见 `feature/custom_recipe/custom_recipe_api.h`。

本域是原 `feature/ui/game_ui_gemcraft.{h,cpp}` 与 `feature/custom_recipe/game_ui_custom_recipe.cpp` 中界面部分的合并归属地；两级之间原先靠「gemcraft 关闭分支调函数地址、恰好命中 custom_recipe 的函数入口 hook」隐式串联，该隐式依赖已消除。

## 3. 文件

| 文件 | 职责 |
|---|---|
| `feature/craft_ui/craft_ui.{h,cpp}` | UIMix 控件原语（唯一实现处）：只读/状态读写 + 界面动作 |
| `feature/craft_ui/craft_ui_hooks.{h,cpp}` | 11 处 UIMix hook 的**单一挂载处** + 文案常量 + 绘制状态 |

`craft_ui.cpp` 内部的 `first_empty_slot()` 是原 `feature/gemcraft/gemcraft_rules.h` 同名算法的内联（该文件已删除）。

## 4. 控件原语（`craft_ui`）

- 状态：`slot(offset)`、`mix_type()`（`[+0x48]`）、`ui_type()`（`[+0x38]`）、`selected_recipe_at(type)` / `set_selected_recipe_at(type,value)`（`[+0x100+type*8]`）、`target_slot_control()` / `target_slot_item()` / `set_target_slot_item(item)`（`[+0x40]`）
- 填入格（`[+0xc8]` 组前 3 子控件）：`stuff_slot_control(i)`、`stuff_item(i)`、`set_stuff_item(i,item)`、`read_stuff_filled(out)`、`read_stuff_categories(out)`（空格填 0）；`kStuffSlotCount = 3`
- 选中与背包：`selected_stuff_index()`（`[+0x128]`，-1 = 未选中）、`select_stuff_slot(i)`、`select_first_empty_stuff_slot()`、`selected_inven_item()`（`[+0xd8]` 组 cursor → `GetData` → `*data`）
- 界面动作：`init_mixing_state()`、`reset_stuff_item_control()`、`refresh_inven_items()`、`reset_stuff_and_refresh()`、`set_ui_type(type)`（仅 0..4，越界拒绝并 warn）、`play_sound(id)`
- 弹窗：`show_text(word_id)` → `fn_popup_create_ok_from_textdata(word_id,0,0,0)`
- hook 辅助：`install_one(hook, target, replacement, backup, name)`

## 5. 挂钩清单（11 处，同一 UIMix 函数只挂一次）

| UIMix 函数 | 语义 |
|---|---|
| `UIMix_ButtonInvenItemSelectExe` | 放料：3 格模式自实现；模块注入的 `kJewelTierUp` 配方走「转调原版 + 纠正 + 费用 0」；其余转调原版 |
| `UIMix_ButtonMixingExe` | 合成按钮：3 格模式查表（未命中 → 弹 94 + 清空补选中；命中 → 弹确认框）；`kJewelTierUp` 走 `CraftGate` 前置（非宝石弹 97 / 已金档弹 105） |
| `UIMix_ButtonMenuListExe` | 页签：转调前确保配方表注入完成（首次点开即能看到注入配方），转调后进入视图自动选中 |
| `UIMix_ButtonRecipeExe` | 配方点击：转调后按 `Form` 借用 type 做 `SetType → InitMixingState → ResetStuffItemControl`，并快照/恢复该 type 的已选槽 |
| `MIXSYSTEM_MakeItem` | 产物：`kJewelTierUp` 读改写宝石数值位；非本层配方转调原版 |
| `X_TEXTCTRL_SetTextControl` | 描述文本落点：按**内容**判定（当前 mixType 命中模块记录 + 文本以原版模板前缀「用3个」开头 + 只做缩短替换），命中则先换成模块文案再转调 |
| `ControlItem_Draw` + `ITEM_DrawPorting` | 3 格填入格**不显示整堆数量**（格子语义 = 1 个单位）；绘制层零日志 |
| `UIMix_ButtonRecipeDraw` / `UIMix_Draw` / `UIMix_ButtonMenuListDraw` | 模块文案的三个绘制窗口作用域（配方按钮、面板标题、页签）；页签保持原文「宝石强化」 |
| `UIMix_ResetStuffItemControl` | 清空填入格后**补选中第一个空格**（函数入口 hook ⇒ 覆盖所有调用者：原版成功链、模块配方失败清空、换型重建） |

前 5 处为**核心链**（任一失败即安装失败且可重试）；其余为**装饰性**，单独挂载、失败只告警，不拖垮核心链。

## 6. 自动选中格（三处）

| 触发 | 行为 |
|---|---|
| 进入合成视图（type 1 且 `[+0x20] != 0`） | 选中第一个空格 |
| 放料之后 | 选中第一个空格 |
| 清空填入格之后（`UIMix_ResetStuffItemControl`） | 选中第一个空格；**不随开关门控**（清空可能来自配方层，不该被别的开关左右） |

## 7. 弹窗与文案（界面层的决策）

界面按配方层回传的枚举决定呈现，文案常量集中在 `craft_ui_hooks.cpp`：

| 结果 | 呈现 |
|---|---|
| `PlaceOutcome::kAlreadyPlaced` | 弹 99（已放入），不清空 |
| `PlaceOutcome::kNotEnoughHeld` | 弹 94（材料不足），不清空（本次放置被拒，格内无新内容） |
| `PlaceOutcome::kPlaced` | 选中第一个空格 |
| `PrepareOutcome::kNoMatch` | 弹 94 + 清空 + 补选中 |
| `PrepareOutcome::kConfirmPending` | 弹确认框 0x12，回调 = `three_slot_execute()` 后按 `CraftOutcome` 收尾 |
| `CraftOutcome::kOk` | 成功收尾：`InitMixingState → ResetStuffItemControl → RefreshInvenItem → 音效 9 → 弹 106`（原版 `UIMix_StartMix` 收尾序列） |
| `CraftOutcome::kStaleSlots` | 弹 94 + 清空 + 补选中 |
| `CraftOutcome::kNotEnoughHeld` | 弹 94，**不清空**（保留已填格便于补料重试） |
| `CraftOutcome::kNoBagSpace` | 弹 5（背包空间不足）+ 清空 + 补选中 |
| `CraftGate::kNotJewel` / `kAlreadyGold` | 弹 97 / 弹 105 |

## 8. 开关

**没有独立开关。** 本域与原 `gemCraftOptimize` 合并为 `customRecipeEnabled`（设置面板显示名「新合成系统」，默认 false）。停用时表注入的记录数会收回，配方查询自然不再命中注入记录。

## 9. 验证证据

- host：`ctest` 15/15
- 真机（APK `8fd23863aa88`）：`custom_recipe craft_ui_hooks.cpp:511 craft ui hooks installed core=5 desc=1 slotcount=1 label=1 title=1 tab=1 reset=1`
- 已实测：3 格放料/合成、失败清空后选中空格、宝石升阶、动态特殊装备配方、含空槽的动态配方、原版「宝石强化」条目、模块配方文案
