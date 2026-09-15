#pragma once

// 特殊装备解锁（special-equip）域入口。
//
// 功能：解除 ITEMDATABASE 记录 +7 bit4 这道「NPC 专属保护」对**非特殊 NPC** 的约束，
// 使玩家侧角色能够穿脱全部带该位保护的装备。全表共 26 条（静态数据实证）：
//   cat 485-506  敌方/剧情专属组（镇魂之杖、冰焰戒指、龙齿剑、巨人族的长靴……）
//   cat 785-787  誓约之剑 / 神速长靴 / 真实的板甲
//   cat 948      伪装用面具
// 该位同时是「不可装备」与「不可脱下」的唯一开关（两个判定函数各读它一次）。
//
// 机制（architecture.md §2.2.1 准入顺序第五档 · LSPosed NativeHook API 单点包裹 x2）：
// 包裹 `CHAR_CanEquipItem(0xe4eb4)` 与 `CHAR_CanUnequipItem(0xe4e2c)`。原版判定链：
//   CanEquipItem:   ① ITEM_IsRealEquip(item) → ② !(rec[+7] & 0x10)
//                   → ③ CHAR_CanChangeEquip(ch) → ④ ch[C_LEVEL] >= ITEM_GetEquipLevel(item)
//   CanUnequipItem: ① !(rec[+7] & 0x10) → ② CHAR_CanChangeEquip(ch)
// 本域只在**原版已返回 0** 时跳过 ② 并重放其余检查；其余各道原样执行。
//
// 关键设计取舍：
//   - **不做「临时清位重跑」**。那需要写 ITEMDATABASE，而 `data_op_equip` 会在 HTTP 线程经
//     `fn_can_equip` 进入本 wrapper，产生对游戏静态表的并发写窗口，违反 architecture.md §9.6
//     （非游戏线程禁止调用有写副作用的游戏函数）。本域全部由**纯读**调用组成。
//   - **不 hook `CHAR_CanChangeEquip`**。它签名 `int(void* ch)`，拿不到 item，无法把放行限定到
//     具体装备；而且它体内**不读 bit4**（只读 `[ch+0x352]` 与 `CHAR_IsSpecialNPC`），hook 它
//     碰不到目标；更重要的是它是**必须保留**的那道检查。
//   - **不直接返回 1**。那会一并跳过 ①③④：非装备物品可被「穿上」、低等级可穿高等级装备，
//     且特殊 NPC 的锁（③）当场失效。
//
// 角色闸门完全交给原版 ③：`CHAR_CanChangeEquip(ch) = ([ch+0x352] >= 0) && !CHAR_IsSpecialNPC(ch)`。
// 于是「特殊 NPC 身上的无法穿脱」由原版逻辑保证——任务特殊 NPC 与不在队伍槽的角色（怪物、
// 场景 NPC）照旧被拒，本域不参与角色判定。
//
// 出售侧（`ITEMDATABASE_IsNoSell` / +6 bit3）本域**完全不触碰**：带 bit3 的装备保持不可出售。
//
// **常开，无配置开关**：该位只影响「能否穿脱」，字符侧没有获取途径，开启不产生任何新行为；
// 而一旦关闭，已持有这些装备的角色会被重新锁住、无法穿脱，所以不提供开关。
// 需要开关的是后续的「获取途径 / 效果回调」等新行为，不是本域。
//
// 线程模型：两个 wrapper 只调用纯读游戏函数，游戏线程与 HTTP 线程均可安全进入。
// 无共享可变状态（唯一计数器是仅用于诊断日志的 atomic）。

// 解析符号并安装两个 NativeHook。幂等；任一依赖缺失或任一 hook 失败即 fail-closed
// （不安装、不半装）。需在 bridge 就绪后由游戏线程的初始化路径调用
// （见 bridge/native/gamebridge.cpp nativeInit）。
void special_equip_install_if_ready();
