# 世界传送 5 选项（world-teleport）

状态：实现。已加入第 5 项关闭、UICHOICE 当前地图标题 Hook 和运行时地图数量门控；monster v25 的传送点与小地图区域路径均已完成本轮真机主链路验收，原版 v1.3.2 尚未验证，Host 测试 9/9。

## 1. 目标

游戏踩到地图边界传送点（小地图传送触发点）时，不再按原版直接切图 / 改版弹出二选一确认框，而是弹出**商店 NPC 同款原生 UICHOICE 选择框**，5 项可直接点选：

| 选项 | 目标 | 费用 |
|---|---|---|
| `<地图名>(id+1)` | 当前地图 id+1 | 300G |
| `<地图名>(id+10)` | 当前地图 id+10 | 3000G |
| `<地图名>(id-1)` | 当前地图 id-1 | 300G |
| `<地图名>(id-10)` | 当前地图 id-10 | 3000G |
| `关闭` | 关闭选择面板 | — |

- **两级交互**：点选项 → 弹出改版同款 YesNo 确认框（「是否传送至<目标地图名>？」）→ 确认=查钱+扣钱+传送；取消=返回选项面板可改选
- 费用沿用改版语义（按距离档：±10=3000、±1=300）；余额不足弹提示、不传送（改版此处 textId 0xa 误显示「狂战士」的 bug 顺带修复，模块自建提示文案）
- 边界回绕保留；运行时上限为 `min(MAPINFOBASE_nRecordCount - 1, 414)`，避免不同版本地图记录数导致目标越界（静态上限反汇编实证 `cmp w0,#0x19e`）
- **版本状态**：monster v25 已真机验证；大修 20260830、monster v23、原版 v1.3.2 当前未取得本轮同等证据
- 地图名经 MAPINFOBASE→MEMORYTEXT 查询，查不到回退「未知地图」（与 monster 版现有兜底一致）
- 选择面板标题由 `UIChoice_Init` 原函数完成后写入「当前地图：<地图名>(id:N)」；仅模块打开 choice 前置位，普通商店/NPC choice 不改标题

## 2. 依据（侦察与反汇编实证）

改版传送链路（大修/monster 注入块 VMA 相同；Ghidra 文档地址=VMA+0x100000）：

1. 触发：`UIPlay_CallMapName`(0xc6670，小地图传送点触发时显示地图名横幅) 尾部被 patch（`b 0xc50f4`）
2. 0xc50f4：helper 0x14e0f0 读主控玩家 `+6` 朝向 → dir 0/2=3000、dir 1/3=300 → `INVEN_GetMoney` 比对；不足 → `UIPopupMsg_CreateOKFromTextData(0xa,…)`（「狂战士」bug）
3. 0xc5058：`UIPopupMsg_CreateYesNoFromTextData(0x3e8, 0, 2, ok=0xd17dc, cancel=0, param=费用)`
4. 文本 trampoline：`CreateFromTextData`(0xca6f4) @0xca718 patch → 特判 textId==0x3e8 拼传送文本（大修「是否传送?Map_Id:N」/ monster「是否传送至X？」兜底「未知地图」）
5. OK 回调：`UIStore_BuyOKInputItemCount`(0xd17dc) 头部 patch → param==300/3000 → `INVEN_MinusMoney` → 读当前 id(0x14e0e0) → 朝向算目标（dir 0:-10 / 1:+1 / 2:+10 / 3:-1）→ 回绕 → helper 0x1ef40c：`MAPCHANGE_Set(mapId,0,0,dir)` + `GAMESTATE_SetState(3)`

现版本文本：大修「是否传送?Map_Id:N」；monster「是否传送至X？」；原版无对话框仅当前地图名横幅。

可复用基建：UICHOICE 全局（`G_UICHOICE_ITEMTEXT_VMA=0x711c60` 6×8B、`G_UICHOICE_COUNT_VMA=0x302d70`、`G_UICHOICE_FOCUS_VMA=0x302d80`、`G_UICHOICE_MAIN_TEXT_VMA=0x302d78`、`F_PANEL_CHOICE_ENTER=0x14a664`）；`UIChoice_Init=0x0b1cd4` 通过 Native Hook API 包装；UIPopupMsg 全家（`F_UIPOPUPMSG_CREATE_YESNO*` 等，game_symbols.h:693-699）；state list 注入 enter 已真机验证（`docs/reference/game/ui.md` 实验⑤，POPUPSTATE_Push 17ms 内回调）；PtrHook ExecuteProc 低风险先例（ui.md 方式①）。

## 3. 技术方案

### 3.1 入口拦截（三版本统一，内容自适应版本判定）

hook 点 = `UIPlay_CallMapName` 尾部 0xc66c8 处 4 字节指令，**指令内容即版本指纹**，无需特征库：

- 原版：`mov x0,#1` → patch 为 `b 模块trampoline`，trampoline 末尾执行 `mov x0,#1` 并跳回 epilogue 0xc66cc（保留横幅与返回语义）
- 大修/monster：`b 0xc50f4` → patch 为 `b 模块trampoline`，改版注入块（查钱/YesNo/文本 trampoline/寄生 OK 回调）**整条变死代码**，费用查扣由模块自理（解决「上游按朝向查费挡住低余额玩家选 +1（300G）」的语义冲突）
- 两种内容都不匹配 → 不启用，记日志（未支持版本安全 no-op，先例 apply_monster_item_count_compat）
- 打开时机：UICHOICE 由 CallMapName 执行流直接 push；选择项确认框在下一逻辑帧创建，避免与 choice 当前帧的绘制/事件上下文冲突

### 3.2 面板实现（首选 UICHOICE，阶段 0 静态结论）

- 打开：写 `G_UICHOICE_ITEMTEXT[0..3]`（**持久静态缓冲**「<地图名>(id±N)」，引用语义）及 `[4]="关闭"`，设置 `G_UICHOICE_COUNT=5`、`G_UICHOICE_FOCUS=0` → push choice 面板（choice 的 state id 运行时扫描 g_sPopupStateList 中 enter==F_PANEL_CHOICE_ENTER 的条目获得，不硬编码）
- 标题时序：push 前生成并保存「当前地图：<地图名>(id:N)」，置位 pending；`UIChoice_Init` wrapper 先调用原函数，再把 `G_UICHOICE_MAIN_TEXT` 指向持久标题缓冲并清除 pending。push 失败清除 pending；pending 未置位时完全透传，避免污染商店/NPC choice。
- **选中接管：单点 PtrHook `UIChoice_ButtonListExe`(0xb1a98)**——它是全部 5 个按钮共用 ExecuteProc（UIChoice_CreateControl 0xb2110 循环内统一设置），hook 一次即接管全部按钮。索引 0..3 读取 `ControlObject_GetCursorIndex`(0x9ea48)，先关闭底层 choice，再由下一逻辑帧创建 YesNo；索引 4 走原 `UIChoice_ButtonListExe` 的完整 choice 清理链（`UI_SetPopupProcessInfo(3,0)` + `EVTSYSTEM_DoCheckAllEvent(8)`）
- 按钮由面板 enter 内部创建（UIChoice_Init 0xb1cd4 + UIChoice_CreateControl 0xb2110），无需事件系统参与；PopupState process/event 回调可沿用原版（静态无 EVTSYSTEM 依赖）
- 确认回调：YesNo 取消由官方流程关闭后，下一逻辑帧重新打开 choice；确认关闭 YesNo 后，再由延迟任务执行 `INVEN_GetMoney` 比对 → `INVEN_MinusMoney` → 按运行时记录数计算目标 id 回绕 → `MAPCHANGE_Set(id,0,0,dir)`+`GAMESTATE_SetState(3)`；余额不足或目标无效时仅提示，不切图
- 地图名：`MAPINFOBASE`（记录 6B，`+0`=名称 text_id，pData GOT=0x2f4000+0xe58）→ `MEMORYTEXT_GetText`(0x118674，自带越界保护返回 NULL) → 空/失败回退「未知地图」；monster v25 本轮真机读取 `nRecordCount=421`，因此实际 `max_map_id=414`；原版 v1.3.2 的运行时数量尚未取得

### 3.3 确认框不可见根因与修复

- 根因：在 UICHOICE 的按钮 ExecuteProc 内直接创建 YesNo 时，`UIPopupMsg` 的逻辑状态已经是 active，但底层 choice 仍占据当前绘制/事件上下文；真机表现为 API 返回 `dialog_popup`，画面仍显示 choice，用户无法看到确认框。
- 修复时序：选择 0..3 时先关闭底层 UICHOICE、恢复 HUD gate，再由下一逻辑帧创建 YesNo；取消后下一逻辑帧重新 Push choice；确认仍通过延迟关闭/传送任务执行，避免切图过渡帧访问已清理控件。`frame_task_add(..., interval=1, count=1)` 的首个派发为注册后的下一次派发，任务返回 `false` 后注销。
- monster v25 真机证据：修复前 `screen=dialog_popup` 但截图仍为 choice；修复后截图显示「是否传送至凯恩的房间？」确认框，取消返回 choice，确认后 `20→21` 且扣 `300G`。证据目录：`.tmp/world-teleport-monster-v25-path-compare/`。

Plan B（真机验证失败时）：自定义 PopupState（实验⑤路径）+ `ControlButton_Create` 原生按钮 ×5，回调全自控。

### 3.3 符号落地清单（阶段 0 已确认，全部有 .dynsym 导出）

| 符号 | VMA | 签名 |
|---|---:|---|
| `UIChoice_ButtonListExe` | 0x0b1a98 | `void(void* control)`（x0=控件） |
| `UIChoice_CreateControl` | 0x0b2110 | 面板 enter 调用，创建按钮 |
| `UIChoice_Process` | 0x0b2104 | `void(void)`，转 ControlScroll_Process |
| `UIChoice_Init` | 0x0b1cd4 | `void(void*)`，原函数完成后写入模块标题 |
| `Scene_Event_POPUP_SC_CHOICE` | 0x14a79c | choice 事件回调，转发 TouchHandle_Event |
| `MAPCHANGE_Set` | 0x09c740 | `void(int map_id, int x, int y, int dir)`，w0-w3 |
| `GAMESTATE_SetState` | 0x151590 | `void(int state)`，w0 |
| `INVEN_GetMoney` | 0x10445c | `int64_t(void)` |
| `INVEN_MinusMoney` | 0x104780 | `int(int64_t amount)`，x0 |
| `MEMORYTEXT_GetText` | 0x118674 | `const char*(uint16_t text_id)`，越界返 NULL |
| `MAPINFOBASE_pData` GOT 槽 | 0x2f4000+0xe58 | `void**` 双层解引用 |
| `MAPINFOBASE_nRecordSize` | 0x3017b8 | `uint8_t`，值=6 |
| `MAPINFOBASE_nRecordCount` | 0x3017ba | `uint16_t`，值=416 |
| `UICHOICE_pMainText` | 0x302d78 | `char*`，choice 主标题指针 |
| `ControlObject_GetCursorIndex` | 0x09ea48 | 读选中索引 |
| 主控玩家指针 GOT | 0x3f6000+0xa50 | 双层解引用，`+6`=朝向 |

## 4. 实施阶段

1. **阶段 0 逆向攻关**（只读研究）：① `Scene_Init_POPUP_SC_CHOICE` 反汇编：按钮由谁创建、ExecuteProc 形态、选中分发链与事件系统依赖 ② 缺口函数实际 VMA（注入块 bl 目标直读+导出表验证）③ 原版 `UIPlay_CallMapName` 触发时机（推论：仅传送点触发，待真机确认）④ `UIChoice_Init` 主标题写入点确认
2. **阶段 1 基础设施**：符号登记（game_symbols.h + symbol_registry.h）+ `feature/world_teleport/` 骨架（登记 CMakeLists.txt）+ 目标 id 计算纯函数 host 测试
3. **阶段 2 大修/monster 接入**：真机回归（含打开时机两案验证）
4. **阶段 3 原版接入**：真机回归
5. **阶段 4 全矩阵验收**：VM-WT 矩阵全绿

## 5. 风险与回退

| 风险 | 缓解 |
|---|---|
| choice 面板非事件上下文崩溃（craft/shop/input_count 前科） | 阶段 0 逆向依赖点 + 真机验证；Plan B 兜底（已验证路径） |
| 面板期间游戏世界不暂停（非事件上下文继续跑怪） | 阶段 2 观察实际表现，必要时补暂停处理 |
| UIPopupMsg 叠加 choice 面板的渲染/输入优先级 | 已修复并验证：创建 YesNo 前关闭底层 choice；取消下一逻辑帧重开 choice；确认按关闭→切图顺序执行 |
| 改版注入块旁路依赖指令 patch | 有 42 点指令 patch 先例（game_patch_core.inc），风险可控 |

## 6. 验证矩阵 VM-WT

交付定义：触及行为面必须附真机日志证据，缺 = `NOT_ACCEPTED`。

| 用例 | 大修 | monster | 原版 |
|---|---|---|---|
| WT-1 踩传送点弹出 4 选项面板（选项=地图名(id±N)） | ☐ | ✅ | ☐ |
| WT-2 点选后弹出「是否传送至X？」确认框 | ☐ | ✅ | ☐ |
| WT-3 确认→扣对应费用（300/3000）→切图成功 | ☐ | ✅ | ☐ |
| WT-4 取消→返回选项面板可改选 | ☐ | ✅ | ☐ |
| WT-5 余额不足→提示且不扣钱不传送（文案正确，非「狂战士」） | ☐ | ☐ | ☐ |
| WT-6 边界回绕：id>414→0、id<0→414 | ☐ | ☐ | ☐ |
| WT-7 关闭面板不扣钱不传送 | ☐ | ☐ | ☐ |
| WT-8 改版原单目标链路不再触发（死代码验证） | ☐ | ☐ | N/A |
| WT-9 面板打开期间游戏表现（暂停/继续）记录 | ☐ | ☐ | ☐ |
| WT-10 传送面板标题为当前地图且普通商店/NPC choice 不被污染 | ☐ | ☐ | ☐ |
| WT-11 选择第 5 项「关闭」→ 面板关闭且不扣钱不传送 | ☐ | ☐ | ☐ |

Host 测试：目标 id 计算（+1/+10/-1/-10、回绕）纯函数对照用例（tests/test_host.cpp）。

monster v25 本轮证据：路径 B（点击小地图非标记区域）打开选择框、选择第一项、确认框可见、取消重开选择框、确认后 `20→21`；日志与截图位于 `.tmp/world-teleport-monster-v25-path-compare/`。

## 7. 决策记录

- UI 路线：原生 UICHOICE（用户定，商店 NPC 同款视觉）；Plan B 自绘兜底
- 费用：保留原语义，改绑距离档（±10=3000、±1=300）
- 范围：三版本都实现
- 选项文本：`<地图名>(id+N)`（用户定）；确认框文案=monster 式「是否传送至X？」
- 二级确认：用户追加——点选项后先确认再传送
