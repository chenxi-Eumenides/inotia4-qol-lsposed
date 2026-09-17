#include "game/access/patches.h"

#include <cstring>

#include "data/native/game_symbols.h"
#include "game_access.h"

namespace qol::game {
namespace {

// 登记表是代码事实；常量全部取自 game_symbols.h，禁止裸地址。
struct SiteEntry {
    const char* name;
    uintptr_t vma;
    size_t offset;
    uint32_t expected_word;
    uint32_t replacement_word;
    PatchKind kind;
};

constexpr SiteEntry kSites[] = {
    // 队伍闸门：BL 重定向到 handler，replacement_word 由 wrapper 地址生成。
    {"P_CHAR_MOVE_PUSH_PARTY", F_CHAR_MOVE_VMA, CHAR_MOVE_PUSH_PARTY_BL_OFF,
     CHAR_MOVE_PUSH_PARTY_BL_WORD, 0, PatchKind::kBlRedirect},
    // 站立闸门：条件分支置 NOP，无透传执行体。
    {"P_CHAR_MOVE_PUSH_STAND", F_CHAR_MOVE_VMA, CHAR_MOVE_PUSH_STAND_CBZ_OFF,
     CHAR_MOVE_PUSH_STAND_CBZ_WORD, ARM64_NOP_WORD, PatchKind::kWordReplace},
    // 帧源锚点（常驻，owner=core）：BL 重定向到帧派发 wrapper。
    {"P_GAMESTATE_DRAWPLAY_DRAWBASE", F_GAMESTATE_DRAWPLAY_VMA,
     F_GAMESTATE_DRAWPLAY_DRAWBASE_CALL_OFF, GAMESTATE_DRAWPLAY_DRAWBASE_CALL_WORD, 0,
     PatchKind::kBlRedirect},
    {"P_MAINPROCESS_NEXT_STATE", F_MAINPROCESS_VMA, F_MAINPROCESS_NEXT_STATE_CALL_OFF,
     MAINPROCESS_NEXT_STATE_CALL_WORD, 0, PatchKind::kBlRedirect},
};

}  // namespace

bool patch_site(const char* name, PatchSite* out) {
    if (name == nullptr || out == nullptr || g_base == 0) {
        return false;
    }
    for (const SiteEntry& entry : kSites) {
        if (std::strcmp(entry.name, name) != 0) {
            continue;
        }
        out->addr = g_base + entry.vma + entry.offset;
        out->expected_word = entry.expected_word;
        out->replacement_word = entry.replacement_word;
        out->kind = entry.kind;
        return true;
    }
    return false;
}

}  // namespace qol::game
