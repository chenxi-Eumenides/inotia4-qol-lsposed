# `craft_lottery` 插件参考

## 摘要

功能插件（纯消费者、默认关闭的叶子）：修改融合器任务奖励物品「X级合成套装」（12 种）使用时的配方抽取行为——奖励池内配方未全部拥有时，只从未拥有的配方中随机；全部拥有后回退原版纯随机；原版「获得配方」弹窗第一行保持原版不动，末行追加「{套装名}已获得：x/y」。

插件通过唯一入口 hook `CHAR_ProcessRecipe` 实现：wrapper 内完成池读取、配方书判定与选择，任何依赖异常都直通原版（fail-soft 降级）。已学状态直接读游戏原生配方书位图（随存档），插件零自建持久化。

除特别说明，文中 `plugins/...`、`contracts/...`、`game/...`、`host/...` 路径均相对 `module/app/src/main/cpp/`。

## 定位与版本

`craft_lottery`，插件版本 `0.1.0`（`plugins/craft_lottery/plugin.cpp:11-12`）。descriptor（`plugin.cpp:106-124`）：

- `requires` / `optional_requires` / `provides` 0 项；JNI 0、HTTP route/endpoint 0。
- `resources` 1 个入口 hook（`plugin.cpp:23-40`）。
- `ports` 1 项：`QOL_PORT_CRAFT_LOTTERY_WRITE` v1.0（`plugin.cpp:17-22`；`contracts/game_ports/ports.h:41`）。
- 生命周期回调：`plugin_install`、`plugin_deactivate`、`plugin_on_enabled`、`plugin_on_disabled`、`plugin_fill_status`、`on_resource_ready`（`plugin.cpp:44-94,119`）。

注册位于 `host/assembly.cpp:230`；port 接线位于 `host/plugin_registry.cpp:265-268`。插件是功能插件叶子：没有 capability 依赖，也没有消费者依赖它。

## 公开能力与路由

- 无 capability `provides`，无 `routes.inc`，无 HTTP query/endpoint/route metadata，也无 JNI。启停只能经 Host 常驻控制面（API 参考 §7.6 配置/feature、§7.7 状态）。
- 唯一 feature 为 `craft_lottery.guarantee`：默认 `0`、分类 `kBasicOptimization`、类型 `kToggle`、priority `-1`、名称「合成器传说保底」、描述「套装优先出未获得的配方」（`plugin.cpp:96-104`）。回调在 `enabled == 0` 时立即 `set_gate(false)`；开启方向不直接开门（由 lifecycle 的 `plugin_on_enabled` 负责）。
- `want_run = enable && any(feature)`（API 参考 §7.6）。本插件只有 1 个 feature，设置页关闭 feature 会在同一次 feature 提交中派生 `enable=false`，随后推进完整 deactivate。

## 游戏机制（原版事实）

原版链路（overhaul arm64，逆向依据见 `docs/refactor/audits/plugin-audit/craft-lottery-20261022/design-card.md` §2）：

1. 使用「X级合成套装」（ITEMDATABASE 下标 706-717，id=下标+30）时 `CHAR_UseItemEx`（0xeb670）按使用效果 id 分派到 `CHAR_ProcessRecipe(use_type)`（0xdcde4）。
2. `ITEMSYSTEM_DecideRecipeID`（0x1081e4）扫描 `ITEMMIXLINKBASE`（pData 0x301a60 / recSize 0x301a68 / recCount 0x301a6a）取 `use_type` 匹配的全部 `RECIPEBASE` 下标，`MATH_GetRandom(0, count-1)` 纯随机选一条；**不查询配方书**（本插件的改造点）。
3. `MIXSYSTEM_AddRecipeBook`（0x11b938）把配方写入配方书位图 `MIXSYSTEM_pRecipeBook`（0x307760）：`RECIPEBASE[idx]` 尾字节 bit5=1 才入书；位偏移 `idx + GetRecipeCount(5) - GetRecipeCount(0)`；已置位则幂等返回。
4. 弹窗：`MEM_Malloc` → `MEMORYTEXT_GetText(TEXTDATABASE[17].u16[0])`（格式串 35333「[%s]的配方已登录到融合器中。」）与 `MEMORYTEXT_GetText(RECIPEBASE[idx].u16[0])`（配方名）→ `CS_knlSprintk` → `UIPopupMsg_CreateOK(buf,2,0,0,0)`。

各套装池（ITEMMIXLINKBASE 52 条 `[use_type, RECIPEBASE 下标]`）：706→17-21、707→22-25、708→26-30、709→31-34、710→35-39、711→40-43、712→44-47、713→48-52、714→53-59、715→60-62、716→63-65、717→66-68；下标 0-16 为普通配方（尾字节 bit5=0，不入配方书）。

## 游戏内行为

### 入口 hook

唯一资源 `hook:CHAR_ProcessRecipe`（`feature.h:13`）：`kEntryHook`、`private`、`required`、`failClosed`、`resumable=true`、`gate=nullptr`（资源跟随插件生命周期），handler 为 `process_recipe_wrapper`（`plugin.cpp:23-40`；符号 `F_CHAR_PROCESS_RECIPE_VMA` = 0xdcde4，`game/symbols/game_symbols.h:854`；解析登记 `game/access/symbols.cpp:105-106`）。

### wrapper 决策序列（`feature.cpp:58-138`）

按顺序，任一步失败即直通原版 `CHAR_ProcessRecipe`（backup）：

1. `backup` 缺失、gate 关闭、port 未注入或函数不齐 → 原版。
2. `pool_read(use_type)` 失败或池空 → 限频 degraded 日志（`note_degraded_once`，`feature.cpp:45-48`）后原版；池空时原版 `DecideRecipeID` 同样返回负值，行为一致。
3. 逐条 `recipe_learned` 判定，任一失败 → 原版。
4. 未学集合非空 → `get_random(len-1)` 取一条（与原版 `MATH_GetRandom(0, count-1)` 同源同分布）；为空 → `decide_recipe_pure`（原版 `ITEMSYSTEM_DecideRecipeID`，允许重复）。
5. 结果复核属池内且非负，否则 → 原版。
6. `add_recipe_book`（原版写入，幂等）；统计 `x = learned_before + (本次配方原本未学 ? 1 : 0)`。
7. `snprintf("已获得：%u/%u", x, 池大小)` → `show_recipe_popup`。弹窗失败不回滚入书（与原版一致），仅记 degraded，返回 1。

### 弹窗追加行

第一行保持原版不动；第二行 `&N{套装名}已获得：x/y`（`&N` 为游戏文本域换行标记，双字符）。套装名由 port 读 `ITEMDATABASE[use_type]` 首 u16（物品 id 与名称文本 id 同域同值，文本表 id 736-747 已验证）经 `MEMORYTEXT_GetText` 取得；「已获得：x/y」为插件 UTF-8 常量（`feature.cpp:125-133`）。追加内容超 buf 容量时安全截断（buf 192 字节，原版 128）。

## 使用的 Host SDK 与 Game 面

- Host SDK：log、in_flight 计数（`feature.cpp:60-61`）、typed 命令无、帧任务无。
- Game Adapter port `QOL_PORT_CRAFT_LOTTERY_WRITE` v1.0（`contracts/game_ports/craft_lottery_port.h:11-30`；id 31，`ports.h:41`），六个函数全部主线程同步：

| 函数 | 语义 | 实现要点（`game/adapter_ports/craft_lottery_port.cpp`） |
|---|---|---|
| `pool_read` | use_type 匹配的全部 RECIPEBASE 下标 | 全表扫描 ITEMMIXLINKBASE；容量不足返回错误并写所需数（`:109-137`） |
| `recipe_learned` | 配方书位图判定 | 位偏移 `idx + GetRecipeCount(5) - GetRecipeCount(0)` 动态调用原版函数；越界报错不伪装未学（`:139-176`） |
| `decide_recipe_pure` | 原版纯随机决策 | 调 `ITEMSYSTEM_DecideRecipeID`，负值=池空（`:178-187`） |
| `add_recipe_book` | 原版配方书写入 | 调 `MIXSYSTEM_AddRecipeBook`（`:189-196`） |
| `get_random` | 原版随机源 | `MATH_GetRandom(0, n)` 闭区间，结果越界报错（`:198-210`） |
| `show_recipe_popup` | 弹窗组合 | 复刻原版序列（TEXTDATABASE[17] 格式串 + RECIPEBASE[idx] 配方名 + sprintk + `&N{套装名}` + extra + CreateOK(2,0,0,0)）；套装名读取失败则只显示 extra（`:218-300`） |

新增符号（`game/symbols/game_symbols.h`）：`F_CHAR_PROCESS_RECIPE_VMA`、`F_ITEMSYSTEM_DECIDE_RECIPE_ID_VMA`、`F_MIXSYSTEM_ADD_RECIPE_BOOK_VMA`、`F_MIXSYSTEM_GET_RECIPE_COUNT_VMA`、`F_MIXSYSTEM_GET_RECIPE_BOOK_SIZE_VMA`、`G_MIXSYSTEM_P_RECIPE_BOOK_VMA`、`G_ITEMMIXLINKBASE_*`、`G_ITEMDATABASE_*`、`G_TEXTDATABASE_*`、`F_CS_KNL_SPRINTK_VMA`、`F_UIPOPUPMSG_CREATE_OK_VMA`。`MATH_GetRandom`/`MEMORYTEXT_GetText`/`MEM_Malloc`/`MEM_Free` 复用既有符号。

## 消费者与职责边界

- 本插件不提供 capability、port 或路由，无消费者。
- 不拥有合成配方语义（`custom_recipe` 是自定义配方唯一 owner）；不改 RECIPEBASE/ITEMMIXLINKBASE 表内容（无 ownership，只读原版表）；不写配方书以外的任何游戏状态。
- 弹窗不改第一行原版文本，不经 `core.custom_ui` 文本注册机制（追加行为非文本替换）。

## 失败语义

- 全部降级路径为「直通原版」：行为回到原版纯随机 + 原版弹窗（无追加行），不阻断使用套装。
- 首次降级记一条 `degraded craft lottery operation=<点>` 告警（进程内限频一次，`feature.cpp:45-48`）；`plugin_fill_status` 不上报 degraded（降级不改变插件健康态）。
- hook 安装失败（required/fail-closed）→ 插件安装失败回滚，不半安装。

## 生命周期

- install（`plugin.cpp:42-74`）：校验 PluginContext/SDK/port_count==1/port ABI 与函数齐备；任一失败幂等清理返回 1。
- `plugin_on_enabled` 置 gate；`plugin_on_disabled` 清 gate（`plugin.cpp:81-89`）。feature 关闭时 Host 推进 deactivate → hook 释放 → 游戏完全走原版。
- deactivate 清 port/sdk/backup 引用（`clear_install_state`，`plugin.cpp:26-33` 处 helper、`:76-79`）。

## 文件地图

| 子职责 | 文件 | 职责 |
|---|---|---|
| descriptor/生命周期/port 校验 | `plugins/craft_lottery/plugin.cpp` | feature、kPorts/kResources、install 校验与清理、gate |
| 业务 wrapper 与日志 | `plugins/craft_lottery/feature.{h,cpp}` | `process_recipe_wrapper` 决策序列、限频 degraded、backup 管理 |
| 设置兼容源 | `plugins/craft_lottery/settings_contribution.{h,cpp}` | 兼容入口，不注册设置项（feature metadata 唯一来自 descriptor） |
| port 契约 | `contracts/game_ports/craft_lottery_port.h`、`ports.h:41` | `QolCraftLotteryWritePort` v1.0 函数表与 id |
| port 实现 | `game/adapter_ports/craft_lottery_port.{h,cpp}` | 表读取、位图判定、原版调用、弹窗组合 |
| 符号 | `game/symbols/game_symbols.h`、`game/access/symbols.cpp:105-106` | VMA 常量与 hook 符号登记 |
| 装配 | `host/assembly.cpp:230`、`host/plugin_registry.cpp:265-268` | descriptor 注册与 port 接线 |
| 测试替身 | `tests/test_game_port_stubs.cpp` | host-only 测试的 port stub |

## 验收与已知限制

- VM-CL-01（真机行为验收）当前 **NOT_ACCEPTED**；验收卡与步骤见 `docs/refactor/audits/plugin-audit/craft-lottery-20261022/design-card.md` §6，登记见 `docs/refactor/verification.md` §5.23。
- 配方书位图偏移公式为静态逆向结论，真机抽取与游戏内配方列表对照后才算 PROVEN。
- 当前符号 VMA 为 overhaul arm64；monster/原版变体按 `game_symbols.h` 变体机制适配前，目标预检失败插件进入 `unsupported`（不崩溃，其他功能不受影响）。
