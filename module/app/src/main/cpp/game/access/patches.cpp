#include "game/access/patches.h"

#include <cstring>

#include "game/symbols/game_symbols.h"
#include "game/read/item_class.h"
#include "game/access/game_access.h"

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
    uintptr_t return_vma = 0;
    uint32_t tail_word = 0;
};

constexpr bool encode_branch(uintptr_t from, uintptr_t to, uint32_t* out) {
    if (from == 0 || to == 0 || out == nullptr) return false;
    const int64_t delta = static_cast<int64_t>(to) - static_cast<int64_t>(from);
    constexpr int64_t kMin = -(int64_t{1} << 27);
    constexpr int64_t kMax = (int64_t{1} << 27) - 4;
    if ((delta & 0x3) != 0 || delta < kMin || delta > kMax) return false;
    *out = 0x14000000u | (static_cast<uint32_t>(delta >> 2) & 0x03ffffffu);
    return true;
}

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
    // 世界传送入口：原版为 mov x0,#1，改版可能已有 B 到旧注入块；两者统一进入模块
    // handler，固定回写 mov x0,#1 后跳回函数尾部。
    {"P_UI_PLAY_CALL_MAP_NAME", F_UI_PLAY_CALL_MAP_NAME_VMA,
     F_UI_PLAY_CALL_MAP_NAME_PATCH_OFF, 0xd2800020u, 0,
     PatchKind::kBranchRedirectWithReturn, nullptr, 0,
     F_UI_PLAY_CALL_MAP_NAME_EPILOGUE_OFF, 0xd2800020u},
    {"P_MAINPROCESS_NEXT_STATE", F_MAINPROCESS_VMA, F_MAINPROCESS_NEXT_STATE_CALL_OFF,
     MAINPROCESS_NEXT_STATE_CALL_WORD, 0, PatchKind::kBlRedirect},
    // 自动出售入口按钮绘制宿主（owner=autosell）：+0x1cc 原 bl UIDesc_Draw 改指模块 wrapper。
    {"P_SCENE_DRAW_EQUIP_DESC", F_SCENE_DRAW_EQUIP_VMA, F_SCENE_DRAW_EQUIP_DESC_CALL_OFF,
     F_SCENE_DRAW_EQUIP_DESC_CALL_WORD, 0, PatchKind::kBlRedirect},

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

    // ---- D2a-3a：extension_bag 声明式站点（owner=plugins/extension_bag；地址/原字与
    // 原 raw 实现逐一核对一致）----
    // 8 条 save callsite（行为编排 owner=core/native/module_save.cpp，见 ADR-0007 §4）。
    {"P_EXT_SAVE_REVIVE", F_SAVE_CALLSITE_REVIVE_VMA, 0, 0x9401945e, 0, PatchKind::kBlRedirect},
    {"P_EXT_SAVE_QUEST_CLEAR_1", F_SAVE_CALLSITE_QUEST_CLEAR_1_VMA, 0, 0x940175d8, 0,
     PatchKind::kBlRedirect},
    {"P_EXT_SAVE_QUEST_CLEAR_2", F_SAVE_CALLSITE_QUEST_CLEAR_2_VMA, 0, 0x940175ad, 0,
     PatchKind::kBlRedirect},
    {"P_EXT_SAVE_QUEST_REVIEW", F_SAVE_CALLSITE_QUEST_REVIEW_VMA, 0, 0x94000e5e, 0,
     PatchKind::kBlRedirect},
    {"P_EXT_SAVE_PROCESS", F_SAVE_CALLSITE_PROCESS_VMA, 0, 0x97ffff6c, 0,
     PatchKind::kBlRedirect},
    {"P_EXT_SAVE_NETWORK_ADD", F_SAVE_CALLSITE_NETWORK_ADD_VMA, 0, 0x97ff2fbe, 0,
     PatchKind::kBlRedirect},
    {"P_EXT_SAVE_NETWORK_PROCESS_1", F_SAVE_CALLSITE_NETWORK_PROCESS_1_VMA, 0, 0x97ff2edf, 0,
     PatchKind::kBlRedirect},
    {"P_EXT_SAVE_NETWORK_PROCESS_2", F_SAVE_CALLSITE_NETWORK_PROCESS_2_VMA, 0, 0x97ff2e44, 0,
     PatchKind::kBlRedirect},
    // C9-C14 draw/gate（功能型 required，装饰型 optional；store 尾调用为 B）。
    {"P_EXT_INVEN_ITEM_DRAW", F_UIEQUIP_DRAW_VMA, 0x94, 0x97fffe33, 0, PatchKind::kBlRedirect},
    {"P_EXT_INVEN_BAG_DRAW", F_UIEQUIP_DRAW_VMA, 0x98, 0x97fffee8, 0, PatchKind::kBlRedirect},
    {"P_EXT_INVEN_DRAW_END", F_SCENE_DRAW_EQUIP_VMA, 0x210, 0x97fd11d2, 0,
     PatchKind::kBlRedirect},
    {"P_EXT_DROP_GATE", F_SAVE_ITEM_ON_EMPTY_CALL_VMA, 0, 0x94012fc8, 0,
     PatchKind::kBlRedirect},
    {"P_EXT_DRAW_GATE", F_ITEM_DRAW_PORTING_CALL_VMA, 0, 0x94016d49, 0,
     PatchKind::kBlRedirect},
    {"P_EXT_DESC_OPEN_GATE", F_UIEQUIP_ITEM_DESC_MAKE_DESC_CALL_VMA, 0, 0x97fffdfe, 0,
     PatchKind::kBlRedirect},
    // C15-C18 store。
    {"P_EXT_STORE_DESC", F_UISTORE_DESC_MAKE_DESC_CALL_VMA, 0, 0x97ffffc9, 0,
     PatchKind::kBlRedirect},
    {"P_EXT_STORE_DRAW_END", F_SCENE_DRAW_STORE_GRPX_END_CALL_VMA, 0, 0x97fcff36, 0,
     PatchKind::kBlRedirect},
    {"P_EXT_STORE_BAG_DRAW", F_UISTORE_DRAW_INVEN_BAG_GROUP_CALL_VMA, 0, 0x17ffff1e, 0,
     PatchKind::kBranchRedirect},
    {"P_EXT_STORE_BUY_GATE_1", F_UISTORE_BUY_FIND_SLOT_CALL_1_VMA, 0, 0x9400c530, 0,
     PatchKind::kBlRedirect},
    {"P_EXT_STORE_BUY_GATE_2", F_UISTORE_BUY_FIND_SLOT_CALL_2_VMA, 0, 0x9400c508, 0,
     PatchKind::kBlRedirect},
    // C19-C20 mix。
    {"P_EXT_MIX_DRAW_END", F_SCENE_DRAW_MIX_GRPX_END_CALL_VMA, 0, 0x97fd0fa8, 0,
     PatchKind::kBlRedirect},
    {"P_EXT_MIX_BAG_DRAW", F_UIMIX_DRAW_INVEN_BAG_GROUP_CALL_VMA, 0, 0x97fffe5e, 0,
     PatchKind::kBlRedirect},
};

// 测试覆盖表：仅 host 测试经 patch_site_override_for_test 写入，生产路径容量恒为 0。
// 用于构造尚未登记的站点（如 B 站点）而无需污染生产的 kSites。
constexpr uint32_t kMaxOverrides = 4;
struct SiteOverride {
    const char* name = nullptr;
    PatchSite site{};
};
SiteOverride g_overrides[kMaxOverrides];
uint32_t g_override_count = 0;

}  // namespace

bool patch_site(const char* name, PatchSite* out) {
    if (name == nullptr || out == nullptr) {
        return false;
    }
    for (uint32_t i = 0; i < g_override_count; ++i) {
        if (std::strcmp(g_overrides[i].name, name) == 0) {
            *out = g_overrides[i].site;
            return true;
        }
    }
    if (g_base == 0) {
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
        out->alternate_expected_word = 0;
        out->return_addr = entry.return_vma == 0 ? 0 :
            g_base + entry.vma + entry.return_vma;
        out->tail_word = entry.tail_word;
        if (entry.kind == PatchKind::kBranchRedirectWithReturn &&
            entry.target_vma == 0) {
            const uintptr_t legacy_target = g_base + F_UI_PLAY_CALL_MAP_NAME_LEGACY_TARGET_VMA;
            if (!encode_branch(out->addr, legacy_target, &out->alternate_expected_word)) {
                return false;
            }
        }
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

bool patch_site_override_for_test(const char* name, const PatchSite& site) {
    if (name == nullptr) {
        return false;
    }
    for (uint32_t i = 0; i < g_override_count; ++i) {
        if (std::strcmp(g_overrides[i].name, name) == 0) {
            g_overrides[i].site = site;
            return true;
        }
    }
    if (g_override_count >= kMaxOverrides) {
        return false;
    }
    g_overrides[g_override_count].name = name;
    g_overrides[g_override_count].site = site;
    ++g_override_count;
    return true;
}

void patch_site_clear_overrides_for_test() {
    for (uint32_t i = 0; i < kMaxOverrides; ++i) {
        g_overrides[i] = SiteOverride{};
    }
    g_override_count = 0;
}

}  // namespace qol::game

namespace qol::features::stack_limit::impl {

contracts::QolItemCountEncoding item_count_encoding(const void* item) {
    if (g_base == 0 || item == nullptr) {
        return contracts::QolItemCountEncoding::kUnknown;
    }
    const uint8_t* it = reinterpret_cast<const uint8_t*>(item);
    const uint16_t flags = *reinterpret_cast<const uint16_t*>(it + I_TYPE);
    uint8_t* class_data =
        *reinterpret_cast<uint8_t**>(*reinterpret_cast<void**>(g_base + G_ITEMCLASS_DATA_GOT_VMA));
    uint8_t* size_ptr = *reinterpret_cast<uint8_t**>(g_base + G_ITEMCLASS_SIZE_GOT_VMA);
    const uint8_t stride = size_ptr != nullptr ? *size_ptr : 0;
    return item_count_encoding_from_flags(flags, class_data, stride);
}

bool read_item_count_field(const void* item, uint32_t* out_value) {
    if (g_base == 0 || item == nullptr || out_value == nullptr) return false;
    *out_value = *reinterpret_cast<const uint32_t*>(
        reinterpret_cast<const uint8_t*>(item) + I_COUNT);
    return true;
}

bool write_item_count_field(void* item, uint32_t value) {
    if (g_base == 0 || item == nullptr) return false;
    *reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(item) + I_COUNT) = value;
    return true;
}

}  // namespace qol::features::stack_limit::impl
