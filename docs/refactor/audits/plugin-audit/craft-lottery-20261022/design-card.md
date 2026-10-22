# `craft_lottery` 设计卡与验收卡（2026-10-22）

## 1. 需求

idea.md 第 8 项「合成器任务抽奖」：合成器（融合器）任务奖励物品「X级合成套装」使用后随机获得一种配方。改为：未获得其可能抽奖结果的全部时，固定只从没有的里面抽；全部都有了，则回到原版的纯随机。用户补充：原版使用该物品有提示条，要求在其后加字，显示「已获得 X/N」。

## 2. 原版机制（overhaul arm64 libgame.so 静态逆向）

| 环节 | 事实 |
|---|---|
| 奖励物品 | ITEMDATABASE 下标 706-717（id=下标+30，736-747）：D/C/B/A/S 级武器/防具合成套装共 12 种；ITEMDESCBASE 同下标 12 条描述「把XX中随机一种的合成配方放进融合器中」。 |
| 抽奖链 | `CHAR_UseItemEx`(0xeb670) 经 `UTIL_GetBitValue(x,15)` 取使用效果 id 分派；配方类调 `CHAR_ProcessRecipe(use_type)`(0xdcde4)。 |
| 池 | `ITEMMIXLINKBASE`（pData 0x301a60 / recSize 0x301a68 / recCount 0x301a6a）52 条记录 `[use_type, RECIPEBASE 下标]`；use_type 706-717 → 配方下标 17-68；下标 0-16 为普通配方（尾字节 0x03，bit5=0）。 |
| 纯随机 | `ITEMSYSTEM_DecideRecipeID`(0x1081e4)：两遍扫描链接表，`MATH_GetRandom(0, count-1)` 选第 k 个匹配，返回其 RECIPEBASE 下标；无匹配返回负值。不查配方书（改造点）。 |
| 已学状态 | `MIXSYSTEM_AddRecipeBook`(0x11b938)：`RECIPEBASE[idx]` 尾字节 bit5=1 才入书；位图位偏移 `idx + GetRecipeCount(5) - GetRecipeCount(0)`；已置位则幂等返回。位图指针 `MIXSYSTEM_pRecipeBook`(0x307760)，容量 `MIXSYSTEM_GetRecipeBookSize`(0x11b450)。 |
| 弹窗 | `CHAR_ProcessRecipe` 内 `MEM_Malloc(128)` → `MEMORYTEXT_GetText(RECIPEBASE[17].u16[0])`（格式串）与 `MEMORYTEXT_GetText(RECIPEBASE[idx].u16[0])`（配方名）→ `CS_knlSprintk` → `UIPopupMsg_CreateOK(buf,2,0,0,0)` → 返回 1。 |
| 数据表 | RECIPEBASE 69 条，u16[0]=1151+下标（配方 id，即文本 id）；ITEMMIXLINKBASE 各套装池：706→17-21、707→22-25、708→26-30、709→31-34、710→35-39、711→40-43、712→44-47、713→48-52、714→53-59、715→60-62、716→63-65、717→66-68。 |

## 3. 设计决策

1. **新功能插件** `craft_lottery`，不并入 `custom_recipe`：后者是自定义配方 owner；本功能是修改原版套装抽奖行为，语义独立（ARCHITECTURE §5.3）。
2. **wrap `CHAR_ProcessRecipe` 而非 hook `ITEMSYSTEM_DecideRecipeID`**：弹窗文本在 ProcessRecipe 内拼接，hook Decide 无法加字；wrap 上层函数后保底与加字都收在 wrapper 内，hook 数量最少（1 条）。
3. **降级语义 fail-soft**：gate 关闭、backup 缺失、port 异常、池空、判定失败等一律直通原版（行为回到原版纯随机+原版弹窗），不阻断游戏链路。
4. **插件零裸地址**：表读取、位图判定、原版函数调用全部经新 port `QOL_PORT_CRAFT_LOTTERY_WRITE` v1.0（Game Adapter 内实现）；插件只做业务判定。
5. **X/N 计算**：入书前统计已学数 learned_before；本次选中配方原本未学则 X=learned_before+1，否则 X 不变（重复抽取情形）；N=池大小。文案「已获得 %u/%u」UTF-8 常量。
6. **追加行格式**：第一行原版「[xxx]的配方已登录到融合器中。」不动；第二行「&N{套装名}已获得：x/y」——套装名由 port 读 `ITEMDATABASE[use_type]` 首 u16（物品 id 与名称文本 id 同域同值，导出表 `TEXT_ID_FIELDS` 与 `text/zh-Hans.json` id 736-747 已验证）经 `MEMORYTEXT_GetText` 补入；「已获得：x/y」为插件 UTF-8 常量。
7. **配方书状态零自建**：跟随游戏原生配方书（存档内），多存档天然隔离；插件不持久化任何数据。

## 4. 实现位置

| 层 | 文件 |
|---|---|
| 符号 | `game/symbols/game_symbols.h`（F_CHAR_PROCESS_RECIPE / F_ITEMSYSTEM_DECIDE_RECIPE_ID / F_MIXSYSTEM_ADD_RECIPE_BOOK / F_MIXSYSTEM_GET_RECIPE_COUNT / F_MIXSYSTEM_GET_RECIPE_BOOK_SIZE / G_MIXSYSTEM_P_RECIPE_BOOK / G_ITEMMIXLINKBASE_* 共 9 条 + F_CS_KNL_SPRINTK / F_UIPOPUPMSG_CREATE_OK） |
| port 契约 | `contracts/game_ports/craft_lottery_port.h`（QolCraftLotteryWritePort v1.0）、`contracts/game_ports/ports.h`（QOL_PORT_CRAFT_LOTTERY_WRITE=31） |
| port 实现 | `game/adapter_ports/craft_lottery_port.{h,cpp}` |
| 插件 | `plugins/craft_lottery/`（descriptor、wrapper、rules、settings 兼容入口） |
| 装配 | `host/assembly.cpp`（descriptor 注册）、`host/plugin_registry.cpp`（port 接线） |
| 测试替身 | `tests/test_game_port_stubs.cpp`（port stub） |

## 5. 已知限制与风险

- 位图偏移公式为静态逆向结论，真机抽取与游戏内配方列表对照后才算 PROVEN。
- monster/原版变体的符号 VMA 未核对（本批仅 overhaul arm64）；变体适配走 game_symbols.h 既有变体机制。
- 弹窗 buf 原版 128 字节，port 实现扩为 192；fmt+名+追加行超长时安全截断（追加行可能被截）。
- `check_layers` 存在既有 `custom_map` 6 条越层（commit 5140b0af 引入），与本批无关。

## 6. 验收卡 VM-CL-01

前置：部署含本功能的 debug 集成包；设置页开启「合成套装保底」。

| # | 步骤 | 预期 |
|---|---|---|
| 1 | 使用任一合成套装（如 D级武器合成套装） | 获得配方；弹窗末行为「已获得 X/N」；N 与该套装描述的类别数一致（如 D级武器=5） |
| 2 | 保留其余配方未抽，连续使用同套装 | 每次获得的配方不与已有重复；X 逐次 +1 |
| 3 | 集齐该套装全部配方后再使用 | 恢复纯随机（允许重复）；X=N |
| 4 | 关闭 feature 后使用套装 | 行为与原版一致：纯随机、弹窗无追加行 |
| 5 | 换存档验证 | 配方书状态随存档隔离，X/N 正确反映当前存档 |
| 6 | 大修/monster/原版三变体各验证 1-2 | 模块不崩溃；未适配变体功能降级为原版行为或不可用，但不影响其他功能 |

证据登记：`verification-log.md`。
