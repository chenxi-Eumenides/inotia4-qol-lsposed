# 模块自定义文本层（custom-text）

> 状态：已实现并真机验证（hook 安装 + 角色面板标签迁移无回归、四字标签渲染正常）｜位置：`module/app/src/main/cpp/feature/ui/module_text.{h,cpp}`

## 1. 目标

让模块能对**任意游戏 text id** 在**指定窗口**内显示**模块自有文本**（游戏文本表里没有的串，如「宝石升阶」），而不是为每个文案各写一套 hook + 硬编码字面量。

## 2. 为什么必须是「单一 owner + 窗口作用域」

两个约束决定了本设施的形态：

1. **`MEMORYTEXT_GetText` 只能有一个 hook owner**。它是全游戏唯一的文本读取热点（232 个调用点），所有走文本表的 UI 文案（按钮 / 标签 / 描述）都经它；而 **LSPosed NativeHook 对同一地址二次挂载会失败**（rc=-1）。真机教训：早期把 `UIDesc_MakeItem@0xb36a0` 串进合成器 hook 主链（该地址已被 attribute_range 占用），导致 5 个核心 hook 全部未安装。所以「多个功能各挂一次」这条路不通，必须由本设施独占该热点，其它功能把文案**登记进内置表**。
2. **替换必须带窗口作用域**。游戏里同一个 text id 会被多个界面复用 —— 例如 35291 既是宝石强化页的**页签名**「宝石强化」（由 `UIMix_ButtonMenuListDraw` 绘制），也是模块配方按钮的 label wordId。无门控地替换就会把页签一起改名。

## 3. 结构

| 部件 | 位置 | 说明 |
|---|---|---|
| 内置表 | `module_text.h` 的 `kEntries` | 唯一真源；条目 = `{语义键, text_id, Scope, 文本}`；`text == nullptr` 表示**显式保持原文**（占位用，维持连续 id 区间） |
| 查表 | `module_text::lookup(text_id, active_scope)` | 纯函数；命中且作用域一致且文本非空才返回字面量，否则 `nullptr`。零分配、无字符串比较，可用于每帧热点 |
| 作用域状态 | `module_text.cpp` 的 `thread_local t_active_scope` | 绘制与取文本同线程，故用 thread_local（既无跨线程可见性需求，又避免 HTTP/预取线程与绘制重叠时被误替换） |
| 作用域守卫 | `module_text::TextScopeGuard` | 窗口入口构造、出口析构；**保存/恢复上一层**而非置空，对「原函数内部再次进入同一 wrapper」天然可重入 |
| hook | `module_text.cpp` 的 `memorytext_get_text_wrapper` | 先无条件调原函数（保持原版行为与任何内部副作用），再查表替换 |

**能力边界：只做读取侧替换** —— 不改控件文本缓冲、不写游戏文本数据（`MEMORYTEXT` / `*BASE` 表全不动）、不新增端点。

## 4. 作用域

| Scope | 窗口 | 设置者 |
|---|---|---|
| `kNone` | 不在任何登记窗口内（默认） | — |
| `kCharacterPanel` | 角色属性面板绘制 | `feature/ui/game_ui_charinfo_zh.cpp`（hook `Scene_Draw_POPUP_SC_CHARACTER_INFO`） |
| `kUimixRecipeButton` | UIMix 配方按钮绘制 | `feature/custom_recipe/game_ui_custom_recipe.cpp`（hook `UIMix_ButtonRecipeDraw`） |
| `kUimixPanelTitle` | UIMix 面板标题绘制（选中配方后标题框里的「当前配方名」） | 同上（hook `UIMix_Draw`） |
| `kUimixPageTab` | UIMix 页签按钮绘制 | 同上（hook `UIMix_ButtonMenuListDraw`）；**该作用域表中无任何条目，用途是「显式不替换」** |

### 4.1 「一个 id、三个窗口」的典型案例（35291）

UIMix 面板里同一个 wordId（35291）被三处复用，要求各不相同：

| 窗口 | 绘制点 | 要求 |
|---|---|---|
| 配方按钮 | `UIMix_ButtonRecipeDraw` | 显示「宝石升阶」 |
| 面板标题 | `UIMix_Draw` 内的「当前配方名」（读配方记录 b0-1） | 显示「宝石升阶」 |
| 宝石强化页页签 | `UIMix_ButtonMenuListDraw`（取 `SYMBOLBASE[146+type]`） | **保持原文「宝石强化」** |

第三处在第二处内部被**嵌套调用**，因此靠 `TextScopeGuard` 的保存/恢复语义把作用域在页签绘制期间压成 `kUimixPageTab`：只有页签那一段不替换，另外两处照常。这条语义由 host test 的 lookup 矩阵用例（同一 id 在三个作用域下结果不同）钉死。

## 5. 内置文案

**角色面板（`kCharacterPanel`）**：游戏文本表漏翻了 11 个战斗属性缩写。35181 LV / 35182 EXP / 35183 HP / 35184 MP 按用户裁决保持原文；35188 M. DEF 面板未显示，显式登记 `nullptr` 占位以维持 35185..35196 连续区间（`.h` 内 `static_assert` 保证该段位于表首、连续升序、无空洞）。

| 键 | id | 缩写 | 文本 |
|---|---|---|---|
| `charinfo.dmg` | 35185 | DMG | 物攻 |
| `charinfo.magic_dmg` | 35186 | M.DMG | 法攻 |
| `charinfo.def` | 35187 | DEF | 防御力 |
| `charinfo.magic_def` | 35188 | M.DEF | （保持原文） |
| `charinfo.crit_rate` | 35189 | CRT | 暴击率 |
| `charinfo.hit_rate` | 35190 | H.RATE | 命中率 |
| `charinfo.crit_dmg` | 35191 | C.DMG | 爆伤 |
| `charinfo.phys_res` | 35192 | P.RES | 物抗 |
| `charinfo.magic_res` | 35193 | M.RES | 法抗 |
| `charinfo.evade` | 35194 | EVD | 闪避率 |
| `charinfo.weapon_block` | 35195 | W.D.R | 武器格挡 |
| `charinfo.shield_block` | 35196 | S.D.R | 盾牌格挡 |

**配方文案（`kUimixRecipeButton` + `kUimixPanelTitle`）**：

| 键 | id | 作用域 | 文本 |
|---|---|---|---|
| `recipe.jewel_tier_up` | 35291 | `kUimixRecipeButton` | 宝石升阶 |
| `recipe.jewel_tier_up.title` | 35291 | `kUimixPanelTitle` | 宝石升阶 |

页签（`kUimixPageTab`）不登记条目 —— 它与配方名同 id，必须保持游戏原文「宝石强化」。

## 6. 新增一条文案的步骤

1. 在 `kEntries` 追加 `{语义键, text_id, Scope, "文本"}`（id 与文案一一对应；若要与既有条目构成连续区间，需同步维护相应的 `static_assert`）。
2. 若该 id 需要新窗口，在 `Scope` 追加枚举 + 在对应功能的 draw 入口构造 `TextScopeGuard`。
3. 在 `test_module_text.cpp` 补断言（表完整性 / lookup 矩阵 / 与其它功能常量的同步）。

## 7. 验收

- **host tests**：`module_text_tests` —— 表完整性（键唯一；文案为纯中文 UTF-8、二至四字即 6/9/12 字节）、角色面板段连续升序、**lookup 作用域矩阵**（含「35291 只在配方按钮窗口内被替换」这条护栏）、语义键查询与跨功能 id 同步。全部 16 个 host test 目标通过。
- **真机（monster v23）**：hook 安装日志
  ```
  module text module_text.cpp:58 module text hook installed entries=14
  ui game_ui_charinfo_zh.cpp:49 charinfo zh panel scope hook installed (texts in module_text)
  custom_recipe game_ui_custom_recipe.cpp:953 custom recipe hooks installed core=5 desc=1 slotcount=1 label=1 title=1 tab=1
  ```
  迁移后角色面板截图复验：11 项标签全部正常（物攻 74 / 法攻 59 / 暴击率 11.3% / 命中率 107.5% / 爆伤 122.7% / 防御力 111 / 物抗 40.9% / 法抗 4.7% / 闪避率 21.6% / 武器格挡 4.5% / 盾牌格挡 0.0%），**四字标签渲染无溢出**。
  合成器面板：用户实机确认配方按钮已显示「宝石升阶」；面板标题（同 id、另一绘制点）原本仍显示「宝石强化」，已按 §4.1 补上 `kUimixPanelTitle` 窗口并复测。
- **未验收**：合成产物数值（宝石 ×1.1）需用户实机合成确认（合成器面板需要 NPC 交互上下文，API 不可开启）。

## 8. 失败与降级

- `MEMORYTEXT_GetText` hook 装不上 → `lookup` 永不生效，全部退化为游戏原文；不崩、不影响功能。
- 各「标记窗口」的 draw hook 单独安装、失败只告警，绝不拖垮其它 hook。
- 未登记 id / 未登记作用域 / `nullptr` 条目一律返回原文。

## 9. 相关文件

| 文件 | 作用 |
|---|---|
| `feature/ui/module_text.{h,cpp}` | 唯一 hook owner、作用域栈与守卫、内置文本表与查表 |
| `feature/ui/game_ui_charinfo_zh.{h,cpp}` | 角色面板绘制窗口 → `kCharacterPanel` 作用域（不再持有文案与 GetText hook） |
| `feature/custom_recipe/game_ui_custom_recipe.cpp` | 配方按钮绘制窗口 → `kUimixRecipeButton` 作用域 |
| `tests/test_module_text.cpp` | 表完整性 / 作用域矩阵 / 跨功能 id 同步 |
| `data/native/game_symbols.h` | `F_UIMIX_BUTTON_RECIPE_DRAW_VMA = 0xbef40`（7 个构建 VMA 一致）+ typedef |
