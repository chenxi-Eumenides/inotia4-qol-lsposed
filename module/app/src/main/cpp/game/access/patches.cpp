#include "game/access/patches.h"

#include <cstring>

#include "data/native/game_symbols.h"
#include "game_access.h"

namespace qol::game {
namespace {

// 登记表是代码事实；常量全部取自 game_symbols.h，禁止裸地址。
// target_macro/target_vma 仅 kBlRedirectToSymbol 使用（BL 目标函数，运行期 fn_resolve 解析）。
struct SiteEntry {
    const char* name;
    uintptr_t vma;
    size_t offset;
    uint32_t expected_word;
    uint32_t replacement_word;
    PatchKind kind;
    const char* target_macro = nullptr;
    uintptr_t target_vma = 0;
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

    // ---- B2a-2：常驻 core patch 站点（原 game_patch_core.inc / game_patch_hub_login.inc）----
    // 位段缩窄 ×2（原 core.inc:15-16）：存档子物品检查位段避开数量高位段 a。
    {"P_SAVE_SAVE_INVENTORY_BITEND", F_SAVE_SAVE_INVENTORY_VMA, 0x78, 0x52800301, 0x528002A1,
     PatchKind::kWordReplace},
    {"P_SAVE_LOAD_INVENTORY_BITEND", F_SAVE_LOAD_INVENTORY_VMA, 0xa8, 0x52800301, 0x528002A1,
     PatchKind::kWordReplace},
    // 上限 clamp ×6（原 core.inc:38-43）：仅作用「数量完整值」判定点，gate=stack_limit。
    {"P_INVEN_MOVE_ITEM_CLAMP_150", F_INVEN_MOVE_ITEM_VMA, 0x150, 0x71018c1f, 0x710f9c1f,
     PatchKind::kWordReplace},
    {"P_INVEN_MOVE_ITEM_CLAMP_158", F_INVEN_MOVE_ITEM_VMA, 0x158, 0x52800c77, 0x52807cf7,
     PatchKind::kWordReplace},
    {"P_INVEN_SAVE_ITEM_DIRECT_CLAMP_DC", F_INVEN_SAVE_ITEM_DIRECT_VMA, 0xdc, 0x71018c7f,
     0x710f9c7f, PatchKind::kWordReplace},
    {"P_INVEN_SAVE_ITEM_DIRECT_CLAMP_F0", F_INVEN_SAVE_ITEM_DIRECT_VMA, 0xf0, 0x71018e9f,
     0x710f9e9f, PatchKind::kWordReplace},
    {"P_INVEN_SAVE_ITEM_DATA_CLAMP_7C", F_INVEN_SAVE_ITEM_DATA_VMA, 0x7c, 0x71018a9f, 0x710f9a9f,
     PatchKind::kWordReplace},
    {"P_INVEN_FIND_SAVE_SLOT_CLAMP_238", F_INVEN_FIND_SAVE_SLOT_VMA, 0x238, 0x71018c1f,
     0x710f9c1f, PatchKind::kWordReplace},
    // S2 直接位读重定向 ×5（原 core.inc:110-116）：arg 点改交物品指针、call 点改指
    // ITEM_GetCumulateCount。两处均为独立 patch 点，各占一条 claim。
    {"P_UISTORE_BUTTON_SELL_EXE_D18D0_ARG", F_UISTORE_BUTTON_SELL_EXE_VMA, 0xb8, 0xb9401260,
     0xaa1303e0, PatchKind::kWordReplace},
    {"P_UISTORE_BUTTON_SELL_EXE_D18D0_CALL", F_UISTORE_BUTTON_SELL_EXE_VMA, 0xc4, 0x9401bb13, 0,
     PatchKind::kBlRedirectToSymbol, "F_GET_CUMULATE_COUNT_VMA", F_GET_CUMULATE_COUNT_VMA},
    {"P_UISTORE_BUTTON_SELL_EXE_D18E8_ARG", F_UISTORE_BUTTON_SELL_EXE_VMA, 0xd0, 0xb9401260,
     0xaa1303e0, PatchKind::kWordReplace},
    {"P_UISTORE_BUTTON_SELL_EXE_D18E8_CALL", F_UISTORE_BUTTON_SELL_EXE_VMA, 0xdc, 0x9401bb0d, 0,
     PatchKind::kBlRedirectToSymbol, "F_GET_CUMULATE_COUNT_VMA", F_GET_CUMULATE_COUNT_VMA},
    {"P_UISTORE_SELL_ITEM_D26B8_ARG", F_UISTORE_SELL_ITEM_VMA, 0xc8, 0xb9401260, 0xaa1303e0,
     PatchKind::kWordReplace},
    {"P_UISTORE_SELL_ITEM_D26B8_CALL", F_UISTORE_SELL_ITEM_VMA, 0xd4, 0x9401b799, 0,
     PatchKind::kBlRedirectToSymbol, "F_GET_CUMULATE_COUNT_VMA", F_GET_CUMULATE_COUNT_VMA},
    {"P_ITEMSYSTEM_DIVIDE_108458_ARG", F_ITEMSYSTEM_DIVIDE_VMA, 0x60, 0xb9401260, 0xaa1303e0,
     PatchKind::kWordReplace},
    {"P_ITEMSYSTEM_DIVIDE_108458_CALL", F_ITEMSYSTEM_DIVIDE_VMA, 0x6c, 0x9400e031, 0,
     PatchKind::kBlRedirectToSymbol, "F_GET_CUMULATE_COUNT_VMA", F_GET_CUMULATE_COUNT_VMA},
    {"P_ITEMSYSTEM_DIVIDE_10848C_ARG", F_ITEMSYSTEM_DIVIDE_VMA, 0x94, 0x2a1603e0, 0xaa1303e0,
     PatchKind::kWordReplace},
    {"P_ITEMSYSTEM_DIVIDE_10848C_CALL", F_ITEMSYSTEM_DIVIDE_VMA, 0x98, 0x9400e026, 0,
     PatchKind::kBlRedirectToSymbol, "F_GET_CUMULATE_COUNT_VMA", F_GET_CUMULATE_COUNT_VMA},
    // 主菜单跳过 Com2uS Hive 登录询问 ×1（原 hub_login.inc:19-21）：cbz 分支目标改写。
    {"P_HUB_LOGIN_SKIP", F_STATE_HIDE_MODULE_MAIN_MENU_VMA, 0x48, 0x34000100, 0x34000360,
     PatchKind::kWordReplace},
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
        out->redirect_target = 0;
        out->kind = entry.kind;
        if (entry.kind == PatchKind::kBlRedirectToSymbol) {
            // 目标缺失即视为站点登记无效（fail-closed），不猜测符号。
            if (entry.target_macro == nullptr || entry.target_vma == 0) {
                return false;
            }
            out->redirect_target = g_base + fn_resolve(entry.target_macro, entry.target_vma);
        }
        return true;
    }
    return false;
}

}  // namespace qol::game
