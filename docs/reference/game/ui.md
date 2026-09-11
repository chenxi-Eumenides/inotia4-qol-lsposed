# 游戏 UI 系统（Inotia 4 盗版大修 20260704）

> 日期：2026-08-17 ｜ 状态：探索沉淀（结论经反汇编实证，自定义方式待实验验证）
> 来源：`archive/tmp-exploration/disasm_text.txt`（36 万行反汇编）+ `apk/decompiled/libgame-symbols.txt`（8297 符号）+ `module/app/src/main/cpp/`（既有 UI 域实现）+ `../api-reference.md`（/api/ui 章节）
> 用途：回答「游戏 UI 面板如何显示、元素如何定义、能否自定义 UI」

## 1. UI 渲染链路（一句话结论）

**游戏 UI 100% 由 libgame.so（C/C++ 自研引擎）在 OpenGL 上绘制**。Android 层仅提供渲染容器与文本输入桥：

```
Android 层（Java）                Native 引擎（libgame.so）
─────────────────────           ─────────────────────────────
MainActivity（壳，com.com2us.inotia4 仅剩壳类）
  └─ CCustomGLSurfaceView           ← OpenGL 渲染容器（wrapper.game 包）
       └─ CRenderer.onDrawFrame（空）→ 纯 native 绘制
            └─ GAMESTATE 状态机（main_menu / world / story 剧情）
                 └─ Scene 场景系统 ──► 27 种 POPUP_SC_* 面板（弹窗场景）
                      ├─ g_sPopupStateList  状态表（27 条 × 64B）
                      ├─ g_arrPopupStack    弹窗栈（32B）
                      ├─ UI_SetPopupProcessInfo(1, id) 打开 / (3, 0) 关闭
                      └─ 生命周期：Scene_Init/Process/Draw/Event/KeyPress/Resume/Terminate
```

**证据**：
- `com.com2us.wrapper.game.CCustomGLSurfaceView` + `CRenderer.onDrawFrame`（空实现）→ 渲染全在 native
- 游戏主包 `com.com2us.inotia4` 只剩 MainActivity/SplashActivity 壳类；`com.com2us.wrapper.ui` 包（CUserInput/CTextInput/CCustomEditText 等）只是 native→Java 文本输入桥（EditText 覆盖层，native 弹输入框→Java 输入→nativeCallback 回传）
- libgame.so 有 674 个 `UI*` 前缀符号 + 189 个 `Scene_*` 场景符号 + 47 个 `UI_Draw*` 绘制函数

## 2. Scene 场景系统（面板的容器）

### 2.1 面板 = PopupScene

每种面板是一个 `POPUP_SC_*` 场景，enter 函数 = `Scene_Init_POPUP_SC_*`。27 种面板（game_symbols.h `F_PANEL_*_ENTER`，symbol_registry.h 全量登记）：

| 面板 | enter 符号 | 可开（API 白名单） |
|---|---|---|
| character_info | `Scene_Init_POPUP_SC_CHARACTER_INFO` | ✅（独立面板） |
| choice | `Scene_Init_POPUP_SC_CHOICE` | ❌（事件驱动） |
| inventory（EQUIP） | `Scene_Init_POPUP_SC_EQUIP` | ✅ |
| input_count | `Scene_Init_POPUP_SC_INPUT_ITEMCOUNT` | ❌（需物品上下文） |
| mercenary | `Scene_Init_POPUP_SC_MERCENARY_MANAGER` | ✅ |
| craft（MIX） | `Scene_Init_POPUP_SC_MIX` | ❌（需 NPC 交互对象） |
| npc | `Scene_Init_POPUP_SC_NPC` | ❌ |
| npc_quest | `Scene_Init_POPUP_SC_NPC_QUEST` | ❌ |
| npc_rest / npc_revive | `Scene_Init_POPUP_SC_NPC_REST/REVIVE` | ❌ |
| options | `Scene_Init_POPUP_SC_OPTION_MMENU` | ❌（主菜单专属） |
| quests | `Scene_Init_POPUP_SC_QUESTMENU` | ✅ |
| save_slot | `Scene_Init_POPUP_SC_SAVESLOT` | ❌ |
| character_select | `Scene_Init_POPUP_SC_SELECT_CHARACTER` | ❌ |
| shortcut | `Scene_Init_POPUP_SC_SHORTCUT_MENU` | ❌ |
| skills | `Scene_Init_POPUP_SC_SKILL` | ✅ |
| shop | `Scene_Init_POPUP_SC_STORE` | ❌（需 NPC） |
| settings | `Scene_Init_POPUP_SC_SYSTEMMENU` | ✅ |
| wipeout | `Scene_Init_POPUP_SC_WIPEOUT` | ❌（死亡自动开） |
| world_map | `Scene_Init_POPUP_SC_WORLDMAP` | ❌（事件驱动） |
| in_app（×6） | `Scene_Init_POPUP_SC_INAPP_ARMOR/GEMSHOP/GOODS/HOT/PACKAGE/WEAPON` | ❌ |
| daily_reward | `Scene_Init_POPUP_SC_DAILY_REWARD` | ❌ |

### 2.2 弹窗栈与状态表

- `G_POPUP_STACK_VMA = 0x728fd8`：`g_arrPopupStack`（32B）。栈数据区：`+8` = 计数（≤27），`+0x18` = 栈数组指针，栈元素 `0x40B/条`，栈顶元素 `+0x10` = enter 函数指针
- `G_POPUP_STATE_LIST_GOT_VMA = 0x2f3000+0x4f0`：`g_sPopupStateList`（27 条 × 64B：`id@+0, enter@+0x10, process@+0x18, f3@+0x28, f4@+0x30, event@+0x38`；`POPUPSTATE_Push` 以 `id×0x40` 索引）
- 打开：`fn_ui_set_popup_process_info(1, state_id)`（`UI_SetPopupProcessInfo` @0xaecc8）；关闭：`(3, 0)`；另有 `(4, 0)`（recover 语义，见 game_patch.cpp）
- 模块已实现：`data_popup_top_vma()`（栈顶 enter VMA → 面板识别）、`data_ui_screen()`（v0.5.42 统一 screen 枚举；UIPopupMsg 独立于 popup 栈，主菜单弹窗也识别为 `dialog_popup`：loading/main_menu/world/tutorial_pause/dialog_*/panel_*/main_menu_*）
- Java 层例外：Android 同意页 `AgreementUIActivity` 不经 libgame.so，native 枚举不感知；由 Kotlin 层（`UiActivityTracker` + `InfoApiServiceImpl.gamestateJson()`）覆盖为独立 `screen=agreement`，与 native `dialog_popup` 是两个域（in_world 守卫分界：native 对话仅 world 内，同意页仅主菜单前）

## 3. ControlObject 控件系统（元素的定义）

### 3.1 通用控件结构（0xf8B，game_symbols.h CO_* 偏移）

```
+0x08 u32  Type          （0=组容器/3=按钮；ControlButton_Create 置 3）
+0x0c u32  Active        （0x20 = 激活，ControlObject_EventProc 校验 ==0x20）
+0x18 i64  Rect.x        +0x20 i64 Rect.y
+0x28 i64  Rect.w        +0x30 i64 Rect.h
+0x40 u64  UserType      （0 通用 / 1 按钮 / 2 物品）
+0x50 ptr  Data          （类型私有数据，按钮=CB 0x78B）
+0x78 u32  Count         （子控件数，父控件遍历子节点用）
+0x88 u32  EventCallType （0x100 按下 / 0x200 点击触发）
+0x90 ptr  Proc          （统一事件分发 = TouchHandle_ControlEventProc）
+0x98 ptr  ControlProc   （类型事件处理器 = ControlButton_ControlEventProc）
+0xa0 ptr  Parent
+0xa8      ChildList     （0x10 内嵌 LINKEDLIST）
```

### 3.2 按钮私有数据（0x78B，game_symbols.h CB_* 偏移）

```
+0x00      Text[32]      按钮文本区（ControlButton_SetText 写）
+0x20 ptr  ExecuteProc   点击回调函数指针（可覆盖 = PtrHook 目标）
+0x28 u32  DrawType
+0x30 i64  DrawID        贴图 id（-1 默认）
+0x38 i64  DrawSubID
+0x60 ptr  DrawProc      绘制回调函数指针（可覆盖 = 自定义外观）
+0x68 u8   State         （0 正常 / 1 选中高亮）
+0x69 u8   Enabled       使能标志（ControlButton_Create 置 1）
```

### 3.3 控件创建（反汇编实证）

`ControlButton_Create`（0xaa710）：`ControlObject_AddControlObject(parent, type=3)` → `ControlObject_SetControlProc(ControlButton_ControlEventProc)` → `SetUserType(1)` → `MEM_Malloc(0x78)` → `ControlObject_SetData` → 初始化 ExecuteProc(来自 x1 参数)/DrawType=0/DrawID=SubID=-1/State=0/Enabled=1。

`UI_CreateGroupBaseControl`（0xaea78）：`ControlObject_AddControlObject(parent=NULL, type=0)` → `SetRect` → 禁用全部触摸事件（UnuseControlEventSelect/Drop/Move/On/Focus/MovePointer）= 纯容器。

`UIMix_CreateMainControl`（0xbf628，面板 Init 代表）：
1. `UI_CreateGroupBaseControl` 建面板背景（坐标硬编码 0x1ce×0x1a7，经 `CalcResolutionWidth/Height` 分辨率适配）
2. 循环 `ControlButton_Create(parent, text)` 建 5 个配方按钮：`SetControlEventCallType(0x200)` → `SetRect`（行布局，h=字体高度+0x29）→ `SetDrawType(5)` → `SetDrawProc(GOT 槽函数)`
3. 调 `UIGameMenu_CreateMainControl/CreateSubControl`（返回控件存实例槽 +0x30/+0x88）、`UIDesc_CreateControl`
4. 各按钮句柄存 `UIMix 实例 + 固定偏移`（如宝石按钮 = 实例+0xa0 = `UIMIX_SLOT_GEM_BTN`）
5. 文本来自 `TEXTDATABASE`（GOT 槽读字符串指针）；绘制回调、排序回调从 GOT 槽取函数指针

**结论：元素定义 = 硬编码代码创建控件树**（无 XML/布局文件）。控件属性（坐标/文本/贴图/回调）通过 `ControlObject_Set*` / `ControlButton_Set*` 系列设置。

### 3.4 控件系统关键符号（libgame.so）

- 创建/操作：`ControlObject_Create/AddControlObject/AddControlObjectBySort/Find/Search/GetRoot/GetParent/SetProc/SetControlProc/SetRect/SetRelativeRect/SetData/GetData/SetType/GetType/SetUserType/GetUserType/SetActive/GetActive/SetShow/GetShow/SetValue/GetValue/SetControlEventCallType/GetControlEventType/RecalcDepth/EnableCursor/ResetCursor`
- 按钮：`ControlButton_Create/SetText/SetDrawType/SetDrawProc/SetExecuteProc(推断)/GetExecuteProc/ControlEventProc`
- 其他控件：`ControlItem_*`（物品）、`ControlScroll_*`（滚动条）、`ControlSkill_*`（技能图标）
- 触摸分发：`TouchHandle_ControlEventProc`（递归控件树）、`TouchHandle_Event/MoveOut/UseControlEventMove/IsControlEventMovePointer/UnuseControlEvent*`
- 排序：`ControlObject_fpDefaultCompare`（控件排序比较函数指针）

## 4. 对话框系统

模块 `data_dialog_content_json`（game_dialog.cpp）已统一检测，优先级从高到低：

| 类型 | 判定 | 机制 |
|---|---|---|
| `agreement`（Java 层，优先于全部 native 检测） | 前台 Activity = `AgreementUIActivity`（`UiActivityTracker` 反射） | 同意页：options `[ok 同意]`；select ok 定位并点击「同意」元素关闭（`AgreementPopup`，WebView 注入 JS）；启动经 `AgreementGate` hook 拦截，仅 main_menu 且非进档宽限期放行（离线时不弹出） |
| `dialog_popup` | `UIPopupMsg_bOn`（G_POPUP_ON @0x3070e8） | 弹窗：`UIPopupMsg_pText`（文本指针）+ `fpOK`/`fpCancel`（回调指针，非空=有按钮）；`UIPopupMsg_i32Type`/`i32DisplayType` |
| `dialog_story` | `data_story_active()` | 剧情 AVG（EVTSYSTEM 驱动），`Event_ButtonOKExe`/`Event_ButtonSkipExe` 推进/跳过 |
| `dialog_npc` | `UICHOICE_nItemCount`/`NPCTASKLIST_nCount` | NPC 对话：`UICHOICE_pItemText`（6×8B 选项文本指针）+ `NPCTASKLIST` 槽数组（32×16B：+0 type/+2 id/+8 文本指针）；`UINpc_InitNPC` 建 NPCBOX+任务列表 |
| `dialog_wipeout` | 栈顶 = `F_PANEL_WIPEOUT_ENTER` | 死亡面板（Wipeout_ButtonRevive/SpecialRevive/GameOverExe） |
| `dialog_quest` | 栈顶 = `F_PANEL_NPC_QUEST_ENTER` | 任务完成面板（UINpcQuest_ButtonOKExe） |
| `dialog_choice` | 栈顶 = `F_PANEL_CHOICE_ENTER` | 事件驱动选择框 |
| `dialog_input_count` | 栈顶 = `F_PANEL_INPUT_COUNT_ENTER` | 数量输入 |
| 面板态 | `data_top_panel_name()` | 其他面板，仅 close（save_slot 另暴露 save） |

**UIPopupMsg 完整 API**（自定义对话框的关键，全部具名符号）：`UIPopupMsg_CreateYesNo/CreateNone/CreateFromTextData/CreateTextScrollControl/SetLayout/GetBaseControl/GetMainControl/GetOKButtonControl/GetTextHeight/Process/Event/Free/ButtonOKExe/ButtonCancelExe/DrawButton/DrawText/DrawDisplayTypeRectBackground/TextScrollDraw` + 全局 `UIPopupMsg_bOn/pText/fpOK/fpCancel/i32Type/i32DisplayType/i32YesNoType/i32Param/i32EventWaitFrame`。

## 5. 绘制原语与资源

| 类别 | 符号 | 用途 |
|---|---|---|
| 图形 | `GRPX_DrawStringInRectWithFont/CreateStringImage/FillRect/SetFontColor/GetTextWidthInRectWithFont` | 字符串/矩形绘制原语 |
| 纹理 | `SGL_DrawTexturePartFlip` | 纹理绘制 |
| 精灵 | `SPR_DrawFlipRotate`（8.5KB 大函数） | 角色/物品精灵 |
| 字体 | `IMGFONT_Create`、`FONT_GetStringWidth`、`GRPX_GetFontHeight` | 图片字体 |
| GL 包装 | `pact*`（pactOrthox/Frustumx/ActiveTexture/MatrixMode/GetScreenCoordinate…） | OpenGL 状态 |
| UI 组合 | `UI_DrawHDotLine/DrawLeftGauge/DrawItemFocus/DrawStringHAlign/DrawNumber/FillCrossRect` | 组合绘制 |
| 其他 | `MW_Graphic_DrawBorderString`、`MAPITEMDROP_Draw`、`EFFECTSYSTEM_DrawGround` | 边框/物品/特效 |

- 静态文本已导出：`apk/static-data/json/text/zh-Hans.json` 等（35811 条多语言字符串，顺序索引 = TEXTDATABASE id）
- 贴图资源：`IMAGEFILEBASE`/`SYMBOLBASE`/`ANIMATION*`（静态表，DrawID 索引）

### 5.1 主菜单环境设置资源（v0.6.8 反汇编 + 真机验证）

- `Scene_Init_POPUP_SC_OPTION_MMENU` 会 `IMGSYS_UnitLoad(0x59)`；该单元提供声音图标、语言箭头等 Options 专用静态分片。左上返回正常态为 `loc 0x3`，按下态为 `loc 0x2`，按下态叠加在正常态上并向左偏移 3 逻辑单位。
- 原版“开/关”胶囊按钮使用 `GetGroupTitleImgType()` 返回的当前语言主题图组，并以 `IMGSYS_GetLoc(group, 0x0f/0x10)` 和 `GRPX_DrawPart(..., type=2, flip=1)` 居中绘制；真机确认 `0x0f=开`、`0x10=关`。非激活胶囊额外居中绘制同图组 `loc 0x9` 的暗态叠加精灵（`UIOption_ButtonListDraw`）。
- 原版 Options 字体可通过 `GRPX_SetFontColorFromRGB(r,g,b)` + `GRPX_DrawStringWithFont(text,x,y,align,font)` 使用；`UIOption_UIButtonListDraw` 使用 RGB `(0xe2,0x9e,0xcb)`，font=1。
- 自定义 PopupState 可加载 `IMGSYS_UnitLoad(0x59)` 后复用这些分片；必须先检查 `IMGSYS_GetGroup/GetLoc` 非空。此前 `GRPX_DrawPart` 崩溃的主因是未加载图像单元导致空分片，而非 Process 回调本身不可绘制。
- `GAMELOADER_DrawBackGround` 是主菜单场景背景调用，不是可独立复用的单张贴图；自定义面板继续以 LCD 保存/恢复后的暗色遮罩作为安全背景路径。

## 6. 自定义 UI 的方式（5 种，实验目标）

### ✅ 已实现先例（v0.5.18，真机验证）

**mmap + PtrHook 控件注入**（`game_patch.cpp data_craft_btn_inject`）：
1. `mmap` RWX 区域手工构造 `ControlObject`(0xf8B) + `CB`(0x78B)，复刻原宝石按钮 rect/贴图
2. 新按钮写回固定控件槽 `[UIMix+UIMIX_SLOT_GEM_BTN]`（`UIMix_Draw` 硬编码枚举该槽 → 新按钮被绘制）
3. `PtrHook` 覆盖原按钮 `ExecuteProc`（点击走控件树递归遍历）
4. 懒注入线程：面板未创建（`UIMix_CreateMainControl` 未跑）时轮询等待

### 5 条可行路径（复杂度递增）

| # | 方式 | 机制 | 风险 |
|---|---|---|---|
| ① | 改按钮行为 | `PtrHook` 覆盖任意控件 ExecuteProc/ControlProc/DrawProc（数据段指针，无 trampoline） | 低 |
| ② | 往现有面板加控件 | 面板打开时调 `ControlObject_Create`/`ControlButton_Create`/`AddControlObjectBySort` 注入新按钮 | 中 |
| ③ | 自定义对话框 | `UIPopupMsg_CreateYesNo/CreateNone/CreateFromTextData` 或改写 `G_POPUP_TEXT`+置 `bOn`+设 `fpOK/fpCancel` | 中 |
| ④ | 改面板文本/外观 | `ControlButton_SetText` / 改写 TEXTDATABASE / 替换 DrawProc | 中 |
| ⑤ | 全新自定义面板 | mmap 构造控件树 + 注入 `g_sPopupStateList` 新状态 + `UI_SetPopupProcessInfo` push | 高 |

### ✅ 实验验证结果（v0.6.7，真机 2026-08-17 全部成功）

| # | 结果 | 验证证据 |
|---|---|---|
| ① | ✅ 成功 | settings 按钮2 ExecuteProc 被 PtrHook 覆盖（orig 保存），自定义回调执行（logcat） |
| ② | ✅ 成功 | `ControlButton_Create` 创建按钮 + 写回绘制枚举槽 `[0x308120]`，按钮被绘制（截图可见图标覆盖） |
| ③ | ✅ 成功 | `UIPopupMsg_Create` 中文文本弹窗，`screen=dialog_popup`，dialog 端点正确读出 UTF-8 文本 |
| ④ | ✅ 成功 | `ControlButton_SetText("EXP4-MODIFIED")` 写入内存验证（btn2_text_hex）+ restore 还原 |
| ⑤ | ✅ 成功 | 注入 state list 条目 26 的 enter → 游戏主循环 `POPUPSTATE_Push` 17ms 内调用自定义 enter → 弹窗显示 |

### 实验关键发现（逆向 + 实测）

1. **UIPopupMsg_Create 文本 = UTF-8 + 引用语义**：`w1` bit0=0（偶数，如 2）时 `UTIL_CopyText` 只存指针不拷贝 → **必须用持久缓冲**（static），否则悬垂指针乱码
2. **popup_free 不清 bOn**：`UIPopupMsg_bOn` 由 `ButtonOKExe`/`ButtonCancelExe` 清除；直接调 popup_free 后需手动清 `G_POPUP_ON_VMA`
3. **ControlObject_SetRect 有 AArch64 x8 参数**（输出 rect，`GetRelativeRect` 写入）→ C++ 调用约定无法传 x8，直接写 `[ctrl+0x18..0x30]` 内存
4. **面板绘制 = 硬编码槽枚举**（非控件树递归）：`Scene_Draw_POPUP_SC_SYSTEMMENU` 枚举按钮1@0x308108、菜单组 6 子控件、按钮2/3/4@0x308120/128/130 → **新控件必须写回枚举槽才可见**
5. **state list 可注入**：`g_sPopupStateList`（27 条 × 64B @0x2f9f58）条目改写 enter/process/f3/f4/event 后，`POPUPSTATE_Push`（游戏主循环调用）会 `blr` 自定义 enter

### 约束与已知崩溃点（模块真机实测）

- 需上下文面板直接打开崩溃（SIGSEGV tombstone）：`options`（GAMELOADER 场景专属）、`craft/shop`（需 NPC 交互对象）、`input_count`（需物品上下文）→ API open_panel 白名单仅 6 个独立面板
- 布局坐标必须过 `CalcResolutionWidth/Height` 分辨率适配
- `ControlObject_EventProc` 校验 Active==0x20；父控件用 `Count`+子链表遍历
- 指令 patch（IAP 屏蔽/沉浸/堆叠上限 42 点）证明改引擎指令可行，可扩展改 UI 行为

## 7. 模块既有 UI 能力（已产品化）

| 能力 | 端点/函数 | 实现 |
|---|---|---|
| 界面状态 | `GET /api/ui`、`/api/ui/screen`、`/api/ui/panel` | game_ui.cpp `data_ui_screen`/`data_top_panel_name` |
| 对话框 | `GET /api/ui/dialog` | game_dialog.cpp `data_dialog_content_json` |
| 操作 | `POST /api/ui/dialog/select`、`start_interact`、`go_main_menu`、`open_panel`、`close_panel` | game_ui.cpp/game_dialog.cpp `data_op_*` |
| 调试 | `GET /api/debug/ui` | game_ui.cpp `data_debug_ui_json` |

## 8. 关联

- 反汇编全文：`archive/tmp-exploration/disasm_text.txt`
- 符号表：`apk/decompiled/libgame-symbols.txt`（8297 符号）
- 既有 UI 域：`module/app/src/main/cpp/game_ui.cpp` / `game_dialog.cpp` / `game_patch.cpp`（按钮注入）/ `game_symbols.h`（CO_/CB_/UIMIX_ 偏移）/ `symbol_registry.h`
- API 规格：`../api-reference.md` 第六章 /api/ui
- 实验记录：`../../history/experiments/ui-experiments.md`（实验完成后更新）
