// contracts/extension_bag.h + game/access/patches.h + game/access/ptr_slots.h host 单测：
// 覆盖契约头的可编译性 / POD 布局 / 枚举值，21 条 D2a-3a 站点在假 g_base 下的解析
// （地址、原字、kind），以及 move_merge 指针槽站点解析。
//
// 测试注入：不改生产路径。设 g_base 为假基址；fn_resolve 恒等返回 VMA；
// game_memory_accessible 恒真；未纳入本 target 的两个槽解析器给出保守桩。

#include "contracts/extension_bag.h"
#include "contracts/move_merge.h"
#include "contracts/version.h"
#include "plugins/extension_bag/plugin.h"
#include "game/symbols/game_symbols.h"
#include "game/access/patches.h"
#include "game/access/ptr_slots.h"
#include "game/access/ui_equip.h"
#include "game/ops/game_ops_common.h"
#include "game/state/game_state.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <type_traits>

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond) do { \
    if (cond) ++g_pass; \
    else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

// game/access/game_access.h 的外部符号在本 target 由测试提供。
uintptr_t g_base = 0;

// T2h-B3#2：本 target 链接 game/read/item_class.cpp，其 debug raw 原语引用以下符号；
// 该原语在本测试不参与断言，stub 仅满足链接。
bool game_in_world() { return false; }
std::string op_err(const char* msg) {
    return std::string("{\"ok\":false,\"error\":\"") + msg + "\"}";
}
bool inventory_item_ref_at(int, int, InventoryItemRef*) { return false; }

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
    static_assert(qol::contracts::kExtensionBagVersionMajor == 3, "major");
static_assert(qol::contracts::kExtensionBagVersionMinor == 0, "minor");
static_assert(qol::contracts::kExtensionBagSaleRemoveVersionMinor == 1, "sale minor");
static_assert(qol::contracts::kExtensionBagPublishedVersionMinor == 1, "published minor");
static_assert(std::strcmp(qol::contracts::kExtensionBagCapabilityId, "extension_bag") == 0,
              "capability id 必须为 extension_bag");
static_assert(std::strcmp(qol::contracts::kInventoryMergeProjectionCapabilityId,
                          "inventory.merge_projection") == 0,
              "projection capability id");
static_assert(std::strcmp(qol::contracts::kMoveMergeControlCapabilityId,
                          "move_merge.control") == 0,
              "move merge capability id");
static_assert(qol::contracts::kInventoryMergeProjectionVersionMajor == 1 &&
                  qol::contracts::kInventoryMergeProjectionVersionMinor == 0,
              "projection capability version");
static_assert(qol::contracts::kMoveMergeControlVersionMajor == 1 &&
                  qol::contracts::kMoveMergeControlVersionMinor == 0,
              "move merge control capability version");
static_assert(static_cast<uint8_t>(qol::contracts::MoveMergeProjectionDropResult::kNotHandled) == 0,
              "projection drop not handled");
static_assert(static_cast<uint8_t>(qol::contracts::MoveMergeProjectionDropResult::kHandled) == 1,
              "projection drop handled");
static_assert(static_cast<uint8_t>(qol::contracts::MoveMergeProjectionDropResult::kRejected) == 2,
              "projection drop rejected");
static_assert(std::is_standard_layout<qol::contracts::InventoryMergeProjectionApi>::value,
              "projection API POD");
static_assert(std::is_trivially_copyable<qol::contracts::InventoryMergeProjectionApi>::value,
              "projection API trivially copyable");
static_assert(std::is_standard_layout<qol::contracts::MoveMergeControlApi>::value,
              "move merge control API POD");
static_assert(std::is_trivially_copyable<qol::contracts::MoveMergeControlApi>::value,
              "move merge control API trivially copyable");

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
// 现有 40 个函数指针前缀保持位置，v3.1 sale operation 只尾追加。
static_assert(sizeof(qol::contracts::ExtensionBagApi) == 41 * sizeof(void*), "Api 成员数/尺寸");
static_assert(offsetof(qol::contracts::ExtensionBagApi, put_jewel) == 39 * sizeof(void*),
              "v2.2 jewel callback must append without moving prior members");
static_assert(offsetof(qol::contracts::ExtensionBagApi, remove_for_sale) == 40 * sizeof(void*),
              "v3.1 sale callback must append without moving prior members");
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
    // v3.1 sale callback is an ABI tail; ExtensionBagApi intentionally has no size prefix.
    CHECK(sizeof(ExtensionBagApi) == 41 * sizeof(void*));
    CHECK(qol::contracts::satisfies({2, 2}, {2, 1}));
    CHECK(!qol::contracts::satisfies({2, 1}, {2, 2}));
    CHECK(static_cast<uint8_t>(ExtensionBagDropResult::kRejected) == 2);
    CHECK(static_cast<uint8_t>(ExtensionBagEquipButtonResult::kBlocked) == 2);
    CHECK(static_cast<uint8_t>(ExtensionBagEquipEventResult::kBlocked) == 2);
    CHECK(sizeof(qol::contracts::ExtensionBagMoveObservation) == 72);

    const qol::contracts::InventoryMergeProjectionApi projection{
        sizeof(qol::contracts::InventoryMergeProjectionApi), 1, 0,
        nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
    const qol::contracts::MoveMergeControlApi control{
        sizeof(qol::contracts::MoveMergeControlApi), 1, 0, nullptr};
    CHECK(qol::contracts::inventory_merge_projection_api_valid(&projection));
    CHECK(qol::contracts::move_merge_control_api_valid(&control));
    CHECK(!qol::contracts::inventory_merge_projection_api_valid(nullptr));
    CHECK(!qol::contracts::move_merge_control_api_valid(nullptr));
    CHECK(static_cast<uint8_t>(qol::contracts::MoveMergeProjectionDropResult::kRejected) == 2);
}

void test_extension_desc_hook_v2_source_contract() {
    std::ifstream source;
    const std::string test_file = __FILE__;
    const size_t separator = test_file.find_last_of("/\\");
    if (separator != std::string::npos) {
        source.open(test_file.substr(0, separator + 1) +
                    "../plugins/extension_bag/ui/detail_hooks.cpp");
    }
    if (!source) {
        source.clear();
        source.open("../plugins/extension_bag/ui/detail_hooks.cpp");
    }
    if (!source) {
        source.clear();
        source.open("../../plugins/extension_bag/ui/detail_hooks.cpp");
    }
    CHECK(source.good());
    if (!source) return;

    const std::string contents((std::istreambuf_iterator<char>(source)),
                               std::istreambuf_iterator<char>());
    CHECK(contents.find("desc_hook_v2_if_available") != std::string::npos);
    CHECK(contents.find("g_extension_desc_item_ids") != std::string::npos);
    CHECK(contents.find("g_extension_desc_item_originals") == std::string::npos);
    CHECK(contents.find("g_sdk->desc_button.bind") == std::string::npos);
}

void test_extension_bag_claim_manifest_source_contract() {
    std::ifstream source;
    const std::string test_file = __FILE__;
    const size_t separator = test_file.find_last_of("/\\");
    if (separator != std::string::npos) {
        source.open(test_file.substr(0, separator + 1) +
                    "../plugins/extension_bag/assembly/claims.cpp");
    }
    if (!source) {
        source.clear();
        source.open("../plugins/extension_bag/assembly/claims.cpp");
    }
    if (!source) {
        source.clear();
        source.open("../../plugins/extension_bag/assembly/claims.cpp");
    }
    CHECK(source.good());
    if (!source) return;

    const std::string contents((std::istreambuf_iterator<char>(source)),
                               std::istreambuf_iterator<char>());
    const size_t claims_begin = contents.find("static const ::qol::contracts::ResourceClaim kClaims[] = {");
    const size_t claims_end = contents.find("\n    };", claims_begin);
    CHECK(claims_begin != std::string::npos && claims_end != std::string::npos);
    if (claims_begin == std::string::npos || claims_end == std::string::npos) return;

    const std::string claims = contents.substr(claims_begin, claims_end - claims_begin);
    size_t resource_count = 0;
    size_t position = 0;
    while ((position = claims.find("\n        {", position)) != std::string::npos) {
        ++resource_count;
        ++position;
    }
    CHECK(resource_count == 19);

    const size_t desc_slots = claims.find("kEbS1DescSlots, kEbS1DescSlots");
    const size_t desc_gate = claims.find("\"patch:P_EXT_DESC_OPEN_GATE\"");
    CHECK(desc_slots != std::string::npos && desc_gate != std::string::npos &&
          desc_slots < desc_gate);
    CHECK(claims.find("kEbDescSlotsOps") != std::string::npos);
    CHECK(claims.find("reinterpret_cast<void*>(&make_desc_equip_gate)") != std::string::npos);
    CHECK(claims.find("kEbM2StoreSellItem") != std::string::npos);
    CHECK(claims.find("kEbP3StorePopup") != std::string::npos);
    CHECK(claims.find("\"patch:P_EXT_STORE_DESC\"") != std::string::npos);
    CHECK(claims.find("\"patch:P_EXT_STORE_DRAW_END\"") != std::string::npos);
    CHECK(claims.find("\"patch:P_EXT_STORE_BAG_DRAW\"") != std::string::npos);
    CHECK(claims.find("\"patch:P_EXT_STORE_BUY_GATE_1\"") != std::string::npos);
    CHECK(claims.find("\"patch:P_EXT_STORE_BUY_GATE_2\"") != std::string::npos);
    CHECK(claims.find("kEbP4MixPopup") == std::string::npos);
    CHECK(claims.find("\"patch:P_EXT_MIX_") == std::string::npos);
}

void test_extension_bag_json_integer_fields() {
    using qol::features::extension_bag::detail::parse_json_int32_field;
    int32_t value = 77;
    CHECK(parse_json_int32_field("{ \"other\": [1, {\"nested\": 2}], \"index\" : -12 }",
                                 "index", &value));
    CHECK(value == -12);
    CHECK(parse_json_int32_field("{\"index\":2147483647}", "index", &value));
    CHECK(value == INT32_MAX);
    CHECK(parse_json_int32_field("{\"index\":-2147483648}", "index", &value));
    CHECK(value == INT32_MIN);

    constexpr const char* kInvalid[] = {
        "{\"index\":12x}",
        "{\"index\":12.0}",
        "{\"index\":12e0}",
        "{\"index\":+12}",
        "{\"index\":2147483648}",
        "{\"index\":-2147483649}",
        "{\"index\":\"12\"}",
        "{\"nested\":{\"index\":12}}",
        "{\"index\":12,\"index\":13}",
        "{\"index\":01}",
    };
    for (const char* json : kInvalid) {
        value = 77;
        CHECK(!parse_json_int32_field(json, "index", &value));
        CHECK(value == 77);
    }
}

void test_extension_bag_top_level_ok_boolean() {
    using qol::features::extension_bag::detail::JsonBooleanValue;
    using qol::features::extension_bag::detail::parse_json_top_level_boolean;
    CHECK(parse_json_top_level_boolean(
              "{ \"nested\": {\"ok\":false}, \"message\":\"literal \\\"ok\\\":false\", \"ok\" : true }",
              "ok") == JsonBooleanValue::kTrue);
    CHECK(parse_json_top_level_boolean("{\"error\":\"failed\", \"ok\" : false}", "ok") ==
          JsonBooleanValue::kFalse);
    CHECK(parse_json_top_level_boolean("{\"nested\":{\"ok\":true}}", "ok") ==
          JsonBooleanValue::kInvalid);
    CHECK(parse_json_top_level_boolean("{\"ok\":1}", "ok") == JsonBooleanValue::kInvalid);
    CHECK(parse_json_top_level_boolean("{\"ok\":true", "ok") ==
          JsonBooleanValue::kInvalid);
    CHECK(parse_json_top_level_boolean("{\"ok\":true} trailing", "ok") ==
          JsonBooleanValue::kInvalid);
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
    test_extension_desc_hook_v2_source_contract();
    test_extension_bag_claim_manifest_source_contract();
    test_extension_bag_json_integer_fields();
    test_extension_bag_top_level_ok_boolean();
    test_patch_sites_resolve();
    test_patch_site_guards();
    test_move_merge_ptr_slot();
    std::printf("extension_bag_contract: pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
