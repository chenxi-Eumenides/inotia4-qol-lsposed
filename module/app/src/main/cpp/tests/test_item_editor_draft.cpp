#include "plugins/item_editor/model/draft.h"
#include "plugins/item_editor/ui/actions.h"

#include <cstdio>

using namespace qol::contracts;
using namespace qol::features::item_editor;
using namespace qol::features::item_editor::panel_internal;

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(condition) do { \
    if (condition) ++g_pass; \
    else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); } \
} while (0)

static QolItemInventoryView equipment_view() {
    QolItemInventoryView view{};
    view.category = 123;
    view.count_supported = 0;
    view.effective_count = 1;
    view.raw_rarity = 12;
    view.socket_total = 3;
    view.socket_filled = 1;
    view.enchant_remaining = 4;
    view.enchant_id = 29;
    view.enchant_level = 7;
    view.magic_rate = 110;
    view.chaos = 3;
    view.chaos_level = 2;
    view.chaos_rate = 85;
    view.damage = 10;
    view.defense = 20;
    view.need_level = 5;
    view.native_option_count = 2;
    view.native_options[0] = {11, 3, 0};
    view.native_options[1] = {22, 4, 0};
    view.jewel_option_count = 1;
    view.jewel_options[0] = {33, 5, 1};
    view.enchant_option_count = 1;
    view.enchant_options[0] = {44, 29, 2};
    return view;
}

static QolItemInventoryView item_view() {
    QolItemInventoryView view{};
    view.category = 5;
    view.count_supported = 1;
    view.effective_count = 7;
    view.raw_rarity = 0;
    view.native_option_count = 1;
    view.native_options[0] = {99, 1, 0};
    return view;
}

int main() {
    CHECK(item_class_valid(0));
    CHECK(item_class_valid(35));
    CHECK(!item_class_valid(-1));
    CHECK(!item_class_valid(36));
    CHECK(item_class_group(0) == ItemClassGroup::kWeapon);
    CHECK(item_class_group(8) == ItemClassGroup::kWeapon);
    CHECK(item_class_group(9) == ItemClassGroup::kEquipment);
    CHECK(item_class_group(16) == ItemClassGroup::kEquipment);  // 盾牌
    CHECK(item_class_group(21) == ItemClassGroup::kEquipment);
    CHECK(item_class_group(22) == ItemClassGroup::kItem);
    CHECK(item_class_group(35) == ItemClassGroup::kItem);
    CHECK(item_class_is_equipment(0));
    CHECK(item_class_is_equipment(16));
    CHECK(!item_class_is_equipment(22));
    CHECK(item_type_for_class(0) == QolItemType::kEquipment);
    CHECK(item_type_for_class(16) == QolItemType::kEquipment);
    CHECK(item_type_for_class(22) == QolItemType::kPotion);
    CHECK(item_type_for_class(24) == QolItemType::kEnchantScroll);
    CHECK(item_type_for_class(25) == QolItemType::kJewel);
    CHECK(item_type_for_class(31) == QolItemType::kBackpack);

    ItemEditorDraft draft{};
    draft_clear(&draft);
    CHECK(draft.class_index == -1);
    CHECK(draft.item_id == -1);
    CHECK(draft.count == 1);
    CHECK(draft.raw_rarity == kDefaultRawRarity);
    CHECK(draft.chaos_rate == kDefaultChaosRate);
    CHECK(draft.jewel_attribute_type == -1);

    const QolItemHandle source{12, 3};
    draft_load(&draft, source, equipment_view(), 7, QolItemType::kEquipment);
    CHECK(draft.source_valid);
    CHECK(draft.class_index == 7);
    CHECK(draft.item_id == 123);
    CHECK(draft.item_type == QolItemType::kEquipment);
    CHECK(draft.count == 1);
    CHECK(draft.raw_rarity == 12);
    CHECK(draft.socket_total == 3 && draft.socket_filled == 1);
    CHECK(draft.enchant_remaining == 4 && draft.enchant_id == 29 && draft.enchant_level == 7);
    CHECK(draft.magic_rate == 110);
    CHECK(draft.chaos_flags == 3);
    CHECK(draft.chaos_level == 0 && draft.chaos_rate == 85);
    CHECK(draft.native_option_count == 2);
    CHECK(draft.jewel_option_count == 1);
    CHECK(draft.enchant_option_count == 1);
    CHECK(!draft.dirty);

    // 同类确认 == 取消：不迁移、不清 item_id。
    CHECK(!draft_change_class(&draft, 7));
    CHECK(draft.item_id == 123);
    CHECK(!draft.dirty);

    // 装备 -> 装备：保留装备数据，丢弃 item_id。
    CHECK(draft_change_class(&draft, 0));
    CHECK(draft.class_index == 0);
    CHECK(draft.item_id == -1);
    CHECK(!draft_has_item(draft));
    CHECK(draft.dirty);
    CHECK(draft.raw_rarity == 12);
    CHECK(draft.socket_total == 3 && draft.socket_filled == 1);
    CHECK(draft.enchant_remaining == 4 && draft.enchant_id == 29 && draft.enchant_level == 7);
    CHECK(draft.magic_rate == 110);
    CHECK(draft.chaos_flags == 3);
    CHECK(draft.chaos_level == 0 && draft.chaos_rate == 85);
    CHECK(draft.native_option_count == 2 && draft.jewel_option_count == 1 &&
          draft.enchant_option_count == 1);
    CHECK(draft.jewel_attribute_type == -1 && draft.jewel_value == 0);

    // 装备 -> 物品：只保留数量，装备数据全部丢弃。
    draft_load(&draft, source, equipment_view(), 7, QolItemType::kEquipment);
    draft.count = 5;
    CHECK(draft_change_class(&draft, 22));
    CHECK(draft.class_index == 22);
    CHECK(draft.item_id == -1);
    CHECK(draft.count == 5);
    CHECK(draft.raw_rarity == kDefaultRawRarity);
    CHECK(draft.socket_total == 0 && draft.socket_filled == 0);
    CHECK(draft.enchant_remaining == 0 && draft.enchant_id == 0 && draft.enchant_level == 0);
    CHECK(draft.magic_rate == 0);
    CHECK(draft.chaos_level == 0 && draft.chaos_rate == kDefaultChaosRate);
    CHECK(draft.native_option_count == 0 && draft.jewel_option_count == 0 &&
          draft.enchant_option_count == 0);
    CHECK(draft.jewel_attribute_type == -1 && draft.jewel_value == 0);

    // 物品 -> 物品：保留数量，丢弃装备数据和 item_id。
    draft_load(&draft, source, item_view(), 22, QolItemType::kPotion);
    CHECK(draft.item_id == 5);
    CHECK(draft.count == 7);
    CHECK(draft_change_class(&draft, 24));
    CHECK(draft.class_index == 24);
    CHECK(draft.item_id == -1);
    CHECK(draft.count == 7);
    CHECK(draft.native_option_count == 0);
    CHECK(draft.jewel_attribute_type == -1 && draft.jewel_value == 0);

    // 物品 -> 装备：空装备骨架，数量 1，装备默认值。
    draft_load(&draft, source, item_view(), 22, QolItemType::kPotion);
    CHECK(draft_change_class(&draft, 0));
    CHECK(draft.class_index == 0);
    CHECK(draft.item_id == -1);
    CHECK(draft.count == 1);
    CHECK(draft.raw_rarity == kDefaultRawRarity);
    CHECK(draft.socket_total == 0);
    CHECK(draft.chaos_rate == kDefaultChaosRate);
    CHECK(draft.native_option_count == 0);

    // 宝石同类确认保留宝石属性；宝石 -> 非宝石物品丢弃。
    QolItemInventoryView gem = item_view();
    gem.category = 30;
    gem.count_supported = 1;
    gem.effective_count = 1;
    gem.jewel_attribute_type = 3;
    gem.jewel_value = 60;
    draft_load(&draft, source, gem, kItemClassGem, QolItemType::kJewel);
    CHECK(!draft_change_class(&draft, kItemClassGem));
    CHECK(draft.jewel_attribute_type == 3 && draft.jewel_value == 60);
    CHECK(draft.item_id == 30);
    CHECK(draft_change_class(&draft, 24));
    CHECK(draft.jewel_attribute_type == -1 && draft.jewel_value == 0);

    CHECK(!draft_change_class(&draft, -1));
    CHECK(!draft_change_class(&draft, kItemClassCount));
    CHECK(!draft_change_class(nullptr, 0));

    // ---- 强化强度 → 附魔 ID 映射（非 C18 5 位 ID；混沌没有附魔行） ----
    // row = 武器 tier*3+group，防具 16+tier*3+(group-3)；非 C18 ID = row+1。
    CHECK(enchant_id_for_strength(EnchantStrength::kLow, 0, 0) == 1);      // 短剑
    CHECK(enchant_id_for_strength(EnchantStrength::kLow, 1, 0) == 2);      // 长剑组
    CHECK(enchant_id_for_strength(EnchantStrength::kLow, 8, 0) == 3);      // 双手组
    CHECK(enchant_id_for_strength(EnchantStrength::kSpecial, 0, 0) == 13);
    CHECK(enchant_id_for_strength(EnchantStrength::kLow, 12, 0) == 17);    // 帽子
    CHECK(enchant_id_for_strength(EnchantStrength::kSpecial, 15, 0) == 29); // 鞋
    CHECK(enchant_id_for_strength(EnchantStrength::kMid, 10, 0) == 21);    // 皮甲
    CHECK(enchant_id_for_strength(EnchantStrength::kSpecial, 11, 0) == 31); // 板甲
    // 混沌卷轴不是附魔行：任何变体/类别都返回 0，避免 5 位字段截断后静默落到别的行。
    CHECK(enchant_id_for_strength(EnchantStrength::kChaos, 0, 0) == 0);
    CHECK(enchant_id_for_strength(EnchantStrength::kChaos, 12, 0) == 0);
    CHECK(enchant_id_for_strength(EnchantStrength::kChaos, 11, 0) == 0);
    CHECK(enchant_id_for_strength(EnchantStrength::kChaos, 12, 4) == 0);
    // C18 把 32 行压进 4 位 ID（行 0..14 → 1..15，行 16..30 → 1..15）。
    CHECK(enchant_id_for_strength(EnchantStrength::kLow, 0, 4) == 1);
    CHECK(enchant_id_for_strength(EnchantStrength::kLow, 12, 4) == 1);
    CHECK(enchant_id_for_strength(EnchantStrength::kSpecial, 15, 4) == 13);
    // 反查：0 和无对应行都回到“无”，混沌 ID 不再被识别为强度。
    CHECK(enchant_strength_for_id(0, 12, 0) == EnchantStrength::kNone);
    CHECK(enchant_strength_for_id(17, 12, 0) == EnchantStrength::kLow);
    CHECK(enchant_strength_for_id(32, 12, 0) == EnchantStrength::kNone);

    // ---- 动作按钮 payload 推导（ui/actions.h 纯函数） ----
    // 启用条件：具体 item_id 且 class 有效。
    ItemEditorDraft empty{};
    draft_clear(&empty);
    CHECK(!action_draft_ready(empty));
    draft_load(&draft, source, equipment_view(), 7, QolItemType::kEquipment);
    CHECK(action_draft_ready(draft));
    draft.item_id = -1;
    CHECK(!action_draft_ready(draft));
    draft_load(&draft, source, equipment_view(), 7, QolItemType::kEquipment);

    // 装备：type_flags = (item_id&0x3ff)<<6 | (raw_rarity&0xf)<<2；
    // count_field = 100<<25 | chaos_level(7b) | chaos_rate<<8（仅 chaos_flags&3）。
    const QolItemPayloadHeader equip_header = action_payload_header(draft);
    CHECK(equip_header.uid == 0);
    CHECK(equip_header.type_flags == (((123 & 0x3ff) << 6) | ((12 & 0xf) << 2)));
    CHECK(equip_header.count_field ==
          ((100u << 25) | (0u & 0x7f) | ((85u & 0xff) << 8)));
    CHECK(equip_header.magic_rate == 110);
    CHECK(equip_header.socket == (((3 & 0xf) << 4) | (1 & 0xf)));
    CHECK(equip_header.enchant ==
          (((29 & 0x1f) << 11) | ((7 & 0x1f) << 6) | ((4 & 0xf) << 2) | (3 & 3)));

    // 混沌位关闭时不写 chaos 位域。
    draft.chaos_flags = 0;
    CHECK(action_payload_header(draft).count_field == (100u << 25));
    draft.chaos_flags = 3;

    // 词条合并 native→jewel→enchant：code = (type<<13) | (id&0x1fff)，value 为 s16。
    QolItemPayloadOption options[kItemPayloadMaxOptionCount];
    const uint32_t option_total =
        action_payload_options(draft, options, kItemPayloadMaxOptionCount);
    CHECK(option_total == 4);
    CHECK(options[0].code == 3 && options[0].value == 11);      // native id=3
    CHECK(options[1].code == 4 && options[1].value == 22);      // native id=4
    CHECK(options[2].code == ((1 << 13) | 5) && options[2].value == 33);
    CHECK(options[3].code == ((2 << 13) | 29) && options[3].value == 44);

    // id < 0 的空词条被跳过，只计数不写出。
    draft.native_options[1].id = -1;
    const uint32_t skipped =
        action_payload_options(draft, options, kItemPayloadMaxOptionCount);
    CHECK(skipped == 3);
    CHECK(options[0].code == 3 && options[1].code == ((1 << 13) | 5) &&
          options[2].code == ((2 << 13) | 29));
    draft_load(&draft, source, equipment_view(), 7, QolItemType::kEquipment);

    // 宝石本体：属性类型/值占用 count_field，不带数量。
    QolItemInventoryView gem_payload = gem;
    gem_payload.raw_rarity = 5;
    draft_load(&draft, source, gem_payload, kItemClassGem, QolItemType::kJewel);
    const QolItemPayloadHeader gem_header = action_payload_header(draft);
    CHECK(gem_header.type_flags == (((30 & 0x3ff) << 6) | ((5 & 0xf) << 2)));
    CHECK(gem_header.count_field == (((3 & 0x3f) << 18) | (60 & 0x7ff)));
    CHECK(gem_header.socket == 0);
    CHECK(gem_header.enchant == 0);

    // 可堆叠物品：count_field = (count&0xff)<<25。
    QolItemInventoryView stack = item_view();
    stack.raw_rarity = 0;
    stack.effective_count = 7;
    draft_load(&draft, source, stack, 22, QolItemType::kPotion);
    const QolItemPayloadHeader stack_header = action_payload_header(draft);
    CHECK(stack_header.count_field == ((7u & 0xff) << 25));
    CHECK(stack_header.type_flags == ((5 & 0x3ff) << 6));
    CHECK(stack_header.magic_rate == 0);
    CHECK(stack_header.enchant == 0);

    // 容量不足时只计数不越界写出。
    draft.native_option_count = kQolItemInventoryNativeOptionCapacity;
    for (uint32_t i = 0; i < kQolItemInventoryNativeOptionCapacity; ++i)
        draft.native_options[i] = {1, 3, 0};
    QolItemPayloadOption small[8];
    const uint32_t overflow = action_payload_options(draft, small, 8);
    CHECK(overflow == kQolItemInventoryNativeOptionCapacity);
    bool small_written = true;
    for (const auto& entry : small)
        if (entry.code != 3 || entry.value != 1) small_written = false;
    CHECK(small_written);

    std::printf("item_editor_draft_tests pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
