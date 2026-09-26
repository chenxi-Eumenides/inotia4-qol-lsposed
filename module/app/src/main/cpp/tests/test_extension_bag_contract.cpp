// contracts/extension_bag.h + game/access/patches.h + game/access/ptr_slots.h host 单测：
// 覆盖契约头的可编译性 / POD 布局 / 枚举值，21 条 D2a-3a 站点在假 g_base 下的解析
// （地址、原字、kind），以及 move_merge 指针槽站点解析。
//
// 测试注入：不改生产路径。设 g_base 为假基址；fn_resolve 恒等返回 VMA；
// game_memory_accessible 恒真；未纳入本 target 的两个槽解析器给出保守桩。

#include "contracts/extension_bag.h"
#include "contracts/version.h"
#include "game/symbols/game_symbols.h"
#include "game/access/patches.h"
#include "game/access/ptr_slots.h"
#include "game/access/ui_equip.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <type_traits>

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond) do { \
    if (cond) ++g_pass; \
    else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

// game/access/game_access.h 的外部符号在本 target 由测试提供。
uintptr_t g_base = 0;

uintptr_t fn_resolve(const char* /*macro_name*/, uintptr_t vma) {
    return vma;
}

bool game_memory_accessible(const void* /*address*/, size_t /*size*/, char /*permission*/) {
    return true;
}

// ptr_slots.cpp 登记表的另两个解析器：本测试不覆盖，保守返回 false。
namespace qol::game {
bool save_slot_delete_hook_target(void** /*out_slot*/, uintptr_t* /*out_expected*/) {
    return false;
}
bool ui_choice_button_slot_target(void** /*out_slot*/, uintptr_t* /*out_expected*/) {
    return false;
}
}  // namespace qol::game

// ---- 契约头：可编译 + POD + sizeof + 枚举值 ----
static_assert(qol::contracts::kExtensionBagVersionMajor == 2, "major");
static_assert(qol::contracts::kExtensionBagVersionMinor == 2, "minor");
static_assert(std::strcmp(qol::contracts::kExtensionBagCapabilityId, "extension_bag") == 0,
              "capability id 必须为 extension_bag");

static_assert(static_cast<uint8_t>(qol::contracts::ExtensionBagDropResult::kNotExtension) == 0, "drop0");
static_assert(static_cast<uint8_t>(qol::contracts::ExtensionBagDropResult::kHandled) == 1, "drop1");
static_assert(static_cast<uint8_t>(qol::contracts::ExtensionBagDropResult::kRejected) == 2, "drop2");
static_assert(static_cast<uint8_t>(qol::contracts::ExtensionBagEquipButtonResult::kNotExtension) == 0,
              "equip-button0");
static_assert(static_cast<uint8_t>(qol::contracts::ExtensionBagEquipButtonResult::kHandled) == 1,
              "equip-button1");
static_assert(static_cast<uint8_t>(qol::contracts::ExtensionBagEquipButtonResult::kBlocked) == 2,
              "equip-button2");
static_assert(static_cast<uint8_t>(qol::contracts::ExtensionBagEquipEventResult::kNotExtension) == 0,
              "equip-event0");
static_assert(static_cast<uint8_t>(qol::contracts::ExtensionBagEquipEventResult::kHandled) == 1,
              "equip-event1");
static_assert(static_cast<uint8_t>(qol::contracts::ExtensionBagEquipEventResult::kBlocked) == 2,
              "equip-event2");

static_assert(std::is_standard_layout<qol::contracts::ExtensionBagApi>::value, "Api POD");
static_assert(std::is_trivially_copyable<qol::contracts::ExtensionBagApi>::value, "Api trivially copyable");
static_assert(std::is_standard_layout<qol::contracts::ExtensionBagMoveObservation>::value,
              "MoveObservation POD");
static_assert(std::is_trivially_copyable<qol::contracts::ExtensionBagMoveObservation>::value,
              "MoveObservation trivially copyable");
// 40 个函数指针成员。
static_assert(sizeof(qol::contracts::ExtensionBagApi) == 40 * sizeof(void*), "Api 成员数/尺寸");
static_assert(offsetof(qol::contracts::ExtensionBagApi, put_jewel) == 39 * sizeof(void*),
              "v2.2 jewel callback must append without moving prior members");
static_assert(sizeof(qol::contracts::ExtensionBagMoveObservation) == 72, "MoveObservation 尺寸");

namespace {

using qol::contracts::ExtensionBagApi;
using qol::contracts::ExtensionBagDropResult;
using qol::contracts::ExtensionBagEquipButtonResult;
using qol::contracts::ExtensionBagEquipEventResult;
using qol::game::PatchSite;
using qol::game::PtrSlotSite;
using qol::game::PatchKind;

constexpr uintptr_t kBase = 0x10000000;

struct SiteExpect {
    const char* name;
    uintptr_t vma;
    size_t offset;
    uint32_t word;
    PatchKind kind;
};

// 21 条站点（D2a-3a），地址/原字/kind 与任务冻结表逐字一致。
constexpr SiteExpect kSites[] = {
    {"P_EXT_SAVE_REVIVE", F_SAVE_CALLSITE_REVIVE_VMA, 0, 0x9401945e, PatchKind::kBlRedirect},
    {"P_EXT_SAVE_QUEST_CLEAR_1", F_SAVE_CALLSITE_QUEST_CLEAR_1_VMA, 0, 0x940175d8,
     PatchKind::kBlRedirect},
    {"P_EXT_SAVE_QUEST_CLEAR_2", F_SAVE_CALLSITE_QUEST_CLEAR_2_VMA, 0, 0x940175ad,
     PatchKind::kBlRedirect},
    {"P_EXT_SAVE_QUEST_REVIEW", F_SAVE_CALLSITE_QUEST_REVIEW_VMA, 0, 0x94000e5e,
     PatchKind::kBlRedirect},
    {"P_EXT_SAVE_PROCESS", F_SAVE_CALLSITE_PROCESS_VMA, 0, 0x97ffff6c, PatchKind::kBlRedirect},
    {"P_EXT_SAVE_NETWORK_ADD", F_SAVE_CALLSITE_NETWORK_ADD_VMA, 0, 0x97ff2fbe,
     PatchKind::kBlRedirect},
    {"P_EXT_SAVE_NETWORK_PROCESS_1", F_SAVE_CALLSITE_NETWORK_PROCESS_1_VMA, 0, 0x97ff2edf,
     PatchKind::kBlRedirect},
    {"P_EXT_SAVE_NETWORK_PROCESS_2", F_SAVE_CALLSITE_NETWORK_PROCESS_2_VMA, 0, 0x97ff2e44,
     PatchKind::kBlRedirect},
    {"P_EXT_INVEN_ITEM_DRAW", F_UIEQUIP_DRAW_VMA, 0x94, 0x97fffe33, PatchKind::kBlRedirect},
    {"P_EXT_INVEN_BAG_DRAW", F_UIEQUIP_DRAW_VMA, 0x98, 0x97fffee8, PatchKind::kBlRedirect},
    {"P_EXT_INVEN_DRAW_END", F_SCENE_DRAW_EQUIP_VMA, 0x210, 0x97fd11d2, PatchKind::kBlRedirect},
    {"P_EXT_DROP_GATE", F_SAVE_ITEM_ON_EMPTY_CALL_VMA, 0, 0x94012fc8, PatchKind::kBlRedirect},
    {"P_EXT_DRAW_GATE", F_ITEM_DRAW_PORTING_CALL_VMA, 0, 0x94016d49, PatchKind::kBlRedirect},
    {"P_EXT_DESC_OPEN_GATE", F_UIEQUIP_ITEM_DESC_MAKE_DESC_CALL_VMA, 0, 0x97fffdfe,
     PatchKind::kBlRedirect},
    {"P_EXT_STORE_DESC", F_UISTORE_DESC_MAKE_DESC_CALL_VMA, 0, 0x97ffffc9,
     PatchKind::kBlRedirect},
    {"P_EXT_STORE_DRAW_END", F_SCENE_DRAW_STORE_GRPX_END_CALL_VMA, 0, 0x97fcff36,
     PatchKind::kBlRedirect},
    {"P_EXT_STORE_BAG_DRAW", F_UISTORE_DRAW_INVEN_BAG_GROUP_CALL_VMA, 0, 0x17ffff1e,
     PatchKind::kBranchRedirect},
    {"P_EXT_STORE_BUY_GATE_1", F_UISTORE_BUY_FIND_SLOT_CALL_1_VMA, 0, 0x9400c530,
     PatchKind::kBlRedirect},
    {"P_EXT_STORE_BUY_GATE_2", F_UISTORE_BUY_FIND_SLOT_CALL_2_VMA, 0, 0x9400c508,
     PatchKind::kBlRedirect},
    {"P_EXT_MIX_DRAW_END", F_SCENE_DRAW_MIX_GRPX_END_CALL_VMA, 0, 0x97fd0fa8,
     PatchKind::kBlRedirect},
    {"P_EXT_MIX_BAG_DRAW", F_UIMIX_DRAW_INVEN_BAG_GROUP_CALL_VMA, 0, 0x97fffe5e,
     PatchKind::kBlRedirect},
};

void test_contract_field_counts() {
    // 10 组函数指针计数：包含 v2.2 typed jewel operation。
    CHECK(sizeof(ExtensionBagApi) == 40 * sizeof(void*));
    CHECK(qol::contracts::satisfies({2, 2}, {2, 1}));
    CHECK(!qol::contracts::satisfies({2, 1}, {2, 2}));
    CHECK(static_cast<uint8_t>(ExtensionBagDropResult::kRejected) == 2);
    CHECK(static_cast<uint8_t>(ExtensionBagEquipButtonResult::kBlocked) == 2);
    CHECK(static_cast<uint8_t>(ExtensionBagEquipEventResult::kBlocked) == 2);
    CHECK(sizeof(qol::contracts::ExtensionBagMoveObservation) == 72);
}

void test_patch_sites_resolve() {
    g_base = kBase;
    for (const SiteExpect& expected : kSites) {
        PatchSite site{};
        CHECK(qol::game::patch_site(expected.name, &site));
        CHECK(site.addr == kBase + expected.vma + expected.offset);
        CHECK(site.expected_word == expected.word);
        CHECK(site.kind == expected.kind);
    }
}

void test_patch_site_guards() {
    PatchSite site{};
    g_base = 0;
    CHECK(!qol::game::patch_site("P_EXT_SAVE_REVIVE", &site));
    g_base = kBase;
    CHECK(!qol::game::patch_site(nullptr, &site));
    CHECK(!qol::game::patch_site("P_NOT_REGISTERED", &site));
    CHECK(!qol::game::patch_site("P_EXT_SAVE_REVIVE", nullptr));
}

void test_move_merge_ptr_slot() {
    g_base = kBase;
    PtrSlotSite site{};
    CHECK(qol::game::ptr_slot_site("ptrslot:MOVE_MERGE_GOT", &site));
    CHECK(site.slot_addr == kBase + G_UIEQUIP_INVEN_ITEM_PROC_GOT_VMA);
    CHECK(reinterpret_cast<uintptr_t>(site.expected_value) ==
          kBase + F_UIEQUIP_INVEN_ITEM_CONTROL_EVENT_PROC_VMA);

    // 直接解析器：基址未就绪 → false。
    void* slot = nullptr;
    uintptr_t expected = 0;
    g_base = 0;
    CHECK(!qol::game::ui_equip_move_merge_slot_target(&slot, &expected));
    g_base = kBase;
    CHECK(!qol::game::ui_equip_move_merge_slot_target(nullptr, &expected));
    CHECK(!qol::game::ui_equip_move_merge_slot_target(&slot, nullptr));
    CHECK(qol::game::ui_equip_move_merge_slot_target(&slot, &expected));
}

}  // namespace

int main() {
    test_contract_field_counts();
    test_patch_sites_resolve();
    test_patch_site_guards();
    test_move_merge_ptr_slot();
    std::printf("extension_bag_contract: pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
