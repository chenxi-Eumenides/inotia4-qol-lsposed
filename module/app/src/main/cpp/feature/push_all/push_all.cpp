// push_all.cpp —— 推怪（推动增强）全局开关。
//
// 两个 patch 协同放开 CHAR_Move 推动分支的 blocker 资格限制。原版 blocker 须同时满足
// 「在队伍（CHAR_GetPartyIndex(blocker) != -1）」且「站立」：
//   patch B（队伍闸门）：BL 重定向 CHAR_Move+0x168 的 bl CHAR_GetPartyIndex → 模块 wrapper。
//            怪物(type=1)伪装为队伍成员使其可推；其余走原函数（队友可推、NPC type=2
//            保持 -1 不可推——场景设施不应被推走）。
//   patch A（站立闸门）：CHAR_Move+0x180 站立检查 cbz 置 NOP。战斗中/行动中的怪物非站立，
//            不放开则推不动（真机已验证：仅 patch A 时怪物仍不可推）。
// 资格矩阵：怪物/队友/召唤物可推，场景设施（商人等 NPC）不可推。
//
// wrapper 在游戏主线程 CHAR_Move 调用链内执行：只读 C_TYPE + 调 fn_* 纯内存读，无线程安全问题。
// 注意 C_TYPE 语义：0=主控、1=怪物、2=队友/NPC/装饰物/召唤物混合——type=2 不能直接拒，
// 队友（祭司/拉卡诺斯 units 实测 type=2）必须靠 GetPartyIndex 区分。
//
// 日志归 platform 域：本文件只产生 patch 安装/还原诊断，logging.md §3 把 call_patch 归 platform。

#include "feature/push_all/push_all.h"

#include "core/native/call_patch.h"
#include "core/native/qol_log.h"
#include "game_access.h"
#include "data/native/game_symbols.h"

namespace {

// CHAR_GetPartyIndex 的模块替代：返回 0 表示「视为队伍成员，可推」，-1 表示不可推。
int push_all_party_filter(void* ch) {
    if (ch == nullptr) return -1;
    const uint8_t type = *reinterpret_cast<uint8_t*>(reinterpret_cast<uint8_t*>(ch) + C_TYPE);
    if (type == 1) return 0;  // 怪物：可推（本功能新增语义）
    // type=0/2：主控、队友、召唤物、NPC 混合。召唤物不在队伍（原判 -1），经
    // CHAR_IsActivePlayerGroup（主控及其召唤物群体）放开；其余交原版判定——
    // 队友 GetPartyIndex>=0 可推，场景设施返回 -1 不可推。
    if (fn_char_is_active_player_group != nullptr &&
        fn_char_is_active_player_group(ch) != 0) {
        return 0;
    }
    // 符号未解析时判不可推：直接解引用空函数指针会在 CHAR_Move 调用链内崩溃。
    if (fn_char_get_party_index == nullptr) return -1;
    return fn_char_get_party_index(ch);
}

}  // namespace

bool push_all_set_enabled(bool on) {
    if (g_base == 0) return false;
    const uintptr_t party_addr = g_base + F_CHAR_MOVE_VMA + CHAR_MOVE_PUSH_PARTY_BL_OFF;
    const uintptr_t stand_addr = g_base + F_CHAR_MOVE_VMA + CHAR_MOVE_PUSH_STAND_CBZ_OFF;
    if (!on) {
        // 关闭：两条闸门都必须还原；revert 幂等（已还原时返回 true），故关→关仍成功。
        const bool a_ok = call_patch_revert_word(stand_addr, CHAR_MOVE_PUSH_STAND_CBZ_WORD);
        const bool b_ok = call_patch_revert_bl(party_addr, CHAR_MOVE_PUSH_PARTY_BL_WORD);
        if (!a_ok || !b_ok) {
            QOL_LOG_ERROR(QolDomain::kPlatform,
                          "push_all disable failed: stand=%d party=%d", a_ok ? 1 : 0,
                          b_ok ? 1 : 0);
        }
        return a_ok && b_ok;
    }
    // 开启：先装队伍闸门，再装站立闸门。站立闸门失败必须回滚队伍闸门——否则只剩队伍闸门
    // 生效（怪物仅在站立时可推），与开关状态不符，且调用方无法感知半开状态。
    const bool b_ok = call_patch_install_bl(party_addr, CHAR_MOVE_PUSH_PARTY_BL_WORD,
                                            reinterpret_cast<void*>(&push_all_party_filter));
    if (!b_ok) {
        QOL_LOG_ERROR(QolDomain::kPlatform,
                      "push_all enable failed: party gate rejected (word mismatch or mprotect denied)");
        return false;
    }
    const bool a_ok =
        call_patch_install_word(stand_addr, CHAR_MOVE_PUSH_STAND_CBZ_WORD, ARM64_NOP_WORD);
    if (!a_ok) {
        call_patch_revert_bl(party_addr, CHAR_MOVE_PUSH_PARTY_BL_WORD);
        QOL_LOG_ERROR(QolDomain::kPlatform,
                      "push_all enable failed: stand gate rejected, party gate rolled back");
        return false;
    }
    return true;
}
