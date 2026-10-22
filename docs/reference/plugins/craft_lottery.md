# `craft_lottery` 插件参考

## 1. 作用与定位

`craft_lottery` 是合成套装保底功能插件：修改融合器任务奖励物品「X级合成套装」（12 种）使用时的配方抽取行为——池内配方未全部拥有时只从未拥有的里面随机；全部拥有后走原版纯随机；原版获得配方弹窗末行追加「已获得 X/N」。不拥有合成配方语义（归 `custom_recipe`），不提供 HTTP。

## 2. 机制与实现

- 原版抽奖链：`CHAR_UseItemEx` → `CHAR_ProcessRecipe(use_type)` → `ITEMSYSTEM_DecideRecipeID`（扫 `ITEMMIXLINKBASE` 池纯随机，不查配方书）→ `MIXSYSTEM_AddRecipeBook`（配方书位图）→ 弹窗。逆向依据见 `docs/refactor/audits/plugin-audit/craft-lottery-20261022/design-card.md` §2。
- 弹窗第一行保持原版「[xxx]的配方已登录到融合器中。」不动（fmt=TEXTDATABASE[17].u16[0]=35333；RECIPEBASE[17].u16[0]=1168 是配方名，不是格式串）；追加行「{套装名}已获得：x/y」——套装名由 port 读 `ITEMDATABASE[use_type]` 首 u16（物品 id=名称文本 id），「已获得：x/y」为插件文案。
- 插件唯一资源为 entry hook `F_CHAR_PROCESS_RECIPE_VMA`（required/fail-closed）。wrapper 内：gate 关闭或 port/表异常一律直通原版（纯随机+原版弹窗，无追加行）。
- 已学判定读游戏原生配方书位图（`MIXSYSTEM_pRecipeBook`，位偏移复刻原版 `AddRecipeBook` 公式），状态跟随存档，插件零自建持久化。
- 表读取、原版函数调用、弹窗组合全部经 `QOL_PORT_CRAFT_LOTTERY_WRITE` v1.0（`game/adapter_ports/craft_lottery_port.cpp`）；插件侧无裸地址。弹窗由 port 复刻原版序列（格式串=RECIPEBASE[17].u16[0]、配方名=RECIPEBASE[idx].u16[0]、`CS_knlSprintk`、追加 `&N已获得 X/N`、`UIPopupMsg_CreateOK(buf,2,0,0,0)`）。

## 3. Descriptor 面

| 项 | 声明 |
|---|---|
| feature | `craft_lottery.guarantee`：Gameplay / toggle / default false / priority -1，名称「合成套装保底」 |
| requires / provides | 无 capability |
| ports | `QOL_PORT_CRAFT_LOTTERY_WRITE` v1.0（install 校验 ABI 与函数齐备） |
| resources | 1 条：`craft_lottery_process_recipe` entry hook @ `F_CHAR_PROCESS_RECIPE_VMA`，required/fail-closed/resumable |
| HTTP/JNI | 无 |

## 4. 行为边界

- feature 关闭：wrapper 直通原版，行为与原版完全一致。
- 集齐后再用：调原版 `ITEMSYSTEM_DecideRecipeID` 纯随机，允许重复。
- 弹窗追加行超长时安全截断；弹窗失败不回滚入书（与原版一致）。
- 未做变体适配声明：当前符号 VMA 为 overhaul arm64；其他变体按 `game_symbols.h` 变体机制适配前，目标预检失败插件进入 `unsupported`。

## 5. 文件地图

路径相对 `module/app/src/main/cpp/`。

| 子职责 | 文件 |
|---|---|
| descriptor/生命周期/port 校验 | `plugins/craft_lottery/plugin.cpp` |
| wrapper 与业务判定 | `plugins/craft_lottery/feature.{h,cpp}`、`rules.{h,cpp}` |
| 设置兼容源 | `plugins/craft_lottery/settings_contribution.{h,cpp}` |
| port 契约 | `contracts/game_ports/craft_lottery_port.h`、`contracts/game_ports/ports.h` |
| port 实现 | `game/adapter_ports/craft_lottery_port.{h,cpp}` |
| 符号 | `game/symbols/game_symbols.h` |
| 装配 | `host/assembly.cpp`、`host/plugin_registry.cpp` |
