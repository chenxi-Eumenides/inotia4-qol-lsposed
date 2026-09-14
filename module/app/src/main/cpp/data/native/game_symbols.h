#pragma once

#include <cstddef>
#include <cstdint>

// ============================================================
// 游戏符号与结构体布局定义（单一来源）
// 由 scripts/analyze/check_symbols.py 解析校验；改动后需同步运行该校验。
// 结构体偏移来源：docs/notes/hook-points.md §3.2（M4.1 反汇编逆向）
// VMA 来源：libgame-symbols.txt（readelf 符号表）
// ============================================================

// ---- 角色结构体偏移 ----
constexpr size_t C_SITUATION = 0x00; // u8 角色情形码（CHAR_SetSituation 0xdc310 写 obj[0]；引擎碰撞 CHARSYSTEM_GetCharacterBlock 0xddaac 要求==1 才判阻挡；尸体死亡→SetSituation(6)/Free(0)，situation!=1 不再阻挡）
constexpr size_t C_TYPE = 0x09;      // int8 角色类型 (0=英雄 1=佣兵)
constexpr size_t C_NAME_ID = 0x0A;   // u16 名称相关 ID（非 text_id；角色名称须用 CHAR_GetName 获取）
constexpr size_t C_CLASS = 0x0D;     // int8 职业索引（0-5；CHARSYSTEM_Produce type==0 分支 f39c0 strb w21(class_idx),[ch+0xd]；type==2 装饰物此字段=type 值）
constexpr size_t C_LEVEL = 0x0E;     // int8 等级
constexpr size_t C_ATTR = 0x24;      // int32 属性数组 [char + attr_id*4 + 0x24]
constexpr size_t C_HP = 0x1F0;       // int32 当前 HP
constexpr size_t C_MP = 0x1F4;       // int32 当前 MP
constexpr size_t C_EQUIP = 0x1F8;    // 装备槽数组 (10 槽 × 8B 指针)
constexpr size_t C_EXP = 0x318;      // int64 当前经验
constexpr size_t C_NEXT_EXP = 0x320; // int64 升级所需经验
constexpr size_t C_STATUS = 0x311;   // u8 状态码 (0=队伍 1=城镇NPC/佣兵 2=怪物/召唤物, frida 实测)
constexpr size_t C_SKILL_LIST = 0x2A0;    // 已学战斗技能链表头（节点见 S_* 偏移）
constexpr size_t C_SKILL_BMP = 0x2B0;     // u16 技能解锁位图
constexpr size_t C_ACTIVE_SKILL = 0x280;  // 当前激活技能节点指针
constexpr size_t C_SKILL_POINTS = 0x328;  // int8 剩余技能点
constexpr size_t C_MERC_SLOT = 0x352;     // s8 佣兵槽索引（-1=非佣兵, frida 实测）
constexpr size_t C_PATH_LIST = 0x2F0;     // 寻路结果 PATHLIST 链表头（节点 +0x00 u16 网格x/+0x02 u16 网格y/+0x08 next）
constexpr size_t C_CTRL_STATE = 0x2E2;    // u8 控制状态（0=AI可自由寻路 7=玩家控制 135=战斗态, frida 实测）
constexpr size_t C_MOVE_TARGET = 0x278;   // 移动目标指针（MoveAsPath 在控制态下要求非空）
constexpr size_t C_EQUIP_SLOTS = 10;
constexpr size_t C_POS_X = 0x02;     // int16 实时 X（CHAR_GetDistance 反汇编证实）
constexpr size_t C_POS_Y = 0x04;     // int16 实时 Y
constexpr size_t C_OBJ_SIZE = 0x430; // 角色对象步长（CHARSYSTEM 池相邻对象间隔, frida 实测）
constexpr int C_CHARSYSTEM_POOL_SLOTS = 100; // 角色池容量 = 0x1a2c0/0x430（CHARSYSTEM_Initialize/ClearAll/Allocate/Find 反汇编硬编码 0x1a2c0 遍历终点）

// ---- CHARLOC 位置登记结构（CHARLOC_Copy/Add 反汇编确认，10B/条）----
constexpr size_t CHARLOC_SIZE = 0x0A; // 位置条目步长（Add 中 idx*8 + idx*2 = idx*10）
constexpr size_t LOC_TYPE = 0x00;     // u8 单位类型
constexpr size_t LOC_POS_X = 0x02;    // u16 实时 X
constexpr size_t LOC_POS_Y = 0x04;    // u16 实时 Y

// ---- 技能节点结构偏移（角色 C_SKILL_LIST 链表，frida 实测 2026-08-05）----
constexpr size_t S_ACTION_ID = 0x00; // u16 技能 action_id
constexpr size_t S_LEVEL = 0x02;     // u8 技能等级
constexpr size_t S_NEXT = 0x18;      // 下一节点指针

// ---- 佣兵槽结构偏移（20B/槽，MERCENARYSYSTEM_Set 反汇编确认）----
constexpr size_t M_TYPE = 0x00;   // u8 类型
constexpr size_t M_FLAGS = 0x0B;  // u8 flags (bit0=已占用 bit1=在队伍)
constexpr size_t M_SLOT_SIZE = 0x14;

// ---- 物品结构体偏移 ----
constexpr size_t I_TYPE = 0x08;  // u16 类型位域 (bit2-5=稀有度, bit6-15=类别)
    constexpr size_t I_COUNT = 0x10; // u32 数量位域 (bit22-31：0=不可堆叠、100=装备、1~999=可堆叠数量)
constexpr size_t I_MAGIC_RATE = 0x18; // u8 魔法伤害倍率（物理伤害×此值/100）
constexpr size_t I_SOCKET = 0x19;     // u8 宝石/插槽位域 (bit0-2=已镶宝石数 bit4-6=插槽等级)
constexpr size_t I_ENCHANT = 0x1A;    // u16 混沌/附魔位域 (bit0=有混沌 bit5-6=附魔等级 bit10-15=附魔ID)
constexpr size_t I_OPTION_LIST = 0x20; // 词缀链表头（节点见 O_* 偏移）

// ---- 词缀节点结构偏移（物品 I_OPTION_LIST 链表）----
constexpr size_t O_INDEX = 0x00; // u16 编码：bit0-6=词缀索引（ITEMOPTINFOBASE 记录下标）bit13-15=type（0=词缀 1=宝石；ITEM_AddOptionEx 0x105ec4 反汇编）
constexpr size_t O_VALUE = 0x02; // s16 词缀值
constexpr size_t O_NEXT = 0x08;  // 下一节点指针

// ---- HP/MP 上限的属性 id ----
constexpr int ATTR_MAX_HP = 0x1e;
constexpr int ATTR_MAX_MP = 0x1f;

// ---- HP/MP 上限缓存字段偏移（由 C_ATTR 基址 + ATTR_MAX_HP/MP * 4 推得，与 CHAR_GetAttr 同偏移体系）----
// 直接读缓存字段可避开 CHAR_GetAttr(attr=0x1e) 的写回副作用（HP>maxHP 时 str w0,[x20,#0x1f0] 钳 HP），
// 该函数在 HTTP/缓存预取线程调用会与游戏主线程属性重算竞争。
constexpr size_t C_MAX_HP = C_ATTR + ATTR_MAX_HP * 4;  // int32 最大 HP [ch+0x9c]
constexpr size_t C_MAX_MP = C_ATTR + ATTR_MAX_MP * 4;  // int32 最大 MP [ch+0xa0]

// ---- 主属性四项字段偏移（CHAR_GetStat 0xdf8d0 反汇编核实：总属性 = Base + Main + Bonus + Sub，无 clamp）----
// 索引 i = 0..4（力量/敏捷/体力/智力/精力）；宽度/符号性由各 getter 的加载指令确定。
constexpr size_t C_STAT_BASE = 0x250;   // s8  基础属性 [ch+0x250+i]（CHAR_GetStatBase 0xdb9e4：add + ldrsb）
constexpr size_t C_STAT_MAIN = 0x256;   // s16 分配主属性 [ch+0x256+i*2]（CHAR_GetStatMain 0xdb9f0：add lsl#1 + ldrsh）
constexpr size_t C_STAT_BONUS = 0x260;  // s8  存档加成 [ch+0x260+i]（CHAR_GetStatBonus 0xdb9fc：add + ldrsb）
constexpr size_t C_STAT_SUB = 0x266;    // s16 动态派生缓存 [ch+0x266+i*2]（CHAR_GetStatSub 0xdf888：add lsl#1 + ldrsh）
// 动态派生脏位（CHAR_IsCalculateStatusOn 0xdba08：ldrb [ch+0x270] + asr by i）。
// bit i 置位 = 第 i 项 sub 尚未重算；CHAR_GetStatSub 此时会先 CHAR_CalculateStatus 重算（写操作）。
constexpr size_t C_STAT_CALC_FLAG = 0x270; // u8 动态派生脏位

// ---- popup state entry 布局（g_sPopupStateList，27 条 × 64B；见 G_POPUP_STATE_LIST_GOT_VMA）----
constexpr size_t POPUP_STATE_COUNT = 27;      // 当前游戏版本的 state 条目数
constexpr size_t POPUP_ENTRY_SIZE = 0x40;     // 每条 64B
constexpr size_t POPUP_ENTRY_ENTER = 0x10;    // enter 回调
constexpr size_t POPUP_ENTRY_PROCESS = 0x18;  // process 回调
constexpr size_t POPUP_ENTRY_F3 = 0x28;       // f3 回调
constexpr size_t POPUP_ENTRY_F4 = 0x30;       // f4 回调
constexpr size_t POPUP_ENTRY_EVENT = 0x38;    // event 回调

// ---- MAPINFOBASE 地图记录布局 ----
constexpr size_t MAPINFOBASE_RECORD_NAME_TEXT_ID = 0x00; // u16 地图名称 text_id

// ---- popup 栈结构（g_arrPopupStack，G_POPUP_STACK_VMA）----
constexpr size_t POPUP_STACK_COUNT = 0x08;    // u32 栈内面板数
constexpr size_t POPUP_STACK_DATA = 0x18;     // u64 条目数组指针

// ---- 存档槽结构（fn_save_get_save_slot 返回对象）----
constexpr size_t SAVESLOT_MAP_ID = 0x00;      // u16 槽位当前地图 id
constexpr size_t SAVESLOT_EXISTS = 0x02;      // u8 槽位是否存在
constexpr size_t SAVESLOT_HERO_PTRS = 0x04;   // void*[3] 槽内 hero 指针数组
constexpr size_t SAVESLOT_HERO_INDEX = 0x1c;  // int8 当前 hero 索引

// ---- 角色朝向 / 交互函数显示 ----
constexpr size_t C_DIRECTION = 0x06;          // u8 角色朝向
constexpr size_t C_FUNC_DISPLAY = 0x0a;       // u16 交互函数显示值（type==2 装饰物；与 C_NAME_ID 同偏移）

// ---- 地面掉落物数组（G_DROP_ARRAY_GOT_VMA，元素步长 0x20）----
constexpr size_t GROUND_ITEM_SIZE = 0x20;     // 元素步长
constexpr size_t GROUND_ITEM_OBJECT = 0x00;   // 掉落物对象指针
constexpr size_t GROUND_ITEM_X = 0x08;        // int16 网格 X
constexpr size_t GROUND_ITEM_Y = 0x0a;        // int16 网格 Y
constexpr size_t GROUND_ITEM_FLAGS = 0x18;    // u8 标志
constexpr uint8_t GROUND_ITEM_FLAG_PICKING = 0x02; // 拾取中标志位

// ---- 拾取事件回调节点（NOTIFIER_Add 数据，CHAR_ActivePickupEvent 读取；模块 mem_malloc(0x18) 构造）----
constexpr size_t PICKUP_EVENT_SIZE = 0x18;
constexpr size_t PICKUP_EVENT_CHAR = 0x00;    // 玩家 char 指针
constexpr size_t PICKUP_EVENT_X = 0x08;       // int32 玩家 x
constexpr size_t PICKUP_EVENT_Y = 0x0c;       // int32 玩家 y
constexpr size_t PICKUP_EVENT_OBJECT = 0x10;  // 掉落物对象指针

// ---- 函数内 callsite 相对偏移 ----
constexpr size_t F_SCENE_DRAW_EQUIP_END_CALL_OFF = 0x210;  // F_SCENE_DRAW_EQUIP_VMA 内 bl GRPX_End 调用点偏移（扩展背包占用）
constexpr size_t F_SCENE_DRAW_EQUIP_DESC_CALL_OFF = 0x1cc; // F_SCENE_DRAW_EQUIP_VMA 内 bl UIDesc_Draw 调用点偏移（自动出售入口按钮绘制宿主，独立于扩展背包）

// ---- 全局变量 VMA ----
constexpr uintptr_t G_MONEY_VMA = 0x7134c0;        // int64 金币
constexpr uintptr_t G_MAP_ID_VMA = 0x713878;       // ⚠️ 历史遗留：实为瓦片矩阵起点（64×64，每字节 1 tile），前两字节 0x0808=2056 是巧合误读，勿用作 mapId（v0.4.28 修正）
constexpr uintptr_t G_CUR_MAP_ID_GOT_VMA = 0x2f4000 + 0xe80;  // 当前地图真实 ID（GOT 双层解引用 u32）：MAP_Load(0x1149d4) 写入（114ae8 str w22,[x1]，x1=*(0x2f4000+0xe80)）；= MAPINFOBASE 记录下标（30=影子丛林1/31=影子丛林2 真机验证）
constexpr uintptr_t G_PARTY_VMA = 0x728ec0;        // 3 个角色指针
constexpr uintptr_t G_ACTIVE_QUEST_VMA = 0x728ff8; // u16 当前任务
constexpr uintptr_t G_INVEN_VMA = 0x7131c0;        // INVEN_pItem 背包槽数组（6袋×0x80，每槽8B 物品指针）
constexpr uintptr_t G_BAG_TABLE_VMA = 0x2f3bc0;    // GOT 槽：*(0x2f3bc0) = 袋表指针（INVEN_GetBagSize 反汇编）
constexpr uintptr_t G_MAIN_MERC_SLOT_VMA = 0x729826; // SAVE_nMainMercenarySlot (u8) 当前控制角色槽
constexpr uintptr_t G_CHAR_POOL_VMA = 0x307538;      // CHARSYSTEM_pPool 角色对象池（指向英雄对象，0x430/对象）
constexpr uintptr_t G_DEALSYSTEM_SALE_LIST_VMA = 0x2f3000 + 0x490;  // GOT 槽：*(此地址) = 商店商品表基址（48 槽 × 16B，步长 0x10；+0 位域 bit0=空、+8 商品对象指针）
constexpr uintptr_t G_CHARLOC_POOL_VMA = 0x307530;   // CHARLOCSYSTEM_pPool 位置登记池（CHARLOC_Copy 反汇编：10B/条）
constexpr uintptr_t G_CHARLOC_COUNT_VMA = 0x307528;  // CHARLOCSYSTEM_nCount (u16) 位置登记条数
constexpr uintptr_t G_PREV_STATE_VMA = 0x307490;      // STATE_nPrevState (u16) 上一个 UI 状态（readelf 符号表）
constexpr uintptr_t G_STATE_VMA = 0x307492;          // STATE_nState (u16) UI 状态机（4=主菜单流程 5=游戏中, frida 实测）
constexpr uintptr_t G_GAMESTATE_VMA = 0x72b068;      // GAMESTATE_nState (u32) 游戏状态
// GAMESTATE_nState 状态值（GAMESTATE_SetState 0x151590 跳转表 + 真机 frida trace 确认）
constexpr uint32_t GAMESTATE_EVENT = 1;       // 剧情对话态（EVTSYSTEM_SetReady 成功 → SetState(1)，对话期间阻塞移动/切图）
constexpr uint32_t GAMESTATE_MAP_CHANGE = 3;  // 切图态（GAMEPLAY_GoMapLink → SetState(3)）
constexpr uintptr_t G_FRAME_COUNT_VMA = 0x2f5648;  // 帧计数 GOT 槽：*(此地址)=u64 计数指针（v0.4.57 实测与 MainProcess 严格 1:1，递增点在 Draw 完成后 d4a20；FPS 系统 0x3075f0 未启用恒 0；旧注"11.5fps"为测量误差已证伪）
constexpr uintptr_t G_INITSTATE_VMA = 0x72b06d;      // INITSTATE_nState (u8) 初始化状态
constexpr uintptr_t G_POPUP_ON_VMA = 0x3070e8;       // UIPopupMsg_bOn (u8) 弹窗/对话框是否激活（readelf 符号表）
constexpr uintptr_t G_POPUP_TEXT_VMA = 0x3070b8;     // UIPopupMsg_pText (8B) 弹窗打开时指向当前文本（v0.3.10 真机验证）
constexpr uintptr_t G_POPUP_FPOK_VMA = 0x3070e0;     // UIPopupMsg_fpOK (8B) 确定回调（非空=有确认按钮）
constexpr uintptr_t G_POPUP_FPCANCEL_VMA = 0x3070d8; // UIPopupMsg_fpCancel (8B) 取消回调（非空=有取消按钮）
constexpr uintptr_t G_POPUP_TYPE_VMA = 0x712518;     // 弹窗类型 (i32)（debug 端点）
constexpr uintptr_t G_POPUP_DISPTYPE_VMA = 0x712510; // 弹窗显示类型 (i32)（debug 端点）
constexpr uintptr_t G_MAINMENU_DRAW_VMA = 0x72a0f8;  // UIMainMenu_bDrawFull (u8) 主菜单是否完整绘制（readelf 符号表）
constexpr uintptr_t G_MAINMENU_BASE_VMA = 0x3099d8;  // 主菜单组控件实例基址（STATE_EnterMainMenu 0x151fac 反汇编：x19=0x309000+0x9d8，按钮控件指针存 +0x10~+0x30）
constexpr uintptr_t G_MAINMENU_MOREGAMES_SLOT = 0x20; // 「更多游戏」按钮控件槽偏移（base+0x20=0x3099f8；ExecuteProc=0x151e38→GotoShowMoreGames 0x8f0d8）
constexpr uintptr_t G_POPUP_STACK_VMA = 0x728fd8;    // g_arrPopupStack (32B) UI 弹窗栈（readelf 符号表）
constexpr uintptr_t G_POPUP_STATE_LIST_GOT_VMA = 0x2f3000 + 0x4f0;  // GOT 槽：*(此地址) = popup state list 基址（g_sPopupStateList，27 条 × 64B：id@+0, enter@+0x10, process@+0x18, f3@+0x28, f4@+0x30, event@+0x38；POPUPSTATE_Push 0x122464 以 id×0x40 索引）
constexpr uintptr_t G_PLAYER_ACTIVE_VMA = 0x728fc0;  // PLAYER_pActivePlayer (8B 指针) 游戏主控角色对象（PLAYER_SetActivePlayer 0x121a7c 写入；GAMEPLAY_DrawFocus 0x9d3ec / CHAR_Process 0xf1c04 读取；CHAR_MoveAsPath 驱动移动的真实对象，区别于 PARTY_GetMember 队伍槽——v0.4.38 移动修复）
constexpr uintptr_t G_MAPINFOBASE_PDATA_GOT_VMA = 0x2f4000 + 0xe58; // MAPINFOBASE_pData GOT 槽（双层解引用后为地图记录数组）
constexpr uintptr_t G_MAPINFOBASE_RECORD_SIZE_VMA = 0x3017b8; // MAPINFOBASE_nRecordSize（u8，值=6）
constexpr uintptr_t G_MAPINFOBASE_RECORD_COUNT_VMA = 0x3017ba; // MAPINFOBASE_nRecordCount（u16，值=416）
constexpr int MAPINFOBASE_STATIC_MAX_MAP_ID = 414; // 原版/改版共同的静态地图 ID 上限；运行时仍须受 record_count 限制
constexpr uintptr_t G_PLAYER_ACTIVE_GOT_VMA = 0x3f6000 + 0xa50; // 主控玩家指针 GOT 槽（双层解引用后为角色对象）
constexpr uintptr_t G_UICHOICE_BUTTON_LIST_EXE_GOT_VMA = 0x2f44d8; // UIChoice_ButtonListExe 函数指针 GOT 槽
constexpr uintptr_t G_UICHOICE_CONTROL_GOT_VMA = 0x302550; // UICHOICE 主控件指针槽（ButtonListExe 读取）
constexpr uintptr_t G_QUEST_SLOT_COUNT_VMA = 0x2f6000 + 0x270;  // GOT 双层解引用 u8 任务槽数量（QUESTSYSTEM_Find 0x12291c ldrb）
constexpr uintptr_t G_QUEST_SLOTS_GOT_VMA = 0x2f4000 + 0x3d0;  // GOT 双层解引用 任务槽数组基址（12B/槽：+0 questId u16；QUESTSYSTEM_Find 0x12292c / QUESTSYSTEM_CopySlot 0x122994）
constexpr uintptr_t G_MERC_SLOTLIST_GOT_VMA = 0x2f6000 + 0x10; // 佣兵槽数组指针（双层解引用 *(*(base+0x2f6000+0x10))，20B/槽；MERCENARYSYSTEM_IsEmptyManagerSlot 0x118b54 反汇编确认）
constexpr uintptr_t G_PLAYER_NEAR_NPC_VMA = 0x728fb8;   // PLAYER_pNearNPC（写者 PLAYER_DoCheckNearNPC 0x120d14）
constexpr uintptr_t G_TUTORIAL_OBJ_GOT_VMA = 0x2f5000 + 0x170;  // GOT 槽：指向教学状态对象，对象头部值 = 教学状态（0=无 6=药水教学激活 2=教学完成；GAMESTATE_PressKeyPlay 0x9d3a4 [x19]==0xe 时劫持按键；frida 实测 hp 低触发 6、用药水回满 → 2）
constexpr uintptr_t G_TUTORIAL_FLAG1_GOT_VMA = 0x2f6000 + 0xbb8; // GOT 槽：教学取消写 0（tutorial_cancel）
constexpr uintptr_t G_TUTORIAL_FLAG2_GOT_VMA = 0x2f3000 + 0x170; // GOT 槽：教学取消写 1（tutorial_cancel）
constexpr uintptr_t G_TUTORIAL_FLAG3_GOT_VMA = 0x2f6000 + 0xee0; // GOT 槽：教学取消写 0（tutorial_cancel）
constexpr uintptr_t G_NPCTASKLIST_INDEX_VMA = 0x307820; // NPCTASKLIST_nIndex (u8) 当前任务索引
constexpr uintptr_t G_NPCTASKLIST_COUNT_VMA = 0x307821; // NPCTASKLIST_nCount (u8) 任务数
constexpr uintptr_t G_NPCTASKLIST_PDATA_VMA = 0x307818; // NPCTASKLIST_pData（8B → 32×16B 槽数组：+0 u8 type、+2 u16 id）
constexpr uintptr_t G_NPCTASKLIST_DESCTEXT_VMA = 0x307810; // NPCTASKLIST_pDescText（对话描述文本）
constexpr uintptr_t G_UICHOICE_ITEMTEXT_VMA = 0x711c60; // UICHOICE_pItemText（6×8B 指针数组选项文本）
constexpr uintptr_t G_UICHOICE_COUNT_VMA = 0x302d70;    // UICHOICE_nItemCount (u8 选项数 ≤6)
constexpr uintptr_t G_UICHOICE_FOCUS_VMA = 0x302d80;    // UICHOICE_nFocusIndex (u8 焦点索引)
constexpr uintptr_t G_UICHOICE_MAIN_TEXT_VMA = 0x302d78; // UICHOICE_pMainText（char* 指针）
constexpr uintptr_t G_UI_QUEST_MENU_STATE_VMA = 0x7125c8;      // UIQuestMenu_ui8State (u8 任务菜单状态)
constexpr uintptr_t G_UI_STORE_BUY_TYPE_VMA = 0x712628;        // UIStore_ui8BuyType (u8 商店购买类型)
constexpr uintptr_t G_UI_STORE_SEL_CLASS_VMA = 0x712630;       // UIStore_ui8SelectedItemClass (u8 商店选中分类)
constexpr uintptr_t G_UI_HELP_STATE_VMA = 0x711c90;            // UIHelp_ui8State (u8 帮助状态)
constexpr uintptr_t G_UI_MMENU_SEL_CLASS_VMA = 0x7135a9;       // MAINMENU_ui8SelectedClass (u8 主菜单选中分类)
constexpr uintptr_t G_UI_MMENU_SAVE_SLOT_VMA = 0x7135aa;       // MAINMENU_ui8SaveSlotType (u8 主菜单存档槽类型)
constexpr uintptr_t G_UI_SHORTCUT_PAGE_VMA = 0x712600;         // UIShortcutMenu_i32Page (i32 快捷栏页码)
constexpr uintptr_t G_UI_QUEST_MENU_MAIN_SIZE_VMA = 0x7125c0;  // UIQuestMenu_nMainListSize (u16 任务菜单主列表大小)
constexpr uintptr_t G_UI_QUEST_MENU_SUB_SIZE_VMA = 0x7125f8;   // UIQuestMenu_nSubListSize (u16 任务菜单子列表大小)
constexpr uintptr_t G_UI_PARTY_MENU_INDEX_VMA = 0x728ed8;      // PARTY_nMenuIndex (u8 队伍菜单索引)
constexpr uintptr_t G_NPCSEL_ID_VMA = 0x728e8e;         // nSelectedID (u16 选中任务 ID)
constexpr uintptr_t G_NPCSEL_TYPE_VMA = 0x728e90;       // nSelectedType (u8 选中任务类型)
constexpr uintptr_t G_NPC_QUEST_IDX_GOT_VMA = 0x2f3000 + 0x240;  // GOT 双层解引用 (ldrsh) 当前 NPC 任务 questId（UINpcQuest_MakeText/ButtonOKExe 读取；路障任务=381）
constexpr uintptr_t G_NPC_QUEST_STATE_GOT_VMA = 0x2f6000 + 0xb40; // GOT 双层解引用 quest 状态表（GOT 槽 → 二级指针(.bss) → 状态表数组(堆)；索引=questId=记录下标，值 0=未接 1=进行 2=可完成 3=已完成；2026-08-16 实测：**st_got 指向堆 0x7c05dc7e6c，state[2]=1 与 API 一致）
constexpr uintptr_t G_QUEST_COUNT_GOT_VMA = 0x2f6000 + 0xe08;  // GOT 双层解引用 u16 quest 总数（QUESTSYSTEM_ChangeQuestState 0x123bb4 ldrh 边界校验；状态表遍历上限）
constexpr uintptr_t G_PLAYER_INDICES_GOT_VMA = 0x2f4000 + 0x120; // GOT 双层解引用 uint8_t*：3 名队员槽位索引数组（存档校验读 [0..2]；game_save.cpp）
constexpr uintptr_t G_FONT_OBJ_GOT_VMA = 0x2f3000 + 0xf88; // GOT 槽：*(此地址)=字体对象指针（自绘按钮 font 来源；game_ui_custom_panel.inc）

// ---- EVTSYSTEM 剧情对话（v0.4.27 readelf 符号确认 + EVTSYSTEM_Draw/PressKey/Process 反汇编）----
constexpr uintptr_t G_EVT_STATE_VMA = 0x713034;      // EVTSYSTEM_nState (u32) 剧情状态：0=无，对话中=3（frida 实测）
constexpr uintptr_t G_EVT_INDEX_VMA = 0x713018;      // EVTSYSTEM_nIndex (u32) 剧情文本索引（推进时递增 30→33→42→80→113）
constexpr uintptr_t G_EVT_ID_VMA = 0x71300c;         // EVTSYSTEM_nID (u32) 事件 ID（剧情中=1）
constexpr uintptr_t G_EVT_DATA_COUNT_VMA = 0x713010; // EVTSYSTEM_nDataCount (u32) 数据计数（剧情中=113）
constexpr uintptr_t G_EVT_PTELLER_VMA = 0x713028;    // EVTSYSTEM_pTeller (8B) 说话人 CHAR 指针（type@+0 x@+2 y@+4；名称用 CHAR_GetName）
constexpr uintptr_t G_EVT_POBJECT_VMA = 0x712ef0;    // EVTSYSTEM_pObject (8B) 立绘对象指针（DrawDialog 中非空=画对话框）
constexpr uintptr_t G_EVT_PFOCUS_VMA = 0x712ef8;     // EVTSYSTEM_pFocusChar (8B) 焦点角色
constexpr uintptr_t G_EVT_PTEXT_VMA = 0x3075d0;      // EVTSYSTEM_pText (8B) 当前对话文本指针（UTF-8，多句 00 00 分隔，pText 指向当前句）
constexpr uintptr_t G_EVT_TEXTCTRL_VMA = 0x713050;   // EVTSYSTEM_TextCtrl (128B)：+0x0=文本指针 +0x2e=推进标志 +0x58=总页 +0x5a=当前页
constexpr uintptr_t G_EVT_DISPLAY_ALPHA_VMA = 0x713008; // EVTSYSTEM_nDisplayAlpha (u8) 显示透明度（world=100）
constexpr uintptr_t G_EVT_OBJECT_TYPE_VMA = 0x7130d4;   // EVTSYSTEM_nObjectType (u8) 对象类型（剧情中=0）
constexpr uintptr_t G_EVT_SCENE_STATE_GOT_VMA = 0x2f6000 + 0xf98; // GOT 槽：*(此地址) = 场景状态数组（u32[]，索引=[0x2f4000+0xa50] 指向 s8）
constexpr uintptr_t G_GAME_RESUME_FLAG_GOT_VMA = 0x2f6000 + 0x8;  // GOT 槽：进档/新建标志（0=读档 GAME_StartResumeGame 前置，enter-slot 清 0；1=新建 STATE_EnterGame 走 GAME_StartNewGame，SaveSlot_GoToNewGame 置 1）
constexpr uintptr_t G_CURRENT_SLOT_GOT_VMA = 0x2f4000 + 0xd20;    // GOT 槽：*(此地址)=当前存档槽 u8 指针（SaveSlot_GoToNewGame/STATE_EnterGame 写，create/enter-slot 用）
constexpr uintptr_t G_PRODUCE_CLASS_GOT_VMA = 0x2f5000 + 0xa00;   // GOT 槽：*(此地址)=职业索引 u8 指针（SelectCharacter_StartGame 写、STATE_EnterGame→GAME_StartNewGame 读作 CHARSYSTEM_Produce 参数）
constexpr uintptr_t G_SELECTED_CLASS_VMA = 0x308080 + 0x8;        // 选角 UI 选中职业 u32（SelectCharacter_StartGame 读取源，select 回调写入）
constexpr uintptr_t G_HUD_GATE_GOT_VMA = 0x2f6000 + 0xc48;       // GOT 槽：HUD 显示开关（写 1=恢复显示，panel_close/recover 用）
constexpr uintptr_t G_DAILY_TRIGGER_GOT_VMA = 0x2f5000 + 0xff8;  // GOT 槽：每日奖励触发标志（写 1=触发，recover_after_hive_block 用）
constexpr uintptr_t G_EVT_SCENE_IDX_GOT_VMA = 0x2f4000 + 0xa50;   // GOT 槽：*(此地址) = 场景索引 (s8)（EVTSYSTEM_PressKey 写场景状态用）

constexpr uintptr_t G_MERC_MAX_GOT_VMA = 0x2f3000 + 0x978;     // 佣兵槽数 GOT 槽（解引用后读 s8；=21=3 队伍槽+18 仓库槽；MERCENARYSYSTEM_IsEmptyManagerSlot 0x118b38 ldrsb 确认）
constexpr uintptr_t G_TILE_GOT_VMA = 0x2f3f48;       // MAP 通行矩阵 GOT（双层解引用 *(*(base+0x2f3f48))，MAP_IsBlocking 反汇编确认；frida 实测与 MAP_nBaseTile 0x7148a8 非同一数据——0x7148a8 为渲染基础瓦片）

// ---- 角色属性/技能/装备表 GOT（v0.5.1 研究新增，CHAR_UpdateAttr 链反汇编确认）----
constexpr uintptr_t G_STAT_ATTR_MAP_COUNT_GOT_VMA = 0x2f4000 + 0x8d0;  // 主属性→attr 映射表记录数（u16；CHAR_UpdateAttrFromStat 0xdf99c）
constexpr uintptr_t G_STAT_ATTR_MAP_SIZE_GOT_VMA = 0x2f4000 + 0xb80;   // 主属性→attr 映射表记录大小（u8，=6B：+0主属性/+1attr/+2参数/+3公式text u16/+5条件）
constexpr uintptr_t G_STAT_ATTR_MAP_DATA_GOT_VMA = 0x2f6000 + 0xa38;   // 主属性→attr 映射表数据（双层解引用；19 条实测：力量→4攻击/敏捷→15命中+13总敏/体力→30HP+17防/智力精力→8魔攻）
constexpr uintptr_t G_SKILL_INFO_DATA_GOT_VMA = 0x2f4000 + 0x9e0;      // 技能信息表数据（双层解引用；recN↔action N，+0=技能名text_id=1220+rec、+0x1D=int16 等级参数；CHAR_GetActMaxLevel 0xe9560）
constexpr uintptr_t G_SKILL_INFO_SIZE_GOT_VMA = 0x2f6000 + 0x150;      // 技能信息表记录大小（u8，=32B）
constexpr uintptr_t G_SKILL_MAXLVL_MAP_DATA_GOT_VMA = 0x2f3000 + 0x758; // max_level→角色偏移映射表（双层解引用；记录+9=角色偏移，CHAR_GetActMaxLevel 用）
constexpr uintptr_t G_SKILL_MAXLVL_MAP_SIZE_GOT_VMA = 0x2f6000 + 0xe68; // max_level 映射表记录大小（u8，=11B）
constexpr uintptr_t G_ITEMCLASS_DATA_GOT_VMA = 0x2f4000 + 0xcf0;       // ITEMCLASSBASE 数据（双层解引用；记录+2=槽位表索引、+7 bit4=不可装备；运行时 23B/条 vs JSON 31B）
constexpr uintptr_t G_ITEMCLASS_SIZE_GOT_VMA = 0x2f5000 + 0x308;       // ITEMCLASSBASE 记录大小（u8）
constexpr uintptr_t G_EQUIP_SLOT_TABLE_DATA_GOT_VMA = 0x2f5000 + 0xb60; // 装备槽位表数据（双层解引用；记录+4=最终槽位 0头/1护手/2斗篷/3体/4鞋/5主手/6副手/7项链/8戒指；CHAR_FindEquipSlot 0xe4fd0）
constexpr uintptr_t G_EQUIP_SLOT_TABLE_SIZE_GOT_VMA = 0x2f3000 + 0x418; // 装备槽位表记录大小（u8）
constexpr uintptr_t G_LEVEL_ATTR_IDX_GOT_VMA = 0x2f3000 + 0xe70;       // 等级驱动属性索引表（双层解引用；索引 u8×9 → 公式表；CHAR_UpdateAttr 0xdfb30 id28/id30）
constexpr uintptr_t G_LEVEL_ATTR_FORMULA_DATA_GOT_VMA = 0x2f5000 + 0x5a0; // 等级驱动属性公式表（双层解引用；记录 u16=公式text；text[9]='960a36*+10/'=attr28、text[1]='640 72a*+'=attr30 HP上限）
constexpr uintptr_t G_DEFAULT_ATTR_COUNT_GOT_VMA = 0x2f3000 + 0xc38;   // 默认属性表记录数（u16；CHARSYSTEM_GetDefaultAttributeValue 0xf4a58）
constexpr uintptr_t G_DEFAULT_ATTR_SIZE_GOT_VMA = 0x2f5000 + 0xa18;    // 默认属性表记录大小（u8，=4B：+0 attr_id/+1 职业位掩码/+2 默认值 int16）
constexpr uintptr_t G_DEFAULT_ATTR_DATA_GOT_VMA = 0x2f6000 + 0xe38;    // 默认属性表数据（双层解引用；22 条实测：attr0=30/attr3=1000/attr31=200 等）
constexpr uintptr_t G_EQUIP_OPT_TABLE_DATA_GOT_VMA = 0x2f5000 + 0x5b0; // 装备词缀表数据（双层解引用；记录+2=类型(int8,==1属性加成)、+3=目标attr id；CHAR_UpdateAttrFromEquipOpt 0xda9d8）
constexpr uintptr_t G_EQUIP_OPT_TABLE_SIZE_GOT_VMA = 0x2f3000 + 0xb08; // 装备词缀表记录大小（u8）

// ---- 佣兵/名字表符号地址（.bss 直接符号，base+VMA；v0.5.4 研究新增，libgame-symbols.txt 核对）----
constexpr uintptr_t MERCENARYINFOBASE_PDATA_VMA = 0x301590;       // 佣兵模板表数据指针（47 条 × 8B：+0 特性/初始装备 text、+2 职业索引|变体、+4 佣兵名 text、+6 特性参数）
constexpr uintptr_t MERCENARYINFOBASE_NSIZE_VMA = 0x301598;       // 佣兵模板表记录大小（u8，=8）
constexpr uintptr_t MERCENARYINFOBASE_NCOUNT_VMA = 0x30159a;      // 佣兵模板表记录数（u16，=47）
constexpr uintptr_t MAXLEVELBASE_PDATA_VMA = 0x301620;            // 职业×等级档装备表数据指针（48 条 × 4B：+0 职业索引|档位、+2 装备名 text）
constexpr uintptr_t MAXLEVELBASE_NSIZE_VMA = 0x301628;            // 职业×等级档表记录大小（u8，=4）
constexpr uintptr_t MAXLEVELBASE_NCOUNT_VMA = 0x30162a;           // 职业×等级档表记录数（u16，=48）
constexpr uintptr_t G_HERO_NAME_TABLE_DATA_GOT_VMA = 0x2f6000 + 0x538; // 英雄名表数据（CHAR_GetName 0xd9c54；name_id×130B/条，+0=名字 text）
constexpr uintptr_t G_MERC_NAME_TABLE_DATA_GOT_VMA = 0x2f6000 + 0x598; // 佣兵名表数据（CHAR_GetName type=1 佣兵分支）


// ---- UI 面板 enter VMA（g_sPopupStateList 27 条 × 64B 中 enter@+0x10 的匹配值；panel_close 栈顶识别 / panel_open 白名单）----
constexpr uintptr_t F_PANEL_CHARACTER_INFO_ENTER = 0x148950; // character_info 角色信息
constexpr uintptr_t F_PANEL_CHOICE_ENTER = 0x14a664;          // choice 选择框（事件驱动）
constexpr uintptr_t F_PANEL_INVENTORY_ENTER = 0x14a8b0;       // inventory 背包（可开）
constexpr uintptr_t F_SCENE_PROCESS_EQUIP_VMA = 0x14ac2c;     // void () Scene_Process_POPUP_SC_EQUIP
constexpr uintptr_t F_SCENE_DRAW_EQUIP_VMA = 0x14a9bc;        // void () Scene_Draw_POPUP_SC_EQUIP
constexpr uintptr_t F_SCENE_EVENT_EQUIP_VMA = 0x14acd0;       // u64 (u64, u64, u64) Scene_Event_POPUP_SC_EQUIP
constexpr uintptr_t F_PANEL_INPUT_COUNT_ENTER = 0x14ad98;     // input_count 数量输入（需物品上下文）
constexpr uintptr_t F_PANEL_MERCENARY_ENTER = 0x14af14;       // mercenary 佣兵（可开）
constexpr uintptr_t F_PANEL_CRAFT_ENTER = 0x14b330;           // craft 合成（需 NPC）
constexpr uintptr_t F_PANEL_NPC_ENTER = 0x14b5dc;             // npc 对话
constexpr uintptr_t F_PANEL_NPC_QUEST_ENTER = 0x14b858;       // npc_quest 任务
constexpr uintptr_t F_PANEL_NPC_REST_ENTER = 0x14ba98;        // npc_rest 休息
constexpr uintptr_t F_PANEL_NPC_REVIVE_ENTER = 0x14bb48;      // npc_revive 复活
constexpr uintptr_t F_PANEL_OPTIONS_ENTER = 0x14be20;         // options 选项（主菜单专属）
constexpr uintptr_t F_PANEL_QUESTS_ENTER = 0x14c218;          // quests 任务（可开）
constexpr uintptr_t F_PANEL_SAVE_SLOT_ENTER = 0x14c720;       // save_slot 存档槽
constexpr uintptr_t F_PANEL_CHAR_SELECT_ENTER = 0x14d670;     // character_select 角色选择
constexpr uintptr_t F_PANEL_SHORTCUT_ENTER = 0x14df04;        // shortcut 快捷栏
constexpr uintptr_t F_PANEL_SKILLS_ENTER = 0x14f194;          // skills 技能（可开）
constexpr uintptr_t F_PANEL_SHOP_ENTER = 0x14f4b8;            // shop 商店（需 NPC）
constexpr uintptr_t F_PANEL_SETTINGS_ENTER = 0x14fb38;        // settings 设置（可开）
constexpr uintptr_t F_PANEL_WIPEOUT_ENTER = 0x1506d8;         // wipeout 死亡面板（自动）
constexpr uintptr_t F_PANEL_WORLD_MAP_ENTER = 0x150f48;       // world_map 世界地图（事件驱动）
constexpr uintptr_t F_PANEL_IN_APP_ENTER = 0x15e054;          // in_app 内购
constexpr uintptr_t F_PANEL_DAILY_REWARD_ENTER = 0x16f050;    // daily_reward 每日奖励
// 未命名面板 enter（panel_close 校验集内，无 panel_open 白名单名）：
constexpr uintptr_t F_PANEL_UNK1_ENTER = 0x15e3dc;
constexpr uintptr_t F_PANEL_UNK2_ENTER = 0x15e740;
constexpr uintptr_t F_PANEL_UNK3_ENTER = 0x15eac8;
constexpr uintptr_t F_PANEL_UNK4_ENTER = 0x15ee70;
constexpr uintptr_t F_PANEL_UNK5_ENTER = 0x15f1f8;
constexpr size_t TILE_ROW_STRIDE = 64;               // 瓦片行字节步长（MAP_IsBlocking 中 y*64+x 索引）
constexpr uint8_t TILE_BLOCK_BIT = 0x08;             // 阻挡标志位（ubfx bit3）

// ---- 骰子（STATUSDICE）状态 ----
// 两处均为 GOT 槽：先解引用取指针，再按位操作（STATUSDICE_Roll/Apply/UI 按钮反汇编确认）。
constexpr uintptr_t G_STATUSDICE_PENDING_GOT_VMA = 0x2f5740;  // GOT 槽：*(此地址) = pending int8[5] 数组指针（STATUSDICE_Roll 写入/Apply 读取，5 项基础属性掷骰结果）
constexpr uintptr_t G_STATUSDICE_FLAG_GOT_VMA = 0x2f37b8;     // GOT 槽：*(此地址) = 确认标志 u8 指针，bit0=1 有未确认掷骰结果（ButtonRollExe 置位、Create/Apply 复位）

// ---- UIEquip 背包面板（move-merge v0.6.8）----
constexpr uintptr_t G_UIEQUIP_INVEN_ITEM_PROC_GOT_VMA = 0x2f5410;
constexpr uintptr_t G_UIEQUIP_PANEL_VMA = 0x3049e0;
constexpr uintptr_t G_UIEQUIP_CUR_BAG_VMA = G_UIEQUIP_PANEL_VMA + 0x61;
constexpr uintptr_t G_UIEQUIP_CUR_BAG_GOT_VMA = 0x2f5000 + 0x6d8; // ptr to UIEquip current bag index, used by UIEquip_DrawInven*
constexpr uintptr_t G_UIEQUIP_DESC_TYPE_VMA = G_UIEQUIP_PANEL_VMA + 0x63;
constexpr uintptr_t G_UIEQUIP_PANEL_CTRL_VMA = G_UIEQUIP_PANEL_VMA + 0x8;
constexpr uintptr_t G_UIEQUIP_PANEL_BAG_CONTAINER_VMA = G_UIEQUIP_PANEL_VMA + 0x50; // 面板 +0x50 = 袋容器控件指针
constexpr size_t BAG_OBJECT_CAPACITY = 0x10; // 袋对象 +0x10 容量位域（bit0..24）

// ---- UIStore 商店面板（P7 store host）----
// UIStore 面板 .bss 基址；商店宿主与 UIEquip 宿主使用独立控件树。
constexpr uintptr_t G_UISTORE_PANEL_VMA = 0x3073e0;
constexpr uintptr_t G_UISTORE_ITEM_CTRL_VMA = G_UISTORE_PANEL_VMA + 0x10;
constexpr uintptr_t G_UISTORE_BAG_CTRL_VMA = G_UISTORE_PANEL_VMA + 0x38;
constexpr uintptr_t G_UISTORE_SELL_BTN_VMA = G_UISTORE_PANEL_VMA + 0x48;
constexpr uintptr_t F_UISTORE_REFRESH_INVEN_BAG_VMA = 0xd2228;
constexpr uintptr_t F_UISTORE_REFRESH_INVEN_ITEM_VMA = 0xd22d0;
constexpr uintptr_t F_UISTORE_MAKE_DESC_VMA = 0xd27a0;
constexpr uintptr_t F_UISTORE_DESC_MAKE_DESC_CALL_VMA = 0xd287c;
constexpr uintptr_t F_UISTORE_INVEN_ITEM_PROC_VMA = 0xd2838;
constexpr uintptr_t F_UISTORE_INVEN_BAG_PROC_VMA = 0xd2394;
constexpr uintptr_t F_UISTORE_DRAW_VMA = 0xd2fc4;
constexpr uintptr_t F_UISTORE_DRAW_INVEN_ITEM_GROUP_VMA = 0xd2bf8;
constexpr uintptr_t F_UISTORE_DRAW_INVEN_BAG_GROUP_VMA = 0xd2d5c;
constexpr uintptr_t F_UISTORE_DRAW_INVEN_BAG_GROUP_CALL_VMA = 0xd30e4; // UIStore_Draw 末尾尾调用 b UIStore_DrawInvenBagGroup（原字 0x17ffff1e，B patch 挂遮蔽 wrapper）
constexpr uintptr_t F_SCENE_DRAW_STORE_VMA = 0x14f5a0;
constexpr uintptr_t F_SCENE_EVENT_STORE_VMA = 0x14f6f8;
constexpr uintptr_t F_SCENE_TERMINATE_STORE_VMA = 0x14f558;
constexpr uintptr_t F_SCENE_DRAW_STORE_GRPX_END_CALL_VMA = 0x14f63c;
// 商店买入预检：UIStore_BuyItem(0xd242c) 在 INVEN_SaveItem 前自行调
// INVEN_FindSaveSlot 找槽，原版袋满直接弹窗（TextData 0xb），到不了
// SaveItem hook 的扩展 adopt。两处 bl 调用点改写为 store buy-gate：
// 原版无空位但扩展袋有空位时放行，让后续 SaveItem hook 触发扩展 adopt。
constexpr uintptr_t F_UISTORE_BUY_ITEM_VMA = 0xd242c;
constexpr uintptr_t F_UISTORE_BUY_FIND_SLOT_CALL_1_VMA = 0xd24a0;  // 普通货物路径（原字 0x9400c530）
constexpr uintptr_t F_UISTORE_BUY_FIND_SLOT_CALL_2_VMA = 0xd2540;  // CopyAsNewUID 复制路径（原字 0x9400c508）

// ---- 物品序列化 + 触摸拖动状态（v0.7.0 扩展背包跨包移动，objdump 逐字节确认）----
constexpr uintptr_t F_SAVE_SAVE_ITEM_VMA = 0x1274f0;    // int (uint8_t* out, void* item) SAVE_SaveItem：序列化物品到 out（u8 长度前缀 + 18B 头 + 4B×N 词缀；总长 ≤255，返回总字节）
constexpr uintptr_t F_SAVE_LOAD_ITEM_VMA = 0x1278a0;    // int (const uint8_t* in, void** out, int* consumed) SAVE_LoadItem：ITEMPOOL_Allocate 重建物品；consumed=前缀+1=记录总长；失败返回 0 且 *out 不清空（调用方须先置空）
constexpr uintptr_t F_ITEMPOOL_FREE_VMA = 0x108160;     // void (void*) ITEMPOOL_Free：释放物品对象回游戏对象池
constexpr uintptr_t G_TOUCH_STATE_VMA = 0x301000 + 0xcf8; // TouchHandle 全局状态（匿名 .bss）：+0x30=拖动中控件 +0x50=释放参数(+0x50=释放控件 +0x58=拖动源控件 +0x60=释放坐标)
constexpr size_t TOUCH_STATE_PREFIX_SIZE = 0x10;           // TouchHandle_ResetMovingControl 清零前缀
constexpr size_t TOUCH_STATE_ACTIVE_CTRL = 0x10;           // TouchHandle 当前激活控件
constexpr size_t TOUCH_STATE_MOVING_CTRL = 0x30;        // TouchHandle_Event 0x17 按下/拖动中控件
constexpr size_t TOUCH_STATE_RELEASE_INPUT_X = 0x18;    // TouchHandle_Event 0x18 输入坐标 x
constexpr size_t TOUCH_STATE_RELEASE_INPUT_Y = 0x20;    // TouchHandle_Event 0x18 输入坐标 y
constexpr size_t TOUCH_STATE_RELEASE_INPUT_PARAM = 0x28; // TouchHandle_Event 0x18 输入第三字段
constexpr size_t TOUCH_STATE_MOVE_ON_CTRL = 0x38;       // TouchHandle_Move 交接控件
constexpr size_t TOUCH_STATE_DROP_EVENT = 0x48;         // TouchHandle release drop 结果
constexpr size_t TOUCH_STATE_RELEASE_CTRL = 0x50;       // TouchHandle_SetReleaseEvent 释放控件
constexpr size_t TOUCH_STATE_DROP_SRC_CTRL = 0x58;      // TouchHandle_SetReleaseEvent 写入的拖动源控件
constexpr size_t TOUCH_STATE_RELEASE_X = 0x60;          // TouchHandle release 坐标 x
constexpr size_t TOUCH_STATE_RELEASE_Y = 0x68;          // TouchHandle release 坐标 y
constexpr size_t ITEM_CTRL_ITEM = 0x00;                 // ControlItem 私有数据 data[0] 的物品指针
constexpr size_t ITEM_CTRL_MOVING_FLAG = 0x0a;          // 物品控件数据块移动标志（ContorlItem_SetMoving 写）
constexpr size_t ITEM_CTRL_ON_FLAG = 0x0b;              // 物品控件数据块选中标志（ContorlItem_SetOn 写）

// ---- UIMix 合成器控件系统（craft-batch-ui，v0.5.18）----
// 全局 0x305550（.bss 无名，直接 VMA 兜底）：UIMix 固定控件指针槽基址（UIMix_CreateMainControl 反汇编确认）。
constexpr uintptr_t G_UIMIX_VMA = 0x305550;          // UIMix 固定控件槽基址（根控件/按钮/材料槽指针表）
constexpr size_t UIMIX_SLOT_GEM_BTN = 0xa0;          // 宝石合成按钮槽偏移（0x3055f0，UIMix_Draw 硬编码枚举绘制）
constexpr size_t UIMIX_SLOT_ITEM_GROUP = 0xd8;       // 物品网格组指针槽（16 个 ControlItem 子控件，UIMix_RefreshInvenItem 反汇编确认）
constexpr size_t UIMIX_SLOT_BAG_GROUP = 0xe0;        // 袋选择组指针槽（6 个 ControlItem 子控件，UIMix_CreateMainControl/UIMix_RefreshInvenBag 反汇编确认）
constexpr size_t UIMIX_SLOT_STATE = 0x20;            // u8 面板状态：0=配方菜单（仅背景/标题/5 配方按钮），1=背包/合成视图（UIMix_SetState/UIMix_GetState/UIMix_Draw 反汇编确认）
// ---- UIMix 宝石合成操作优化（gem-craft-optimization 阶段1）----
constexpr size_t UIMIX_SLOT_STUFF_GROUP = 0xc8;      // 填入格材料组指针槽（前 3 子控件 = 3 个填入格；UIMix_ButtonInvenItemSelectExe 0xc2528 ldr [x1,#0xc8] 反汇编确认）
constexpr size_t UIMIX_SLOT_DESC_MENU = 0x130;       // 详情菜单按钮槽（ExecuteProc=UIMix_ButtonInvenItemSelectExe 0xc2328；UIMix_RefreshInvenItem/UIMix_SetDescMenu 写入）
constexpr size_t UIMIX_SLOT_STUFF_LIST = 0xe8;       // 材料 itemId 列表指针槽（type 1 时首个 32 位字段 = 当前配方材料档位 category；0xc23c8 ldr x23,[x21,#0xe8] + 0xc24c0 ldr w2,[x23]）
constexpr size_t UIMIX_SLOT_TYPE = 0x38;             // u8 合成类型（0=混沌 1=宝石 2=打孔 3=其它 4=传说；UIMix_GetType 0xbf480 读 0x305588=G_UIMIX_VMA+0x38）
constexpr size_t UIMIX_SLOT_SELECTED_STUFF = 0x128;  // i64 当前选中填入格下标（-1=未选中；0xc24e0 ldr x0,[x21,#0x128] 反汇编确认，UIMix_StuffItemControlEventProc 写入）
constexpr size_t UIMIX_SLOT_RECIPE_GROUP = 0x18;     // 配方组控件槽（UIMix_CreateRecipeGroupControl 0xbfbb4 str x0,[x19,#0x18]；子按钮 ExecuteProc=UIMix_ButtonRecipeExe）
constexpr size_t UIMIX_SLOT_MENU_BUTTON_BASE = 0x60; // 5 个类型/菜单按钮槽起始（UIMix_CreateMainControl 0xbf6f4 add x21,x19,#0x60 + 0xbf700 str x0,[x21,x20,lsl#3]，步长 8）
constexpr int UIMIX_MENU_BUTTON_COUNT = 5;           // 类型/菜单按钮数量（槽 0x60..0x80）
constexpr size_t UIMIX_SLOT_CRAFT_BUTTON = 0x98;     // 合成按钮槽（UIMix_CreateMainControl 0xbf7d8 str x0,[x19,#0x98]；ExecuteProc=UIMix_ButtonMixingExe）
constexpr size_t UIMIX_SLOT_MIXTYPE = 0x48;          // u32 当前 mixType（所选配方；UIMix_ButtonMixingExe 0xc222c 读 [+0xf8] 前由配方写入；阶段2 前置改写）
constexpr size_t UIMIX_SLOT_COST = 0xf8;             // i64 合成费用（UIMix_InitMixingState 依配方费用文本 CAL_Calculate 写入；UIMix_ButtonMixingExe 0xc2230 读）

// ControlObject 结构（0xf8 字节，ControlObject_Create @0x9e4ec / ControlButton_Create @0xaa710 反汇编）
constexpr size_t CO_TYPE = 0x08;             // u32 Type（button=3）
constexpr size_t CO_ACTIVE = 0x0c;           // u32 Active（0x20 激活；ControlObject_EventProc 校验 ==0x20）
constexpr size_t CO_RECT_X = 0x18;           // i64 rect x
constexpr size_t CO_RECT_Y = 0x20;           // i64 rect y
constexpr size_t CO_RECT_W = 0x28;           // i64 rect w
constexpr size_t CO_RECT_H = 0x30;           // i64 rect h
constexpr size_t CO_USERTYPE = 0x40;         // u64 UserType（0 通用/1 按钮/2 物品）
constexpr size_t CO_DATA = 0x50;             // ptr Data（类型私有数据）
constexpr size_t CO_COUNT = 0x78;            // u32 子控件数（父控件遍历子节点用）
constexpr size_t CO_EVENT_CALL_TYPE = 0x88;  // u32 ControlEventCallType（0x100 按下/0x200 点击触发）
constexpr size_t CO_PROC = 0x90;             // ptr Proc（统一事件分发 = TouchHandle_ControlEventProc）
constexpr size_t CO_CONTROL_PROC = 0x98;     // ptr ControlProc（类型事件处理器 = ControlButton_ControlEventProc）
constexpr size_t CO_PARENT = 0xa0;           // ptr Parent
constexpr size_t CO_CHILD_LIST = 0xa8;       // 0x10 内嵌 LINKEDLIST ChildList
constexpr size_t CO_SIZE = 0xf8;             // ControlObject 总大小

// 按钮私有数据（0x78 字节，ControlObject+0x50 指向；ControlButton_Create 反汇编确认）
constexpr size_t CB_EXECUTE_PROC = 0x20;     // ptr ExecuteProc（点击回调函数指针）
constexpr size_t CB_DRAW_TYPE = 0x28;        // u32 DrawType
constexpr size_t CB_DRAW_ID = 0x30;          // i64 DrawID（贴图 id，-1 默认）
constexpr size_t CB_DRAW_SUB_ID = 0x38;      // i64 DrawSubID
constexpr size_t CB_DRAW_PROC = 0x60;        // ptr DrawProc（绘制函数指针）
constexpr size_t CB_STATE = 0x68;            // u8 State（0 正常/1 选中高亮）
constexpr size_t CB_ENABLED = 0x69;          // u8 使能标志（ControlButton_Create 置 1）
constexpr size_t CB_SIZE = 0x78;             // 按钮私有数据总大小

// ---- 函数 VMA ----
constexpr uintptr_t F_GET_MONEY_VMA = 0x10445c;      // int64 ()
constexpr uintptr_t F_GET_MEMBER_VMA = 0x11f384;     // void* (int)
constexpr uintptr_t F_GET_MENU_CHARACTER_VMA = 0x120338; // void* () PARTY_GetMenuCharacter：当前装备/物品菜单选中角色
constexpr uintptr_t F_GET_PARTY_SIZE_VMA = 0x11f3a4; // int ()
constexpr uintptr_t F_GET_ATTR_VMA = 0xdfd18;        // int32 (void*, int)
constexpr uintptr_t F_GET_EQUIP_VMA = 0xda20c;       // void* (void*, int)
constexpr uintptr_t F_GET_EXP_VMA = 0xd9b54;         // int64 (void*)
constexpr uintptr_t F_GET_NEXT_EXP_VMA = 0xd9b68;    // int64 (void*)
constexpr uintptr_t F_GET_RARITY_VMA = 0x10d700;     // int (void*)
constexpr uintptr_t F_GET_BAG_SIZE_VMA = 0x103250;   // int (int)
constexpr uintptr_t F_INVEN_GET_EMPTY_BAG_SLOT_VMA = 0x103280; // int () 查找原版空袋槽
constexpr uintptr_t F_INVEN_IS_EMPTY_BAG_VMA = 0x1032e0; // int (int) 判断原版袋是否为空
constexpr uintptr_t F_INVEN_IS_HAVING_EMPTY_SLOT_VMA = 0x103460; // int (int needed, int include_task_bag)
constexpr uintptr_t F_GET_BIT_VMA = 0x140528;        // int (int,int,int)
    constexpr uintptr_t F_GET_CUMULATE_COUNT_VMA = 0x106094; // int (void*) 堆叠数量：可堆叠类返回 bit22-31，不可堆叠类（装备）返回 1
constexpr uintptr_t F_GET_DAMAGE_VMA = 0x1099f0;     // int (void*) 物品攻击
constexpr uintptr_t F_GET_DEFENSE_VMA = 0x109cc0;    // int (void*) 物品防御
constexpr uintptr_t F_GET_STAT_VMA = 0xdf8d0;        // int (void*, int) 主属性总属性=Base+Main+Bonus+Sub (0=力量 1=敏捷 2=体力 3=智力 4=精力)
// ⚠️ F_GET_STAT 经 CHAR_GetStatSub(0xdf888) 在动态派生脏位置位时会重算并写回，非纯读；
// 非游戏线程请用 game_state.h 的 char_stat_total() 直读 C_STAT_* 字段。
constexpr uintptr_t F_GET_STAT_BASE_VMA = 0xdb9e4;   // int (void*, int) 基础属性 [ch+0x250+i] s8
constexpr uintptr_t F_GET_STAT_BONUS_VMA = 0xdb9fc;  // int (void*, int) 加成属性 [ch+0x260+i] s8（存档独立保存）
constexpr uintptr_t F_GET_STATUS_POINT_VMA = 0xd9c44; // int (void*) 剩余能力点
constexpr uintptr_t F_GET_STAT_MAIN_VMA = 0xdb9f0;    // int (void*, int) 读主属性 [ch+0x256+i*2]（i=0-4 力量/敏捷/体力/智力/精力）
constexpr uintptr_t F_SET_STAT_MAIN_VMA = 0xdf1c4;    // void (void*, int, int) 写主属性 + CHAR_ResetAttrFromStat 重算衍生
constexpr uintptr_t F_SET_STAT_BASE_VMA = 0xdf170;    // void (void*, int, int) 写基础属性 [ch+0x250+i] s8 + 重算衍生 + SV 同步
constexpr uintptr_t F_PUT_JEWEL_VMA = 0x10bcb4;       // int (void*, void*) 镶嵌宝石（equipItem+jewelItem）；返回 0=成功/2=无孔/3=非宝石或空装备
constexpr uintptr_t F_IS_JEWEL_VMA = 0x10b964;        // int (int32_t) 类别是否为宝石
constexpr uintptr_t F_ENCHANT_ITEM_VMA = 0x10b330;    // int (void*, int32_t) 强化装备（equipItem + scrollCategory）；返回 0=成功
constexpr uintptr_t F_IS_ENCHANT_SCROLL_VMA = 0x10b2f0; // int (int32_t) 类别是否为强化卷轴（武器 16-20/946、防具 21-25/947）
constexpr uintptr_t F_SAVE_IS_OK_VMA = 0x128c14;       // int (void) SAVE_IsOK：原版应用前存档状态校验
constexpr uintptr_t F_CHAR_INITIALIZE_STATUS_VMA = 0xe68c8;  // void (void*) 属性重置：5 项主属性归 0 + 能力点按 (等级-1)×职业基础值 还原
constexpr uintptr_t F_CHAR_INITIALIZE_SKILL_VMA = 0xe67c8;   // void (void*) 技能重置：移除技能链表非基础技能（ACTLIST_RemoveNode）+ 技能点按职业还原（CHAR_SetSkillPoint）+ 清快捷键 + 重算属性
constexpr uintptr_t F_CHAR_SET_ACTION_ID_VMA = 0xe79ec;      // void (void*, int32_t, void*) 释放技能动作（ch+actionId+目标指针；内部 FindAction→SetAction 写 [ch+0x280]）。⚠️ 第 3 参是目标对象指针非 level（技能动作 type==2 读 [target+2]/[target+4] 坐标算朝向）
constexpr uintptr_t F_SET_LEVEL_VMA = 0xe05a0;               // int (void*, int32_t) 设置角色等级：写 [ch+0xe] + CHAR_SetNextExperience(0xd9c28) + CHAR_InitializeFromLevel(0xdf2c0) + 升级加能力点/技能点（表驱动）+ 回满血蓝（C_HP/C_MP=GetAttr(0x1e/0x1f)）。⚠️ 只允许升级/同级（b.le 分支），降级直接返回 0
constexpr uintptr_t F_CHAR_GET_ENEMY_TARGET_VMA = 0xe42b4;   // void* (void*, int32_t, int32_t) 获取敌人目标（[ch+0x2c8] bit13 或 [ch+0x278] 有则返回，否则 FindBestTargetByAct 自动找）
constexpr uintptr_t F_QUESTSYSTEM_FIND_VMA = 0x122914;       // int (int32_t) 按 questId 找任务槽索引（槽数组 [0x2f4000+0x3d0] 步长 12B +0 questId u16；未找到返回 -1）
constexpr uintptr_t F_QUESTSYSTEM_REMOVE_SLOT_VMA = 0x1229a4;  // int (int32_t) 删除任务槽（CopySlot 前移 + QUEST_Initialize 末槽清空 + 槽数-1；返回 1 成功）
constexpr uintptr_t F_QUESTSYSTEM_IS_COMPLETE_VMA = 0x122cc8;        // int (int32_t) 任务目标是否达成（纯读；type0 恒真/type1-2 物品数量/type3-4 槽进度/type5-7 恒假）
constexpr uintptr_t F_QUESTSYSTEM_CHANGE_QUEST_STATE_VMA = 0x123bb4; // int (int32_t questId, int32_t newState) 任务状态迁移（唯一运行时写入者；state 分派表 0x24b760）
constexpr uintptr_t F_SAVE_VMA = 0x129600;                // int (void) 完整静默保存（内部校验 SV_GoldGet/StatPoint/SkillPoint → KEY_ResetActive → 细分 SaveInformation/Player/CharacterAll/Inventory/Quest/Event/ETC 序列化；校验失败返回 0）
constexpr uintptr_t F_GAMESTATE_SET_STATE_VMA = 0x151590;  // void (int32_t) 游戏状态机切换（STATE_nState：4=主菜单 5=world；state==4 分支 GAME_Exit + STATE_Set(4) + Enter 回调）
// ---- 退出存档回调发起点：GAMESTATE_SetState state==4 分支的 bl GAME_Exit（save-exit-callback）----
// GAMESTATE_SetState 内 state==4 分支跳转表落点 +0xb0 的 `bl GAME_Exit`（原字 0x97febb3d）；
// GAME_Exit 在 .text 内仅此一个调用点，故仅覆盖 world -> 主菜单（不含退档到选角/杀进程）。
constexpr uintptr_t F_GAME_EXIT_VMA = 0x100334;                    // void GAME_Exit() 退出当前存档（卸载系统/回主菜单）
constexpr size_t F_GAMESTATE_SET_STATE_GAME_EXIT_CALL_OFF = 0xb0;  // GAMESTATE_SetState 内 state==4 分支 bl GAME_Exit
using GameExitFn = void (*)();                                     // void GAME_Exit()
constexpr uintptr_t F_SAVE_GET_SAVE_SLOT_VMA = 0x1289e4;    // void* (int32_t) 存档槽结构指针（[0x2f5000+0xe40] + slot×0x1d；slot>2 返 0）。槽结构：b0=存在标志 b2=槽标志 +0x1c=角色类型
constexpr uintptr_t F_UI_SET_POPUP_PROCESS_INFO_VMA = 0xaecc8;  // int (int32_t id, int32_t data) 注册 popup 流程（Array_Add 到 popup 数组 [0x2f5000+0xc38]）
constexpr uintptr_t F_GAME_START_RESUME_GAME_VMA = 0x1002e8;  // int (int32_t slot) 启动游戏读档（GAME_Initialize → [0x2f6000+0xd20]=slot → STATE_Set(5) → MAPCHANGE_Set → GAMESTATE_SetState(3) → 主循环读档进 world）
constexpr uintptr_t F_SAVE_CREATE_SAVE_SLOT_VMA = 0x129b38;    // void (void) 初始化全部 3 槽（循环 SAVESLOT_Initialize + SAVE_LoadSaveSlot 加载存档到槽区）
constexpr uintptr_t F_SAVE_LOAD_SAVE_SLOT_VMA = 0x1298dc;      // int (int32_t, void*) 按槽加载单个存档到 SAVESLOT 结构
constexpr uintptr_t F_SAVESLOT_GET_HERO_VMA = 0x14cda4;       // void* (void*) 取主控角色指针（[slot+0x1c] 索引 → [slot+0x4+idx*8]）
constexpr uintptr_t F_STATE_SET_VMA = 0xd46a8;                // void (int32_t) 写状态机 state（*[0x2f5000+0xf8] = state；STATE_NextStartProcess 驱动 enter 回调）
// ---- 逻辑相位帧锚点：MainProcess 内 bl STATE_NextStartProcess（frame-dispatch-host 阶段 2）----
// MainProcess@0xd4984（.dynsym 导出）内 +0x40 的 `bl STATE_NextStartProcess`（原字 0x97ffff3d）。
// wrapper 先派发 kFramePointLogicPre，再复刻原调用；消费点位于帧计数自增与 Draw 之前，
// 状态切换会在旧态 Process/Draw 空指针门（GAMESTATE_SetState 清函数指针）保护下安全落地。
constexpr uintptr_t F_MAINPROCESS_VMA = 0xd4984;                          // void MainProcess()（游戏逻辑帧根，GLThread）
constexpr size_t F_MAINPROCESS_NEXT_STATE_CALL_OFF = 0x40;                // 内 bl STATE_NextStartProcess（字 0x97ffff3d）
constexpr uintptr_t F_STATE_NEXT_START_PROCESS_VMA = 0xd46b8;             // void STATE_NextStartProcess()
constexpr uintptr_t F_GAME_EXIT_SAVE_SLOT_SELECT_CHAR_VMA = 0x10013c;  // void (void) 点空槽进选角（GAME_Initialize + MAP_Load(6) + MAINMENU_CreateSelectCharList；SaveSlot_GoToNewGame 调用）
constexpr uintptr_t F_SELECT_CHARACTER_START_GAME_VMA = 0x14de98;      // void (void) 选角确认开始（[0x2f5000+0xa00]=选中职业 + STATE_Set(5) + UI_SetPopupProcessInfo(4,0) + Flurry 统计；SelectCharacter_ButtonStartExe 调用）
constexpr uintptr_t F_TUTORIAL_START_VMA = 0x16ceb0;          // void (void) 新档教学初始化（重置 10 处教学标志 + 教学事件数组 [0x2f4000+0xce0]×5=0x63）
constexpr uintptr_t F_SAVE_GET_SAVE_FILE_NAME_VMA = 0x125d08;  // void (int32_t slot, char* out) 取存档文件名到 out（SaveSlot_GoToNewGame 删档用）
constexpr uintptr_t F_CS_FS_REMOVE_VMA = 0x1b27bc;            // int (char* path, int32_t) 删除文件（SaveSlot_GoToNewGame 删旧档）
constexpr uintptr_t F_SAVESLOT_DELETE_VMA = 0x14c4e8;          // void (int32_t slot) 删档确认回调：SAVE_GetSaveFileName + CS_fsRemove 删 dat + SAVE_DestroySaveSlot/SAVE_CreateSaveSlot
constexpr uintptr_t G_SAVESLOT_DELETE_GOT_VMA = 0x2f3fa8;      // .got 槽（RELRO 只读）：SaveSlot_SlotButtonDelExe 经此取删档 OK 回调，值 = SaveSlot_Delete

// ---- 进入存档（读档/新档）发起点：按钮 ExecuteProc 内调用点（save-enter-callback）----
// 读档：SaveSlot_SlotButtonExe@0x14cd08 内 +0x88 的 `bl GAME_StartResumeGame`（原字 0x97fecd56）。
// 新档：SelectCharacter_ButtonStartExe@0x14dee0 内 +0x10 的 `bl SelectCharacter_StartGame`（原字 0x97ffffea）。
constexpr uintptr_t F_SAVESLOT_SLOT_BUTTON_EXE_VMA = 0x14cd08;              // void SaveSlot_SlotButtonExe(...)（读档按钮）
constexpr size_t F_SAVESLOT_SLOT_BUTTON_EXE_RESUME_CALL_OFF = 0x88;         // 内 bl GAME_StartResumeGame（字 0x97fecd56）
constexpr uintptr_t F_SELECTCHAR_BUTTON_START_EXE_VMA = 0x14dee0;          // void SelectCharacter_ButtonStartExe()（新档确认）
constexpr size_t F_SELECTCHAR_BUTTON_START_EXE_STARTGAME_CALL_OFF = 0x10;  // 内 bl SelectCharacter_StartGame（字 0x97ffffea）

// ---- 存档文件加解密链 VMA（save-export 存档管理器，overhaul v1.3.2 逆向）----
constexpr uintptr_t F_HUBSAVE_GET_KEY_VMA = 0x9001c;    // const char* () 存档加密密钥字符串指针（本机 "1234567"）
constexpr uintptr_t F_SAVE_LOAD_DATA_VMA = 0x129260;    // int (int32_t slot, void** outBuf, int* outLen) 读槽明文：成功返回 1；outBuf 为 MEM_Malloc 明文缓冲（须 MEM_Free），outLen = 文件长度-3（明文长度）
constexpr uintptr_t F_MEM_FREE_VMA = 0xa8f18;           // void (void*) 游戏堆释放（与 MEM_Malloc 同堆，跨堆 free 会崩）
constexpr uintptr_t F_ENCRYPT_PROCESS2_VMA = 0xa413c;   // int (void* buf, int len, int mode, const char* key) 就地加解密：mode==0 加密（写入 len+3 字节，缓冲区需 len+3 容量）、mode!=0 解密；成功返回 1
constexpr uintptr_t F_NPCSYSTEM_CHECK_FUNCTION_DISPLAY_VMA = 0x11e760; // int (int32_t funcDisplay) 判断 NPC 功能显示类型（读 npc+0xa u16）：0=普通功能弹 UI、1=任务交付/接取直接执行、2=不可交互
constexpr uintptr_t F_UINPC_INIT_VMA = 0xc2cfc;              // u8 (void) NPC 交互触发（UINpc_InitNPC：建 NPCBOX+任务列表+功能列表；前置 PLAYER_pNearNPC 已设）
constexpr uintptr_t F_UINPC_EXE_CURRENT_TASK_VMA = 0xc3070;  // void (void) 执行当前选中任务（slot=GetSlot(nIndex)→SetSelectedTask→ExeNpcTask 跳转表）
constexpr uintptr_t F_NPCTASKLIST_MAKE_DLG_VMA = 0x11e6a4;   // char* (void) 对话下一句（按 slot type 读 desc 表文本 ID → MEMORYTEXT）
constexpr uintptr_t F_PLAYER_DO_CHECK_NEAR_NPC_VMA = 0x120d14; // void (void) 检查附近 NPC（设 PLAYER_pNearNPC=0x728fb8，type==1 非队员距离<0x18）
constexpr uintptr_t F_EVTSYSTEM_DO_CHECK_ALL_EVENT_VMA = 0xfb2a8; // void (int32_t) 遍历所有未激活事件检查触发条件（攻击/交互键链：GAMESTATE_PressKeyPlay 0x9d2e4 分支，参数=2 交互检查模式；条件满足→SetReady 激活事件，路障/NPC 交互入口）
constexpr uintptr_t F_EVT_SET_STATE_VMA = 0xfab38;        // void (int32_t) 剧情状态设置（EVTSYSTEM_SetState，0=退出剧情）
constexpr uintptr_t F_EVENT_BUTTON_OK_EXE_VMA = 0x9c4ac;   // int (void) 剧情确认按钮（Event_ButtonOKExe：读 [0x2f4000+0xf0]→[obj+0x10] 键码→EVTSYSTEM_PressKey）
constexpr uintptr_t F_EVENT_BUTTON_SKIP_EXE_VMA = 0x9c488; // int (void) 剧情跳过按钮（Event_ButtonSkipExe：读 [0x2f4000+0xf0]→[obj+0x40] 键码→EVTSYSTEM_PressKey→SetState(7)+DestroyType(2)）
constexpr uintptr_t F_UINPC_QUEST_BUTTON_OK_EXE_VMA = 0xc3414; // int (void) NPC 任务完成按钮（UINpcQuest_ButtonOKExe：读 questIdx [0x2f3000+0x240] ldrsh→stateTbl[questIdx]==2 完成分支：UI_SetPopupProcessInfo(3,0)+QUESTSYSTEM_ChangeQuestState(id,3)+EVTSYSTEM_DoCheckAllEvent(id)；==0 接任务、==1 仅关面板）
constexpr uintptr_t F_TEXTCTRL2_MOVE_NEXT_PAGE_VMA = 0x13d3c0; // void (void* ctrl) 文本控件翻下一页（当前页+1<总页才动，否则无操作；调后重置 +0x2e 推进标志）
constexpr uintptr_t F_KEY_SET_CODE_VMA = 0x10f7f4;        // void (int32_t code) 注入按键码（KEY_SetCode：写 [0x2f4000+0x50] 指向的当前键码）
constexpr uintptr_t F_CHAR_GET_SKILL_USAGE_VMA = 0xe496c;    // int (void*) 战斗 AI 技能总开关（读 [ch+0x3a0] bit0-2）
constexpr uintptr_t F_CHAR_SET_SKILL_USAGE_VMA = 0xe4cc0;    // void (void*, int) 写 [ch+0x3a0] bit0-2（AI 技能开关 0-7）
constexpr uintptr_t F_GET_NAME_VMA = 0xd9c54;         // char* (void*) 角色名称（UTF-8 字符串）
constexpr uintptr_t F_GET_ACT_MAX_LEVEL_VMA = 0xe9560; // int (void*, int) 技能最大等级（表1 +0x1D → 表2 偏移 → [ch+0x2B2] bit1-4，v0.5.1 实机验证）
constexpr uintptr_t F_SET_ACT_MAX_LEVEL_VMA = 0xe9614; // int (void*, int, int) 写 [ch+0x2B2+偏移] bit1-4（技能书提升路径，v0.5.4 反汇编确认）
constexpr uintptr_t F_FIND_MERC_SLOT_VMA = 0xf4254;   // void* (int) 按佣兵槽找角色（CHARSYSTEM_FindAsMercenarySlot）
constexpr uintptr_t F_SEARCH_PATH_VMA = 0xdb094;      // int (void*, int, int, int) 角色寻路（CHAR_SearchPath：目标像素+flag）
constexpr uintptr_t F_CHAR_GET_BLOCK_VMA = 0xddaac;    // int (void*) 单位是否阻挡（CHARSYSTEM_GetCharacterBlock，ret=2 阻挡；BFS 阻挡判定参考，nav 单位占用过滤）
constexpr uintptr_t F_CHAR_GET_AREA_RECT_VMA = 0xdd584; // void (void*, int, int, int16_t[4]) 单位碰撞矩形（CHAR_GetAreaRect：(obj, x, y, rect) 输出绝对像素 rect=[min_x,min_y,max_x,max_y]，偏移来自矩形表A/B，索引 obj+0x3ce）

// ---- 写操作函数 VMA（2026-08-05 objdump 逆向确认，见 docs/notes/control-capability.md §5）----
constexpr uintptr_t F_SET_MONEY_VMA = 0x10449c;        // void (int64) 设金币
constexpr uintptr_t F_ADD_MONEY_VMA = 0x1044e4;        // int (int64) 加金币（溢出返回 0）
constexpr uintptr_t F_MINUS_MONEY_VMA = 0x104780;      // int (int64) 减金币（不足返回 0）
constexpr uintptr_t F_FIND_ITEM_VMA = 0x10438c;        // void* (int32) INVEN_FindItem 按类别查找原版物品
constexpr uintptr_t F_INVEN_HAVE_ITEM_VMA = 0x104870; // int (int32) INVEN_HaveItem 原版物理背包是否有类别
constexpr uintptr_t F_INVEN_GET_ITEM_COUNT_VMA = 0x104260; // int (int32) INVEN_GetItemCount 原版类别数量
constexpr uintptr_t F_INVEN_FIND_ITEM_SLOT_VMA = 0x103704; // int (void*, int8_t*) INVEN_FindItemSlot
constexpr uintptr_t F_REMOVE_ITEM_VMA = 0x104044;      // int (void*) INVEN_RemoveItem 按 item 指针删（内部 FindItemSlot+RemoveItemDirect）
constexpr uintptr_t F_ITEM_GET_PRICE_VMA = 0x109f50;   // int (void*) ITEM_GetPrice 读静态表价格（item+8 字段 + ITEM_GetAbilityLevel）
constexpr uintptr_t F_ITEM_GET_SELL_PRICE_VMA = 0x10a500; // int (void*) ITEM_GetSellPrice 原版最终出售价格
constexpr uintptr_t F_ITEM_GET_ABILITY_LEVEL_VMA = 0x1091f4; // int (void*) ITEM_GetAbilityLevel 能力等级（所需等级）：非损坏 → ITEMSYSTEM_GetAbilityLevel(category)（ITEMCLASSBASE 记录+3 int8）；损坏 → 直读同偏移（0x1091f4 反汇编）
// ---- 属性显示范围（attribute-range-display）----
constexpr uintptr_t F_ITEMSYSTEM_GET_OPTION_VALUE_VMA = 0x109020;      // int (int optionIndex, int level, int flag, void* item) 词缀值：CAL 公式 + 修正后在 [base/2, base] 掷一次随机（0x109020 反汇编）
constexpr uintptr_t F_ITEMSYSTEM_GET_JEWEL_OPTION_VALUE_VMA = 0x108f90; // int (int type, void* item) 宝石值：按 (类别,类型) 算 X 后在 [X, 2X] 掷一次随机（0x108f90 反汇编）
constexpr uintptr_t F_MATH_GET_RANDOM_VMA = 0xa8bcc;                   // int (int min, int max) 闭区间随机数（0xa8bcc 反汇编）
constexpr uintptr_t F_UIDESC_ADD_OPTION_VMA = 0xb343c;                 // void (void* builder, int type, int optIdx, int value) 详情选项行：写 "$<码>…$B" 内联颜色串（0xb343c 反汇编）
constexpr uintptr_t F_UIDESC_MAKE_ITEM_VMA = 0xb36a0;                  // 物品详情构造入口；x0=item（0xb36a0 反汇编）
constexpr uintptr_t G_UIDESC_TEXT_BUF_VMA = 0x303dc0;                  // UIDesc 详情文本缓冲基址（0xb36a0 起始 memset 目标，容量 0x200）
constexpr size_t G_UIDESC_TEXT_BUF_SIZE = 0x200;                       // 详情文本缓冲容量（0xb36a4 mov x2,#0x200）
constexpr uintptr_t F_ITEM_GET_BUY_PRICE_VMA = 0x10a200;  // int (void*) 买入价（ITEM_GetPrice + MERCENARYGROUPSKILLSYSTEM 折扣系数）
  constexpr uintptr_t F_INVEN_FIND_SAVE_SLOT_VMA = 0x103960;  // int (void*, int8_t*) 查找空槽并写入物理槽编码
constexpr uintptr_t F_INVEN_SAVE_ITEM_VMA = 0x104528;   // int (void*, void*) 物品存入背包槽
constexpr uintptr_t F_DEALSYSTEM_FIND_SALE_BY_ID_VMA = 0xf636c;  // void* (void*) 按物品类别找商店商品槽（遍历 saleList 到 +0x300 步长 0x10）
constexpr uintptr_t F_INVEN_MOVE_ITEM_VMA = 0x104934;  // int (void*,int,int,int) INVEN_MoveItem 物品移动/堆叠合并（item+count+targetBag+targetSlot）
constexpr uintptr_t F_SET_EXP_VMA = 0xd9b5c;           // void (void*, int32) 设经验
constexpr uintptr_t F_ADD_EXP_VMA = 0xe7028;           // int (void*, int32, u8) 加经验（走升级判定链）
constexpr uintptr_t F_SET_STATUS_POINT_VMA = 0xd9c4c;  // void (void*, int32) 设能力点（写 +0x32a）
constexpr uintptr_t F_SET_AUTO_ATTACK_VMA = 0xe4cf4;   // void (void*, int32) 自动攻击开关
constexpr uintptr_t F_EQUIP_ITEM_VMA = 0xe51c0;        // int (void*, void*) 穿装备（自动找槽，槽占用返回 0）
constexpr uintptr_t F_EQUIP_ITEM_FROM_INVEN_TO_SLOT_VMA = 0xe5368; // int (void*,int,int,int) 原版装备交换入口
constexpr uintptr_t F_UNEQUIP_VMA = 0xe2f68;           // int (void*, int32) 脱装备槽→背包
constexpr uintptr_t F_SET_EQUIP_ITEM_VMA = 0xe2e8c;    // void (void*, int32, void*) 写装备槽指针（CHAR_EquipItemFromInvenToSlot e555c 调用；传 nullptr 清槽）
constexpr uintptr_t F_CAN_EQUIP_VMA = 0xe4eb4;         // int (void*, void*) 可否装备
constexpr uintptr_t F_FIND_EQUIP_SLOT_VMA = 0xe4fd0;   // int (void*, void*) 计算目标装备槽（-1=不可装备）
constexpr uintptr_t F_GET_EQUIP_ITEM_VMA = 0xda20c;    // void* (void*, int32) 读指定装备槽物品指针
constexpr uintptr_t F_IS_SPECIAL_NPC_VMA = 0xe4d90;    // int (void*) 是否任务特殊 NPC（type==2 且表 bit2）
constexpr uintptr_t F_LEARN_ACTION_VMA = 0xe2390;      // void* (void*, int32, int32) 学习/升级技能
constexpr uintptr_t F_SET_ACTIVE_PLAYER_VMA = 0x11f584; // int (int32) 切换主控角色
constexpr uintptr_t F_PARTY_SWAP_VMA = 0x11ff5c;       // void (int32, int32) 交换队伍槽
constexpr uintptr_t F_SET_POSITION_VMA = 0x12aa14;     // void (int32, int32) 全队传送（写 +0x2/+0x4）
constexpr uintptr_t F_CHANGE_MAP_VMA = 0x114fc4;       // void (int32, int32, int32, int32) 切图（mapId,x,y,dir）

// ---- 合法操作函数 VMA（v0.3.1，玩家游戏内可做的事，见 control-capability.md §5.1）----
constexpr uintptr_t F_MOVE_AS_PATH_VMA = 0xe9db8;      // int (void*) 沿已存路径移动（读 +0x2f0 PATHLIST）
constexpr uintptr_t F_CHAR_MOVE_VMA = 0xe9808;         // int (void*, int, int*, u8) 方向键移动（mode 0-3=上/下/右/左，delta 像素/帧，flag 方向键状态）
constexpr uintptr_t F_CHAR_PICK_ITEM_ALL_VMA = 0xec4d8; // int (void*, int32_t) 拾取范围内所有掉落物（CHAR_PickItemAll，官方移动按键链 GAMESTATE_PressKeyPlay 0x9d10c 以半径 0x18=24px 调用；后台线程调用会触发拾取音效 SOUNDSYSTEM_Play 空句柄崩溃，模块改用 nav_pick_items 复刻数据路径跳过音效）
constexpr uintptr_t F_MEM_MALLOC_VMA = 0xa8d94;         // void* (size_t) 游戏堆分配（节点须用游戏堆，回调 CHAR_ActivePickupEvent 内部 MEM_Free 释放，跨堆 free 会崩）
constexpr uintptr_t F_NOTIFIER_ADD_VMA = 0x11e1b8;     // void (int type, int seq, void* callback, void* data) 延迟回调入队（NOTIFIER_Add→Create+AddTail，主线程 NOTIFIER_Process 回调；type=1 拾取，seq=0/2/4 递增）
constexpr uintptr_t G_NOTIFIER_PICKUP_SLOT_VMA = 0x2f5000 + 0x948; // GOT 槽：*(此地址) = CHAR_ActivePickupEvent(0xdd15c) 回调函数指针（PickItemAll 0xec678 ldr 后作 NOTIFIER_Add 的 callback 参数）
constexpr uintptr_t G_DROP_COUNT_GOT_VMA = 0x2f5000 + 0x818; // GOT 槽：*(此地址) = 掉落物计数变量（int8，PickItemAll 0xec4fc ldr）
constexpr uintptr_t G_DROP_ARRAY_GOT_VMA = 0x2f6000 + 0x560; // GOT 槽：*(此地址) = 掉落物数组变量，** = 数组基址（0x20 步长，+0x0 掉落物对象指针 +0x8 x +0xa y +0x18 标志）
constexpr uintptr_t F_CHAR_SET_DIRECTION_VMA = 0xdc548; // void (void*, int dir) 设置朝向（写 [ch+0x6]=dir，dir 0-3 写 [ch+0x7]=subdir；CHAR_Move 不更新朝向，移动前须调此函数）
constexpr uintptr_t F_CHAR_REMOVE_PATH_VMA = 0xdb064;  // void (void*) 清除已存路径（打断移动）
constexpr uintptr_t F_MAP_SET_FOCUS_VMA = 0x11336c;    // void (int32 x, int32 y) 像素坐标；写焦点 + MAP_SetDisplayInformation 转 4 个滚动偏移（摄像机=MAP Focus 体系）
constexpr uintptr_t F_GAMEPLAY_GO_MAP_LINK_BY_CHAR_VMA = 0x9cdc0;  // int (void* ch, int32 tile_x, int32 tile_y) 按角色触发出口检测→MAPCHANGE_Set→切图状态机
constexpr uintptr_t F_CHAR_SET_TARGET_VMA = 0xdc754;    // void (void*, void*) 设置攻击目标（写 [ch+0x278]）
constexpr uintptr_t F_CHAR_STOP_COMBAT_VMA = 0xe7c24;   // void (void*) 停止战斗（清战斗标志+移除仇恨+动作复位）
constexpr uintptr_t F_CONSUME_ITEM_VMA = 0x1047bc;     // void (void*) 消耗 1 个（使用药水/卷轴）
constexpr uintptr_t F_CHAR_USE_ITEM_EX_VMA = 0xeb670;  // void (void* ch, void* item, int flag) 物品效果分派核心
constexpr uintptr_t F_CHAR_PROCESS_SHORTCUT_VMA = 0xec028; // int (void* ch, int shortcut) 快捷栏处理
constexpr uintptr_t F_REMOVE_ITEM_DIRECT_VMA = 0x103fd8; // int (int32 bag, int32 slot) 按槽删物品；返回值不可信（v0.6.15 真机实测：删除成功仍返回 0），成功判定必须用删后槽位读取验证（与 data_op_discard_item 一致）
constexpr uintptr_t F_INCLUDE_PARTY_VMA = 0x118e04;    // int (void*) 佣兵入队（内部校验）
constexpr uintptr_t F_EXCLUDE_PARTY_VMA = 0x118d0c;    // int (void*) 佣兵离队
constexpr uintptr_t F_MERCENARY_RELEASE_VMA = 0x118ab4; // void (int) 佣兵遣散：FindAsMercenarySlot→清 +0x352/-0x3cc 标志→MERCENARYSLOT_Initialize→GAMESTATE_SetState
constexpr uintptr_t F_ITEMDATA_IS_USE_VMA = 0x1058ac;  // int (int32 itemId) 物品是否可使用（ITEMDATABASE_IsUse，读表 +2 u8 ∈ {0x16,0x17} 可消耗）
constexpr uintptr_t F_ITEMDATABASE_IS_NO_SELL_VMA = 0x105864; // int (int32 itemId) ITEMDATABASE_IsNoSell
constexpr uintptr_t F_POPUPSTATE_EXIST_VMA = 0x1223f8; // int () 弹窗栈是否有激活状态（readelf 符号表）
constexpr uintptr_t F_BUTTON_OK_EXE_VMA = 0xca9d8;      // void () 弹窗确定按钮执行（bOn=0 + 调 fpOK(param)；无参直接调用，v0.3.11 frida 验证）
constexpr uintptr_t F_BUTTON_CANCEL_EXE_VMA = 0xcaa78;  // void () 弹窗取消按钮执行（bOn=0 + 调 fpCancel(param) 或 Free；无参直接调用）
constexpr uintptr_t F_TUTORIAL_GETSTATE_VMA = 0x16de40; // int () 教学状态轮转（CHAR_ProcessShortcut 0xec340 教学完成链：读 [0x2f4000+0xce0] 对象 5 槽左移 + 写 0x63 到 [+0x28]，返回旧首槽值）
constexpr uintptr_t F_NETWORKSTORE_SET_STATE_VMA = 0x15b0c4;  // void (int) 写 NetworkStore state（0 复位）——反汇编证实 SetState@0x15b0c4，0x15b0d0 是 GetState（只读）
constexpr uintptr_t F_OPEN_ITEM_BOX_VMA = 0x10e970;   // int (int32_t category) 开箱（按表权重随机出物品，内部调 INVEN_SaveItem）
constexpr uintptr_t F_RELEASE_SEALED_VMA = 0x10af4c;  // int (int32_t category) 解封（类别需在 0x3a6-0x3ab）
constexpr uintptr_t F_IS_DICE_VMA = 0x10be60;         // int (int32_t category) 是否骰子（类别 ∈[0x34,0x38]）
constexpr uintptr_t F_STATUSDICE_ROLL_VMA = 0x138338; // int (int32_t charIdx, int32_t type) 掷骰：纯表驱动计算写 pending[0..4]（charIdx=[ch+0xd] 0-5 职业索引，type=category-0x34 0-4；不读 UI，同步调用安全）
constexpr uintptr_t F_IS_SEALED_VMA = 0x10be50;       // int (int32_t category) 是否可解封（类别 ∈[0x3a6,0x3ab]，与 ReleaseSealed 内联判定一致）
constexpr uintptr_t F_IS_ITEMBOX_VMA = 0x10cda0;     // int (int32_t category) 是否开箱类（类别 ∈[0x3ef,0x3f1]，UIEquip_SetDescMenu 开箱按钮判定）
constexpr uintptr_t F_MAKE_ITEM_VMA = 0x10c6c8;      // void* (int32_t category, int32_t lookup_key, int32_t flag) ITEMSYSTEM_MakeItem 创建物品对象；arg2 是静态表查找/品质参数非数量（产物量=0x10ca3c CAL 公式，域 ≤99）
constexpr uintptr_t F_CREATE_ITEM_VMA = 0x10be9c;    // void* (int32_t category, int32_t, int32_t, int32_t) ITEMSYSTEM_CreateItem 创建物品对象（无 search_tbl 校验，OP 直调可靠）
// ---- 堆叠上限 patch 点所在函数 VMA（stack-limit-999，v0.5.18；符号名见 libgame-symbols.txt）----
constexpr uintptr_t F_ITEMSYSTEM_DIVIDE_VMA = 0x1083f8;       // ITEMSYSTEM_Divide 拆堆
constexpr uintptr_t F_INVEN_SAVE_ITEM_DIRECT_VMA = 0x103bf0;  // INVEN_SaveItemDirect 存入堆叠
constexpr uintptr_t F_INVEN_SAVE_ITEM_ON_EMPTY_VMA = 0x104be0;  // int (void* item, int bag) INVEN_SaveItemOnEmpty：目标袋内找空槽后 SaveItemDirect；成功路径返回值为 1，失败分支返回语义待静态闭合
constexpr uintptr_t F_INVEN_SAVE_ITEM_DATA_VMA = 0x104614;    // int (int32_t category, int32_t count) INVEN_SaveItemData 批量创建并保存物品
constexpr uintptr_t F_INVEN_CHECK_SAVE_IN_NOT_EMPTY_SLOT_VMA = 0x103d78; // INVEN_CheckSaveInNotEmptySlot 槽检查
constexpr uintptr_t F_ITEM_IS_REAL_EQUIP_VMA = 0x105ab8;      // int (void*) ITEM_IsRealEquip，装备 marker 判定（非数量 patch 点）
constexpr uintptr_t F_ITEM_IS_REAL_BROKEN_VMA = 0x105b78;     // int (void*) ITEM_IsRealBroken，损坏装备 marker 判定（非数量 patch 点）
constexpr uintptr_t F_UI_POPUP_MSG_CREATE_OK_FROM_TEXT_DATA_VMA = 0xca778;  // UIPopupMsg_CreateOKFromTextData
constexpr uintptr_t F_INVEN_REMOVE_ITEM_DATA_VMA = 0x1040a8;  // int (int32_t category, int32_t count) INVEN_RemoveItemData 按类别/数量删除物理库存数据
constexpr uintptr_t F_INVEN_GET_CUMULATE_SAVE_SLOT_EX_VMA = 0x1051b0; // INVEN_GetCumulateSaveSlotEx 找堆叠槽
constexpr uintptr_t F_INVEN_CALCULATE_EMPTY_SLOT_COUNT_FOR_SAVE_VMA = 0x103f10; // int (int,int) INVEN_CalculateEmptySlotCountForSave
constexpr uintptr_t F_INVEN_GET_EMPTY_SAVE_SLOT_EX_VMA = 0x105070; // int (int,int,int8_t*,int,int*) INVEN_GetEmptySaveSlotEx
constexpr uintptr_t F_INVEN_GET_NEEDED_SAVE_SLOT_EX_VMA = 0x105440; // int (int,int,int8_t*,int,int*,int8_t*,int) INVEN_GetNeededSaveSlotEx
constexpr uintptr_t F_UISTORE_BUTTON_SELL_EXE_VMA = 0xd1818;  // UIStore_ButtonSellExe 商店卖出按钮
constexpr uintptr_t F_UISTORE_SELL_ITEM_VMA = 0xd25f0;        // UIStore_SellItem 商店卖出
constexpr uintptr_t F_GAME_START_NEW_GAME_VMA = 0x10017c;     // GAME_StartNewGame 新游戏初始数量
constexpr uintptr_t F_ITEMSYSTEM_PROCESS_UNPACK_VMA = 0x10ce50; // ITEMSYSTEM_ProcessUnpack 拆包
constexpr uintptr_t F_MAPITEMSYSTEM_CREATE_ITEM_VMA = 0x116e10; // MAPITEMSYSTEM_CreateItem 地图掉落
constexpr uintptr_t F_NETWORKSTORE_ADD_ITEM_VMA = 0x15d640;   // NetworkStore_AddItem 网络商店
constexpr uintptr_t F_SAVE_REVISE_CHARACTER_LOCATION_VMA = 0x125fbc; // SAVE_ReviseCharacterLocation 读数量
constexpr uintptr_t F_UIMIX_START_MIX_VMA = 0xc0870;          // UIMix_StartMix 合成产物数量
constexpr uintptr_t F_UIMIX_REFRESH_INVEN_ITEM_VMA = 0xc04fc; // void () UIMix_RefreshInvenItem：按当前袋号刷新 16 格物品网格
constexpr uintptr_t F_UIMIX_DRAW_INVEN_BAG_GROUP_VMA = 0xc13ec; // void () UIMix_DrawInvenBagGroup：画 6 袋按钮并按当前袋 GOT 高亮（R-57 遮蔽点）
constexpr uintptr_t F_UIMIX_DRAW_INVEN_BAG_GROUP_CALL_VMA = 0xc1a74; // UIMix_Draw 内 bl UIMix_DrawInvenBagGroup 调用点（原字 0x97fffe5e，BL patch 挂遮蔽 wrapper）
// ---- UIMix 宝石合成操作优化（gem-craft-optimization 阶段1）----
// 调用链终点挂钩：进入视图（菜单/配方按钮 ExecuteProc 尾部写 [+0x128]=-1）、放料
// （desc 按钮 ExecuteProc 尾部 SetItem）、合成清空（UIMix_StartMix 内 bl UIMix_ResetStuffItemControl）。
constexpr uintptr_t F_UIMIX_GET_TYPE_VMA = 0xbf47c;   // int () UIMix_GetType：返回 [0x305588] u8 合成类型（readelf .dynsym 核对）
constexpr uintptr_t F_UIMIX_BUTTON_INVEN_ITEM_SELECT_EXE_VMA = 0xc2328; // void (void*) UIMix_ButtonInvenItemSelectExe：desc 放入按钮 ExecuteProc，type 1 放料校验/放入（readelf .dynsym 核对）
constexpr uintptr_t F_UIMIX_BUTTON_MENU_LIST_EXE_VMA = 0xc05c0; // void (void*) UIMix_ButtonMenuListExe：类型/菜单按钮 ExecuteProc，尾部 0xc0700 str [+0x128]=-1（readelf .dynsym 核对）
constexpr uintptr_t F_UIMIX_BUTTON_RECIPE_EXE_VMA = 0xc0390;   // void (void*) UIMix_ButtonRecipeExe：配方按钮 ExecuteProc，尾部 0xc03e8 str [+0x128]=-1（readelf .dynsym 核对）
constexpr uintptr_t F_UIMIX_BUTTON_MIXING_EXE_VMA = 0xc21ec;   // void (void*) UIMix_ButtonMixingExe：合成按钮 ExecuteProc，type1 读 [+0xf8] 费用判金币后弹 YesNo 确认（readelf .dynsym 核对）
constexpr uintptr_t F_UIMIX_INIT_MIXING_STATE_VMA = 0xc0038;   // void () UIMix_InitMixingState：依 mixType 重算 stuffList 与费用 [+0xf8]（不清填入格；readelf .dynsym 核对）
constexpr uintptr_t F_UIMIX_RESET_STUFF_ITEM_CONTROL_VMA = 0xc0240; // void () UIMix_ResetStuffItemControl：清空填入格（readelf .dynsym 核对）
constexpr uintptr_t F_CONTROL_OBJECT_GET_CURSOR_VMA = 0x9ea7c; // void* (void*) ControlObject_GetCursor：父控件返回选中子控件 [+0x70]，否则 0（readelf .dynsym 核对）
constexpr size_t F_UIMIX_START_MIX_RESET_STUFF_CALL_OFF = 0x258; // UIMix_StartMix 内 bl UIMix_ResetStuffItemControl 调用点偏移（0xc0ac8，原字 0x97fffdde）
// 三个按钮 ExecuteProc 初值 GOT 槽（新建按钮即从这些槽读取；R_AARCH64_RELATIVE 核对）。
constexpr uintptr_t G_UIMIX_DESC_EXE_GOT_VMA = 0x2f4598;   // → UIMix_ButtonInvenItemSelectExe(0xc2328)
constexpr uintptr_t G_UIMIX_MENU_EXE_GOT_VMA = 0x2f6658;   // → UIMix_ButtonMenuListExe(0xc05c0)
constexpr uintptr_t G_UIMIX_RECIPE_EXE_GOT_VMA = 0x2f40d0; // → UIMix_ButtonRecipeExe(0xc0390)
constexpr uintptr_t G_UIMIX_CRAFT_EXE_GOT_VMA = 0x2f6d60;  // → UIMix_ButtonMixingExe(0xc21ec)；0xbf7d0 ldr x1,[x1,#0xd60] → ControlButton_Create
constexpr uintptr_t F_SCENE_EVENT_MIX_VMA = 0x14b52c;         // u64 (u64,u64,u64) Scene_Event_POPUP_SC_MIX（合成器 popup state event 回调）
constexpr uintptr_t F_SCENE_DRAW_MIX_GRPX_END_CALL_VMA = 0x14b474; // Scene_Draw_POPUP_SC_MIX 内 bl GRPX_End 调用点（原字 0x97fd0fa8，BL patch 挂 draw_end wrapper）
constexpr uintptr_t F_SAVE_SAVE_INVENTORY_VMA = 0x127d8c;     // SAVE_SaveInventory 存档背包（子物品检查位段）
constexpr uintptr_t F_SAVE_SAVE_INVENTORY_CALLSITE_VMA = 0x129770; // SAVE_Save 内唯一 bl 调用点（BL→门禁 wrapper）
constexpr uintptr_t F_SAVE_CALLSITE_REVIVE_VMA = 0xc4488; // UINpcRevive_Revive_Confirm → SAVE_Save
constexpr uintptr_t F_SAVE_CALLSITE_QUEST_CLEAR_1_VMA = 0xcbea0; // UIQuestMenu_ClearUIInAppProcess → SAVE_Save
constexpr uintptr_t F_SAVE_CALLSITE_QUEST_CLEAR_2_VMA = 0xcbf4c; // UIQuestMenu_ClearUIInAppProcess → SAVE_Save
constexpr uintptr_t F_SAVE_CALLSITE_QUEST_REVIEW_VMA = 0x125c88; // QUESTSYSTEM_AcceptReivew → SAVE_Save
constexpr uintptr_t F_SAVE_CALLSITE_PROCESS_VMA = 0x129850; // SAVE_ProcessSave → SAVE_Save
constexpr uintptr_t F_SAVE_CALLSITE_NETWORK_ADD_VMA = 0x15d708; // NetworkStore_AddItem → SAVE_Save
constexpr uintptr_t F_SAVE_CALLSITE_NETWORK_PROCESS_1_VMA = 0x15da84; // NetworkStore_Process → SAVE_Save
constexpr uintptr_t F_SAVE_CALLSITE_NETWORK_PROCESS_2_VMA = 0x15dcf0; // NetworkStore_Process → SAVE_Save
constexpr uintptr_t F_SAVE_ITEM_ON_EMPTY_CALL_VMA = 0xb8cc0; // bl INVEN_SaveItemOnEmpty(0x104be0) 的 callsite（drop gate 门禁 wrapper；原字 0x94012fc8）
constexpr uintptr_t F_ITEM_DRAW_PORTING_CALL_VMA = 0xaaf28; // ControlItem_Draw 内 bl ITEM_DrawPorting 的 callsite（draw gate 门禁 wrapper；原字 0x94016d49）
constexpr uintptr_t F_SAVE_LOAD_INVENTORY_VMA = 0x127ea4;     // SAVE_LoadInventory 读档背包（子物品检查位段）
constexpr uintptr_t F_UIEQUIP_IS_APPLY_STUFF_VMA = 0xb8d4c;
constexpr uintptr_t F_UIEQUIP_APPLY_STUFF_VMA = 0xb8df8; // void (void*, void*) 原版镶嵌/强化并消费材料
constexpr uintptr_t F_UIEQUIP_EQUIP_CONTROL_EVENT_PROC_VMA = 0xb8f7c; // uint64 (control,event,x2,param)
constexpr uintptr_t F_UIEQUIP_GET_ITEM_SLOT_INDEX_VMA = 0xb7910;
constexpr uintptr_t F_UIEQUIP_REFRESH_ITEM_AREA_VMA = 0xb7a00;
constexpr uintptr_t F_UIEQUIP_UPDATE_CHAR_EQUIP_VMA = 0xb7784; // void (void) ButtonEquipExe b7d40 装备后刷新角色装备槽显示
constexpr uintptr_t F_UIEQUIP_REFRESH_BAG_AREA_VMA = 0xb78bc; // void (void) ButtonEquipExe b7e0c 装袋后刷新原版袋槽区域
// ---- 佣兵徽章（英雄徽章）使用链（R-63）----
constexpr uintptr_t F_UIEQUIP_BUTTON_USE_MERCENARY_SEAL_EXE_VMA = 0xb8144; // void (void* button) 佣兵徽章专用使用按钮（UIEquip_SetDescMenu 面板 offset 0x98 挂载；0xb8144→SAVE_IsOK→IsEmptyManagerSlot→MakeMercenary）
constexpr uintptr_t F_ITEMSYSTEM_IS_MERCENARY_SEAL_VMA = 0x10be70; // int (int32 category) category∈42..50 或 928..933 为佣兵徽章
constexpr uintptr_t F_MERCENARYSYSTEM_IS_EMPTY_MANAGER_SLOT_VMA = 0x118b30; // int (void) 佣兵管理器是否有空槽（0x118b30 反汇编：无参，读 G_MERC_MAX_GOT/G_MERC_SLOTLIST_GOT）
constexpr uintptr_t F_MERCENARYSYSTEM_MAKE_MERCENARY_VMA = 0x119658; // void* (void* item) 生成佣兵（唯一调用点 0xb818c；内部 INVEN_RemoveItem 消耗，返回 null=失败）
constexpr uintptr_t F_SOUNDSYSTEM_PLAY_VMA = 0x1377f0;   // void(int16 id) 原版 UI 音效（袋切换=0x11，反汇编 b8c34）
constexpr uintptr_t G_SND_FX_VMA = 0x307850;             // g_sndFx 音效句柄表（判空防崩）
constexpr uintptr_t F_CONTROL_ITEM_SET_ITEM_VMA = 0xaad60;      // (ctrl, item) 控件物品指针（RefreshItemArea b7a64）
constexpr uintptr_t F_CONTROL_OBJECT_SET_SHOW_VMA = 0x9dc28;    // (ctrl, int)（RefreshItemArea b7a44）
// ControlObject_SetActive(0x9dbd8)/GetChild(0x9eacc) 已在上方登记（508/510 行附近）
constexpr uintptr_t F_UIEQUIP_DRAW_VMA = 0xb764c;
constexpr uintptr_t F_UIEQUIP_DRAW_INVEN_ITEM_VMA = 0xb6fac;
constexpr uintptr_t F_UIEQUIP_DRAW_INVEN_BAG_VMA = 0xb7284;
constexpr uintptr_t F_ITEM_DRAW_PORTING_VMA = 0x10644c; // void (item*, x, y, type, flip) 原版物品图标/数量绘制
constexpr uintptr_t F_UIDESC_SET_OFF_VMA = 0xb2b48;
constexpr uintptr_t F_UIDESC_GET_DATA_VMA = 0xb2bd0;  // void* UIDesc_GetData()：当前 desc 面板物品对象（ButtonEquipExe b7c2c 同源）
constexpr uintptr_t F_UIDESC_DRAW_VMA = 0xb56f4;      // void () UIDesc_Draw：物品详情面板绘制（Scene_Draw_POPUP_SC_EQUIP +0x1cc bl）
constexpr uintptr_t F_TOUCHHANDLE_SET_CURSOR_VMA = 0xa3b80;
constexpr uintptr_t F_UIEQUIP_INVEN_ITEM_CONTROL_EVENT_PROC_VMA = 0xb911c;
// ---- 世界传送（world-teleport）----
constexpr uintptr_t F_UICHOICE_BUTTON_LIST_EXE_VMA = 0x0b1a98; // void (void*) UICHOICE 选项按钮 ExecuteProc
constexpr uintptr_t F_UICHOICE_CREATE_CONTROL_VMA = 0x0b2110; // void (void*) UICHOICE 创建选项控件
constexpr uintptr_t F_UICHOICE_PROCESS_VMA = 0x0b2104; // void () UICHOICE 处理函数
constexpr uintptr_t F_UI_CHOICE_INIT_VMA = 0x0b1cd4; // void (void*) UIChoice_Init
constexpr uintptr_t F_SCENE_EVENT_POPUP_SC_CHOICE_VMA = 0x14a79c; // 选择面板事件回调
constexpr uintptr_t F_MAPCHANGE_SET_VMA = 0x09c740; // void (int,int,int,int) MAPCHANGE_Set
constexpr uintptr_t F_MEMORYTEXT_GET_TEXT_VMA = 0x118674; // const char* (uint16_t) MEMORYTEXT_GetText
constexpr uintptr_t F_UI_PLAY_CALL_MAP_NAME_VMA = 0x0c6664; // UIPlay_CallMapName 函数入口（dynsym）
constexpr size_t F_UI_PLAY_CALL_MAP_NAME_PATCH_OFF = 0x64; // 尾部入口 patch：0xc66c8
constexpr size_t F_UI_PLAY_CALL_MAP_NAME_EPILOGUE_OFF = 0x68; // patch 后跳回：0xc66cc
constexpr uintptr_t F_UI_PLAY_CALL_MAP_NAME_LEGACY_TARGET_VMA = 0x0c50f4; // 改版原传送注入块目标
// ---- 合成系统（MIXSYSTEM，craft-batch-ui v0.5.18，libgame-symbols.txt 核对）----
constexpr uintptr_t F_MAKE_MIX_VMA = 0x11af58;       // int (int32_t mixType, void** outItem) MIXSYSTEM_MakeItem：产物生成（词条定向继承由游戏处理），0=成功非 0=失败
constexpr uintptr_t F_USE_STUFF_VMA = 0x11b300;      // void (int32_t mixType, void* stuffList) MIXSYSTEM_UseStuff：遍历材料槽逐条删材料（模块改用 RemoveItemDirect，此处仅登记）
constexpr uintptr_t F_GET_COST_VMA = 0x11ab64;       // int64 (int32_t mixType, void* item) MIXSYSTEM_GetCost：读配方表费用文本 → CAL_Calculate，负数=非法配方
// ---- UIMix 控件回调函数（按钮注入复用，均为 .dynsym 具名符号）----
constexpr uintptr_t F_TOUCH_HANDLE_CONTROL_EVENT_PROC_VMA = 0xa3590;   // TouchHandle_ControlEventProc（ControlObject+0x90 Proc）
constexpr uintptr_t F_TOUCH_HANDLE_RESET_SELECTED_CONTROL_VMA = 0xa3414; // TouchHandle_ResetSelectedControl
constexpr uintptr_t F_TOUCH_HANDLE_RESET_MOVING_CONTROL_VMA = 0xa371c;  // TouchHandle_ResetMovingControl
constexpr uintptr_t F_CONTROL_BUTTON_CONTROL_EVENT_PROC_VMA = 0xaa818; // ControlButton_ControlEventProc（ControlObject+0x98 ControlProc）
constexpr uintptr_t F_UIMIX_BUTTON_DRAW_MIXING_GEM_VMA = 0xbf218;      // UIMix_ButtonDrawMixingGem（按钮 DrawProc，复用原宝石按钮贴图）
// ---- UI 实验控件系统符号（ui-exp v0.6.7，disasm_text.txt 反汇编确认签名）----
// 面板实例槽：UISettings = 0x308000（Scene_Init_POPUP_SC_SYSTEMMENU 0x14fb38 反汇编：根控件@+0x100、按钮@+0x108/+0x120/+0x128/+0x130、菜单组@+0x110、主控件@+0x148）
constexpr uintptr_t G_UISETTINGS_VMA = 0x308000;        // UISettings 固定控件槽基址（面板打开时创建控件树）
constexpr uintptr_t G_POPUP_STATE_LIST_OBJ_VMA = 0x2f9f58; // g_sPopupStateList 全局对象（27 条 × 64B = 1728B，readelf 符号表；GOT 槽 [0x2f3000+0x4f0] 指向此表）
constexpr uintptr_t F_CONTROL_OBJECT_CREATE_VMA = 0x9e4ec;   // void* (u32 type, void* x1, void* x2, void* x3) 建控件对象（MEM_Malloc 0xf8 + CreateControlInfo + ChildList/Sibling 初始化）
constexpr uintptr_t F_CONTROL_OBJECT_ADD_CONTROL_OBJECT_VMA = 0x9e598; // void* (void* parent, void* x1, void* x2, u32 type, void* x4) 建控件并挂父 ChildList 尾（父 Count+1）
constexpr uintptr_t F_CONTROL_OBJECT_ADD_CONTROL_OBJECT_BY_SORT_VMA = 0x9f580; // void* (void* parent, ..., 排序插入，ControlObject_fpDefaultCompare 比较)
constexpr uintptr_t F_CONTROL_OBJECT_SET_RECT_VMA = 0x9de74;  // void (void*, i64 x, i64 y, i64 w, i64 h) 写 rect@+0x18/20/28/30
constexpr uintptr_t F_CONTROL_OBJECT_SET_CONTROL_EVENT_CALL_TYPE_VMA = 0x9dcf8; // u32 (void*, u32) 写 EventCallType@+0x88（0x200=点击触发）
constexpr uintptr_t F_CONTROL_OBJECT_SET_DATA_VMA = 0x9df2c;   // void* (void*, void*) 写 Data@+0x50
constexpr uintptr_t F_CONTROL_BUTTON_CREATE_VMA = 0xaa710;     // void* (void* parent, char* text) 建按钮（type=3 + MEM_Malloc 0x78 按钮数据）
constexpr uintptr_t F_CONTROL_BUTTON_SET_TEXT_VMA = 0xaa7dc;   // void (void*, char*) 写按钮文本 data+0x00（32B）
constexpr uintptr_t F_CONTROL_BUTTON_SET_DRAW_TYPE_VMA = 0xaaa88; // void (void*, u32) 写 DrawType@+0x28
constexpr uintptr_t F_CONTROL_BUTTON_SET_DRAW_ID_VMA = 0xaaaa8;  // void (void*, i64) 写 DrawID@+0x30
constexpr uintptr_t F_CONTROL_BUTTON_SET_DRAW_SUB_ID_VMA = 0xaaae0; // void (void*, i64) 写 DrawSubID@+0x38
constexpr uintptr_t F_CONTROL_BUTTON_SET_DRAW_PROC_VMA = 0xaac04;   // void (void*, ptr) 写 DrawProc@+0x60（ControlButton_Draw 0xaac2c 非空即 blr 调用）
constexpr uintptr_t F_UI_CREATE_GROUP_BASE_CONTROL_VMA = 0xaea78;  // void* (void* parent, i64 x, i64 y, i64 w, i64 h) 建组容器（type=0 禁全部触摸）
constexpr uintptr_t F_UIPOPUPMSG_CREATE_VMA = 0xca54c;        // void (char* text, u32 len, u32 dispType, u32 type) 建弹窗：清旧 + 拷贝文本 + 建文本控件 + SetLayout + 置 bOn（type 0/1/2/3）
constexpr uintptr_t F_UIPOPUPMSG_CREATE_NONE_VMA = 0xca950;   // void (char* text, u32 len, u32 dispType, u32 type) 无按钮弹窗（内部 type=2）
constexpr uintptr_t F_UIPOPUPMSG_CREATE_YESNO_VMA = 0xca8dc;  // void (char*, u32, u32, u32, fn ok, fn cancel, void* param) YesNo 弹窗（内部 type=1 + CreateButtonControl×2 + 存 fpOK/fpCancel/param）。param 语义=费用 int 值：CreateYesNo/CreateYesNoFromTextData 把 x6 存入 *(GOT 0x2f4698) 全局槽，UINpcQuest_DrawEndPopup 0xc32ec 读该槽、>0 时 MONEY_DrawWithUnit(0x11c6ec) 渲染价格栏——禁止传指针（会被当钱数显示乱值）；回调内取上下文须用模块自有全局
constexpr uintptr_t F_UIPOPUPMSG_CREATE_YESNO_FROM_TEXTDATA_VMA = 0xca7d4; // void (u32 textId, u32 dispType, u32 formatArg, fn ok, fn cancel, void* param) 原版格式化 YesNo 弹窗
constexpr uintptr_t F_UIPOPUPMSG_CREATE_FROM_TEXTDATA_VMA = 0xca6f4; // void (u32 textId, u32 x1, u32 x2) 按 TEXTDATABASE 文本 id 弹窗（MEMORYTEXT_GetText + CS_knlSprintk 格式化）
constexpr uintptr_t F_UIPOPUPMSG_FREE_VMA = 0xca4e8;          // void () 销毁弹窗（删主控件 + UTIL_ReleaseText + 销毁文本控件）
constexpr uintptr_t F_POPUPSTATE_PUSH_VMA = 0x122424;         // int (u32 state_id) 压弹窗栈（读 [0x2f3000+0x4f0] state list + id×0x40：blr enter@+0x10 → ArrayStack_Push process/f3/f4/event）
// ---- UI 绘制原语符号（ui-settings v0.6.9，libgame-symbols.txt 核对，docs/system/ui.md §5）----
constexpr uintptr_t G_FONT_OBJ_SLOT_VMA = 0x2f3000 + 0xf88;   // GOT 槽：*(此地址) = 字体对象指针（UI_DrawStringHAlign 0xaf02c 反汇编：str font, [*slot] 写入对象首字段）
constexpr uintptr_t F_GRPX_START_VMA = 0x8f2fc;               // void () 绘制开始（GRPX 上下文）
constexpr uintptr_t F_GRPX_END_VMA = 0x8f314;                 // void () 绘制结束
constexpr uintptr_t F_GRPX_FILL_RECT_VMA = 0x8fb30;           // void (i32 x, i32 y, i32 w, i32 h, u32 abgr) 纯色矩形（alpha 嵌 ABGR 高字节；GRPX_FillRectAlpha 的 alpha>0x64 直接 return，勿用）
constexpr uintptr_t F_GRPX_FILL_RECT_ALPHA_VMA = 0x8fccc;     // void (i32 x, i32 y, i32 w, i32 h, u32 rgb565, u32 alpha_pct) 半透明矩形；颜色是 RGB565（低 16 位 R5G6B5，经 GRPX_GetColorFromGRPWithAlpha 0x8fc88 展开），alpha 是 0..100 百分比（>0x64 直接 return 不绘制）。注意与 GRPX_FillRect（ABGR8888）格式不同
constexpr uintptr_t F_GRPX_SET_FONT_COLOR_VMA = 0x8fe58;      // void (u32 abgr) 设置文字颜色（GRPX_SetFontColor，ABGR 白=0xFFFFFFFF）
constexpr uintptr_t F_GRPX_DRAW_STRING_WITH_FONT_VMA = 0x8f9b0; // void (char*, i32 x, i32 y, i32 align, i32 font) 原版字体绘制
constexpr uintptr_t F_GRPX_SET_FONT_COLOR_RGB_VMA = 0x8fdb4;  // void (i32 r, i32 g, i32 b) 原版 RGB 字体配色
constexpr uintptr_t F_GRPX_DRAW_PART_VMA = 0x8f368;           // void (group*, i32 x, i32 y, loc*, i32 type, i32 flip, i32) 静态贴图分片绘制
constexpr uintptr_t F_UI_DRAW_STRING_HALIGN_VMA = 0xaf02c;    // void (char* text, i32 x, i32 y, i32 font, i32 align) 画文字（font 写入 [*G_FONT_OBJ_SLOT_VMA] 首字段；align 0左/1中/2右）
constexpr uintptr_t F_UI_DRAW_STRING_IN_WIDTH_WITH_FONT_VMA = 0xb190c; // void (char* text, i32 x, i32 y, i32 width, i32 font, u32 color, i32 align, u32 color2) 官方完整文字封装（取字体对象→SetFontColor→DrawStringWithFont；UIDesc_Draw 0xb5890/UINpc_Draw 0xc2c3c 实证 font=1 有效）
constexpr uintptr_t F_MW_GRAPHIC_DRAW_STRING_VMA = 0xa24cc;   // void (i32 x, i32 y, char* text, i32 align, i32 mode) UI_DrawStringHAlign 下层（mode 2→y-6, 3→y-12）
constexpr uintptr_t F_GRP_SAVE_LCD_VMA = 0xa666c;             // void () 保存 LCD 背景缓冲（RefreshLCDFlag==1 时调用）
constexpr uintptr_t F_GRP_RESTORE_LCD_VMA = 0xa66ac;          // void () 恢复 LCD 背景缓冲（RefreshLCDFlag==0 时调用）
constexpr uintptr_t F_UI_SET_REFRESH_LCD_FLAG_VMA = 0xaea60;  // void (u32) 写 RefreshLCDFlag
constexpr uintptr_t F_UI_GET_REFRESH_LCD_FLAG_VMA = 0xaea6c;  // u32 () 读 RefreshLCDFlag
constexpr uintptr_t F_CALC_RESOLUTION_WIDTH_VMA = 0x94098;    // i32 (i32) 分辨率适配宽度（面板坐标须过此函数）
constexpr uintptr_t F_CALC_RESOLUTION_HEIGHT_VMA = 0x940b0;   // i32 (i32) 分辨率适配高度
constexpr uintptr_t F_IMGSYS_UNIT_LOAD_VMA = 0x90550;         // void (i32 unit) 加载图像单元（已加载时幂等返回）
constexpr uintptr_t F_IMGSYS_UNIT_UNLOAD_VMA = 0x905a0;       // void (i32 unit) 卸载图像单元
constexpr uintptr_t F_IMGSYS_GET_GROUP_VMA = 0x908b4;         // void* (i32 unit) 取已加载图像组
constexpr uintptr_t F_IMGSYS_GET_LOC_VMA = 0x908c4;           // void* (i32 unit, i32 loc) 取图像分片位置
constexpr uintptr_t F_GET_GROUP_TITLE_IMG_TYPE_VMA = 0x9083c; // i32 () 取当前语言主题图像组
// ---- UIOption 面板（主菜单环境设置）控件槽（Scene_Init_POPUP_SC_OPTION_MMENU 0x14be20 + UIOption_CreateMainControl 0xc4c30 反汇编）----
constexpr uintptr_t G_UIOPTION_ROOT_CTRL_VMA = 0x306ff0;      // UIOption root 组控件槽（UIOption_CreateMainControl 创建，8 按钮挂此控件子链；UIOption_Draw 0xc4ed0 动态遍历 GetCount/GetChild 绘制 → 挂子链新控件即被绘制）
constexpr uintptr_t G_UIOPTION_INSTANCE_VMA = 0x307000;       // UIOption 实例/组控件槽（+0x10 偏移写入）
constexpr uintptr_t G_OPTION_SCENE_ROOT_CTRL_VMA = 0x307fd8;  // OPTION 场景 root 组控件槽（Scene_Init 创建全屏组，button1@+0x8 左上/button2@+0x10 右上/主 group@+0x18；Terminate 时 TouchHandle_DeleteControl 删除整树）
constexpr uintptr_t F_CONTROL_OBJECT_GET_COUNT_VMA = 0x9eab8; // u32 (void* ctrl) 子控件数（父控件遍历子节点用）
constexpr uintptr_t F_CONTROL_OBJECT_GET_CHILD_VMA = 0x9eacc; // void* (void* ctrl, u32 index) 取第 index 子控件
constexpr uintptr_t F_UIEQUIP_MAKE_DESC_VMA = 0xb8980;          // (ctrl, 0) 详情面板（读控件物品）
constexpr uintptr_t F_UIEQUIP_ITEM_DESC_MAKE_DESC_CALL_VMA = 0xb9188; // InvenItemControlEventProc 事件 0x80 调 MakeDesc 的唯一 bl（BL→装备按钮 hook wrapper）
constexpr uintptr_t F_UIEQUIP_BUTTON_EQUIP_EXE_VMA = 0xb7c18;   // void (void*) 装备按钮 execute：记录+2==0x1f 背包分支扫袋 1..4，全满弹 6
constexpr uintptr_t F_UIEQUIP_BUTTON_USE_EXE_VMA = 0xb80b8;     // void (void*) 使用按钮 execute：调用 CHAR_UseItemEx
constexpr uintptr_t F_UIEQUIP_BUTTON_USE_AFTER_CONFIRM_EXE_VMA = 0xb95f8; // void (void*) 确认后使用按钮 execute（IsUseAfterConfirm 类物品）：关 desc → UIEquip_ConfirmUseItem 弹确认 → OK=UIEquip_OKConfrimUseItem（先 INVEN_FindItemSlot 找槽，扩展物品不在 INVEN 被拦）
constexpr uintptr_t F_UIEQUIP_CONFIRM_USE_ITEM_VMA = 0xb943c;    // (party_char, item) 弹确认使用 YesNo；OK 回调=OKConfrimUseItem（0xb8478）
constexpr uintptr_t F_UIEQUIP_OK_CONFIRM_USE_ITEM_VMA = 0xb8478; // 确认使用 OK 回调：INVEN_FindItemSlot 定位后调 CHAR_UseItemEx
constexpr uintptr_t F_UIEQUIP_BUTTON_DESTROY_EXE_VMA = 0xb6240; // void (void*) 销毁按钮 execute：创建确认弹窗
constexpr uintptr_t F_UIEQUIP_BUTTON_UNEQUIP_EXE_VMA = 0xb7e14; // void (void*) 卸下按钮 execute：desc_type=0 卸装备、desc_type=1 卸袋（b7f74）
constexpr uintptr_t F_UIEQUIP_OK_DESTROY_ITEM_VMA = 0xb83d0;    // void () 销毁确认回调
// UIEquip_OKDestroyItem(0xb83d0) 全量反汇编核实：无参、无物品入参寄存器——背包
// 结算分支从面板上下文读 desc_type/bag/ctrl 后调 0x1261c4。仅两处调用者：
//  - 按钮预演：UIEquip_ButtonDestroyExe 0x126288 `bl 0xb83d0`（进入前 x23=0x666）；
//    settle 0x126298 `cmp x23,#0x666` 命中后只计算展示金额、不加钱/不删堆，并经
//    OKDestroyItem 尾声把金额放 x0 返回给弹窗第 6 参（确认框显示金额）。
//  - 弹窗 OK：UIPopupMsg_ButtonOKExe 0xcaa14 `blr x1`（fpOK）；真实结算。
// 两态由 hook 的 thread_local「正在 UIEquip_ButtonDestroyExe 内」标记区分（返回地址
// 经 Dobby 桥后不可信，见 native_inventory_hook.cpp）。仅弹窗 OK 真实接管；按钮预演
// 只回填展示金额，否则按下按钮即提前售出、取消无法回滚。
// G_UIEQUIP_DESC_TYPE_VMA == 2 表示「背包物品详情」（0=装备详情、1=扩展/其它袋详情）。
// 原版 OKDestroyItem 仅 2 走 0x1261c4 背包结算分支。
constexpr uint8_t kUIEquipBagDescType = 2;
// 装备页详情出售结算（无名局部函数；唯一调用点 UIEquip_OKDestroyItem@0xb8468 bl 0x1261c4，
// .dynsym 无符号，fn_resolve 无符号名回退本 VMA）。0x1261fc `ldr w0,[x21,#0x10]` +
// 0x126208 `bl UTIL_GetBitValue(_,31,25)` 只读 b 段（S2 下 199→71 结算缺陷根因）；
// 物品指针在 x21（0x1261f0 `mov x21,x0`），由 R-51 重定向为
// `mov x0,x21; bl ITEM_GetCumulateCount`。同一函数返回的弹窗 i32Param 即确认框金额。
constexpr uintptr_t F_UIEQUIP_SELL_SETTLE_VMA = 0x1261c4;
constexpr uintptr_t F_UIEQUIP_MAKE_DESC_TAIL_VMA = 0xb89c0;    // MakeDesc 尾跳 SetDescMenu（唯一调用，B 指令→菜单门禁）
constexpr uintptr_t F_UIEQUIP_SET_DESC_MENU_VMA = 0xb8504;     // void () 详情菜单按钮生成（使用/装备/丢弃）
constexpr uintptr_t F_CONTROL_OBJECT_GET_ABSOLUTE_RECT_VMA = 0x9e748; // UiRect (ctrl)（x8 sret 出参）——禁止 C++ 直调（真机 SIGSEGV，见 game_access.h 注释）；取 rect 用手工父链读
constexpr uintptr_t F_TOUCH_HANDLE_DELETE_CONTROL_VMA = 0xa3c04;     // void (ctrl) 删除控件（信息面板销毁）
constexpr uintptr_t F_CONTROL_OBJECT_SET_CONTROL_PROC_VMA = 0x9dfbc;   // void (ctrl, proc) 设置控件事件 proc（ControlItem_Create 内部同款）
constexpr uintptr_t F_CONTROL_OBJECT_SET_USER_TYPE_VMA = 0x9dbc4;      // void (ctrl, u32) 设置控件类型（2=ControlItem）
constexpr uintptr_t F_TOUCH_HANDLE_UNUSE_CONTROL_EVENT_MOVE_VMA = 0xa3e94; // void (ctrl) 控件不参与 TouchHandle 移动（ControlItem_Create 内部同款）
constexpr uintptr_t F_CONTROL_OBJECT_GET_USER_TYPE_VMA = 0x9dbbc;    // u32 (ctrl) 控件类型（2=ControlItem，SetItem 内部门禁）
constexpr uintptr_t F_CONTROL_OBJECT_GET_DATA_VMA = 0x9df18;  // void* (void* ctrl) 取控件私有数据块
constexpr uintptr_t F_CONTROL_OBJECT_GET_CURSOR_INDEX_VMA = 0x9ea48; // int (void* ctrl) 控件光标索引（背包列表控件→槽下标；UIEquip_OKDestroyItem 0xb845c 调用）
constexpr uintptr_t F_CONTROL_OBJECT_SET_ACTIVE_VMA = 0x9dbd8; // void (void* ctrl, u32 active) 写 Active@+0x0c（0x20=激活，ControlObject_EventProc 校验 ==0x20）
constexpr uintptr_t F_CONTROL_BUTTON_DRAW_VMA = 0xaac2c;      // void (void* ctrl) 按钮绘制（GetData 非空 + [data+0x60] DrawProc 非空 → blr DrawProc(x0=ctrl)）
// wipeout 死亡面板按钮（v0.4.35）：官方 UIWipeout 按钮执行函数，均 int() 无参
constexpr uintptr_t F_WIPEOUT_BUTTON_REVIVE_VMA = 0x1505a8;          // int () 复活（网络链：CS_netGetActiveNetwork 判定→NetworkStore_Enter+C2S_HubBeginWithFlow；离线弹 OK 弹窗 TextData 0x4e）
constexpr uintptr_t F_WIPEOUT_BUTTON_SPECIAL_REVIVE_VMA = 0x150640; // int () 特殊复活（同网络链，参数 0x3e7 不同）
constexpr uintptr_t F_WIPEOUT_BUTTON_GAMEOVER_VMA = 0x1502ac;       // int () 游戏结束（弹 YesNo 弹窗 TextData 0x14 确认）

// ---- 函数签名 ----
using GetMoneyFn = int64_t (*)();
using GetMemberFn = void* (*)(int);
using GetMenuCharacterFn = void* (*)();
using GetPartySizeFn = int (*)();
using GetAttrFn = int32_t (*)(void*, int);
using GetEquipFn = void* (*)(void*, int);
using GetExpFn = int64_t (*)(void*);
using GetRarityFn = int (*)(void*);
using IsRealEquipFn = int (*)(void*);  // ITEM_IsRealEquip(item 指针) → 非 0 = 真实装备（比不可堆叠更精确）
using GetBagSizeFn = int (*)(int);
using GetEmptyBagSlotFn = int (*)();
using IsEmptyBagFn = int (*)(int);
using GetBitFn = int (*)(int, int, int);
using GetItemStatFn = int (*)(void*);
using GetAttrFn2 = int (*)(void*, int);
using GetStatusPointFn = int (*)(void*);
using GetNameFn = char* (*)(void*);
using GetActMaxLevelFn = int (*)(void*, int);
using EvtSetStateFn = void (*)(int32_t);
using TextctrlMoveNextPageFn = void (*)(void*);
using KeySetCodeFn = void (*)(int32_t);
using FindMercSlotFn = void* (*)(int);
using SearchPathFn = int (*)(void*, int, int, int);
using IntVoidFn = int (*)();
using IntIntFn = int (*)(int32_t);

// ---- 写操作函数签名 ----
using SetMoneyFn = void (*)(int64_t);
using AddMoneyFn = int (*)(int64_t);
using FindItemFn = void* (*)(int32_t);              // INVEN_FindItem(category)
  using HaveItemFn = int (*)(int32_t);                // INVEN_HaveItem(category)
  using GetItemCountFn = int (*)(int32_t);             // INVEN_GetItemCount(category)
  using IsHavingEmptySlotFn = int (*)(int32_t, int32_t); // INVEN_IsHavingEmptySlot(needed, include_task_bag)
using InvenFindItemSlotFn = int (*)(void*, int8_t*); // INVEN_FindItemSlot(item, out_slot)
using InvenCalculateEmptySlotCountForSaveFn = int (*)(int32_t, int32_t);
using InvenGetEmptySaveSlotExFn = int (*)(int32_t, int32_t, int8_t*, int32_t, int32_t*);
using InvenGetNeededSaveSlotExFn = int (*)(int32_t, int32_t, int8_t*, int32_t, int32_t*, int8_t*, int32_t);
using InvenGetCumulateSaveSlotExFn = int (*)(int32_t, int32_t, int8_t*, int32_t, int32_t*);
using RemoveItemFn = int (*)(void*);          // INVEN_RemoveItem(item 指针)
using ItemGetPriceFn = int (*)(void*);        // ITEM_GetPrice(item 指针) → 静态表价格
using ItemGetSellPriceFn = int (*)(void*);    // ITEM_GetSellPrice(item 指针) → 原版最终出售价格
using ItemGetAbilityLevelFn = int (*)(void*); // ITEM_GetAbilityLevel(item 指针) → 能力等级（所需等级，读 ITEMCLASSBASE 记录+3 int8，0x1091f4 反汇编）
using InvenMoveItemFn = int (*)(void*, int, int, int);  // INVEN_MoveItem(item, count, targetBag, targetSlot)
using ItemSystemDivideFn = void* (*)(void*, int32_t); // ITEMSYSTEM_Divide(item, count) -> new item
using ControlObjectGetDataFn = void* (*)(void*);
using UiEquipIsApplyStuffFn = int (*)(void*, void*);
using UiEquipApplyStuffFn = void (*)(void*, void*);
using UiEquipEquipControlEventProcFn = uint64_t (*)(void*, uint64_t, void*, void*);
using UiEquipGetItemSlotIndexFn = int (*)(void*);
using UiEquipRefreshItemAreaFn = void (*)();
using UiEquipRefreshBagAreaFn = void (*)();
using UiStoreRefreshInvenBagFn = void (*)();
using UiStoreRefreshInvenItemFn = void (*)();
using UiMixRefreshInvenItemFn = void (*)();
using UiStoreMakeDescFn = void (*)(void*, void*);
using UiEquipUpdateCharEquipFn = void (*)();
using UiDescSetOffFn = void (*)();
using UiDescGetDataFn = void* (*)();
using UiDescDrawFn = void (*)();  // UIDesc_Draw()
using UiEquipMakeDescFn = void (*)(void*, void*);  // UIEquip_MakeDesc(ctrl, 0)：读控件物品生成详情面板（袋标签二次点击原版语义）
using UiPopupMsgCreateOkFromTextDataFn = void (*)(uint32_t, uint32_t, uint32_t, uint32_t);
// ---- 佣兵徽章使用链 typedef（R-63）----
using UiEquipButtonUseMercenarySealExeFn = void (*)(void*);   // UIEquip_ButtonUseMercenarySealExe(button)
using ItemSystemIsMercenarySealFn = int (*)(int32_t);         // ITEMSYSTEM_IsMercenarySeal(category)
using MercenarySystemIsEmptyManagerSlotFn = int (*)();        // MERCENARYSYSTEM_IsEmptyManagerSlot()
using MercenarySystemMakeMercenaryFn = void* (*)(void*);      // MERCENARYSYSTEM_MakeMercenary(item) → 佣兵指针/null
using TouchHandleSetCursorFn = void (*)(void*, void*);
using TouchHandleResetMovingControlFn = void (*)();
using TouchHandleResetSelectedControlFn = uint64_t (*)();
using UiEquipInvenItemControlEventProcFn = uint64_t (*)(void*, uint64_t, void*, void*);
using ItemDrawPortingFn = void (*)(void*, int32_t, int32_t, int32_t, int32_t);
using SetExpFn = void (*)(void*, int32_t);
using SetLevelFn = int (*)(void*, int32_t);   // CHAR_SetLevel(0xe05a0)：返回 1=成功（升级/同级）/ 0=降级拒绝
using AddExpFn = int (*)(void*, int32_t, uint8_t);
using SetStatusPointFn = void (*)(void*, int32_t);
using GetStatMainFn = int (*)(void*, int32_t);
using SetStatMainFn = void (*)(void*, int32_t, int32_t);
using SetStatBaseFn = void (*)(void*, int32_t, int32_t);
using RollStatusDiceFn = int (*)(int32_t, int32_t);
using GetCumulateCountFn = int (*)(void*);
using PutJewelFn = int (*)(void*, void*);
using IsJewelFn = int (*)(int32_t);
using EnchantItemFn = int (*)(void*, int32_t);
using IsEnchantScrollFn = int (*)(int32_t);
// 属性显示范围（attribute-range-display）
using ItemSystemGetOptionValueFn = int (*)(int option_index, int level, int flag, void* item); // ITEMSYSTEM_GetOptionValue
using ItemSystemGetJewelOptionValueFn = int (*)(int type, void* item);                          // ITEMSYSTEM_GetJewelOptionValue
using MathGetRandomFn = int (*)(int min, int max);                                              // MATH_GetRandom
using UiDescAddOptionFn = void (*)(void* builder, int type, int option_index, int value);       // UIDesc_AddOption
using UiDescMakeItemFn = uintptr_t (*)(void* item, void* character, void* arg2);                // UIDesc_MakeItem（x0 透传）
using SaveIsOkFn = int (*)();
using CharInitializeStatusFn = void (*)(void*);
using CharInitializeSkillFn = void (*)(void*);
using CharSetActionIdFn = void (*)(void*, int32_t, void*);
using CharGetEnemyTargetFn = void* (*)(void*, int32_t, int32_t);
using QuestSystemFindFn = int (*)(int32_t);
using QuestSystemRemoveSlotFn = int (*)(int32_t);
using QuestSystemIsCompleteFn = int (*)(int32_t);
using QuestSystemChangeQuestStateFn = int (*)(int32_t, int32_t);
using SaveFn = int (*)();
using GamestateSetStateFn = void (*)(int32_t);
using StateNextStartProcessFn = void (*)();  // MainProcess 内逻辑帧状态推进（无参）
using SaveGetSaveSlotFn = void* (*)(int32_t);
using SaveLoadSaveSlotFn = int (*)(int32_t, void*);
using UiSetPopupProcessInfoFn = int (*)(int32_t, int32_t);
using GameStartResumeGameFn = int (*)(int32_t);
using SaveCreateSaveSlotFn = void (*)();
using SaveslotGetHeroFn = void* (*)(void*);
using StateSetFn = void (*)(int32_t);
using GameExitSaveSlotSelectCharFn = void (*)();
using SelectCharacterStartGameFn = void (*)();
using TutorialStartFn = void (*)();
using SaveGetSaveFileNameFn = void (*)(int32_t, char*);
using CsFsRemoveFn = int (*)(char*, int32_t);
// ---- 存档文件加解密链签名（save-export 存档管理器）----
using HubSaveGetKeyFn = const char* (*)();
using SaveLoadDataFn = int (*)(int32_t, void**, int*);
using MemFreeFn = void (*)(void*);
using EncryptProcess2Fn = int (*)(void*, int, int, const char*);
using ItemGetBuyPriceFn = int (*)(void*);
using InvenFindSaveSlotFn = int (*)(void*, int8_t*);
using InvenSaveItemFn = int (*)(void*, void*);
using SaveItemFn = int (*)(void*);  // INVEN_SaveItem(item)：唯一"已创建物品放入背包"漏斗（0x104528，只用 x0；FindSaveSlot 失败返回 0）
using InvenSaveItemDirectFn = int (*)(void*, int32_t, int32_t);  // INVEN_SaveItemDirect(item, bag, slot) 指定袋槽入库（空槽写入/同类堆叠合并）
using InvenRemoveItemDataFn = int (*)(int32_t, int32_t);  // INVEN_RemoveItemData(category, count)：按类别批量删除（count<0 语义未冻结，wrapper fail-closed 不修正）
using InvenSaveItemOnEmptyFn = int (*)(void*, int32_t);  // INVEN_SaveItemOnEmpty(item, bag) 目标袋内找空槽入库
using DealSystemFindSaleByIdFn = void* (*)(void*);
using UinpcInitFn = uint8_t (*)();
using NpcSystemCheckFunctionDisplayFn = int (*)(int32_t);
using UinpcExeTaskFn = void (*)();
using UinpcQuestButtonOkExeFn = int (*)();
using NpctasklistMakeDlgFn = char* (*)();
using PlayerCheckNearNpcFn = void (*)();
using GetSkillUsageFn = int (*)(void*);
using SetSkillUsageFn = void (*)(void*, int32_t);
using SetAutoAttackFn = void (*)(void*, int32_t);
using EquipItemFn = int (*)(void*, void*);
using EquipItemFromInvenToSlotFn = int (*)(void*, int32_t, int32_t, int32_t);
using UnequipFn = int (*)(void*, int32_t);
using ButtonEquipExeFn = void (*)(void*);  // UIEquip_ButtonEquipExe(button)：全路径返回 1，返回值无消费方
using ButtonDestroyExeFn = void (*)(void*);  // UIEquip_ButtonDestroyExe(button)：详情出售按钮，创建确认弹窗前先预演 OKDestroyItem 取展示金额
using ButtonUnequipExeFn = void (*)(void*);  // UIEquip_ButtonUnequipExe(button)
using CanEquipFn = int (*)(void*, void*);
using FindEquipSlotFn = int (*)(void*, void*);
using GetEquipItemFn = void* (*)(void*, int32_t);
using SetEquipItemFn = void (*)(void*, int32_t, void*);
using IsSpecialNpcFn = int (*)(void*);
using LearnActionFn = void* (*)(void*, int32_t, int32_t);
using SetActivePlayerFn = int (*)(int32_t);
using PartySwapFn = void (*)(int32_t, int32_t);
using SetPositionFn = void (*)(int32_t, int32_t);
using ChangeMapFn = void (*)(int32_t, int32_t, int32_t, int32_t);
using UiChoiceButtonListExeFn = void (*)(void*); // UIChoice_ButtonListExe(control)
using UiChoiceCreateControlFn = void (*)(void*); // UIChoice_CreateControl(control)
using UiChoiceProcessFn = void (*)(); // UIChoice_Process()
using UiChoiceInitFn = void (*)(void*); // UIChoice_Init(control)
using SceneEventPopupScChoiceFn = uint64_t (*)(uint64_t, uint64_t, uint64_t);
using MapchangeSetFn = void (*)(int32_t, int32_t, int32_t, int32_t);
using MemorytextGetTextFn = const char* (*)(uint16_t);

// ---- 合法操作函数签名 ----
using MoveAsPathFn = int (*)(void*);
using CharMoveFn = int (*)(void*, int, int, unsigned char);
using CharPickItemAllFn = int (*)(void*, int32_t);
using CharGetAreaRectFn = void (*)(void*, int, int, int16_t*);
using MemMallocFn = void* (*)(size_t);
using NotifierAddFn = void (*)(int, int, void*, void*);
using CharSetDirectionFn = void (*)(void*, int);
using CharRemovePathFn = void (*)(void*);
using MapSetFocusFn = void (*)(int32_t, int32_t);
using GoMapLinkByCharFn = int (*)(void*, int32_t, int32_t, int32_t);  // 4参: (ch, tile_x, tile_y, use_dir)，use_dir=0 走 MAP_FindMapLinkNoDir（不查角色朝向，只查出口 tile 坐标）
using CharSetTargetFn = void (*)(void*, void*);
using CharStopCombatFn = void (*)(void*);
using ConsumeItemFn = void (*)(void*);
using CharUseItemExFn = int (*)(void*, void*, int);  // 返回 1=成功(内部已消耗) 0=失败
using UiEquipOkConfirmUseItemFn = void (*)(void*);  // UIEquip_OKConfrimUseItem(item)：确认使用回调
// UIEquip_OKDestroyItem()：无参销毁/出售确认回调（背包结算分支读面板上下文取袋/槽）。
// 返回值仅按钮预演读取（弹窗展示金额）；真实弹窗 OK 调用忽略返回值，故用 uint64_t 捕获 x0。
using UiEquipOkDestroyItemFn = uint64_t (*)();
using CharProcessShortcutFn = int (*)(void*, int); // 返回 1=处理成功，0=未处理或失败
using RemoveItemDirectFn = int (*)(int32_t, int32_t);
using IncludePartyFn = int (*)(void*);
using ExcludePartyFn = int (*)(void*);
using MercenaryReleaseFn = void (*)(int32_t);
using ItemIsUseFn = int (*)(int32_t);
using PopupStateExistFn = int (*)();
using OpenItemBoxFn = int (*)(int32_t);
using ReleaseSealedFn = int (*)(int32_t);
using IsDiceFn = int (*)(int32_t);
using IsSealedFn = int (*)(int32_t);
using IsItemBoxFn = int (*)(int32_t);
using MakeItemFn = void* (*)(int32_t, int32_t, int32_t);
using CreateItemFn = void* (*)(int32_t, int32_t, int32_t, int32_t);
using NetworkStoreSetStateFn = void (*)(int);
using MakeMixFn = int (*)(int32_t, void**);       // MIXSYSTEM_MakeItem：0=成功（*outItem 已填），非 0=失败
using GetCostFn = int64_t (*)(int32_t, void*);    // MIXSYSTEM_GetCost：合成费用（负数=非法配方）
using SaveSaveItemFn = int (*)(uint8_t*, void*);  // SAVE_SaveItem(out, item)：序列化物品，返回总字节（uxtb 截断，须 ≤255）
using SaveLoadItemFn = int (*)(const uint8_t*, void**, int*);  // SAVE_LoadItem(in, &out, &consumed)：重建物品，1=成功
using ItemPoolFreeFn = void (*)(void*);           // ITEMPOOL_Free(item)：释放物品对象

// ---- UI 实验函数签名（ui-exp v0.6.7）----
using ControlObjectCreateFn = void* (*)(uint32_t type, void* x1, void* x2, void* x3);
using ControlObjectAddFn = void* (*)(void* parent, void* x1, void* x2, uint32_t type, void* x4);
using ControlObjectSetRectFn = void (*)(void* ctrl, int64_t x, int64_t y, int64_t w, int64_t h);
using ControlObjectSetEventCallTypeFn = uint32_t (*)(void* ctrl, uint32_t type);
using ControlObjectSetDataFn = void* (*)(void* ctrl, void* data);
using ControlButtonCreateFn = void* (*)(void* parent, char* text);
using ControlButtonSetTextFn = void (*)(void* ctrl, char* text);
using ControlButtonSetDrawTypeFn = void (*)(void* ctrl, uint32_t type);
using ControlButtonSetDrawIDFn = void (*)(void* ctrl, int64_t id);
using ControlButtonSetDrawSubIDFn = void (*)(void* ctrl, int64_t subId);
using ControlButtonSetDrawProcFn = void (*)(void* ctrl, void* proc);
using UiCreateGroupBaseControlFn = void* (*)(void* parent, int64_t x, int64_t y, int64_t w, int64_t h);
using UiPopupMsgCreateFn = void (*)(char* text, uint32_t len, uint32_t dispType, uint32_t type);
using UiPopupMsgCreateYesNoFn = void (*)(char* text, uint32_t len, uint32_t dispType, uint32_t type,
                                          void* okFn, void* cancelFn, void* param);
using UiPopupMsgCreateYesNoFromTextDataFn = void (*)(uint32_t textId, uint32_t dispType,
                                                     uint32_t formatArg, void* okFn,
                                                     void* cancelFn, void* param);
using UiPopupMsgCreateFromTextDataFn = void (*)(uint32_t textId, uint32_t x1, uint32_t x2);
using UiPopupMsgFreeFn = void (*)();
using PopupStatePushFn = int (*)(uint32_t state_id);
using ButtonExecuteProcFn = void (*)(void* ctrl);

// ---- UI 绘制原语函数签名（ui-settings v0.6.9）----
using GrpxStartFn = void (*)();
using GrpxEndFn = void (*)();
using GrpxFillRectFn = void (*)(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t abgr);
using GrpxFillRectAlphaFn = void (*)(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t rgb565, uint32_t alpha_pct);  // rgb565 + alpha 百分比（非 ABGR8888）
using GrpxSetFontColorFn = void (*)(uint32_t abgr);
using GrpxDrawStringWithFontFn = void (*)(char* text, int32_t x, int32_t y, int32_t align, int32_t font);
using GrpxSetFontColorRgbFn = void (*)(int32_t r, int32_t g, int32_t b);
using GrpxDrawPartFn = void (*)(void* group, int32_t x, int32_t y, void* loc, int32_t type, int32_t flip, int32_t unknown);
using UiDrawStringHAlignFn = void (*)(char* text, int32_t x, int32_t y, int32_t font, int32_t align);
using UiDrawStringInWidthWithFontFn = void (*)(char* text, int32_t x, int32_t y, int32_t width, int32_t font, uint32_t color, int32_t align, uint32_t color2);
using MwGraphicDrawStringFn = void (*)(int32_t x, int32_t y, char* text, int32_t align, int32_t mode);
using GrpSaveLcdFn = void (*)();
using GrpRestoreLcdFn = void (*)();
using UiSetRefreshLcdFlagFn = void (*)(uint32_t flag);
using UiGetRefreshLcdFlagFn = uint32_t (*)();
using CalcResolutionFn = int32_t (*)();
using ImgsysUnitFn = void (*)(int32_t unit);
using ImgsysGetGroupFn = void* (*)(int32_t unit);
using ImgsysGetLocFn = void* (*)(int32_t unit, int32_t loc);
using GetGroupTitleImgTypeFn = int32_t (*)();
using ControlObjectGetCountFn = uint32_t (*)(void* ctrl);
using ControlObjectGetChildFn = void* (*)(void* ctrl, uint32_t index);
using ControlObjectGetDataFn = void* (*)(void* ctrl);
using ControlObjectGetCursorIndexFn = int (*)(void* ctrl);
using ControlObjectSetActiveFn = void (*)(void* ctrl, uint32_t active);
using ControlItemSetItemFn = void (*)(void* ctrl, void* item);
using ControlObjectSetShowFn = void (*)(void* ctrl, uint32_t show);
using ControlButtonDrawFn = void (*)(void* ctrl);

// ---- UIMix 宝石合成操作优化函数签名（gem-craft-optimization 阶段1）----
using ControlObjectGetCursorFn = void* (*)(void*);          // ControlObject_GetCursor(ctrl)
using UIMixGetTypeFn = int (*)();                           // UIMix_GetType()
using UIMixButtonInvenItemSelectExeFn = void (*)(void*);    // UIMix_ButtonInvenItemSelectExe(ctrl)
using UIMixButtonMenuListExeFn = void (*)(void*);           // UIMix_ButtonMenuListExe(ctrl)
using UIMixButtonRecipeExeFn = void (*)(void*);             // UIMix_ButtonRecipeExe(ctrl)
using UIMixButtonMixingExeFn = void (*)(void*);             // UIMix_ButtonMixingExe(ctrl)：合成按钮 ExecuteProc
using UIMixInitMixingStateFn = void (*)();                  // UIMix_InitMixingState()：依当前 mixType 重算 stuffList/费用
using UIMixResetStuffItemControlFn = void (*)();            // UIMix_ResetStuffItemControl()

// ---- 自动出售阶段 A：GAMESTATE_DrawPlay draw-end 宿主调用点（auto-sell）----
// 符号存在性已用 NDK r26d llvm-objdump 核对：GAMESTATE_DrawPlay@0x9d6cc。
constexpr uintptr_t F_GAMESTATE_DRAWPLAY_VMA = 0x9d6cc;  // void GAMESTATE_DrawPlay()

// ---- 统一帧派发宿主锚点：GAMESTATE_DrawPlay 内 +0x20 的 `bl MAP_DrawBase`（首个渲染调用之前）----
// 原指令字 0x9401d18a；DrawPlay 内 byte==1 分支经 GRP_AddColorTone 后 b 0x9d6ec 汇聚到此，
// 故每次 DrawPlay 必执行。
constexpr uintptr_t F_MAP_DRAWBASE_VMA = 0x111d14;              // void MAP_DrawBase()
constexpr size_t F_GAMESTATE_DRAWPLAY_DRAWBASE_CALL_OFF = 0x20; // GAMESTATE_DrawPlay 内 bl MAP_DrawBase
using MapDrawBaseFn = void (*)();                               // void MAP_DrawBase()
