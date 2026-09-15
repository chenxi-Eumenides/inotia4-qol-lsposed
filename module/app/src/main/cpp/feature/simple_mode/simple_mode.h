#pragma once

#include <string>

// 简单模式（simple mode）域入口。
//
// 功能：开启后
//   ① 怪物/敌人最大生命在生成与任何属性重算路径上减半；
//   ② 玩家侧对怪物造成的伤害 +50%（×1.5）；
//   ③ 玩家侧受到的伤害 −50%（×0.5）。
//
// 机制（依 architecture.md §2.2.1 准入顺序，经深反汇编论证后采用 Tier ⑤ LSPosed NativeHook API
// 单点包裹，详见 docs/development/features/simple-mode.md）：
//   - 伤害倍率：包裹 CHAR_AddDamage 入口。该函数是全部 15 条伤害路径的唯一汇合点，函数体第 3 条
//     语句即 CHAR_AddLife(受害者, -伤害)，故入口即「生效前最后一刻」；只改 w2 一个入参，
//     扣血/承伤记录/仇恨/飘字/音效全部一致缩放。
//   - 最大生命：包裹 CHAR_UpdateAttrFromMonster。CHAR_UpdateAttr 对 C_TYPE==1 必然分派至此，
//     且属性脏位是惰性重算 → 生成/升级/装备/buff/难度 任何路径都会经过。
//
// 线程模型：开关由 JVM 线程写（nativeSetSimpleModeEnabled），游戏线程读（原子）。
// 两个 wrapper 只在游戏线程被调用，wrapper 内会调游戏查询函数（阵营判定）——
// 这些函数均为纯读、无写回副作用，但不得在其它线程调用本域任何入口。

// 开关（默认关闭）。任意线程可调用。
void simple_mode_set_enabled(bool enabled);
bool simple_mode_enabled();

// 状态 JSON：enabled / installed / attempted / 两个 hook 的入口地址与是否走了跳板跟随 /
// 符号解析与阵营依赖是否齐备。供真机验收与排障，任何线程可调用。
std::string simple_mode_status_json();

// 解析符号并安装两个 NativeHook。幂等；任一步失败即 fail-closed（不安装、不半装）。
// 需在 bridge 就绪后由游戏线程的初始化路径调用（见 gamebridge.cpp nativeInit）。
void simple_mode_install_if_ready();
