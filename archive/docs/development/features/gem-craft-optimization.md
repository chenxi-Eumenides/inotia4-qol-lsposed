# 合成器宝石合成操作优化（gem-craft-optimization）——已归档

**本功能已不存在。** 其实现于 2026-09-16 按「界面归界面，结果归结果」的架构裁决拆分并入：

| 原属 gemcraft | 现状 |
|---|---|
| 自动选中格（进入视图 / 放料后 / 清空后三处） | 归 `feature/craft_ui`（`craft_ui.{h,cpp}` + `craft_ui_hooks.{h,cpp}`） |
| UIMix 控件读写原语（`uimix_slot` / `read_filled_slots` / `selected_inven_item` / `first_empty_slot` …） | 归 `feature/craft_ui`（原 `feature/gemcraft/gemcraft_rules.{h,cpp}` 已删除） |
| 宝石合成的配方规则 | 早已存在于配方表中，本次起**统一走隐式配方**：`{28,28,28}→29`、`{29,29,29}→30`、`{30,30,30}→31`、`{31,31,31}→32`（`custom_recipe_catalog.h` 的 `kThreeSlotRecipes`，均 order-independent） |
| 阶段 2「改写原版流程」三件套（放料解绑 `stuffList[0]` / 同档校验弹 0x62 / mixType 改写为 `12+(category-28)`） | **已删除**，不再需要：3 格模式下放料与合成由配方层自实现 |
| 配置开关 `gemCraftOptimize` | **已删除**；两功能共用 `customRecipeEnabled`（设置面板显示名「新合成系统」） |

## 当前文档

- 界面层：`docs/development/features/craft-ui.md`
- 配方层：`docs/development/features/custom-craft-recipe.md`

## 归档说明

本目录下 `gem-craft-optimization.md` 是**历史设计书原文**，保留其撰写时的方案与真机验证记录（含已废弃的放料解绑 / 同档校验 / mixType 改写三件套与 `gemCraftOptimize` 开关）。
**它不是当前实现依据**；当前状态以上面两份文档与代码为准。归档日期：2026-09-16。
