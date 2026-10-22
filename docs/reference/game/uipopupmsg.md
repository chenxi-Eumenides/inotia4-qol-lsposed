# 原生 UIPopupMsg 文本弹窗索引域与调用点

## 机制

- 文本弹窗创建族 `UIPopupMsg_Create{OK,YesNo,None}FromTextData` / `CreateFromTextData`：首参 = `TEXTDATABASE`（表 index 73，record_size 2，共 190 行）行号。游戏侧先读该行 u16 得 MEMORYTEXT 文本 id，再以 `MEMORYTEXT_GetText(id)` 取字符串。例：row 97 → id 35413 →「只有宝石道具才可以。」
- 直接文本族 `UIPopupMsg_CreateOK/CreateYesNo/CreateNone/CreateDisplayFree/Create`：首参为字符串指针，文案不经 TEXTDATABASE（如 Hub 登录询问经 `GetCharStringFromPlist` 按 `GetConfigLanguage()` 从 plist 取本地化文案）。
- 全部 190 行的行号 → id → 文本可由 `apk/static-data/json/tables/TEXTDATABASE.json`（含 text_0 中文列）直接重建；MEMORYTEXT 全量为 `apk/static-data/json/text/zh-Hans.json`（数组下标 = id）。
- 呈现状态与按钮回调见 `ui.md` §3.5：`UIPopupMsg_bOn`（G_POPUP_ON @0x3070e8）+ `pText` + `fpOK`/`fpCancel`。

## 调用点解析方法与结论（monster 包 arm64-v8a，ELF vaddr）

解析方法：Ghidra 反编译 `UIPopupMsg_Create*FromTextData` 全部引用（92 处），提取调用首参常量。索引多数为运行期计算（教程链、事件脚本、商店状态），静态常量解析仅覆盖少数；运行期计算调用点需按函数定点分析。

### 已解析（索引为字面量）

| 行 | 文本 | 调用点 |
|---:|---|---|
| 1 | 是谁在窥视我的剑啊！！！ | `ITEMSYSTEM_AppearSwordMaster` 0x1fac64 |
| 5 | 背包空间不足，无法进行操作。 | `QUESTSYSTEM_ChangeQuestState`、`UIInApp_ButtonBuyItemExe` 等 4 函数 6 处 |
| 6 | 背包已满。 | `UIEquip_ButtonEquipExe`（2 处）、`UIEquip_ButtonUnequipExe`（4 处）、`UIMercenary_ButtonWithdrawExe` |
| 7 | 当前的背包不是空的。 | `UIEquip_ButtonUnequipExe`（4 处，与行 6 同分支群） |
| 8 | 是否购买？ | `UIStore_ButtonBuyExe` 0x1c1768、`UINpc_ExeNpcTask` 0x1b3064 等 |
| 9 | 是否出售？ | `UIStore_ButtonSellExe` 0x1c18bc、`UIStore_SellOKInputItemCount` 0x1c1980 等 |
| 10 | 您的金币不足。 | `UINpcRest_NoMoney_Confirm` 0x1b4230、`UINpc_ExeNpcTask` 0x1ed2b4 等 |

### 定点结论（选自行清单的行）

| 行 | 文本 | 结论 |
|---:|---|---|
| 79 | Hub 登录询问 | 该文案的实际弹窗路径不走 TEXTDATABASE：`STATE_HideModuleMainMenu`（0x152ff4）→ plist 文案 → `UIPopupMsg_CreateYesNo` @0x153098，已被 `P_HUB_LOGIN_SKIP`（+0x48 cbz 改写）压制；row 79 无其它静态引用 |
| 131 | 免费版导购 | 无调用点：id 35447 无静态引用；`UIDownloadPlus_CreatePopup`（0xb5ba0）只读 row 158/160（NO MORE ADS 购买框，`MEM_ReadUint16(TEXTDATABASE + record_size*0x9e/0xa0)`）。freefull 包内为死数据 |
| 174–188 | 新手触摸提示 | 非 UIPopupMsg：`TutorialDescription`（0x16cff4）按教程 id 从 .so 指针表取文案，由 `UIPlayPorting_Draw`（0xc8bd0–0xc9838 教程分支）绘制；TEXTDATABASE 行是同文案 DB 拷贝。见 `tutorial.md` |
| 4 | 咕咚咕咚~ | 泉水剧情对话，行索引由 EVTSYSTEM 事件脚本运行期传入，无硬编码调用点 |
| 23 | 佣兵活动室的空间不够了。 | 无硬编码；同函数群已证：`MERCENARYSYSTEM_IncludeParty` 队满→row 66（0x42）、失败→row 12（0xc）；`UIMercenary_OpenSlotEndProcess`→row 142（0x8e）。row 23 由雇佣失败路径运行期传入 |

## wrapper 层弹窗辅助（Java/JNI，`com.com2us.wrapper.WrapperUserDefined`）

libgame.so 经 GetStaticMethodID 反射调用，文案由 native/服务器传入，Java 层无静态文案：

| 方法 | 行为 |
|---|---|
| `ExitPopup(title, content, "btn1\|btn2")` | 双按钮 AlertDialog：确认→`nativeExitCB()`（native 实现，退出游戏）；第二键仅 dismiss |
| `AndroidPopupOpen(title, content, type)` | OK/Cancel 或仅 OK 弹窗（按钮文案硬编码英文） |
| `alertPopup(byte[] title, byte[] content)` | 字节按 `MainActivity.TextEncodingType`（UTF-8）解码 + OK 弹窗 |
| `GetNoticeHttp(url)` | HTTP GET 原文返回（native `jRequestNotice` @0x8d158 调用）；老 wrapper 公告通道，与 ActiveUser 更新通知弹窗无关 |
| `GotoURL(uri)` / `GotoPLUS(marketType)` | 商店跳转：`requestCollaboration` 构造 `market://details?id=`（及 SKT/KT/LGT 韩国商店 intent）；native 侧 JNI 桥为 `jGotoURL`/`jGotoPLUS`/`jGotoMoreGame` |

libgame.so 导出 `CS_UpdateNoticeShowPopup`/`CS_UpdateNoticeGetPopupInfo`（调 Java `jniShowNoticePopup`/`jniGetNoticePopupInfo`）在本包无调用者（死代码）；ActiveUser 更新通知弹窗的活路径见 `ui.md` §2.2。
