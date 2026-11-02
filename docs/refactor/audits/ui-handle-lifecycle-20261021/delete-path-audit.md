# 游戏控件销毁路径审计（C 方案可行性）

日期：2026-10-21。方式：只读线性反汇编 `.tmp/autosell-audit/libgame.dis`（libgame.so，monster v29 对应版本）+ `game/symbols/game_symbols.h`。未经真机验证的结论标 `UNPROVEN`。

目标：判断能否在游戏侧「控件删除」统一入口挂 call patch，作为插件句柄失效的全局信号（C 方案）。背景：插件挂在游戏控件树上的控件会随面板关闭/切页被销毁，句柄地址可能被复用（AS-19）。

## 1. 结论

**部分可行。** 只挂 `TouchHandle_DeleteControl @0xa3c04` 覆盖不全：该函数只删除传入对象，不会逐子控件递归；已证实还有 `ControlObject_DeleteChild` 与多处直接调用 `ControlObject_Delete` 的路径。推荐挂在 `ControlObject_Delete @0x9eb3c` 入口（所有已证实路径都会进入），但「装备页袋容器整棵子树由谁释放」仍 `UNPROVEN`，需要真机实验确认。

## 2. `TouchHandle_DeleteControl @0xa3c04`

- 取选中控件并 `ControlObject_Find`（`0xa3c18–0xa3c2c`）；命中时清光标（`0xa3c34–0xa3c3c`）。
- 只调用一次 `ControlObject_Delete`：`0xa3c44: bl 0x9eb3c`。**没有逐子控件循环。**

## 3. `ControlObject_Delete @0x9eb3c`

1. 调用 `DeleteProc`（`ctrl+0xf0`，`0x9eb58–0x9eb70`）。
2. 若为父控件且 `CO_COUNT` 非零，调用 `LINKEDLIST_clear`（`0x9ec10–0x9ec28`）。
3. 从父控件 ChildList 摘除（`0x9eb80–0x9eb98`）。
4. 清字段并 `MEM_Free`（`0x9ebac–0x9ebd4`）。

`LINKEDLIST_clear @0xa06cc` 逐个 `LINKEDLIST_remove @0xa0430`，其中调用链表里 `[list+0x20]`/`[list+0x18]` 的间接回调（`0xa418–0xa425`）。回调目标未解析，**不能据此断言会或不会递归释放子控件（UNPROVEN）**。

明确的逐子控件删除是 `ControlObject_DeleteChild @0x9ec4c`：`0x9ec7c–0x9ec9c` 循环取 ChildList head → `ControlObject_Delete`，头到尾。

## 4. 控件释放路径清单

| 路径 | 入口 | 经 `0xa3c04` | 证据 |
|---|---|---:|---|
| 对象分配/释放 | `ControlObject_Create @0x9e4ec` → `ControlObject_Delete @0x9eb3c` | 不一定 | `0x9e4fc–0x9e514` `MEM_Malloc(0xf8)`；`0x9ebcc–0x9ebd4` `MEM_Free` |
| TouchHandle 删除 | `TouchHandle_DeleteControl @0xa3c04` | 是 | `0xa3c44: bl 0x9eb3c` |
| 删除直接子控件 | `ControlObject_DeleteChild @0x9ec4c` | 否 | `0x9ec90: bl 0x9eb3c`；调用者：`UIDesc_ResetParentControl @0xb2af8`、`UIDesc_ResetMenuGroup @0xb5324`、`UIMercenary_ResetListMenuGroup @0xbd688`、`UIMix_CreateRecipeGroupControl @0xbfb24`、`UIQuestMenu_ClearUIInAppProcess @0xcbe48`、`UIQuestMenu_UpdateListControl @0xcc038`、`UIQuestMenu_ButtonListExe @0xcc2c4` |
| 直接 `ControlObject_Delete` | `LinkedList_Delete @0x9ec2c`、`ControlObject_DestroyInstance @0x9eccc`、`GameUIStack_Pop @0x9fbf4`、`GameUIStack_Destroy @0x9fc8c`、`InputLayer_Destroy @0xac51c`、`TouchGameObject_DeleteItem @0xae118`、`TouchGameObject_DeleteCharacter @0xae324` | 否 | 调用点：`0x9ec40`、`0x9ecf8`、`0x9fc40`、`0x9fcc0`、`0xac53c`、`0xae168`、`0xae36c` |
| 控件私有数据释放 | `ControlButton_Delete @0xaa6f0`、`ControlItem_Delete @0xaac78`、`ControlScroll_Delete @0xab024`、`ControlSkill_Delete @0xabbf4` | 经 DeleteProc 间接 | 各自 `MEM_Free @0xa8f18`，释放的是 `ControlObject+0x50` 私有数据，不是对象本体 |
| C++ `operator delete` | `_ZdlPv @0x209ae0` | 未知 | 大量调用，但无证据指向 ControlObject；`~ControlObject` 路径 **UNPROVEN** |

## 5. 背包装备页拆除链

- 创建：`Scene_Init_POPUP_SC_EQUIP @0x14a8b0` → `UIEquip_CreateInvenControl @0xb6688`；袋容器写入面板 `+0x50`（`0xb6760–0xb6764`），对应槽 `G_UIEQUIP_PANEL_BAG_CONTAINER_VMA = 0x304a30`。
- 关闭：`Scene_Terminate_POPUP_SC_EQUIP @0x14a97c` 从面板 `0x307000+0xf38` 取**顶层控件**，`0x14a98c` 调 `TouchHandle_DeleteControl @0xa3c04` → `ControlObject_Delete @0x9eb3c` → `MEM_Free`。
- 切页：`UIGameMenu_SetCurrentMenu @0xb995c` 只写当前菜单字节，随后 `ControlObject_SetCursor @0x9f1c0` → `ControlObject_MoveCursor @0x9f054`，无显式删除调用。
- **袋容器整棵子树的实际释放函数与顺序：UNPROVEN**；目前只证明顶层控件走 `0xa3c04`。

## 6. 挂钩点与覆盖矩阵

| 挂钩点 | 覆盖 | 风险/限制 |
|---|---|---|
| `TouchHandle_DeleteControl @0xa3c04` | 装备/商店/合成器及多数 PopupState 顶层删除；含 `UIPopupMsg_Free @0xca4e8`、`UIShortcutMenu_Free @0xcd790` | 不覆盖 `ControlObject_DeleteChild` 与直接 `ControlObject_Delete`；看不到子控件，不能据回调次数判断整树 |
| **推荐：`ControlObject_Delete @0x9eb3c` 入口** | 所有已证实删除路径都会进入（含批量/直接删除） | 入口处可在 `LINKEDLIST_clear` 前读 `CO_COUNT=+0x78` 并遍历子树失效化；遍历须避免调用会重入删除的游戏 API；线程归属与异常回调 **UNPROVEN** |
| `ControlObject_Delete @0x9ebd4`（`+0x98` 调用点） | 只标记当前对象 | 此时 ChildList 已处理、`CO_COUNT` 将在 `0x9ebcc` 清零，太晚 |
| `MEM_Free @0xa8f18` + 创建登记 | 兜底（若 ChildList 回调绕过 `0x9eb3c`） | `MEM_Free` 是全局分配器入口，命中频繁；需 O(1) 查表、避免锁竞争；`_ZdlPv` 是否另有控件释放 **UNPROVEN** |

## 7. 未证明项与最小真机实验

- `0x2f4a80` 链表回调的最终目标，以及是否递归释放子控件：`UNPROVEN`。
- UIEquip 袋容器在关闭/切页时实际被释放的对象集合与顺序：`UNPROVEN`。
- 删除是否总在主线程、间接回调是否可重入/异常退出：`UNPROVEN`。
- `TouchHandle_DeleteControl` 是否会被链表回调再次进入：`UNPROVEN`。

最小实验：真机同时记录 `0xa3c04`、`0x9eb3c`、`0x9ebd4`、`MEM_Free` 的对象指针、父指针、`CO_COUNT`、调用点与线程；分别关闭/切换装备页，确认袋容器及其中的插件哨兵控件进入哪个释放点、是否发生地址复用、旧代际句柄是否被拒绝。

## 8. 与已落地机制的关系

- 插件侧的锚定层（`ui_components` 1.20/1.21 + adapter 1.25/1.26）已能在**有绘制调用的帧**证明句柄存活并重建，覆盖了本次事故的全部可复现路径。
- C 方案的价值是补上「页面关闭后无绘制调用、控件已被游戏销毁」的时间窗，并让 EB 这类裸控件挂载不再依赖各自的生命周期判断。
- 若审计实验证明 `ControlObject_Delete @0x9eb3c` 覆盖袋容器子树，可在该入口维护「指针→代际」失效表；届时锚定层的身份字段比对可退化为一次代际比较，`forget_native` 仍用于组件表清理。
